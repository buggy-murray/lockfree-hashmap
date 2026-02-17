/*
 * hashmap.c — Lock-free concurrent hash map (split-ordered lists)
 *
 * Implementation of Shalev & Shavit's split-ordered lists with
 * Harris's lock-free linked list as the backbone.
 *
 * Key concepts:
 * - All elements live in a single sorted linked list
 * - Sort key = bit-reversed hash (split ordering)
 * - Bucket array = pointers into the list (lazy-initialized sentinels)
 * - Resize = double bucket array + lazy sentinel insertion (no rehash)
 * - Delete = mark next pointer's LSB (logical), then CAS unlink (physical)
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#define _GNU_SOURCE
#include "hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

/* Thread-local epoch slot (set via hashmap_thread_register) */
static __thread int tls_epoch_slot = -1;

/* ──────────────────────────────────────────────────────────────────
 * Harris-style marked pointer helpers
 *
 * The LSB of the `next` pointer is used as a "mark" bit.
 * Marked = logically deleted. Physical removal on next traversal.
 * ────────────────────────────────────────────────────────────────── */

#define MARK_BIT    ((uintptr_t)1)

static inline struct hm_node *get_ptr(uintptr_t tagged)
{
    return (struct hm_node *)(tagged & ~MARK_BIT);
}

static inline bool is_marked(uintptr_t tagged)
{
    return (tagged & MARK_BIT) != 0;
}

static inline uintptr_t make_tagged(struct hm_node *ptr, bool mark)
{
    return (uintptr_t)ptr | (mark ? MARK_BIT : 0);
}

/* ──────────────────────────────────────────────────────────────────
 * Bit reversal for split ordering
 * ────────────────────────────────────────────────────────────────── */

static inline uint64_t reverse_bits(uint64_t x)
{
    x = ((x & 0x5555555555555555ULL) << 1)  | ((x >> 1)  & 0x5555555555555555ULL);
    x = ((x & 0x3333333333333333ULL) << 2)  | ((x >> 2)  & 0x3333333333333333ULL);
    x = ((x & 0x0F0F0F0F0F0F0F0FULL) << 4) | ((x >> 4)  & 0x0F0F0F0F0F0F0F0FULL);
    x = ((x & 0x00FF00FF00FF00FFULL) << 8)  | ((x >> 8)  & 0x00FF00FF00FF00FFULL);
    x = ((x & 0x0000FFFF0000FFFFULL) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFULL);
    x = (x << 32) | (x >> 32);
    return x;
}

/*
 * Hash function (splitmix64 finalizer — excellent distribution)
 */
static inline uint64_t hash_key(uint64_t key)
{
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return key;
}

/*
 * Split-ordered key for regular (non-dummy) nodes.
 * Bit-reverse the hash, then set LSB to 1 to distinguish from dummies.
 */
static inline uint64_t make_so_regular(uint64_t key)
{
    return reverse_bits(hash_key(key)) | 1;
}

/*
 * Split-ordered key for dummy (sentinel) nodes.
 * Bit-reverse the bucket index. LSB is 0 (dummy < regular in same bucket).
 */
static inline uint64_t make_so_dummy(size_t bucket)
{
    return reverse_bits((uint64_t)bucket);
}

/* ──────────────────────────────────────────────────────────────────
 * Lock-free list operations (Harris, 2001)
 * ────────────────────────────────────────────────────────────────── */

/*
 * find — Search for the position of `so_key` in the sorted list.
 *
 * Returns true if a node with this so_key exists (and sets *out_curr).
 * Sets *out_prev to the predecessor's `next` field (for CAS insertion).
 *
 * Also physically removes any marked (logically deleted) nodes encountered.
 */
static bool list_find(epoch_t *epoch, struct hm_node *head, uint64_t so_key,
                      _Atomic(uintptr_t) **out_prev, struct hm_node **out_curr)
{
retry:
    ;
    _Atomic(uintptr_t) *prev = &head->next;
    uintptr_t cur_tagged = atomic_load_explicit(prev, memory_order_acquire);
    struct hm_node *curr = get_ptr(cur_tagged);

    while (curr) {
        uintptr_t next_tagged = atomic_load_explicit(&curr->next, memory_order_acquire);
        struct hm_node *next = get_ptr(next_tagged);

        if (is_marked(next_tagged)) {
            /* curr is logically deleted — try to physically unlink */
            uintptr_t expected = make_tagged(curr, false);
            if (!atomic_compare_exchange_strong_explicit(
                    prev, &expected, make_tagged(next, false),
                    memory_order_acq_rel, memory_order_acquire)) {
                goto retry;  /* lost race, restart traversal */
            }
            /* Successfully unlinked — retire via EBR */
            if (epoch)
                epoch_retire(epoch, curr);
            curr = next;
            continue;
        }

        if (curr->so_key >= so_key) {
            *out_prev = prev;
            *out_curr = curr;
            return (curr->so_key == so_key);
        }

        prev = &curr->next;
        curr = next;
    }

    *out_prev = prev;
    *out_curr = NULL;
    return false;
}

static struct hm_node *node_alloc(uint64_t key, uint64_t so_key,
                                   void *value, bool is_dummy)
{
    struct hm_node *n = calloc(1, sizeof(struct hm_node));
    if (!n) return NULL;
    n->key = key;
    n->so_key = so_key;
    atomic_store_explicit(&n->value, value, memory_order_relaxed);
    atomic_store_explicit(&n->next, 0, memory_order_relaxed);
    n->is_dummy = is_dummy;
    return n;
}

/*
 * list_insert — Insert a node into the sorted list.
 *
 * If a node with the same so_key already exists:
 *   - For dummy nodes: return the existing node (idempotent)
 *   - For regular nodes: CAS-update the value
 *
 * Returns the node (either new or existing).
 */
static struct hm_node *list_insert(struct hm_node *head, struct hm_node *new_node)
{
    while (1) {
        _Atomic(uintptr_t) *prev;
        struct hm_node *curr;

        if (list_find(NULL, head, new_node->so_key, &prev, &curr)) {
            /* Node with this so_key already exists */
            if (new_node->is_dummy) {
                free(new_node);
                return curr;  /* reuse existing dummy */
            }
            /* Check for exact key match (not just so_key) */
            if (!curr->is_dummy && curr->key == new_node->key) {
                /* Same key: update value */
                atomic_store_explicit(&curr->value,
                    atomic_load_explicit(&new_node->value, memory_order_relaxed),
                    memory_order_release);
                free(new_node);
                return curr;
            }
            /* so_key collision with different original key — need to insert
             * after curr. Adjust so_key slightly to maintain uniqueness.
             * Actually, different keys can have same so_key. We insert anyway
             * and the list will have adjacent nodes with same so_key but
             * different keys. find() must scan past same-so_key nodes. */
        }

        /* Insert new_node between prev and curr */
        atomic_store_explicit(&new_node->next, make_tagged(curr, false),
                              memory_order_relaxed);
        uintptr_t expected = make_tagged(curr, false);
        if (atomic_compare_exchange_strong_explicit(
                prev, &expected, make_tagged(new_node, false),
                memory_order_acq_rel, memory_order_acquire)) {
            return new_node;  /* success */
        }
        /* CAS failed — retry from the top */
    }
}

/*
 * list_delete — Logically delete a node by marking its next pointer.
 *
 * Returns the value of the deleted node, or NULL if not found.
 */
static void *list_delete(struct hm_node *head, uint64_t so_key, uint64_t key)
{
    while (1) {
        _Atomic(uintptr_t) *prev;
        struct hm_node *curr;

        if (!list_find(NULL, head, so_key, &prev, &curr))
            return NULL;  /* not found */

        /* Verify it's the right key (not a dummy or hash collision) */
        if (curr->is_dummy || curr->key != key)
            return NULL;

        void *val = atomic_load_explicit(&curr->value, memory_order_acquire);

        /* Logical delete: mark curr->next */
        uintptr_t next_tagged = atomic_load_explicit(&curr->next, memory_order_acquire);
        if (is_marked(next_tagged))
            return NULL;  /* already deleted */

        if (!atomic_compare_exchange_strong_explicit(
                &curr->next, &next_tagged,
                make_tagged(get_ptr(next_tagged), true),
                memory_order_acq_rel, memory_order_acquire)) {
            continue;  /* retry */
        }

        /* Physical removal (best-effort, will be cleaned up by find) */
        uintptr_t expected = make_tagged(curr, false);
        atomic_compare_exchange_strong_explicit(
            prev, &expected, make_tagged(get_ptr(next_tagged), false),
            memory_order_acq_rel, memory_order_acquire);

        return val;
    }
}

/* ──────────────────────────────────────────────────────────────────
 * Bucket management
 * ────────────────────────────────────────────────────────────────── */

/*
 * Get the parent bucket index for lazy initialization.
 * Parent of bucket i is i with the highest set bit cleared.
 */
static inline size_t get_parent(size_t bucket)
{
    /* Clear the highest set bit */
    size_t msb = (size_t)1 << (63 - __builtin_clzl(bucket));
    return bucket & ~msb;
}

/*
 * Ensure bucket `idx` is initialized (has a dummy sentinel in the list).
 * Recursively initializes parent buckets as needed.
 */
static void initialize_bucket(hashmap_t *map, size_t idx)
{
    struct hm_node **buckets = atomic_load_explicit(&map->buckets, memory_order_acquire);
    size_t cap = atomic_load_explicit(&map->size, memory_order_acquire);

    if (idx >= cap) return;
    if (buckets[idx] != NULL) return;  /* already initialized */

    /* Ensure parent is initialized */
    size_t parent = get_parent(idx);
    if (parent != idx)
        initialize_bucket(map, parent);

    /* Create and insert dummy sentinel */
    struct hm_node *dummy = node_alloc(0, make_so_dummy(idx), NULL, true);
    if (!dummy) return;

    struct hm_node *inserted = list_insert(&map->head, dummy);

    /* CAS the bucket pointer (another thread may have beat us) */
    struct hm_node *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            (_Atomic(struct hm_node *) *)&buckets[idx],
            &expected, inserted,
            memory_order_acq_rel, memory_order_acquire)) {
        /* Another thread initialized it — that's fine, expected now points to it */
    }
}

/* ──────────────────────────────────────────────────────────────────
 * Resize
 * ────────────────────────────────────────────────────────────────── */

static void maybe_resize(hashmap_t *map)
{
    size_t count = atomic_load_explicit(&map->count, memory_order_relaxed);
    size_t cap   = atomic_load_explicit(&map->size, memory_order_relaxed);

    if (count * 100 < cap * HASHMAP_LOAD_FACTOR)
        return;  /* below threshold */

    size_t new_cap = cap * 2;
    struct hm_node **old_buckets = atomic_load_explicit(&map->buckets, memory_order_acquire);
    struct hm_node **new_buckets = calloc(new_cap, sizeof(struct hm_node *));
    if (!new_buckets) return;  /* resize failed, keep going */

    /* Copy existing bucket pointers */
    memcpy(new_buckets, old_buckets, cap * sizeof(struct hm_node *));

    /* CAS the bucket array */
    if (atomic_compare_exchange_strong_explicit(
            &map->buckets, &old_buckets, new_buckets,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&map->size, new_cap, memory_order_release);
        /* Retire old bucket array via EBR */
        epoch_retire(&map->epoch, old_buckets);
    } else {
        free(new_buckets);  /* another thread resized first */
    }
}

/* ──────────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────────── */

static void node_free_cb(void *ptr)
{
    free(ptr);
}

hashmap_t *hashmap_create(void)
{
    hashmap_t *map = calloc(1, sizeof(hashmap_t));
    if (!map) return NULL;

    struct hm_node **buckets = calloc(HASHMAP_INIT_CAP, sizeof(struct hm_node *));
    if (!buckets) {
        free(map);
        return NULL;
    }

    atomic_store(&map->buckets, buckets);
    atomic_store(&map->size, HASHMAP_INIT_CAP);
    atomic_store(&map->count, 0);

    /* Initialize head sentinel (so_key = 0, smallest possible) */
    map->head.so_key = 0;
    map->head.is_dummy = true;
    atomic_store(&map->head.next, 0);
    atomic_store(&map->head.value, NULL);

    /* Bucket 0 points to head */
    buckets[0] = &map->head;

    /* Initialize epoch-based reclamation */
    epoch_init(&map->epoch, node_free_cb);

    return map;
}

int hashmap_thread_register(hashmap_t *map)
{
    int slot = epoch_register(&map->epoch);
    tls_epoch_slot = slot;
    return slot;
}

void hashmap_thread_unregister(hashmap_t *map, int slot)
{
    epoch_unregister(&map->epoch, slot);
    tls_epoch_slot = -1;
}

void hashmap_destroy(hashmap_t *map)
{
    if (!map) return;

    /* Drain any pending retired nodes */
    epoch_destroy(&map->epoch);

    /* Walk the list and free all nodes (except head, which is embedded) */
    uintptr_t tagged = atomic_load(&map->head.next);
    while (tagged) {
        struct hm_node *node = get_ptr(tagged);
        if (!node) break;
        tagged = atomic_load(&node->next);
        free(node);
    }

    free(atomic_load(&map->buckets));
    free(map);
}

void *hashmap_put(hashmap_t *map, uint64_t key, void *value)
{
    if (key == 0 || !value) return NULL;

    int slot = tls_epoch_slot;
    if (slot >= 0) epoch_enter(&map->epoch, slot);

    uint64_t so_key = make_so_regular(key);
    size_t cap = atomic_load_explicit(&map->size, memory_order_acquire);
    size_t bucket = hash_key(key) & (cap - 1);

    initialize_bucket(map, bucket);

    struct hm_node **buckets = atomic_load_explicit(&map->buckets, memory_order_acquire);
    struct hm_node *bucket_head = buckets[bucket];
    if (!bucket_head) bucket_head = &map->head;  /* fallback */

    /* Try to find existing node first */
    _Atomic(uintptr_t) *prev;
    struct hm_node *curr;

    if (list_find(&map->epoch, bucket_head, so_key, &prev, &curr)) {
        if (curr && !curr->is_dummy && curr->key == key) {
            void *old = atomic_exchange_explicit(&curr->value, value,
                                                  memory_order_acq_rel);
            if (slot >= 0) epoch_exit(&map->epoch, slot);
            return old;  /* updated existing */
        }
    }

    /* Insert new node — list_insert handles concurrent races */
    struct hm_node *node = node_alloc(key, so_key, value, false);
    if (!node) {
        if (slot >= 0) epoch_exit(&map->epoch, slot);
        return NULL;
    }

    struct hm_node *result = list_insert(bucket_head, node);

    if (slot >= 0) epoch_exit(&map->epoch, slot);

    if (result != node) {
        return NULL;
    }

    atomic_fetch_add_explicit(&map->count, 1, memory_order_relaxed);
    maybe_resize(map);

    return NULL;  /* new insertion */
}

void *hashmap_get(hashmap_t *map, uint64_t key)
{
    if (key == 0) return NULL;

    int slot = tls_epoch_slot;
    if (slot >= 0) epoch_enter(&map->epoch, slot);

    uint64_t so_key = make_so_regular(key);
    size_t cap = atomic_load_explicit(&map->size, memory_order_acquire);
    size_t bucket = hash_key(key) & (cap - 1);

    initialize_bucket(map, bucket);

    struct hm_node **buckets = atomic_load_explicit(&map->buckets, memory_order_acquire);
    struct hm_node *bucket_head = buckets[bucket];
    if (!bucket_head) bucket_head = &map->head;

    _Atomic(uintptr_t) *prev;
    struct hm_node *curr;

    void *result = NULL;
    if (list_find(&map->epoch, bucket_head, so_key, &prev, &curr)) {
        if (curr && !curr->is_dummy && curr->key == key) {
            result = atomic_load_explicit(&curr->value, memory_order_acquire);
        }
    }

    if (slot >= 0) epoch_exit(&map->epoch, slot);
    return result;
}

void *hashmap_remove(hashmap_t *map, uint64_t key)
{
    if (key == 0) return NULL;

    int slot = tls_epoch_slot;
    if (slot >= 0) epoch_enter(&map->epoch, slot);

    uint64_t so_key = make_so_regular(key);
    size_t cap = atomic_load_explicit(&map->size, memory_order_acquire);
    size_t bucket = hash_key(key) & (cap - 1);
    initialize_bucket(map, bucket);

    struct hm_node **buckets = atomic_load_explicit(&map->buckets, memory_order_acquire);
    struct hm_node *bucket_head = buckets[bucket];
    if (!bucket_head) bucket_head = &map->head;

    void *val = list_delete(bucket_head, so_key, key);

    if (slot >= 0) epoch_exit(&map->epoch, slot);

    if (val) {
        atomic_fetch_sub_explicit(&map->count, 1, memory_order_relaxed);
    }

    return val;
}

size_t hashmap_count(hashmap_t *map)
{
    return atomic_load_explicit(&map->count, memory_order_relaxed);
}
