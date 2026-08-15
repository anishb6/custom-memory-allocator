# Custom Memory Allocator in C

A from-scratch implementation of `malloc()` / `free()` semantics, built to
understand exactly how dynamic memory management works below the C
standard library.

## What it does

- Manages a fixed 1 MB simulated heap (`static uint8_t heap[1MB]`)
- `my_malloc(size)` — allocates memory using **first-fit** search over a
  linked list of block headers
- `my_free(ptr)` — frees a block and **coalesces** it with the next block
  if that block is also free, reducing external fragmentation
- **Block splitting** — if a free block is larger than the requested size,
  it's split so the leftover space becomes a new free block, avoiding
  internal fragmentation
- `print_heap()` — walks the heap and prints every block's address, size,
  and status (free/used), useful for visualizing fragmentation live

## Design decisions

- **First-fit over best-fit**: O(n) per allocation, simple to reason about
  and implement correctly under time pressure. Best-fit reduces wasted
  space but is slower per call and can still fragment memory elsewhere —
  a known tradeoff.
- **Singly linked list of headers**: each block carries `size`, `is_free`,
  and a `next` pointer to the next block in memory order.
- **Forward-only coalescing**: a freed block merges with the block after
  it, but not the block before it (that would require a doubly linked
  list with a `prev` pointer). Documented here as a known limitation and
  the clear next improvement.
- **Static array instead of `sbrk()`/`mmap()`**: keeps the project fully
  portable and dependency-free. A real allocator would request memory
  pages from the OS; this simulates that with a fixed buffer.

## Known limitations / next steps

- No backward coalescing (needs a doubly linked / prev pointer)
- Not thread-safe — concurrent `my_malloc`/`my_free` calls would race on
  the shared free list; a mutex around the critical section would fix this
- No `realloc()` implementation yet
- Fixed heap size instead of dynamically growing via `sbrk()`

## Build & run

```bash
gcc -Wall -o test_run src/allocator.c test/test.c
./test_run
```

## Testing

The allocator was tested with:

- Allocating multiple blocks
- Freeing a block and reusing its space
- Splitting a free block
- Coalescing free blocks
- Handling allocation requests larger than the available heap

The test program prints the state of the simulated heap after each operation.

## Sample output

```
=== After allocating A(100), B(200), C(50) ===
---- Heap State ----
Block 0 | addr: 0x... | size: 100 bytes | status: USED
Block 1 | addr: 0x... | size: 200 bytes | status: USED
Block 2 | addr: 0x... | size: 50 bytes  | status: USED
Block 3 | addr: 0x... | size: 1048130 bytes | status: FREE
---------------------

=== Freeing A, then D (watch coalescing merge free blocks) ===
---- Heap State ----
Block 0 | addr: 0x... | size: 200 bytes | status: FREE   <- merged blocks
Block 1 | addr: 0x... | size: 50 bytes  | status: USED
Block 2 | addr: 0x... | size: 1048130 bytes | status: FREE
---------------------
```
