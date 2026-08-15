#include "allocator.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define HEAP_SIZE (1024 * 1024)   // 1 MB simulated heap
#define HEADER_SIZE (sizeof(block_header_t))

// Every allocated or free block starts with this header.
typedef struct block_header {
    size_t size;                // usable size of this block (not counting header)
    int is_free;                // 1 = free, 0 = in use
    struct block_header *next;  // next block in memory order
} block_header_t;

// The simulated heap. In a real OS this would come from sbrk()/mmap().
static uint8_t heap[HEAP_SIZE];
static block_header_t *heap_start = NULL;

void heap_init(void) {
    heap_start = (block_header_t *)heap;
    heap_start->size = HEAP_SIZE - HEADER_SIZE;
    heap_start->is_free = 1;
    heap_start->next = NULL;
}

void *my_malloc(size_t size) {
    if (size == 0 || heap_start == NULL) return NULL;

    block_header_t *curr = heap_start;

    // First-fit search: walk the list until we find a free block big enough.
    while (curr != NULL) {
        if (curr->is_free && curr->size >= size) {
            // Split the block if there's enough leftover space to justify
            // creating a new header (avoid internal fragmentation).
            size_t remaining = curr->size - size;
            if (remaining > HEADER_SIZE) {
                block_header_t *new_block =
                    (block_header_t *)((uint8_t *)curr + HEADER_SIZE + size);
                new_block->size = remaining - HEADER_SIZE;
                new_block->is_free = 1;
                new_block->next = curr->next;

                curr->size = size;
                curr->next = new_block;
            }
            curr->is_free = 0;
            return (void *)((uint8_t *)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    // No suitable block found.
    return NULL;
}

void my_free(void *ptr) {
    if (ptr == NULL) return;

    block_header_t *block =
        (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    block->is_free = 1;

    // Coalesce with the next block if it's also free.
    // This fights external fragmentation by merging adjacent free space.
    if (block->next != NULL && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    // Note: coalescing with the PREVIOUS block would require a doubly
    // linked list (a backward pointer). Left out here for simplicity —
    // this is a great thing to mention as a known limitation in an interview.
}

void print_heap(void) {
    block_header_t *curr = heap_start;
    int index = 0;
    printf("---- Heap State ----\n");
    while (curr != NULL) {
        printf("Block %d | addr: %p | size: %zu bytes | status: %s\n",
               index, (void *)curr, curr->size,
               curr->is_free ? "FREE" : "USED");
        curr = curr->next;
        index++;
    }
    printf("---------------------\n");
}
