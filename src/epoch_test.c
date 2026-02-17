/*
 * epoch_test.c — Tests for epoch-based reclamation
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#define _GNU_SOURCE
#include "epoch.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

static _Atomic int free_count = 0;

static void test_free_fn(void *ptr)
{
    atomic_fetch_add(&free_count, 1);
    free(ptr);
}

static void test_basic(void)
{
    printf("=== test_basic_epoch ===\n");

    epoch_t e;
    epoch_init(&e, test_free_fn);
    atomic_store(&free_count, 0);

    int slot = epoch_register(&e);
    assert(slot >= 0);

    /* Enter critical section */
    epoch_enter(&e, slot);

    /* Retire some nodes */
    for (int i = 0; i < 10; i++) {
        int *p = malloc(sizeof(int));
        *p = i;
        epoch_retire(&e, p);
    }

    /* Exit — nodes shouldn't be freed yet (we're still in the epoch) */
    epoch_exit(&e, slot);

    /* Enter+exit a few more times to advance epochs */
    for (int round = 0; round < 5; round++) {
        epoch_enter(&e, slot);
        epoch_exit(&e, slot);
    }

    printf("  freed %d/10 nodes\n", atomic_load(&free_count));
    assert(atomic_load(&free_count) == 10);

    epoch_unregister(&e, slot);
    epoch_destroy(&e);
    printf("  PASSED\n\n");
}

/* Multi-threaded: producers retire, consumers enter/exit to drive reclamation */
#define MT_THREADS 4
#define MT_RETIRES 1000

struct mt_epoch_args {
    epoch_t *e;
    int slot;
};

static void *mt_epoch_worker(void *arg)
{
    struct mt_epoch_args *a = (struct mt_epoch_args *)arg;

    for (int i = 0; i < MT_RETIRES; i++) {
        epoch_enter(a->e, a->slot);

        int *p = malloc(sizeof(int));
        *p = i;
        epoch_retire(a->e, p);

        epoch_exit(a->e, a->slot);
    }

    return NULL;
}

static void test_multithreaded_epoch(void)
{
    printf("=== test_multithreaded_epoch ===\n");

    epoch_t e;
    epoch_init(&e, test_free_fn);
    atomic_store(&free_count, 0);

    pthread_t threads[MT_THREADS];
    struct mt_epoch_args args[MT_THREADS];

    for (int i = 0; i < MT_THREADS; i++) {
        args[i].e = &e;
        args[i].slot = epoch_register(&e);
        assert(args[i].slot >= 0);
        pthread_create(&threads[i], NULL, mt_epoch_worker, &args[i]);
    }

    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(threads[i], NULL);
        epoch_unregister(&e, args[i].slot);
    }

    /* Drive final reclamation */
    int slot = epoch_register(&e);
    for (int i = 0; i < 5; i++) {
        epoch_enter(&e, slot);
        epoch_exit(&e, slot);
    }
    epoch_unregister(&e, slot);

    int total_retired = MT_THREADS * MT_RETIRES;
    int freed = atomic_load(&free_count);
    printf("  retired %d, freed %d\n", total_retired, freed);

    /* All should eventually be freed */
    epoch_destroy(&e);
    freed = atomic_load(&free_count);
    printf("  after destroy: freed %d/%d\n", freed, total_retired);
    assert(freed == total_retired);

    printf("  PASSED\n\n");
}

int main(void)
{
    printf("Epoch-Based Reclamation Test Suite\n");
    printf("===================================\n\n");

    test_basic();
    test_multithreaded_epoch();

    printf("All epoch tests passed.\n");
    return 0;
}
