/* worker.c — bounded blocking thread pool. See worker.h. */

#include "worker.h"
#include "cmcp.h"

#include <pthread.h>
#include <stdlib.h>

/* Queue depth. With submit() blocking when full, this is just how far
 * the run loop may read ahead of the workers before it throttles —
 * 64 is ample; the loop is one producer. */
#define POOL_QUEUE_CAP 64

typedef struct {
    void (*fn)(void *);
    void  *arg;
} pool_job_t;

struct cmcp_pool {
    pthread_t       *threads;
    size_t           n_threads;     /* threads actually started */

    pool_job_t       jobs[POOL_QUEUE_CAP];
    size_t           head;          /* next job to pop  */
    size_t           tail;          /* next slot to push */
    size_t           count;         /* jobs in the ring  */

    int              shutdown;      /* set by free(); no further submits */

    pthread_mutex_t  mu;
    pthread_cond_t   not_empty;     /* workers wait here */
    pthread_cond_t   not_full;      /* submit waits here */
};

/* Worker loop: pop and run jobs. Once shutdown is set, the worker
 * keeps draining whatever is still queued — so every job's function
 * runs — and exits only when the ring is empty. */
static void *pool_worker(void *arg) {
    cmcp_pool_t *p = (cmcp_pool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (p->count == 0 && !p->shutdown)
            pthread_cond_wait(&p->not_empty, &p->mu);
        if (p->count == 0) {                 /* empty + shutdown → exit */
            pthread_mutex_unlock(&p->mu);
            return NULL;
        }
        pool_job_t job = p->jobs[p->head];
        p->head = (p->head + 1) % POOL_QUEUE_CAP;
        p->count--;
        pthread_cond_signal(&p->not_full);
        pthread_mutex_unlock(&p->mu);

        job.fn(job.arg);
    }
}

cmcp_pool_t *cmcp_pool_new(size_t n_threads) {
    if (n_threads < 1) return NULL;

    cmcp_pool_t *p = (cmcp_pool_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->threads = (pthread_t *)calloc(n_threads, sizeof *p->threads);
    if (!p->threads) { free(p); return NULL; }

    if (pthread_mutex_init(&p->mu, NULL) != 0) {
        free(p->threads); free(p);
        return NULL;
    }
    if (pthread_cond_init(&p->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&p->mu);
        free(p->threads); free(p);
        return NULL;
    }
    if (pthread_cond_init(&p->not_full, NULL) != 0) {
        pthread_cond_destroy(&p->not_empty);
        pthread_mutex_destroy(&p->mu);
        free(p->threads); free(p);
        return NULL;
    }

    /* Spawn workers. If one fails, shut the started ones down cleanly. */
    for (size_t i = 0; i < n_threads; i++) {
        if (pthread_create(&p->threads[i], NULL, pool_worker, p) != 0) {
            pthread_mutex_lock(&p->mu);
            p->shutdown = 1;
            pthread_cond_broadcast(&p->not_empty);
            pthread_mutex_unlock(&p->mu);
            for (size_t j = 0; j < p->n_threads; j++)
                pthread_join(p->threads[j], NULL);
            pthread_cond_destroy(&p->not_full);
            pthread_cond_destroy(&p->not_empty);
            pthread_mutex_destroy(&p->mu);
            free(p->threads); free(p);
            return NULL;
        }
        p->n_threads++;
    }
    return p;
}

int cmcp_pool_submit(cmcp_pool_t *p, void (*fn)(void *), void *arg) {
    if (!p || !fn) return CMCP_EINVAL;

    pthread_mutex_lock(&p->mu);
    while (p->count == POOL_QUEUE_CAP && !p->shutdown)
        pthread_cond_wait(&p->not_full, &p->mu);
    if (p->shutdown) {
        pthread_mutex_unlock(&p->mu);
        return CMCP_EINVAL;
    }
    p->jobs[p->tail].fn  = fn;
    p->jobs[p->tail].arg = arg;
    p->tail = (p->tail + 1) % POOL_QUEUE_CAP;
    p->count++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->mu);
    return CMCP_OK;
}

void cmcp_pool_free(cmcp_pool_t *p) {
    if (!p) return;

    pthread_mutex_lock(&p->mu);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_cond_broadcast(&p->not_full);
    pthread_mutex_unlock(&p->mu);

    for (size_t i = 0; i < p->n_threads; i++)
        pthread_join(p->threads[i], NULL);

    pthread_cond_destroy(&p->not_full);
    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->mu);
    free(p->threads);
    free(p);
}
