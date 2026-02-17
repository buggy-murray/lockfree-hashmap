#define _GNU_SOURCE
#include "hashmap.h"
#include <stdio.h>
#include <assert.h>

int main(void)
{
    printf("Creating map...\n");
    hashmap_t *map = hashmap_create();
    assert(map != NULL);

    int v1 = 42;
    printf("Putting key=1...\n");
    void *old = hashmap_put(map, 1, &v1);
    printf("put returned %p\n", old);

    printf("Getting key=1...\n");
    void *got = hashmap_get(map, 1);
    printf("get returned %p (expected %p)\n", got, (void *)&v1);

    printf("Count: %zu\n", hashmap_count(map));

    hashmap_destroy(map);
    printf("Done.\n");
    return 0;
}
