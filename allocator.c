/*
 * allocator.c - A small first-fit, coalescing, free-list memory allocator.
 *
 * Memory layout
 * -------------
 * A single large arena is reserved once via mmap() (falling back to a
 * static byte array if mmap is unavailable). The arena is carved into
 * a singly linked chain of "blocks", each block consisting of a fixed
 * size header immediately followed by the block's payload:
 *
 *   +----------------+------------------------------+
 *   | block_header_t | payload (block->size bytes)  |
 *   +----------------+------------------------------+
 *   ^                ^
 *   header pointer   pointer returned to caller (header + 1)
 *
 * block_header_t {
 *     size_t size;               size of the payload area, not
 *                                 including the header itself
 *     bool   free;               true if this block is available
 *     struct block_header *next; next block in address order (or NULL
 *                                 for the last block in the arena)
 *     unsigned int magic;        MAGIC_ALLOC / MAGIC_FREE - used to
 *                                 sanity check pointers passed to free()
 *                                 and to catch double frees.
 * }
 *
 * Blocks always cover the arena contiguously: header->next, when
 * non-NULL, points exactly at (char *)header + sizeof(header) + size.
 * There are never gaps between blocks and never overlaps - heap_check()
 * verifies this.
 *
 * Placement strategy: first-fit. malloc() walks the block list from the
 * start of the arena and returns the first free block big enough to
 * hold the request, splitting off the remainder as a new free block
 * when there is enough room left over.
 *
 * Coalescing: free() always attempts to merge the freed block with its
 * immediate physical neighbours (previous and next) when they are also
 * free, which keeps fragmentation down.
 */

#include "allocator.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#define HAVE_MMAP 1
#endif

/* Magic markers stamped into every header. Used both as a lightweight
 * corruption check and to detect double-free: a block that is already
 * marked MAGIC_FREE_TAG cannot legitimately be freed again. */
#define MAGIC_ALLOC_TAG 0xA110CA7Eu
#define MAGIC_FREE_TAG  0xF4EEF4EEu

/* Alignment for payload sizes/addresses. */
#define ALIGNMENT 16u

/* Smallest payload size worth splitting off into its own free block.
 * Splitting into anything smaller than this would waste more on header
 * overhead than it saves. */
#define MIN_PAYLOAD 16u

typedef struct block_header {
    size_t size;               /* payload size in bytes */
    bool free;                 /* is this block available? */
    struct block_header *next; /* next block, address order */
    unsigned int magic;        /* MAGIC_ALLOC_TAG or MAGIC_FREE_TAG */
} block_header_t;

static void *g_arena_base = NULL;
static size_t g_arena_size = 0;
static block_header_t *g_heap_start = NULL;

#if !HAVE_MMAP
static unsigned char g_static_arena[ARENA_SIZE];
#endif

static size_t align_up(size_t n, size_t alignment) {
    return (n + (alignment - 1)) & ~(alignment - 1);
}

static void heap_init(void) {
    if (g_heap_start != NULL) {
        return;
    }

#if HAVE_MMAP
    void *mem = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        mem = NULL;
    }
#else
    void *mem = g_static_arena;
#endif

#if HAVE_MMAP
    if (mem == NULL) {
        /* Fall back to the static arena if mmap failed for some reason. */
        static unsigned char fallback_arena[ARENA_SIZE];
        mem = fallback_arena;
    }
#endif

    g_arena_base = mem;
    g_arena_size = ARENA_SIZE;

    g_heap_start = (block_header_t *)g_arena_base;
    g_heap_start->size = g_arena_size - sizeof(block_header_t);
    g_heap_start->free = true;
    g_heap_start->next = NULL;
    g_heap_start->magic = MAGIC_FREE_TAG;
}

void heap_reset(void) {
    /* Force re-initialization of a single, fully free block. Reuses the
     * same arena allocation across calls (does not re-mmap). */
    if (g_arena_base == NULL) {
        heap_init();
        return;
    }
    g_heap_start = (block_header_t *)g_arena_base;
    g_heap_start->size = g_arena_size - sizeof(block_header_t);
    g_heap_start->free = true;
    g_heap_start->next = NULL;
    g_heap_start->magic = MAGIC_FREE_TAG;
}

static block_header_t *block_from_ptr(void *ptr) {
    return ((block_header_t *)ptr) - 1;
}

static bool ptr_in_arena(void *ptr) {
    return ptr >= g_arena_base &&
           (char *)ptr < (char *)g_arena_base + g_arena_size;
}

/* Split `block` so that only `payload_needed` bytes remain in it,
 * turning the leftover tail into a brand new free block, provided
 * there's enough room left over to make that worthwhile. */
static void maybe_split(block_header_t *block, size_t payload_needed) {
    size_t leftover = block->size - payload_needed;
    if (leftover < sizeof(block_header_t) + MIN_PAYLOAD) {
        return; /* not worth splitting */
    }

    char *new_block_addr = (char *)(block + 1) + payload_needed;
    block_header_t *new_block = (block_header_t *)new_block_addr;
    new_block->size = leftover - sizeof(block_header_t);
    new_block->free = true;
    new_block->magic = MAGIC_FREE_TAG;
    new_block->next = block->next;

    block->size = payload_needed;
    block->next = new_block;

    /* The block immediately after the newly split-off free block may
     * already be free too (e.g. when splitting off the tail of a
     * block during realloc-shrink, right before an existing free
     * block) - coalesce them so we never leave two adjacent free
     * blocks un-merged. */
    if (new_block->next != NULL && new_block->next->free) {
        block_header_t *next = new_block->next;
        new_block->size += sizeof(block_header_t) + next->size;
        new_block->next = next->next;
    }
}

void *my_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    heap_init();

    size_t aligned = align_up(size, ALIGNMENT);
    if (aligned < MIN_PAYLOAD) {
        aligned = MIN_PAYLOAD;
    }

    block_header_t *cur = g_heap_start;
    while (cur != NULL) {
        if (cur->free && cur->size >= aligned) {
            maybe_split(cur, aligned);
            cur->free = false;
            cur->magic = MAGIC_ALLOC_TAG;
            return (void *)(cur + 1);
        }
        cur = cur->next;
    }

    /* No block big enough - arena exhausted. Fail gracefully. */
    return NULL;
}

static block_header_t *find_prev(block_header_t *target) {
    if (g_heap_start == target) {
        return NULL;
    }
    block_header_t *cur = g_heap_start;
    while (cur != NULL) {
        if (cur->next == target) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

void my_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (g_heap_start == NULL || !ptr_in_arena(ptr)) {
        fprintf(stderr, "my_free: pointer %p is not within the arena, ignoring\n", ptr);
        return;
    }

    block_header_t *block = block_from_ptr(ptr);

    if (block->magic == MAGIC_FREE_TAG) {
        fprintf(stderr, "my_free: double free detected at %p, ignoring\n", ptr);
        return;
    }
    if (block->magic != MAGIC_ALLOC_TAG) {
        fprintf(stderr, "my_free: corrupted or invalid pointer %p, ignoring\n", ptr);
        return;
    }

    block->free = true;
    block->magic = MAGIC_FREE_TAG;

    /* Coalesce with next neighbour. */
    if (block->next != NULL && block->next->free) {
        block_header_t *next = block->next;
        block->size += sizeof(block_header_t) + next->size;
        block->next = next->next;
    }

    /* Coalesce with previous neighbour. */
    block_header_t *prev = find_prev(block);
    if (prev != NULL && prev->free) {
        prev->size += sizeof(block_header_t) + block->size;
        prev->next = block->next;
    }
}

void *my_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) {
        return NULL;
    }
    /* Overflow check for nmemb * size. */
    if (nmemb > (size_t)-1 / size) {
        return NULL;
    }
    size_t total = nmemb * size;
    void *ptr = my_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *my_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return my_malloc(size);
    }
    if (size == 0) {
        my_free(ptr);
        return NULL;
    }
    if (!ptr_in_arena(ptr)) {
        return NULL;
    }

    block_header_t *block = block_from_ptr(ptr);
    if (block->magic != MAGIC_ALLOC_TAG) {
        fprintf(stderr, "my_realloc: invalid pointer %p\n", ptr);
        return NULL;
    }

    size_t aligned = align_up(size, ALIGNMENT);
    if (aligned < MIN_PAYLOAD) {
        aligned = MIN_PAYLOAD;
    }

    if (aligned <= block->size) {
        /* Shrinking (or same size): split off the tail if worthwhile. */
        maybe_split(block, aligned);
        return ptr;
    }

    /* Growing: try to absorb a free, physically-adjacent next block
     * in place before falling back to malloc+copy+free. */
    if (block->next != NULL && block->next->free &&
        block->size + sizeof(block_header_t) + block->next->size >= aligned) {
        block_header_t *next = block->next;
        block->size += sizeof(block_header_t) + next->size;
        block->next = next->next;
        maybe_split(block, aligned);
        return ptr;
    }

    void *new_ptr = my_malloc(size);
    if (new_ptr == NULL) {
        return NULL; /* original block is left untouched, per realloc contract */
    }
    size_t to_copy = block->size < size ? block->size : size;
    memcpy(new_ptr, ptr, to_copy);
    my_free(ptr);
    return new_ptr;
}

bool heap_check(void) {
    if (g_heap_start == NULL) {
        return true; /* nothing allocated yet */
    }

    char *expected_addr = (char *)g_heap_start;
    char *arena_end = (char *)g_arena_base + g_arena_size;
    block_header_t *cur = g_heap_start;
    block_header_t *prev = NULL;

    while (cur != NULL) {
        if ((char *)cur != expected_addr) {
            fprintf(stderr, "heap_check: block at %p does not start where expected (%p)\n",
                    (void *)cur, (void *)expected_addr);
            return false;
        }
        if ((char *)cur < (char *)g_arena_base || (char *)(cur + 1) > arena_end) {
            fprintf(stderr, "heap_check: block header at %p is outside the arena\n", (void *)cur);
            return false;
        }
        char *payload_end = (char *)(cur + 1) + cur->size;
        if (payload_end > arena_end) {
            fprintf(stderr, "heap_check: block at %p overruns the arena (size=%zu)\n",
                    (void *)cur, cur->size);
            return false;
        }
        if (cur->free && cur->magic != MAGIC_FREE_TAG) {
            fprintf(stderr, "heap_check: free block at %p has bad magic\n", (void *)cur);
            return false;
        }
        if (!cur->free && cur->magic != MAGIC_ALLOC_TAG) {
            fprintf(stderr, "heap_check: used block at %p has bad magic\n", (void *)cur);
            return false;
        }
        if (cur->free && cur->next != NULL && cur->next->free) {
            fprintf(stderr, "heap_check: two adjacent free blocks were not coalesced (%p, %p)\n",
                    (void *)cur, (void *)cur->next);
            return false;
        }

        prev = cur;
        expected_addr = payload_end;
        cur = cur->next;
    }

    if (prev != NULL && expected_addr != arena_end) {
        fprintf(stderr, "heap_check: last block does not reach the end of the arena "
                        "(ends at %p, arena ends at %p)\n",
                (void *)expected_addr, (void *)arena_end);
        return false;
    }

    return true;
}

void heap_dump(void) {
    if (g_heap_start == NULL) {
        printf("(heap not initialized)\n");
        return;
    }

    printf("---- heap dump (arena %p, %zu bytes) ----\n", g_arena_base, g_arena_size);
    size_t index = 0;
    for (block_header_t *cur = g_heap_start; cur != NULL; cur = cur->next, index++) {
        printf("  [%zu] addr=%p header_size=%zu payload=%zu bytes state=%s\n",
               index, (void *)cur, sizeof(block_header_t), cur->size,
               cur->free ? "FREE" : "USED");
    }
    printf("-----------------------------------------\n");
}

size_t heap_free_block_count(void) {
    size_t count = 0;
    for (block_header_t *cur = g_heap_start; cur != NULL; cur = cur->next) {
        if (cur->free) {
            count++;
        }
    }
    return count;
}

size_t heap_used_block_count(void) {
    size_t count = 0;
    for (block_header_t *cur = g_heap_start; cur != NULL; cur = cur->next) {
        if (!cur->free) {
            count++;
        }
    }
    return count;
}

size_t heap_total_block_count(void) {
    size_t count = 0;
    for (block_header_t *cur = g_heap_start; cur != NULL; cur = cur->next) {
        count++;
    }
    return count;
}
