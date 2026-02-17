/*
 * epoch.h — Epoch-based memory reclamation (EBR)
 *
 * Provides safe deferred freeing for lock-free data structures.
 * Threads announce entry/exit from critical sections. Nodes are
 * retired (deferred free) and actually freed when no thread can
 * still hold a reference.
 *
 * Design: 3-epoch system (Fraser, 2004; Keir Fraser's PhD thesis)
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

#define EPOCH_COUNT     3
#define EPOCH_MAX_THREADS 64

/* Callback for freeing a retired node */
typedef void (*epoch_free_fn)(void *ptr);

/*
 * epoch_t — Global epoch state
 */
typedef struct epoch {
    _Atomic uint64_t    global_epoch;
    _Atomic uint64_t    thread_epochs[EPOCH_MAX_THREADS];
    _Atomic bool        thread_active[EPOCH_MAX_THREADS];

    /* Per-epoch retire lists (simple linked list of pending frees) */
    struct epoch_node  *retire_lists[EPOCH_COUNT];
    _Atomic uint32_t    retire_counts[EPOCH_COUNT];

    epoch_free_fn       free_fn;
} epoch_t;

struct epoch_node {
    struct epoch_node *next;
    void              *ptr;
};

/*
 * epoch_init — Initialize the epoch system
 *
 * @free_fn: Function to call to actually free retired nodes
 */
void epoch_init(epoch_t *e, epoch_free_fn free_fn);

/*
 * epoch_destroy — Destroy and free all pending retired nodes
 */
void epoch_destroy(epoch_t *e);

/*
 * epoch_register — Register the current thread (returns thread slot)
 *
 * Must be called once per thread before using epoch_enter/exit.
 */
int epoch_register(epoch_t *e);

/*
 * epoch_unregister — Unregister a thread slot
 */
void epoch_unregister(epoch_t *e, int slot);

/*
 * epoch_enter — Enter a critical section (read-side)
 *
 * Must be called before accessing any shared data.
 * Returns the current epoch.
 */
uint64_t epoch_enter(epoch_t *e, int slot);

/*
 * epoch_exit — Exit a critical section
 */
void epoch_exit(epoch_t *e, int slot);

/*
 * epoch_retire — Defer freeing of a node
 *
 * The node will be freed (via free_fn) once it's safe — i.e., when
 * all threads have advanced past the current epoch.
 */
void epoch_retire(epoch_t *e, void *ptr);

/*
 * epoch_try_advance — Try to advance the global epoch and reclaim
 *
 * Called periodically (e.g., on epoch_enter). If all threads have
 * observed the current epoch, advance and free the oldest retire list.
 */
void epoch_try_advance(epoch_t *e);

#endif /* EPOCH_H */
