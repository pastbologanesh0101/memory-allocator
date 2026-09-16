# Memory Allocator

A small, self-contained implementation of `malloc`/`free`/`calloc`/`realloc`
in C, built entirely on top of a single pre-reserved memory arena. It does
not call the system's `malloc` internally — the arena is reserved once via
`mmap` (falling back to a static buffer if `mmap` is unavailable) and every
byte handed back to the caller is carved out of it by hand using a
free-list-based heap manager.

## Design

### Memory layout

The arena is a fixed-size block of memory (`ARENA_SIZE`, 1 MiB by default,
see `allocator.h`). It is divided into a contiguous chain of **blocks**.
Each block is a header immediately followed by its payload:

```
+------------------+--------------------------------+
|  block_header_t  |   payload (block->size bytes)  |
+------------------+--------------------------------+
^                   ^
header pointer      pointer returned to the caller (header + 1)
```

The header is:

```c
typedef struct block_header {
    size_t size;                /* payload size in bytes */
    bool   free;                /* is this block available? */
    struct block_header *next;  /* next block, in address order */
    unsigned int magic;         /* MAGIC_ALLOC_TAG / MAGIC_FREE_TAG */
} block_header_t;
```

Blocks are singly linked in address order and always cover the arena
contiguously — there are never gaps or overlaps between them:
`block->next` (when non-`NULL`) always points exactly at
`(char *)block + sizeof(block_header_t) + block->size`. `heap_check()`
verifies this invariant, along with header magic values.

The `magic` field also lets `my_free` detect **double frees**: a block
that already carries `MAGIC_FREE_TAG` cannot legitimately be freed again,
so a second `my_free()` call on the same pointer is caught, logged to
`stderr`, and safely ignored instead of corrupting the free list.

### Placement strategy: first-fit

`my_malloc` walks the block list from the start of the arena and returns
the first free block that is large enough for the request. If the block
has enough left-over space after satisfying the request
(`>= sizeof(block_header_t) + MIN_PAYLOAD`), it is split in place: the
front portion becomes the allocated block, and a brand new free block
header is written for the remainder.

### Coalescing

`my_free` marks a block free and then attempts to merge it with its
immediate physical neighbours:

1. If the **next** block is also free, the two are merged into one.
2. The list is then walked from the head to find the **previous**
   block (the header format is a singly linked list, so this is an
   O(n) walk); if that block is also free, it absorbs the just-freed
   block too.

This keeps adjacent free space from fragmenting into lots of tiny
unusable holes. `heap_check()` also independently verifies that no two
adjacent blocks are ever both marked free, which would indicate a missed
coalesce.

### `realloc` / `calloc`

* `my_calloc(nmemb, size)` allocates `nmemb * size` bytes (with an
  overflow check) and zero-fills them.
* `my_realloc(ptr, size)`:
  * `ptr == NULL` behaves like `my_malloc(size)`.
  * `size == 0` behaves like `my_free(ptr)` and returns `NULL`.
  * Shrinking a block splits off the unused tail as a new free block
    and returns the same pointer.
  * Growing a block first tries to absorb a free, physically-adjacent
    next block in place (no copy needed); if that isn't possible or
    isn't big enough, it falls back to `malloc` a new block, `memcpy`
    the old contents over, and `free` the old block.

### Failure behavior

Requests that cannot be satisfied (including a request larger than the
whole arena) simply return `NULL` — they never crash and never corrupt
the heap. `heap_check()`/`heap_dump()` walk the whole block list and can
be called any time (including from tests) to confirm the heap is still
internally consistent.

## Building

```sh
make              # builds the allocator, the demo binary, and the test binary
make run-demo     # builds (if needed) and runs the demo program
make test         # builds (if needed) and runs the unit test binary
make clean        # removes build artifacts
```

Requires only a C11 compiler (`cc`/`gcc`/`clang`) and `make`.

## Running the demo

```sh
make run-demo
```

The demo allocates a handful of differently-sized blocks, frees some of
them to show coalescing in action, grows a buffer with `realloc` while
preserving its contents, uses `calloc`, deliberately tries to allocate
more memory than the whole arena (and shows it fails gracefully), and
dumps/validates the heap (`heap_dump` / `heap_check`) at each step.

## Running the tests

```sh
make test
```

`tests/test_allocator.c` contains a tiny, hand-rolled assert-based test
runner (no external test framework dependency) with unit tests covering:

1. Basic alloc/free round-trip.
2. Multiple simultaneous allocations don't overlap.
3. Freeing and re-allocating the same size reuses the same memory.
4. Freeing adjacent blocks coalesces them into a single free block
   (verified via block counts).
5. `realloc` growing a block preserves its existing contents.
6. `realloc` shrinking a block preserves its existing contents.
7. `calloc` zero-initializes its buffer.
8. Allocating more than the arena size fails gracefully (returns `NULL`,
   does not crash).
9. Double-freeing a pointer is caught and does not corrupt the heap.

Continuous integration (`.github/workflows/tests.yml`) builds the
project and runs this test binary on every push and pull request.

## License

MIT — see [LICENSE](LICENSE).
