#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

// Initialize the heap. Must be called once before any alloc/free.
void heap_init(void);

// Custom malloc: returns pointer to usable memory, or NULL if no space.
void *my_malloc(size_t size);

// Custom free: marks the block as free and coalesces with neighbors.
void my_free(void *ptr);

// Debug utility: prints every block in the heap with address, size, status.
void print_heap(void);

#endif
