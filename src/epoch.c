/*
 * epoch.c — Epoch-based memory reclamation
 *
 * 3-epoch EBR: global epoch advances when all active threads
 * have entered the current epoch. Nodes retired 2 epochs ago
 * are safe to free.
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#define _GNU_SOURCE
#include "epoch.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Mutex for retire list manipulation (retire lists are not lock-free
 * themselves, but this is the slow path) */
static pthread_mutex_t retire_lock = PTHREAD_MUTEX_INITIALIZER;

void epoch_init(epoch_t *e, epoch_free_fn free_fn)
{
    memset(e, 0, sizeof(*e));
    atomic_store(&e->global_epoch, 0);
    for (int i = 0; i < EPOCH_MAX_THREADS; i++) {
        atomic_store(&e->thread_epochs[i], 0);
        atomic_store(&e->thread_active[i], false);
    }
    for (int i = 0; i < EPOCH_COUNT; i++) {
        e->retire_lists[i] = NULL;
        atomic_store(&e->retire_counts[i], 0);
    }
    e->free_fn = free_fn;
}

static void free_retire_list(epoch_t *e, int epoch_idx)
{
    struct epoch_node *node = e->retire_lists[epoch_idx];
    e->retire_lists[epoch_idx] = NULL;
    atomic_store(&e->retire_counts[epoch_idx], 0);

    while (node) {
        struct epoch_node *next = node->next;
        if (e->free_fn) {
            e->free_fn(node->ptr);
        }
        free(node);
        node = next;
    }
}

void epoch_destroy(epoch_t *e)
{
    pthread_mutex_lock(&retire_lock);
    for (int i = 0; i < EPOCH_COUNT; i++) {
        free_retire_list(e, i);
    }
    pthread_mutex_unlock(&retire_lock);
}

int epoch_register(epoch_t *e)
{
    for (int i = 0; i < EPOCH_MAX_THREADS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&e->thread_active[i], &expected, true)) {
            atomic_store(&e->thread_epochs[i], atomic_load(&e->global_epoch));
            return i;
        }
    }
    return -1;  /* too many threads */
}

void epoch_unregister(epoch_t *e, int slot)
{
    if (slot >= 0 && slot < EPOCH_MAX_THREADS) {
        atomic_store(&e->thread_active[slot], false);
    }
}

uint64_t epoch_enter(epoch_t *e, int slot)
{
    uint64_t ge = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    atomic_store_explicit(&e->thread_epochs[slot], ge, memory_order_release);

    /* Periodically try to advance */
    epoch_try_advance(e);

    return ge;
}

void epoch_exit(epoch_t *e, int slot)
{
    /* Set thread epoch to "inactive" sentinel — use UINT64_MAX to indicate
     * the thread is not in a critical section (always "past" any epoch) */
    atomic_store_explicit(&e->thread_epochs[slot], UINT64_MAX, memory_order_release);
}

void epoch_retire(epoch_t *e, void *ptr)
{
    struct epoch_node *node = malloc(sizeof(struct epoch_node));
    if (!node) {
        /* Can't defer — free immediately (unsafe, but prevents leak) */
        if (e->free_fn) e->free_fn(ptr);
        return;
    }
    node->ptr = ptr;

    uint64_t ge = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    int idx = (int)(ge % EPOCH_COUNT);

    pthread_mutex_lock(&retire_lock);
    node->next = e->retire_lists[idx];
    e->retire_lists[idx] = node;
    atomic_fetch_add(&e->retire_counts[idx], 1);
    pthread_mutex_unlock(&retire_lock);
}

void epoch_try_advance(epoch_t *e)
{
    uint64_t ge = atomic_load_explicit(&e->global_epoch, memory_order_acquire);

    /* Check if all active threads have observed the current epoch */
    for (int i = 0; i < EPOCH_MAX_THREADS; i++) {
        if (!atomic_load_explicit(&e->thread_active[i], memory_order_acquire))
            continue;
        uint64_t te = atomic_load_explicit(&e->thread_epochs[i], memory_order_acquire);
        if (te != UINT64_MAX && te < ge)
            return;  /* thread i hasn't caught up yet */
    }

    /* All threads are at current epoch — try to advance */
    if (atomic_compare_exchange_strong_explicit(&e->global_epoch, &ge, ge + 1,
            memory_order_acq_rel, memory_order_acquire)) {
        /* Successfully advanced. Free the retire list from 2 epochs ago. */
        /* New epoch is ge+1. Safe to free epoch (ge+1)-2 = ge-1.
         * Index: (ge-1) mod EPOCH_COUNT */
        int safe_idx = (int)((ge - 1 + EPOCH_COUNT) % EPOCH_COUNT);

        pthread_mutex_lock(&retire_lock);
        free_retire_list(e, safe_idx);
        pthread_mutex_unlock(&retire_lock);
    }
}
