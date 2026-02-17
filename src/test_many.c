#define _GNU_SOURCE
#include "hashmap.h"
#include <stdio.h>
#include <assert.h>

int main(void)
{
    hashmap_t *map = hashmap_create();
    int values[1000];

    printf("Inserting 1000 keys...\n");
    for (int i = 0; i < 1000; i++) {
        values[i] = i;
        void *old = hashmap_put(map, (uint64_t)(i + 1), &values[i]);
        if (old != NULL) {
            printf("  ERROR: key %d returned non-NULL on first insert\n", i+1);
        }
        if ((i + 1) % 100 == 0)
            printf("  inserted %d, count=%zu\n", i + 1, hashmap_count(map));
    }

    printf("Verifying...\n");
    int found = 0;
    for (int i = 0; i < 1000; i++) {
        void *v = hashmap_get(map, (uint64_t)(i + 1));
        if (v == &values[i]) found++;
    }
    printf("  %d/1000 found\n", found);

    hashmap_destroy(map);
    printf("Done.\n");
    return 0;
}
