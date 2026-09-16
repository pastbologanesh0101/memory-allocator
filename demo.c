/*
 * demo.c - Small interactive-ish demonstration of the custom allocator.
 *
 * Allocates a variety of sizes, frees some of them in an order chosen to
 * show off coalescing, reallocs one buffer larger, and dumps/validates
 * the heap at each interesting step.
 */

#include <stdio.h>
#include <string.h>

#include "allocator.h"

static void print_heading(const char *title) {
    printf("\n=== %s ===\n", title);
}

int main(void) {
    print_heading("Initial heap state");
    heap_dump();
    printf("heap_check: %s\n", heap_check() ? "OK" : "FAILED");

    print_heading("Allocating a variety of sizes");
    char *a = my_malloc(32);
    char *b = my_malloc(128);
    char *c = my_malloc(64);
    char *d = my_malloc(256);
    strcpy(a, "hello from block a");
    strcpy(b, "hello from block b");
    strcpy(c, "hello from block c");
    strcpy(d, "hello from block d");
    heap_dump();
    printf("heap_check: %s\n", heap_check() ? "OK" : "FAILED");

    print_heading("Freeing b and c (adjacent) to demonstrate coalescing");
    my_free(b);
    my_free(c);
    heap_dump();
    printf("heap_check: %s\n", heap_check() ? "OK" : "FAILED");
    printf("free blocks: %zu, used blocks: %zu\n",
           heap_free_block_count(), heap_used_block_count());

    print_heading("Allocating into the coalesced hole");
    char *e = my_malloc(150);
    strcpy(e, "reused the coalesced space");
    heap_dump();

    print_heading("Growing 'a' with realloc (preserves contents)");
    a = my_realloc(a, 512);
    printf("a now contains: %s\n", a);
    heap_dump();

    print_heading("calloc a zeroed buffer");
    int *nums = my_calloc(10, sizeof(int));
    int all_zero = 1;
    for (int i = 0; i < 10; i++) {
        if (nums[i] != 0) all_zero = 0;
    }
    printf("calloc buffer all zero? %s\n", all_zero ? "yes" : "no");

    print_heading("Attempting an allocation larger than the arena");
    void *too_big = my_malloc(ARENA_SIZE * 2);
    printf("my_malloc(ARENA_SIZE * 2) returned: %p (expected NULL)\n", too_big);

    print_heading("Cleaning up");
    my_free(a);
    my_free(d);
    my_free(e);
    my_free(nums);
    heap_dump();
    printf("heap_check: %s\n", heap_check() ? "OK" : "FAILED");
    printf("free blocks: %zu, used blocks: %zu, total blocks: %zu\n",
           heap_free_block_count(), heap_used_block_count(), heap_total_block_count());

    print_heading("Double free demonstration (should warn, not crash)");
    my_free(d); /* already freed above */

    printf("\nDemo finished successfully.\n");
    return 0;
}
