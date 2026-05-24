#include "cmcp.h"
#include "cmcp_types.h"

#include <string.h>

const char *cmcp_errstr(int err) {
    switch (err) {
    case CMCP_OK:           return "ok";
    case CMCP_EINVAL:       return "invalid argument";
    case CMCP_ENOMEM:       return "out of memory";
    case CMCP_EIO:          return "I/O error";
    case CMCP_EPARSE:       return "parse error";
    case CMCP_ETRANSPORT:   return "transport error";
    case CMCP_EPROTOCOL:    return "protocol error";
    case CMCP_ETIMEOUT:     return "timeout";
    case CMCP_ECANCELLED:   return "cancelled";
    case CMCP_EUNSUPPORTED: return "unsupported";
    case CMCP_ESCHEMA:      return "schema validation failed";
    case CMCP_ENOTFOUND:    return "not found";
    case CMCP_EHANDLER:     return "handler error";
    default:                return "unknown error";
    }
}

/* RFC 5424 syslog level names, indexed by cmcp_log_level_t. */
static const char *const k_log_level_names[] = {
    "debug", "info", "notice", "warning",
    "error", "critical", "alert", "emergency",
};

const char *cmcp_log_level_to_name(cmcp_log_level_t lvl) {
    if ((int)lvl < 0 || (int)lvl >= (int)(sizeof k_log_level_names /
                                          sizeof k_log_level_names[0])) {
        return NULL;
    }
    return k_log_level_names[lvl];
}

int cmcp_log_level_from_name(const char *name, cmcp_log_level_t *out_level) {
    if (!name || !out_level) return -1;
    for (size_t i = 0;
         i < sizeof k_log_level_names / sizeof k_log_level_names[0]; i++) {
        if (strcmp(name, k_log_level_names[i]) == 0) {
            *out_level = (cmcp_log_level_t)i;
            return 0;
        }
    }
    return -1;
}
