# Lock-Free Concurrent Hash Map

A lock-free concurrent hash map in C using split-ordered lists
(Shalev & Shavit, JACM 2006) with Harris-style linked list deletion (DISC 2001)
and epoch-based memory reclamation (Fraser, 2004).

**Author:** G.H. Murray

## Features

- **Lock-free** get/put/remove via CAS atomics (C11 `<stdatomic.h>`)
- **Split-ordered lists** — single sorted list with bucket sentinels
- **Amortized resize** — double bucket array, lazy sentinel initialization
- **Harris deletion** — mark-based logical delete, physical cleanup on traversal
- **Epoch-based reclamation** — safe deferred freeing with per-thread retire lists
- **Bit-reversed hashing** — elements naturally partition across buckets

## Architecture

```
Buckets:  [0]  [1]  [2]  [3]  [4]  [5]  [6]  [7] ...
           |    |    |    |    |    |    |    |
           v    v    v    v    v    v    v    v
Head → d0 → d4 → d2 → d6 → d1 → d5 → d3 → d7 → ...
              ↑         ↑         ↑         ↑
          regular    regular   regular   regular
           nodes      nodes     nodes     nodes
```

### Memory Reclamation

3-epoch EBR system with **per-thread retire lists** (no mutex on retire path):

1. Each thread announces its epoch on critical section entry
2. Deleted nodes are retired to the thread's local list (lock-free)
3. When all threads have advanced past an epoch, that epoch's nodes are freed
4. Reclamation runs automatically on `epoch_enter`

## Building

```bash
make                  # Build hashmap test
make build/epoch_test # Build epoch standalone test
make run              # Build and run hashmap tests
make clean            # Clean
```

Requires: GCC (C11), pthreads.

## API

```c
#include "hashmap.h"

hashmap_t *map = hashmap_create();

// Each thread must register before operations
int slot = hashmap_thread_register(map);

hashmap_put(map, 42, my_value);
void *v = hashmap_get(map, 42);
void *old = hashmap_remove(map, 42);

// Unregister when done (drains pending retires)
hashmap_thread_unregister(map, slot);
hashmap_destroy(map);
```

Keys are `uint64_t` (0 is reserved). Values are `void *` (non-NULL).

## Tests

- **test_basic** — insert, get, update, remove
- **test_many_keys** — 10K keys with resize triggers
- **test_multithreaded** — 8 threads × 10K keys × 3 ops (240K total)
- **test_basic_epoch** — EBR single-thread retire + reclaim
- **test_multithreaded_epoch** — 4-thread concurrent retire/reclaim

## Performance

8-thread benchmark (240K ops: put + get + remove):
- With global retire mutex: ~6.9s
- With per-thread retire lists: ~5.4s (22% improvement)

Hot path is `list_find` (99.9% of time) — inherent cost of sorted linked list traversal.

## Known Limitations

- Key 0 and NULL values are reserved
- Max 64 concurrent threads (EPOCH_MAX_THREADS)
- `list_find` traversal is O(n/k) where k = active buckets

## References

- Shalev & Shavit, "Split-Ordered Lists: Lock-Free Extensible Hash Tables" (JACM 2006)
- Harris, "A Pragmatic Implementation of Non-Blocking Linked-Lists" (DISC 2001)
- Fraser, "Practical Lock-Freedom" (PhD thesis, Cambridge, 2004)

## License

MIT
