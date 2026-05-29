/* Helpers shared by the stdio + HTTP soak drivers.
 *
 * Header-only; each driver TU pulls in only what it needs. Kept here
 * (not under include/) because soak is a test harness, not part of
 * the library's public surface.
 *
 * The two drivers share:
 *   - /proc/<pid>/status + /proc/<pid>/fd sampling for VmRSS, FD
 *     count, thread count (the drift metrics);
 *   - a 4096-entry latency ring buffer with p50/p99 quantiles per
 *     sample interval;
 *   - the `tools/call echo` workload payload;
 *   - the CSV header so run.sh's awk can read either driver's output.
 *
 * What's NOT shared: spawning the child (pipes vs ephemeral TCP),
 * argv parsing, the main loop. Each driver owns its setup. */

#ifndef CMCP_SOAK_COMMON_H
#define CMCP_SOAK_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include "cmcp.h"
#include "cmcp_client.h"
#include "cmcp_json.h"
#include "cmcp_types.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/* ====================================================================== */
/* Wall-clock helper                                                       */
/* ====================================================================== */

static inline double soak_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ====================================================================== */
/* /proc sampling                                                          */
/* ====================================================================== */

typedef struct {
    long rss_kb;
    int  threads;
    int  fd_count;
} soak_proc_t;

/* VmRSS + Threads from /proc/<pid>/status; FD count from
 * /proc/<pid>/fd. FD count is more honest than FDSize (which only
 * grows) for leak detection. */
static inline void soak_read_proc(pid_t pid, soak_proc_t *out) {
    out->rss_kb = -1; out->threads = -1; out->fd_count = -1;

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "VmRSS:", 6) == 0)
                sscanf(line + 6, "%ld", &out->rss_kb);
            else if (strncmp(line, "Threads:", 8) == 0)
                sscanf(line + 8, "%d", &out->threads);
        }
        fclose(f);
    }

    snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    DIR *d = opendir(path);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] != '.') n++;
        }
        closedir(d);
    }
    out->fd_count = n;
}

/* ====================================================================== */
/* Latency ring buffer + quantiles                                         */
/* ====================================================================== */
/* Reset each sample interval; reports p50/p99 over the just-completed
 * window. Capped at 4096 entries — at sub-ms call latency and 5s
 * sample interval we typically fit well within. */

#define SOAK_LAT_CAP 4096

typedef struct {
    long   buf[SOAK_LAT_CAP];
    size_t n;
} soak_latbuf_t;

static inline int soak_lat_cmp(const void *a, const void *b) {
    long da = *(const long *)a, db = *(const long *)b;
    return (da > db) - (da < db);
}

static inline void soak_latbuf_quantiles(const soak_latbuf_t *l,
                                          long *p50, long *p99) {
    if (l->n == 0) { *p50 = 0; *p99 = 0; return; }
    long *tmp = (long *)malloc(l->n * sizeof(long));
    if (!tmp) { *p50 = 0; *p99 = 0; return; }
    memcpy(tmp, l->buf, l->n * sizeof(long));
    qsort(tmp, l->n, sizeof(long), soak_lat_cmp);
    *p50 = tmp[l->n / 2];
    *p99 = tmp[(l->n * 99) / 100];
    free(tmp);
}

/* ====================================================================== */
/* Workload                                                                */
/* ====================================================================== */

static inline int soak_do_one_call(cmcp_client_t *c, long *out_us) {
    cmcp_json_t *params = cmcp_json_new_object();
    cmcp_json_object_set(params, "name", cmcp_json_new_string("echo"));
    cmcp_json_t *args = cmcp_json_new_object();
    cmcp_json_object_set(args, "text",
                          cmcp_json_new_string("soak workload payload"));
    cmcp_json_object_set(params, "arguments", args);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cmcp_rpc_message_t resp;
    cmcp_rpc_message_init(&resp);
    int rc = cmcp_client_request(c, "tools/call", params, &resp);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    cmcp_rpc_message_clear(&resp);

    *out_us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L
            + (long)(t1.tv_nsec - t0.tv_nsec) / 1000L;
    return rc;
}

/* ====================================================================== */
/* CSV row schema (shared so run.sh + run_http.sh can use one awk).        */
/* ====================================================================== */

#define SOAK_CSV_HEADER \
    "elapsed_s,parent_rss_kb,parent_fd,parent_threads," \
    "child_rss_kb,child_fd,child_threads,calls,p50_us,p99_us\n"

#endif /* CMCP_SOAK_COMMON_H */
