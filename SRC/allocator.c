#include "allocator.h"
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ALIGNMENT 8              // every payload address is a multiple of this
#define BLOCK_MAGIC 0xA110CA7Eu  // tag stored in every live header
#define HEADER_SIZE (sizeof(block_header_t))

// Every block (free or in use) starts with this header. The payload that
// the user receives starts right after it, at (uint8_t *)header + HEADER_SIZE.
//
//   | header | payload ...... | header | payload ...... | ...
//   ^ block                     ^ block->next
//
// Blocks tile the heap exactly: next == (uint8_t *)block + HEADER_SIZE + size.
// That is why "next in the list" is also "next in memory", and why prev/next
// are enough to find physical neighbours in O(1).
typedef struct block_header {
    size_t size;                // payload bytes (multiple of 8), excludes header
    struct block_header *next;  // next block in address order, NULL for the last
    struct block_header *prev;  // previous block in address order, NULL for the first
    uint32_t is_free;           // 1 = free, 0 = in use
    uint32_t magic;             // BLOCK_MAGIC while the header is live; 0 once
                                // the header has been merged into a neighbour
} block_header_t;

// If this fires (e.g. on a 32-bit build) the header would break alignment.
_Static_assert(sizeof(block_header_t) % ALIGNMENT == 0,
               "header size must be a multiple of the alignment");

// The simulated heap. In a real OS this would come from sbrk()/mmap().
// alignas guarantees heap[0] is 8-byte aligned; because the header size and
// every payload size are multiples of 8, so is every payload address.
static alignas(ALIGNMENT) uint8_t heap[HEAP_SIZE];
static block_header_t *heap_start = NULL;
static fit_policy_t policy = FIT_FIRST;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Round n up to the next multiple of ALIGNMENT. Callers must make sure n is
// not close to SIZE_MAX (my_malloc/my_realloc reject n > HEAP_SIZE first).
static size_t align_up(size_t n) {
    return (n + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1);
}

static void *payload_of(block_header_t *b) {
    return (uint8_t *)b + HEADER_SIZE;
}

// Turn a user pointer into its header, or return NULL if the pointer cannot
// be a live allocation. O(1): no list walk. Checks, in order:
//   1. heap initialized
//   2. pointer lies inside the heap (compared as integers: comparing
//      unrelated pointers with < is undefined in C)
//   3. pointer is 8-byte aligned
//   4. header carries the magic tag (rejects most interior/garbage pointers
//      and headers that were merged away)
//   5. block is in use (rejects double free)
// This is best effort, not a proof: a pointer into the middle of a payload
// that happens to hold the magic value would slip through.
static block_header_t *validate_ptr(void *ptr) {
    if (heap_start == NULL || ptr == NULL) return NULL;

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t lo = (uintptr_t)heap + HEADER_SIZE;
    uintptr_t hi = (uintptr_t)heap + HEAP_SIZE;
    if (addr < lo || addr >= hi) return NULL;
    if (addr % ALIGNMENT != 0) return NULL;

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    if (b->magic != BLOCK_MAGIC) return NULL;
    if (b->is_free) return NULL;
    return b;
}

// ---------------------------------------------------------------------------
// Split / coalesce: the two operations that keep the list tidy
// ---------------------------------------------------------------------------

// SPLIT. Shrink block `b` to `size` payload bytes and turn the tail into a
// new free block, so we do not hand out more memory than was asked for.
//
//   before: | hdr_b | .................. size_b .................. | next...
//   after:  | hdr_b | .. size .. | hdr_new | ..... size_b - size - HEADER | next...
//
// Precondition: size is a multiple of 8 and size <= b->size.
// The tail must be able to hold a header plus at least ALIGNMENT payload
// bytes; otherwise we leave the slack inside b (a little internal
// fragmentation beats an unusable zero-byte block).
// Returns the new free block, or NULL if no split happened.
// This function does NOT touch b->is_free; the caller decides that.
static block_header_t *split_block(block_header_t *b, size_t size) {
    size_t remaining = b->size - size;
    if (remaining < HEADER_SIZE + ALIGNMENT) return NULL;

    block_header_t *rest = (block_header_t *)((uint8_t *)b + HEADER_SIZE + size);
    rest->size = remaining - HEADER_SIZE;
    rest->is_free = 1;
    rest->magic = BLOCK_MAGIC;

    // Splice `rest` into the doubly linked list right after b. There are
    // four pointers to fix: rest->prev, rest->next, b->next and the old
    // next block's prev. Forgetting the last one is the classic bug.
    rest->prev = b;
    rest->next = b->next;
    if (b->next != NULL) b->next->prev = rest;
    b->next = rest;

    b->size = size;
    return rest;
}

// ABSORB. Merge block `gone` into `keep`, where gone == keep->next. The
// header of `gone` and its payload become part of keep's payload.
static void absorb(block_header_t *keep, block_header_t *gone) {
    keep->size += HEADER_SIZE + gone->size;
    keep->next = gone->next;
    if (gone->next != NULL) gone->next->prev = keep;
    gone->magic = 0;   // dead header: stale pointers to it are now rejected
}

// COALESCE. `b` is a free block that was just created or enlarged. Merge it
// with its free physical neighbours and return the surviving block.
// Constant time: it only looks at b->next and b->prev, never walks the list.
//
//   case 1 (forward):  [b free][next free]            -> [b ........]
//   case 2 (backward): [prev free][b free]            -> [prev ......]
//   case 3 (both):     [prev free][b free][next free] -> [prev ........]
//
// Backward merging is what the old singly linked list could not do: given
// only `next`, there is no O(1) way to find the block before b.
static block_header_t *coalesce(block_header_t *b) {
    if (b->next != NULL && b->next->is_free) {
        absorb(b, b->next);
    }
    if (b->prev != NULL && b->prev->is_free) {
        block_header_t *p = b->prev;
        absorb(p, b);
        b = p;
    }
    return b;
}

// ---------------------------------------------------------------------------
// Placement policies
// ---------------------------------------------------------------------------

// Find a free block with at least `size` payload bytes, or NULL.
static block_header_t *find_fit(size_t size) {
    block_header_t *best = NULL;

    for (block_header_t *cur = heap_start; cur != NULL; cur = cur->next) {
        if (!cur->is_free || cur->size < size) continue;

        if (policy == FIT_FIRST) return cur;   // first big-enough block wins

        // FIT_BEST: remember the smallest block that fits. On a tie the
        // earlier (lower-address) block is kept because we use '<'.
        if (best == NULL || cur->size < best->size) {
            best = cur;
            if (best->size == size) break;     // exact fit cannot be beaten
        }
    }
    return best;
}

void set_fit_policy(fit_policy_t p) {
    if (p == FIT_FIRST || p == FIT_BEST) policy = p;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void heap_init(void) {
    heap_start = (block_header_t *)heap;
    heap_start->size = HEAP_SIZE - HEADER_SIZE;
    heap_start->is_free = 1;
    heap_start->magic = BLOCK_MAGIC;
    heap_start->next = NULL;
    heap_start->prev = NULL;
}

void *my_malloc(size_t size) {
    if (size == 0 || heap_start == NULL) return NULL;
    if (size > HEAP_SIZE) return NULL;   // also keeps align_up from overflowing
    size = align_up(size);

    block_header_t *b = find_fit(size);
    if (b == NULL) return NULL;

    split_block(b, size);   // hand back the unused tail as a new free block
    b->is_free = 0;
    return payload_of(b);
}

void my_free(void *ptr) {
    block_header_t *b = validate_ptr(ptr);   // NULL, outside heap, bad, double free
    if (b == NULL) return;

    b->is_free = 1;
    coalesce(b);   // O(1): merges with next and/or prev if they are free
}

void *my_realloc(void *ptr, size_t size) {
    if (ptr == NULL) return my_malloc(size);
    if (size == 0) {
        my_free(ptr);
        return NULL;
    }

    block_header_t *b = validate_ptr(ptr);
    if (b == NULL) return NULL;          // not a live allocation: do nothing
    if (size > HEAP_SIZE) return NULL;   // can never fit; b is untouched
    size_t need = align_up(size);

    // Case 1: shrink (or same size). Stay in place, give the tail back.
    // The tail's right neighbour may be free, so coalesce it forward.
    if (need <= b->size) {
        block_header_t *rest = split_block(b, need);
        if (rest != NULL) coalesce(rest);
        return ptr;
    }

    // Case 2: grow in place by absorbing a free next block, if the combined
    // block is big enough. The pointer does not change and nothing is copied.
    block_header_t *n = b->next;
    if (n != NULL && n->is_free && b->size + HEADER_SIZE + n->size >= need) {
        absorb(b, n);
        // We may have absorbed more than needed; give the surplus back.
        // (The block after the surplus is not free, because n was free and
        // the heap never has two adjacent free blocks, so no coalesce here.)
        split_block(b, need);
        return ptr;
    }

    // Case 3: allocate elsewhere, copy, free the old block. b is still in
    // use while my_malloc searches, so it cannot hand b out. If my_malloc
    // fails we return NULL before touching anything: the caller keeps the
    // original pointer and its data, as realloc requires.
    void *new_ptr = my_malloc(size);
    if (new_ptr == NULL) return NULL;
    memcpy(new_ptr, ptr, b->size);   // b->size < need <= new block size
    my_free(ptr);
    return new_ptr;
}

void *my_calloc(size_t nmemb, size_t size) {
    // nmemb * size must not wrap around. Dividing is the portable overflow
    // test: if size > SIZE_MAX / nmemb, then nmemb * size > SIZE_MAX.
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;

    size_t total = nmemb * size;
    void *p = my_malloc(total);   // returns NULL for total == 0
    if (p != NULL) memset(p, 0, total);
    return p;
}

heap_stats_t heap_stats(void) {
    heap_stats_t s = {0, 0, 0, 0, 0};
    for (block_header_t *cur = heap_start; cur != NULL; cur = cur->next) {
        s.total_blocks++;
        if (cur->is_free) {
            s.total_free += cur->size;
            s.free_blocks++;
            if (cur->size > s.largest_free) s.largest_free = cur->size;
        } else {
            s.used_bytes += cur->size;
        }
    }
    return s;
}

void print_heap(void) {
    block_header_t *curr = heap_start;
    int index = 0;
    printf("---- Heap State (%s-fit) ----\n",
           policy == FIT_FIRST ? "first" : "best");
    while (curr != NULL) {
        printf("Block %d | addr: %p | offset: %7zu | size: %7zu bytes | %s\n",
               index, (void *)curr, (size_t)((uint8_t *)curr - heap),
               curr->size, curr->is_free ? "FREE" : "USED");
        curr = curr->next;
        index++;
    }
    heap_stats_t s = heap_stats();
    printf("blocks: %zu | used: %zu | free: %zu (largest %zu)\n",
           s.total_blocks, s.used_bytes, s.total_free, s.largest_free);
    printf("-----------------------------\n");
}
