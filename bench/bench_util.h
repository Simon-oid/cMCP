/* Tiny helpers shared by the bench binaries. Header-only; each bench
 * TU pulls in what it needs. The bench tree is deliberately thin —
 * these are not part of the library. */

#ifndef CMCP_BENCH_UTIL_H
#define CMCP_BENCH_UTIL_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====================================================================== */
/* Wall-clock helpers                                                      */
/* ====================================================================== */

static inline long long bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static inline double bench_now_s(void) {
    return (double)bench_now_ns() / 1e9;
}

/* ====================================================================== */
/* Latency histogram: one fixed buffer of per-call microsecond samples.    */
/* qsort + percentile lookup is good enough at our scale (<200k samples).  */
/* ====================================================================== */

typedef struct {
    long  *samples;
    size_t cap;
    size_t n;
} bench_lat_t;

static inline int bench_lat_init(bench_lat_t *l, size_t cap) {
    l->samples = (long *)malloc(cap * sizeof(long));
    if (!l->samples) return -1;
    l->cap = cap;
    l->n   = 0;
    return 0;
}

static inline void bench_lat_free(bench_lat_t *l) {
    free(l->samples);
    l->samples = NULL;
    l->cap = l->n = 0;
}

static inline void bench_lat_record(bench_lat_t *l, long us) {
    if (l->n < l->cap) l->samples[l->n++] = us;
}

static int bench_lat_cmp(const void *a, const void *b) {
    long da = *(const long *)a, db = *(const long *)b;
    return (da > db) - (da < db);
}

typedef struct {
    long    p50_us;
    long    p95_us;
    long    p99_us;
    long    p999_us;
    double  mean_us;
    long    min_us;
    long    max_us;
} bench_summary_t;

static inline void bench_lat_summarize(bench_lat_t *l, bench_summary_t *out) {
    memset(out, 0, sizeof *out);
    if (l->n == 0) return;
    qsort(l->samples, l->n, sizeof(long), bench_lat_cmp);
    out->p50_us  = l->samples[l->n / 2];
    out->p95_us  = l->samples[(l->n * 95)  / 100];
    out->p99_us  = l->samples[(l->n * 99)  / 100];
    out->p999_us = l->samples[(l->n * 999) / 1000];
    out->min_us  = l->samples[0];
    out->max_us  = l->samples[l->n - 1];

    double sum = 0;
    for (size_t i = 0; i < l->n; i++) sum += (double)l->samples[i];
    out->mean_us = sum / (double)l->n;
}

/* ====================================================================== */
/* CSV row emitter. All benches share one schema so results.csv is grep-   */
/* able.                                                                   */
/* ====================================================================== */

#define BENCH_CSV_HEADER \
    "bench,iterations,wall_ms,throughput_per_s," \
    "min_us,p50_us,p95_us,p99_us,p999_us,max_us,mean_us,extra\n"

static inline void bench_emit_row(FILE *out,
                                   const char       *bench_name,
                                   size_t            iterations,
                                   double            wall_s,
                                   const bench_summary_t *s,
                                   const char       *extra) {
    double tput = (wall_s > 0.0) ? ((double)iterations / wall_s) : 0.0;
    fprintf(out,
            "%s,%zu,%.3f,%.2f,%ld,%ld,%ld,%ld,%ld,%ld,%.2f,%s\n",
            bench_name,
            iterations,
            wall_s * 1000.0,
            tput,
            s->min_us, s->p50_us, s->p95_us, s->p99_us, s->p999_us,
            s->max_us, s->mean_us,
            extra ? extra : "");
    fflush(out);
}

#endif /* CMCP_BENCH_UTIL_H */
