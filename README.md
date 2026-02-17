# Lock-Free Concurrent Hash Map

A lock-free concurrent hash map in C using split-ordered lists
(Shalev & Shavit, JACM 2006) with Harris-style linked list deletion (DISC 2001).

**Author:** G.H. Murray

## Features

- **Lock-free** get/put/remove via CAS atomics (C11 `<stdatomic.h>`)
- **Split-ordered lists** — single sorted list with bucket sentinels
- **Amortized resize** — double bucket array, lazy sentinel initialization
- **Harris deletion** — mark-based logical delete, physical cleanup on traversal
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

Dummy nodes (d0-d7) are bucket sentinels in split order.
Regular nodes are inserted after their bucket's sentinel.
Searches start from the bucket sentinel, not the list head.

## Building

```bash
make        # Build test binary
make run    # Build and run tests
make clean  # Clean
```

Requires: GCC (C11), pthreads.

## API

```c
#include "hashmap.h"

hashmap_t *map = hashmap_create();

hashmap_put(map, 42, my_value);
void *v = hashmap_get(map, 42);
void *old = hashmap_remove(map, 42);

hashmap_destroy(map);
```

Keys are `uint64_t` (0 is reserved). Values are `void *` (non-NULL).

## Status

Phase 1: Correct, tested (single + multi-threaded). Performance is functional
but not optimized — bucket initialization still walks from head for parent buckets.

Known limitations:
- No memory reclamation for deleted nodes (needs epoch-based or hazard pointers)
- Resize leaks old bucket arrays (acceptable for now, rare operation)
- Key 0 and NULL values are reserved

## References

- Shalev & Shavit, "Split-Ordered Lists: Lock-Free Extensible Hash Tables" (JACM 2006)
- Harris, "A Pragmatic Implementation of Non-Blocking Linked-Lists" (DISC 2001)

## License

MIT
