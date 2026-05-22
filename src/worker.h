/* worker.h — internal fixed-size thread pool for the server run loop.
 *
 * Not a public header: it lives in src/ and is used only by server.c.
 * A bounded blocking queue feeds N worker threads. Submit blocks while
 * the queue is full, which becomes backpressure onto the run loop.
 * cmcp_pool_free() drains every queued job before joining the threads,
 * so each job's function (and thus its destructor) runs exactly once.
 */
#ifndef CMCP_WORKER_H
#define CMCP_WORKER_H

#include <stddef.h>

typedef struct cmcp_pool cmcp_pool_t;

/* Create a pool of `n_threads` workers (n_threads >= 1). Returns NULL
 * on allocation or pthread failure. */
cmcp_pool_t *cmcp_pool_new(size_t n_threads);

/* Enqueue fn(arg) to run on a worker thread. Blocks while the queue is
 * full. Returns CMCP_OK, or CMCP_EINVAL on bad arguments or once
 * cmcp_pool_free() has begun — on a non-OK return the caller still
 * owns `arg`. */
int cmcp_pool_submit(cmcp_pool_t *p, void (*fn)(void *), void *arg);

/* Stop accepting submits, let the workers drain every queued job, join
 * all threads, then free the pool. Safe to call with NULL. */
void cmcp_pool_free(cmcp_pool_t *p);

#endif
