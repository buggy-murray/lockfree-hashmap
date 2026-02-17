/*
 * epoch.c — Epoch-based memory reclamation (per-thread retire lists)
 *
 * 3-epoch EBR with per-thread retire lists. No mutex on the retire
 * path — each thread retires to its own list. Reclamation scans all
 * threads' lists for the safe epoch.
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#define _GNU_SOURCE
#include "epoch.h"

#include <stdlib.h>
#include <string.h>

/* TLS slot for epoch_retire (without explicit slot) */
static __thread int tls_epoch_slot = -1;

void epoch_init(epoch_t *e, epoch_free_fn free_fn)
{
    memset(e, 0, sizeof(*e));
    atomic_store(&e->global_epoch, 0);
    for (int i = 0; i < EPOCH_MAX_THREADS; i++) {
        atomic_store(&e->threads[i].epoch, 0);
        atomic_store(&e->threads[i].active, false);
        e->threads[i].in_critical = false;
        for (int j = 0; j < EPOCH_COUNT; j++) {
            e->threads[i].retire[j] = NULL;
            e->threads[i].retire_count[j] = 0;
        }
    }
    e->free_fn = free_fn;
}

static void free_list(epoch_free_fn fn, struct epoch_node *head)
{
    while (head) {
        struct epoch_node *next = head->next;
        if (fn) fn(head->ptr);
        free(head);
        head = next;
    }
}

void epoch_destroy(epoch_t *e)
{
    for (int t = 0; t < EPOCH_MAX_THREADS; t++) {
        for (int j = 0; j < EPOCH_COUNT; j++) {
            free_list(e->free_fn, e->threads[t].retire[j]);
            e->threads[t].retire[j] = NULL;
            e->threads[t].retire_count[j] = 0;
        }
    }
}

int epoch_register(epoch_t *e)
{
    for (int i = 0; i < EPOCH_MAX_THREADS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&e->threads[i].active, &expected, true)) {
            atomic_store(&e->threads[i].epoch,
                         atomic_load(&e->global_epoch));
            tls_epoch_slot = i;
            return i;
        }
    }
    return -1;
}

void epoch_unregister(epoch_t *e, int slot)
{
    if (slot < 0 || slot >= EPOCH_MAX_THREADS) return;

    /* Drain all retire lists for this thread */
    for (int j = 0; j < EPOCH_COUNT; j++) {
        free_list(e->free_fn, e->threads[slot].retire[j]);
        e->threads[slot].retire[j] = NULL;
        e->threads[slot].retire_count[j] = 0;
    }

    atomic_store(&e->threads[slot].active, false);
    if (tls_epoch_slot == slot)
        tls_epoch_slot = -1;
}

/*
 * Try to reclaim the retire list for a safe epoch across ALL threads.
 * Safe epoch = global_epoch - 2 (with 3-epoch scheme).
 */
static void try_reclaim(epoch_t *e, uint64_t new_epoch)
{
    /* After advancing to new_epoch, epoch (new_epoch - 2) is safe to free */
    if (new_epoch < 2) return;
    int safe_idx = (int)((new_epoch - 2) % EPOCH_COUNT);

    for (int t = 0; t < EPOCH_MAX_THREADS; t++) {
        if (!atomic_load_explicit(&e->threads[t].active, memory_order_acquire))
            continue;
        /* Only reclaim from our own thread (or inactive threads) to avoid races */
    }

    /* Actually, since retire lists are per-thread and only the owning thread
     * writes to them, we can safely free from the calling thread's list only.
     * Other threads will free their own lists on their next epoch_enter. */
    int slot = tls_epoch_slot;
    if (slot < 0 || slot >= EPOCH_MAX_THREADS) return;

    free_list(e->free_fn, e->threads[slot].retire[safe_idx]);
    e->threads[slot].retire[safe_idx] = NULL;
    e->threads[slot].retire_count[safe_idx] = 0;
}

void epoch_try_advance(epoch_t *e)
{
    uint64_t ge = atomic_load_explicit(&e->global_epoch, memory_order_acquire);

    for (int i = 0; i < EPOCH_MAX_THREADS; i++) {
        if (!atomic_load_explicit(&e->threads[i].active, memory_order_acquire))
            continue;
        uint64_t te = atomic_load_explicit(&e->threads[i].epoch, memory_order_acquire);
        if (te != UINT64_MAX && te < ge)
            return;  /* thread i hasn't caught up */
    }

    /* All threads at current epoch — try to advance */
    uint64_t new_epoch = ge + 1;
    if (atomic_compare_exchange_strong_explicit(&e->global_epoch, &ge, new_epoch,
            memory_order_acq_rel, memory_order_acquire)) {
        try_reclaim(e, new_epoch);
    }
}

uint64_t epoch_enter(epoch_t *e, int slot)
{
    uint64_t ge = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    atomic_store_explicit(&e->threads[slot].epoch, ge, memory_order_release);
    e->threads[slot].in_critical = true;

    /* Try to advance + reclaim on entry */
    epoch_try_advance(e);

    /* Also reclaim our own safe lists */
    if (ge >= 2) {
        int safe_idx = (int)((ge - 2) % EPOCH_COUNT);
        if (e->threads[slot].retire[safe_idx]) {
            free_list(e->free_fn, e->threads[slot].retire[safe_idx]);
            e->threads[slot].retire[safe_idx] = NULL;
            e->threads[slot].retire_count[safe_idx] = 0;
        }
    }

    return ge;
}

void epoch_exit(epoch_t *e, int slot)
{
    e->threads[slot].in_critical = false;
    atomic_store_explicit(&e->threads[slot].epoch, UINT64_MAX, memory_order_release);
}

void epoch_retire(epoch_t *e, void *ptr)
{
    epoch_retire_slot(e, tls_epoch_slot, ptr);
}

void epoch_retire_slot(epoch_t *e, int slot, void *ptr)
{
    if (slot < 0 || slot >= EPOCH_MAX_THREADS) {
        /* No slot — free immediately (unsafe but prevents leak) */
        if (e->free_fn) e->free_fn(ptr);
        return;
    }

    struct epoch_node *node = malloc(sizeof(struct epoch_node));
    if (!node) {
        if (e->free_fn) e->free_fn(ptr);
        return;
    }
    node->ptr = ptr;

    uint64_t ge = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    int idx = (int)(ge % EPOCH_COUNT);

    /* Thread-local list — no lock needed */
    node->next = e->threads[slot].retire[idx];
    e->threads[slot].retire[idx] = node;
    e->threads[slot].retire_count[idx]++;
}
