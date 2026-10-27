# Custom Memory Allocator in C

A from-scratch heap allocator with `malloc` / `free` / `realloc` / `calloc`
semantics over a simulated 1 MB heap, built to understand how dynamic memory
management works below the C standard library. It uses no libc `malloc` or
`free`: the backing store is a `static` array.

## What it does

- **Allocator API**: `my_malloc`, `my_free`, `my_realloc`, `my_calloc`
  (see `src/allocator.h`), plus `heap_init`, `heap_stats` and `print_heap`.
- **Simulated 1 MB heap** (`HEAP_SIZE`), a `static` array aligned to 8 bytes.
- **8-byte aligned payloads**: the header size and every block size are
  multiples of 8, and the heap array itself is aligned.
- **Doubly linked block list** (`prev` / `next` in every header), in address
  order.
- **Block splitting**: a free block bigger than the request is cut in two.
- **Bidirectional coalescing**: `my_free` merges the block with its free
  next *and* previous neighbours in constant time.
- **Two placement policies, selectable at runtime**: `set_fit_policy(FIT_FIRST)`
  or `set_fit_policy(FIT_BEST)`.
- **Safe misuse handling**: `my_free(NULL)`, double frees and pointers outside
  the heap (or not at the start of a live block) are ignored.
- **`heap_stats()`** returns free bytes, largest free block, free-block count,
  bytes in use and block count. `print_heap()` shows every block.
- **Test suite** (`test/test.c`) and a **fragmentation benchmark**
  (`bench/compare.c`).

## Build, test, benchmark

```bash
make          # build allocator.o, the test binary and the benchmark into build/
make test     # build tests with -Wall -Wextra -Werror -std=c11
              #   -fsanitize=address,undefined, then run them
make bench    # run the first-fit vs best-fit comparison
make clean
```

Requires `gcc` (or another C11 compiler via `make CC=clang`) and `make`.
The header layout assumes a 64-bit target (a `_Static_assert` checks that the
header size is a multiple of 8).

## Design decisions

### Block layout
```
| header (32 B) | payload ...... | header | payload ...... | ...
```
Each header stores `size` (payload bytes), `next`, `prev`, `is_free` and a
`magic` tag. Blocks tile the heap exactly, so the next block in the list is
also the next block in memory.

### Doubly linked list
With only a `next` pointer, a freed block cannot find the block before it
without walking the list from the start, which is O(n). With `prev`, free
reaches both physical neighbours in O(1). The cost is 8 extra bytes per header.
Every operation that changes the list (split, coalesce, free, realloc) keeps
`prev` and `next` in sync.

### Alignment
`my_malloc` rounds the requested size up to a multiple of 8 and the header is
32 bytes, so if `heap[0]` is 8-byte aligned then every payload address is too.
The rounding is internal fragmentation (at most 7 bytes per block).

### Splitting
After picking a free block, `split_block` turns the unused tail into a new
free block, but only if the tail can hold a header plus at least 8 bytes.
Otherwise the slack stays inside the allocated block rather than creating an
unusable fragment.

### Coalescing
`my_free` marks the block free and merges it with its next neighbour (if free)
and then with its previous neighbour (if free), so three adjacent blocks
collapse into one. It only looks at `prev`/`next`, so it is O(1). Because
every free merges immediately, the heap never contains two adjacent free
blocks; the test suite checks this invariant after almost every operation.
Merged-away headers have their `magic` cleared so stale pointers are rejected.

### realloc
- `ptr == NULL` -> `my_malloc(size)`; `size == 0` -> free and return `NULL`.
- **Shrink**: split in place and give the tail back (merging it with a free
  next block if there is one).
- **Grow**: first try to absorb a free next block (no copy, same pointer);
  otherwise allocate a new block, `memcpy`, free the old one.
- On failure it returns `NULL` and the original block is untouched.

### Pointer validation
`my_free`/`my_realloc` check that the pointer is inside the heap, 8-byte
aligned, that the header before it has the magic tag, and that the block is
not already free. This is O(1) and catches the common mistakes (stack
pointers, `NULL`, double free, merged-away blocks, most interior pointers). It
is best effort: it cannot prove a pointer is a real allocation.

### First-fit vs best-fit
- **First-fit** takes the first free block that is big enough. It stops
  early, so it is usually faster per allocation, and it tends to keep big free
  space at the high-address end of the heap.
- **Best-fit** scans the whole list and takes the smallest block that fits
  (stopping early on an exact fit). It wastes the least space *in that block*,
  but it costs a full scan and tends to leave many tiny leftover slivers.
- Neither is always better; it depends on the workload. See the measurements
  below.

### Static array instead of `sbrk()` / `mmap()`
Keeps the project portable and dependency-free. A real allocator asks the OS
for pages; this simulates that with a fixed buffer.

## Fragmentation comparison

`make bench` runs three deterministic workloads (`srand(42)`, identical random
sequence for both policies) and prints utilization and external fragmentation
at the end of each:

- utilization = used payload bytes / heap size
- external fragmentation = 1 - largest_free / total_free (0 if nothing is free)

Workloads: (a) random mixed sizes with random frees, (b) many small blocks,
free every other one, then allocate larger blocks, (c) a steady state of
FIFO alloc/free of a fixed number of live blocks.

Results from one run on the development machine (`make bench`, gcc, Linux):

```
Workload: (a) random mixed sizes, random frees
policy     | utilization | ext. frag | free blocks | largest free |  total free |   failed
first-fit  |      62.23% |    65.02% |         274 |       115872 |      331216 |   0/30876
best-fit   |      61.84% |    79.72% |         133 |        68904 |      339816 |   0/30876

Workload: (b) small blocks, free every other, then larger allocations
first-fit  |      68.41% |    99.84% |        1551 |          256 |      156400 | 1587/8000
best-fit   |      68.01% |    99.84% |        1534 |          256 |      161184 | 1587/8000

Workload: (c) steady state: FIFO replacement of 1000 live blocks
first-fit  |      55.89% |     2.51% |           5 |       419584 |      430400 |   0/60000
best-fit   |      56.05% |     8.64% |           6 |       391616 |      428672 |   0/60000
```

What these runs show: **on these workloads best-fit did not reduce external
fragmentation.** In (a) and (c) first-fit ended with a *larger* largest free
block (lower fragmentation by this metric); in (b) both policies fail the
same number of allocations. Best-fit did end with fewer free blocks in (a).
The numbers come from `rand()`, so they may differ on another libc, and they
are three specific workloads, not a general proof about either policy.

## Testing

`make test` runs 19 tests; each prints `[PASS]`/`[FAIL]` and the program exits
non-zero on the first failure. They cover basic allocation, data integrity,
reuse of a freed hole, splitting, forward / backward / two-sided coalescing,
freeing everything back to a single block, realloc (NULL, size 0, shrink, grow
in place, grow by move, failure leaves original intact), calloc (zeroing and
overflow), 8-byte alignment over many sizes, out-of-memory, invalid pointers
and double frees, both fit policies, and `heap_stats`.

The tests `#include` `allocator.c` directly (white-box testing) so a
consistency checker can walk the real block list. After almost every operation
it verifies that headers plus payloads add up to the heap size, `prev`/`next`
agree with each other and with memory layout, no two adjacent blocks are free,
and all sizes are multiples of 8. The tests build with AddressSanitizer and
UndefinedBehaviorSanitizer.

## Known limitations

- **Not thread-safe**: concurrent calls would race on the block list; a mutex
  around the public functions would fix this.
- **Fixed heap size**: 1 MB, it cannot grow via `sbrk()`/`mmap()`.
- Allocation is O(n) in the number of blocks (no free lists or size bins).
- Pointer validation is best effort (see above).
