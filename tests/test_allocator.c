/*
 * test_allocator.c - Hand-rolled, assert-style unit tests for the
 * custom allocator. No external test framework dependency: each test
 * is a plain function that returns true/false, and main() runs them
 * all, reporting a pass/fail summary and exiting non-zero on failure
 * (so `make test` / CI can detect regressions).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../allocator.h"

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("    FAILED: %s (line %d)\n", #cond, __LINE__);         \
            return 0;                                                      \
        }                                                                  \
    } while (0)

#define RUN_TEST(fn)                                                       \
    do {                                                                   \
        g_tests_run++;                                                     \
        printf("[ RUN ] %s\n", #fn);                                       \
        heap_reset();                                                      \
        if (fn()) {                                                       \
            printf("[ PASS] %s\n", #fn);                                   \
        } else {                                                          \
            printf("[ FAIL] %s\n", #fn);                                   \
            g_tests_failed++;                                              \
        }                                                                  \
    } while (0)

/* 1. Basic alloc/free round trip. */
static int test_basic_alloc_free(void) {
    void *p = my_malloc(64);
    CHECK(p != NULL);
    CHECK(heap_check());
    my_free(p);
    CHECK(heap_check());
    return 1;
}

/* 2. Multiple allocations don't overlap. */
static int test_no_overlap(void) {
    char *a = my_malloc(50);
    char *b = my_malloc(50);
    char *c = my_malloc(50);
    CHECK(a && b && c);

    memset(a, 0xAA, 50);
    memset(b, 0xBB, 50);
    memset(c, 0xCC, 50);

    for (int i = 0; i < 50; i++) {
        CHECK((unsigned char)a[i] == 0xAA);
        CHECK((unsigned char)b[i] == 0xBB);
        CHECK((unsigned char)c[i] == 0xCC);
    }
    CHECK(heap_check());
    my_free(a);
    my_free(b);
    my_free(c);
    return 1;
}

/* 3. Freeing and reallocating the same-sized block reuses the exact
 *    same address (deterministic with first-fit + no other activity). */
static int test_free_then_realloc_reuses_space(void) {
    void *p1 = my_malloc(100);
    CHECK(p1 != NULL);
    my_free(p1);
    CHECK(heap_check());

    void *p2 = my_malloc(100);
    CHECK(p2 != NULL);
    CHECK(p2 == p1); /* first-fit should hand back the same freed block */
    my_free(p2);
    return 1;
}

/* 4. Coalescing merges adjacent free blocks (checked via block count). */
static int test_coalescing_merges_free_blocks(void) {
    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c);

    size_t total_before = heap_total_block_count();
    (void)total_before;

    my_free(a);
    my_free(c);
    /* a and c are not adjacent to each other (b sits between them), so
     * we should still have 3 blocks: FREE(a) USED(b) FREE(c). */
    CHECK(heap_free_block_count() == 2);
    CHECK(heap_used_block_count() == 1);

    my_free(b);
    /* Now all three are free and physically adjacent -> must coalesce
     * down into a single free block. */
    CHECK(heap_check());
    CHECK(heap_total_block_count() == 1);
    CHECK(heap_free_block_count() == 1);
    return 1;
}

/* 5. realloc growing a block preserves existing data. */
static int test_realloc_grow_preserves_data(void) {
    char *p = my_malloc(16);
    CHECK(p != NULL);
    memcpy(p, "hello world", 12);

    char *grown = my_realloc(p, 512);
    CHECK(grown != NULL);
    CHECK(memcmp(grown, "hello world", 12) == 0);
    CHECK(heap_check());
    my_free(grown);
    return 1;
}

/* 6. realloc shrinking a block preserves the retained data and keeps
 *    the heap consistent. */
static int test_realloc_shrink_preserves_data(void) {
    char *p = my_malloc(256);
    CHECK(p != NULL);
    memset(p, 0x5A, 256);
    memcpy(p, "shrink-me", 9);

    char *shrunk = my_realloc(p, 32);
    CHECK(shrunk != NULL);
    CHECK(memcmp(shrunk, "shrink-me", 9) == 0);
    CHECK(heap_check());
    my_free(shrunk);
    return 1;
}

/* 7. calloc zero-initializes the returned buffer. */
static int test_calloc_zero_initializes(void) {
    unsigned char *buf = my_calloc(64, 1);
    CHECK(buf != NULL);
    for (int i = 0; i < 64; i++) {
        CHECK(buf[i] == 0);
    }

    int *nums = my_calloc(20, sizeof(int));
    CHECK(nums != NULL);
    for (int i = 0; i < 20; i++) {
        CHECK(nums[i] == 0);
    }
    my_free(buf);
    my_free(nums);
    return 1;
}

/* 8. Allocating more than the arena size fails gracefully. */
static int test_oversized_allocation_fails_gracefully(void) {
    void *p = my_malloc(ARENA_SIZE * 4);
    CHECK(p == NULL);
    CHECK(heap_check()); /* heap must still be consistent afterwards */

    /* A slightly-more-than-arena request (accounting for header
     * overhead) should also fail rather than corrupt anything. */
    void *p2 = my_malloc(ARENA_SIZE + 1);
    CHECK(p2 == NULL);
    CHECK(heap_check());
    return 1;
}

/* 9. Double free is caught and does not corrupt the heap. */
static int test_double_free_is_handled(void) {
    void *p = my_malloc(64);
    CHECK(p != NULL);
    my_free(p);
    CHECK(heap_check());

    /* Second free of the same pointer must be a safe no-op (it prints a
     * warning to stderr but must not corrupt bookkeeping). */
    my_free(p);
    CHECK(heap_check());

    /* The heap should still function normally afterwards. */
    void *q = my_malloc(64);
    CHECK(q != NULL);
    CHECK(heap_check());
    my_free(q);
    return 1;
}

int main(void) {
    RUN_TEST(test_basic_alloc_free);
    RUN_TEST(test_no_overlap);
    RUN_TEST(test_free_then_realloc_reuses_space);
    RUN_TEST(test_coalescing_merges_free_blocks);
    RUN_TEST(test_realloc_grow_preserves_data);
    RUN_TEST(test_realloc_shrink_preserves_data);
    RUN_TEST(test_calloc_zero_initializes);
    RUN_TEST(test_oversized_allocation_fails_gracefully);
    RUN_TEST(test_double_free_is_handled);

    printf("\n%d/%d tests passed\n", g_tests_run - g_tests_failed, g_tests_run);
    return g_tests_failed == 0 ? 0 : 1;
}
