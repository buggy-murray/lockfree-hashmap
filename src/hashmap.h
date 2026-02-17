/*
 * hashmap.h — Lock-free concurrent hash map
 *
 * Based on split-ordered lists (Shalev & Shavit, JACM 2006)
 * with Harris-style lock-free linked list (DISC 2001).
 *
 * Features:
 * - Lock-free get/put/remove via CAS
 * - Amortized resize without stop-the-world rehash
 * - Split ordering: elements sorted by bit-reversed hash
 *
 * Author: G.H. Murray
 * Date:   2026-02-17
 */

#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* Initial capacity (must be power of 2) */
#define HASHMAP_INIT_CAP    16

/* Load factor threshold for resize (percentage) */
#define HASHMAP_LOAD_FACTOR 75

/*
 * struct hm_node — A node in the lock-free sorted linked list.
 *
 * The `next` pointer uses the LSB as a "mark" bit for logical deletion
 * (Harris's technique). When marked, the node is logically deleted
 * and will be physically unlinked by the next traversal.
 */
struct hm_node {
    _Atomic(uintptr_t)  next;       /* next ptr | mark bit in LSB       */
    uint64_t            key;        /* Original key (0 = sentinel)       */
    uint64_t            so_key;     /* Split-ordered key (bit-reversed)  */
    _Atomic(void *)     value;      /* User value (NULL = deleted/dummy) */
    bool                is_dummy;   /* true for bucket sentinel nodes    */
};

/*
 * hashmap_t — The hash map.
 */
typedef struct hashmap {
    _Atomic(struct hm_node **) buckets;  /* Array of bucket pointers    */
    _Atomic(size_t)            size;     /* Current capacity (power of 2) */
    _Atomic(size_t)            count;    /* Number of active elements    */
    struct hm_node             head;     /* List head sentinel           */
} hashmap_t;

/*
 * hashmap_create — Create a new hash map
 */
hashmap_t *hashmap_create(void);

/*
 * hashmap_destroy — Destroy the hash map and free all nodes
 *
 * NOT thread-safe with concurrent operations. Call after all threads done.
 */
void hashmap_destroy(hashmap_t *map);

/*
 * hashmap_put — Insert or update a key-value pair
 *
 * @key:   Key (must be non-zero; 0 is reserved for sentinels)
 * @value: Value to associate (must be non-NULL)
 *
 * Returns previous value if key existed, NULL if new insertion.
 * Thread-safe, lock-free.
 */
void *hashmap_put(hashmap_t *map, uint64_t key, void *value);

/*
 * hashmap_get — Look up a value by key
 *
 * Returns the value, or NULL if not found.
 * Thread-safe, lock-free (wait-free in practice).
 */
void *hashmap_get(hashmap_t *map, uint64_t key);

/*
 * hashmap_remove — Remove a key from the map
 *
 * Returns the removed value, or NULL if not found.
 * Thread-safe, lock-free.
 */
void *hashmap_remove(hashmap_t *map, uint64_t key);

/*
 * hashmap_count — Return current number of elements
 */
size_t hashmap_count(hashmap_t *map);

#endif /* HASHMAP_H */
