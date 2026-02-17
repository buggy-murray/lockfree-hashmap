/*
 * test.c — Tests for lock-free concurrent hash map
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#define _GNU_SOURCE
#include "hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>

static void test_basic(void)
{
    printf("=== test_basic ===\n");

    hashmap_t *map = hashmap_create();
    assert(map != NULL);
    int slot = hashmap_thread_register(map);

    int v1 = 42, v2 = 99, v3 = 7;

    /* Insert */
    assert(hashmap_put(map, 1, &v1) == NULL);
    assert(hashmap_put(map, 2, &v2) == NULL);
    assert(hashmap_put(map, 3, &v3) == NULL);
    assert(hashmap_count(map) == 3);

    /* Get */
    assert(hashmap_get(map, 1) == &v1);
    assert(hashmap_get(map, 2) == &v2);
    assert(hashmap_get(map, 3) == &v3);
    assert(hashmap_get(map, 4) == NULL);

    printf("  insert/get: OK\n");

    /* Update */
    int v4 = 100;
    void *old = hashmap_put(map, 2, &v4);
    assert(old == &v2);
    assert(hashmap_get(map, 2) == &v4);
    assert(hashmap_count(map) == 3);

    printf("  update: OK\n");

    /* Remove */
    old = hashmap_remove(map, 1);
    assert(old == &v1);
    assert(hashmap_get(map, 1) == NULL);
    assert(hashmap_count(map) == 2);

    /* Remove non-existent */
    assert(hashmap_remove(map, 999) == NULL);

    printf("  remove: OK\n");

    hashmap_thread_unregister(map, slot);
    hashmap_destroy(map);
    printf("  PASSED\n\n");
}

static void test_many_keys(void)
{
    printf("=== test_many_keys ===\n");

    hashmap_t *map = hashmap_create();
    assert(map != NULL);
    int slot = hashmap_thread_register(map);

    /* Insert 10000 keys — should trigger multiple resizes */
    int values[10000];
    for (int i = 0; i < 10000; i++) {
        values[i] = i;
        assert(hashmap_put(map, (uint64_t)(i + 1), &values[i]) == NULL);
    }
    assert(hashmap_count(map) == 10000);
    printf("  inserted 10000 keys\n");

    /* Verify all present */
    for (int i = 0; i < 10000; i++) {
        void *v = hashmap_get(map, (uint64_t)(i + 1));
        assert(v == &values[i]);
    }
    printf("  all 10000 keys found\n");

    /* Remove half */
    for (int i = 0; i < 5000; i++) {
        void *v = hashmap_remove(map, (uint64_t)(i + 1));
        assert(v == &values[i]);
    }
    assert(hashmap_count(map) == 5000);
    printf("  removed 5000, count=%zu\n", hashmap_count(map));

    /* Verify remaining */
    for (int i = 5000; i < 10000; i++) {
        assert(hashmap_get(map, (uint64_t)(i + 1)) == &values[i]);
    }
    printf("  remaining 5000 verified\n");

    hashmap_thread_unregister(map, slot);
    hashmap_destroy(map);
    printf("  PASSED\n\n");
}

/* ── Multi-threaded test ── */

#define MT_THREADS  8
#define MT_OPS      10000

struct mt_args {
    hashmap_t *map;
    int thread_id;
    int ok;
};

static void *mt_worker(void *arg)
{
    struct mt_args *a = (struct mt_args *)arg;
    int slot = hashmap_thread_register(a->map);
    int base = a->thread_id * MT_OPS;
    int ok = 0;

    /* Each thread works on its own key range to avoid remove races */
    for (int i = 0; i < MT_OPS; i++) {
        uint64_t key = (uint64_t)(base + i + 1);
        int *val = malloc(sizeof(int));
        *val = (int)key;
        hashmap_put(a->map, key, val);
    }

    for (int i = 0; i < MT_OPS; i++) {
        uint64_t key = (uint64_t)(base + i + 1);
        void *v = hashmap_get(a->map, key);
        if (v && *(int *)v == (int)key) ok++;
    }

    for (int i = 0; i < MT_OPS; i++) {
        uint64_t key = (uint64_t)(base + i + 1);
        void *v = hashmap_remove(a->map, key);
        if (v) free(v);
    }

    a->ok = ok;
    hashmap_thread_unregister(a->map, slot);
    return NULL;
}

static void test_multithreaded(void)
{
    printf("=== test_multithreaded ===\n");

    hashmap_t *map = hashmap_create();
    assert(map != NULL);

    pthread_t threads[MT_THREADS];
    struct mt_args args[MT_THREADS];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < MT_THREADS; i++) {
        args[i].map = map;
        args[i].thread_id = i;
        args[i].ok = 0;
        pthread_create(&threads[i], NULL, mt_worker, &args[i]);
    }

    int total_ok = 0;
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_ok += args[i].ok;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_nsec - start.tv_nsec) / 1e6;

    int total_ops = MT_THREADS * MT_OPS * 3;  /* put + get + remove */
    printf("  %d threads × %d keys × 3 ops = %d total ops in %.2f ms\n",
           MT_THREADS, MT_OPS, total_ops, ms);
    printf("  %d/%d gets succeeded\n", total_ok, MT_THREADS * MT_OPS);
    assert(total_ok == MT_THREADS * MT_OPS);
    assert(hashmap_count(map) == 0);

    hashmap_destroy(map);
    printf("  PASSED\n\n");
}

int main(void)
{
    printf("Lock-Free Hash Map Test Suite\n");
    printf("==============================\n\n");

    test_basic();
    test_many_keys();
    test_multithreaded();

    printf("All tests passed.\n");
    return 0;
}
