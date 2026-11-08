/* seal.c — HMAC-SHA256 seal for MCP `requestState` (see seal.h). */
#include "seal.h"

#include "cmcp.h"
#include "cmcp_json.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEAL_PREFIX     "v1."
#define SEAL_PREFIX_LEN 3u
#define MAC64_LEN       43u   /* base64url of 32 bytes, unpadded */

/* ---------------------------------------------------------------- */
/* base64url (RFC 4648 §5, no padding)                               */
/* ---------------------------------------------------------------- */

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Emit the top `n` sextets of the 24-bit group `v` (n = 2..4). */
static size_t emit_group(char *out, unsigned v, unsigned n) {
    for (unsigned k = 0; k < n; k++)
        out[k] = B64URL[(v >> (18u - 6u * k)) & 63u];
    return n;
}

char *cmcp_b64url_encode(const unsigned char *in, size_t len) {
    size_t out_len = (len / 3) * 4 + ((len % 3) ? (len % 3) + 1 : 0);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    for (; i + 3 <= len; i += 3) {
        unsigned v = ((unsigned)in[i] << 16u) | ((unsigned)in[i + 1] << 8u) |
                     (unsigned)in[i + 2];
        o += emit_group(out + o, v, 4);
    }
    if (len - i == 1) {
        o += emit_group(out + o, (unsigned)in[i] << 16u, 2);
    } else if (len - i == 2) {
        unsigned v = ((unsigned)in[i] << 16u) | ((unsigned)in[i + 1] << 8u);
        o += emit_group(out + o, v, 3);
    }
    out[o] = '\0';
    return out;
}

static int b64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

unsigned char *cmcp_b64url_decode(const char *in, size_t len, size_t *out_len) {
    if (len % 4 == 1) return NULL;            /* no valid encoding */
    size_t cap = (len / 4) * 3 + 3;
    unsigned char *out = (unsigned char *)malloc(cap + 1);
    if (!out) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64url_value(in[i]);
        if (v < 0) { free(out); return NULL; }
        acc = (acc << 6u) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
        }
        acc &= (1u << bits) - 1u;
    }
    /* Leftover bits must be zero, so every input has one encoding. */
    if (acc != 0) { free(out); return NULL; }
    out[o] = '\0';
    *out_len = o;
    return out;
}

/* ---------------------------------------------------------------- */
/* Seal                                                              */
/* ---------------------------------------------------------------- */

/* Build the JSON payload {"m","n","x","s"}; returns an emitted string. */
static char *payload_emit(const char *method, const char *name,
                          const char *state, long long expiry) {
    cmcp_json_t *o = cmcp_json_new_object();
    if (!o) return NULL;
    cmcp_json_t *vals[4] = {
        cmcp_json_new_string(method),
        cmcp_json_new_string(name ? name : ""),
        cmcp_json_new_int(expiry),
        cmcp_json_new_string(state),
    };
    static const char *const keys[4] = {"m", "n", "x", "s"};
    char *text = NULL;
    int ok = 1;
    for (unsigned i = 0; i < 4; i++) {
        if (!ok || !vals[i] || cmcp_json_object_set(o, keys[i], vals[i]) != 0) {
            cmcp_json_free(vals[i]);   /* set takes ownership only on success */
            ok = 0;
        }
    }
    if (ok) text = cmcp_json_emit(o);
    cmcp_json_free(o);
    return text;
}

int cmcp_seal_make(const unsigned char *key, size_t key_len,
                   const char *method, const char *name,
                   const char *state, long long expiry,
                   char **out_token) {
    if (!out_token) return CMCP_EINVAL;
    *out_token = NULL;
    if (!key || key_len == 0 || !method || !state) return CMCP_EINVAL;

    char *payload = payload_emit(method, name, state, expiry);
    if (!payload) return CMCP_ENOMEM;
    char *p64 = cmcp_b64url_encode((const unsigned char *)payload,
                                   strlen(payload));
    free(payload);
    if (!p64) return CMCP_ENOMEM;

    /* MAC over "v1." + payload64: the version is authenticated too. */
    size_t signed_len = SEAL_PREFIX_LEN + strlen(p64);
    size_t total = signed_len + 1 + MAC64_LEN;
    char *tok = (char *)malloc(total + 1);
    char *m64 = NULL;
    if (tok) {
        snprintf(tok, total + 1, "%s%s", SEAL_PREFIX, p64);
        unsigned char mac[CMCP_SHA256_LEN];
        cmcp_hmac_sha256(key, key_len, tok, signed_len, mac);
        m64 = cmcp_b64url_encode(mac, sizeof mac);
    }
    free(p64);
    if (!tok || !m64) { free(tok); free(m64); return CMCP_ENOMEM; }
    snprintf(tok + signed_len, total + 1 - signed_len, ".%s", m64);
    free(m64);
    *out_token = tok;
    return CMCP_OK;
}

/* Check the MAC of `token` (already known to start with "v1.").
 * `dot` points at the separator before the MAC. 1 when authentic. */
static int mac_ok(const unsigned char *key, size_t key_len,
                  const char *token, const char *dot) {
    size_t signed_len = (size_t)(dot - token);
    size_t mac_len = 0;
    unsigned char *got = cmcp_b64url_decode(dot + 1, strlen(dot + 1), &mac_len);
    if (!got) return 0;
    unsigned char want[CMCP_SHA256_LEN];
    cmcp_hmac_sha256(key, key_len, token, signed_len, want);
    int ok = mac_len == CMCP_SHA256_LEN &&
             cmcp_ct_equal(got, want, CMCP_SHA256_LEN);
    free(got);
    return ok;
}

static int str_field_is(const cmcp_json_t *o, const char *k, const char *want) {
    const cmcp_json_t *v = cmcp_json_object_get(o, k);
    return v && v->type == CMCP_JSON_STRING &&
           strcmp(cmcp_json_string(v), want) == 0;
}

/* Copy the "s" field out of an authenticated payload once its binding
 * (method, name, expiry) checks out. */
static int payload_state(const cmcp_json_t *o, const char *method,
                         const char *name, long long now, char **out_state) {
    const cmcp_json_t *x = cmcp_json_object_get(o, "x");
    const cmcp_json_t *s = cmcp_json_object_get(o, "s");
    if (!str_field_is(o, "m", method) ||
        !str_field_is(o, "n", name ? name : "") ||
        !x || x->type != CMCP_JSON_INT || now > cmcp_json_int(x) ||
        !s || s->type != CMCP_JSON_STRING)
        return CMCP_EPROTOCOL;

    size_t sl = cmcp_json_string_len(s);
    char *copy = (char *)malloc(sl + 1);
    if (!copy) return CMCP_ENOMEM;
    memcpy(copy, cmcp_json_string(s), sl);
    copy[sl] = '\0';
    *out_state = copy;
    return CMCP_OK;
}

int cmcp_seal_open(const unsigned char *key, size_t key_len,
                   const char *token, const char *method, const char *name,
                   long long now, char **out_state) {
    if (!out_state) return CMCP_EINVAL;
    *out_state = NULL;
    if (!key || key_len == 0 || !token || !method) return CMCP_EINVAL;

    size_t len = strlen(token);
    if (len > CMCP_SEAL_MAX_TOKEN || len <= SEAL_PREFIX_LEN ||
        memcmp(token, SEAL_PREFIX, SEAL_PREFIX_LEN) != 0)
        return CMCP_EPROTOCOL;
    const char *dot = strrchr(token, '.');
    if (!dot || dot < token + SEAL_PREFIX_LEN) return CMCP_EPROTOCOL;

    /* Authenticate before parsing anything the client sent. */
    if (!mac_ok(key, key_len, token, dot)) return CMCP_EPROTOCOL;

    size_t plen = 0;
    const char *p64 = token + SEAL_PREFIX_LEN;
    unsigned char *payload = cmcp_b64url_decode(p64, (size_t)(dot - p64), &plen);
    if (!payload) return CMCP_EPROTOCOL;
    cmcp_json_t *o = cmcp_json_parse((const char *)payload, plen);
    free(payload);

    int rc = CMCP_EPROTOCOL;
    if (o && o->type == CMCP_JSON_OBJECT)
        rc = payload_state(o, method, name, now, out_state);
    cmcp_json_free(o);
    return rc;
}
