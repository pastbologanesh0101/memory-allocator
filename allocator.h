/*
 * allocator.h - Public interface for the mini malloc/free implementation.
 *
 * This is a self-contained, free-list based heap manager. It does NOT
 * wrap the system malloc/free: all memory is served out of a single
 * large arena that is reserved once (via mmap, falling back to a static
 * buffer) and then carved up by hand using block headers.
 */

#ifndef MY_ALLOCATOR_H
#define MY_ALLOCATOR_H

#include <stddef.h>
#include <stdbool.h>

/* Total size of the arena the allocator manages (bytes). */
#define ARENA_SIZE (1024 * 1024) /* 1 MiB */

/* Public allocation API - mirrors the standard library shape. */
void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_calloc(size_t nmemb, size_t size);
void *my_realloc(void *ptr, size_t size);

/*
 * heap_check - Walk the entire heap and validate invariants:
 *   - every block's size fits within the arena bounds
 *   - blocks are contiguous with no gaps/overlaps
 *   - the free list only contains blocks marked free
 * Returns true if the heap is consistent, false otherwise.
 * Prints a diagnostic message on the first violation found.
 */
bool heap_check(void);

/*
 * heap_dump - Print a human readable listing of every block in the
 * heap (address, size, free/used) to stdout. Useful for demos and
 * debugging.
 */
void heap_dump(void);

/*
 * heap_free_block_count / heap_used_block_count - Count blocks in the
 * current heap by state. Exposed mainly so tests can verify coalescing
 * behavior (e.g. "two adjacent free blocks became one").
 */
size_t heap_free_block_count(void);
size_t heap_used_block_count(void);
size_t heap_total_block_count(void);

/*
 * heap_reset - Reset the allocator to a single, fully free block
 * spanning the whole arena. Intended for use between unit tests so
 * each test starts from a clean heap.
 */
void heap_reset(void);

#endif /* MY_ALLOCATOR_H */
