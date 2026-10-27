#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

// Size of the simulated heap (a static array inside allocator.c).
#define HEAP_SIZE (1024 * 1024)   // 1 MB

// Placement policy used by my_malloc (and the "move" path of my_realloc).
typedef enum {
    FIT_FIRST,   // take the first free block that is big enough (fast)
    FIT_BEST     // scan everything, take the smallest free block that fits
} fit_policy_t;

// Snapshot of the heap, produced by heap_stats(). All sizes are payload
// bytes (block headers are not counted).
typedef struct {
    size_t total_free;     // sum of the sizes of all free blocks
    size_t largest_free;   // size of the biggest single free block
    size_t free_blocks;    // number of free blocks
    size_t used_bytes;     // sum of the sizes of all in-use blocks
    size_t total_blocks;   // free + in-use blocks
} heap_stats_t;

// Initialize the heap as one big free block. Must be called before any
// alloc/free. Calling it again wipes the heap. It does not change the
// fit policy.
void heap_init(void);

// malloc semantics: returns an 8-byte aligned pointer to at least `size`
// usable bytes, or NULL if size == 0 or no free block is big enough.
void *my_malloc(size_t size);

// free semantics: frees the block and merges it with free neighbours in
// O(1). NULL, pointers outside the heap, bad pointers and double frees are
// ignored.
void my_free(void *ptr);

// realloc semantics:
//   ptr == NULL  -> same as my_malloc(size)
//   size == 0    -> frees ptr, returns NULL
//   otherwise    -> resizes (in place when possible, else move + copy).
// On failure returns NULL and leaves the original block untouched.
void *my_realloc(void *ptr, size_t size);

// calloc semantics: allocates nmemb*size zeroed bytes. Returns NULL if the
// multiplication overflows, if the total is 0, or if there is no space.
void *my_calloc(size_t nmemb, size_t size);

// Select the placement policy at runtime. Unknown values are ignored.
void set_fit_policy(fit_policy_t policy);

// Walk the block list and return usage numbers (see heap_stats_t).
heap_stats_t heap_stats(void);

// Debug utility: prints every block in the heap with address, offset,
// size and status, followed by a one-line summary.
void print_heap(void);

#endif
