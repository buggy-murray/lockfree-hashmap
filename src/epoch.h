/*
 * epoch.h — Epoch-based memory reclamation (EBR)
 *
 * Provides safe deferred freeing for lock-free data structures.
 * Threads announce entry/exit from critical sections. Nodes are
 * retired (deferred free) and actually freed when no thread can
 * still hold a reference.
 *
 * Design: 3-epoch system (Fraser, 2004) with per-thread retire lists
 * to eliminate mutex contention on the retire path.
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#ifndef EPOCH_H
#define EPOCH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define EPOCH_COUNT       3
#define EPOCH_MAX_THREADS 64

/* Callback for freeing a retired node */
typedef void (*epoch_free_fn)(void *ptr);

struct epoch_node {
    struct epoch_node *next;
    void              *ptr;
};

/*
 * Per-thread state: epoch + retire lists (no sharing, no locks needed)
 */
typedef struct epoch_thread {
    _Atomic uint64_t   epoch;       /* Last observed global epoch      */
    _Atomic bool       active;      /* Registered?                     */
    bool               in_critical; /* Currently in epoch_enter/exit?  */

    /* Per-epoch retire lists — thread-local, no contention */
    struct epoch_node *retire[EPOCH_COUNT];
    uint32_t           retire_count[EPOCH_COUNT];
} epoch_thread_t;

/*
 * epoch_t — Global epoch state
 */
typedef struct epoch {
    _Atomic uint64_t    global_epoch;
    epoch_thread_t      threads[EPOCH_MAX_THREADS];
    epoch_free_fn       free_fn;
} epoch_t;

/*
 * epoch_init — Initialize the epoch system
 */
void epoch_init(epoch_t *e, epoch_free_fn free_fn);

/*
 * epoch_destroy — Destroy and free all pending retired nodes
 */
void epoch_destroy(epoch_t *e);

/*
 * epoch_register — Register the current thread (returns thread slot)
 */
int epoch_register(epoch_t *e);

/*
 * epoch_unregister — Unregister a thread slot (drains its retire lists)
 */
void epoch_unregister(epoch_t *e, int slot);

/*
 * epoch_enter — Enter a critical section (read-side)
 */
uint64_t epoch_enter(epoch_t *e, int slot);

/*
 * epoch_exit — Exit a critical section
 */
void epoch_exit(epoch_t *e, int slot);

/*
 * epoch_retire — Defer freeing of a node (thread-local, lock-free)
 *
 * Uses thread-local slot from the calling thread. Pass slot explicitly.
 */
void epoch_retire(epoch_t *e, void *ptr);

/*
 * epoch_retire_slot — Retire with explicit slot (avoids TLS lookup)
 */
void epoch_retire_slot(epoch_t *e, int slot, void *ptr);

/*
 * epoch_try_advance — Try to advance the global epoch and reclaim
 */
void epoch_try_advance(epoch_t *e);

#endif /* EPOCH_H */
