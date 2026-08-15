#include <stdio.h>
#include <string.h>
#include "../src/allocator.h"

int main(void) {
    heap_init();

    printf("=== Initial heap (one big free block) ===\n");
    print_heap();

    // Allocate three blocks of different sizes.
    char *a = (char *)my_malloc(100);
    char *b = (char *)my_malloc(200);
    char *c = (char *)my_malloc(50);

    strcpy(a, "Block A data");
    strcpy(b, "Block B data");
    strcpy(c, "Block C data");

    printf("\n=== After allocating A(100), B(200), C(50) ===\n");
    print_heap();

    // Free B (the middle block) - creates a "hole" between A and C.
    printf("\n=== Freeing B (creates a hole in the middle) ===\n");
    my_free(b);
    print_heap();

    // Allocate D, smaller than B's freed space - should reuse/split B's hole.
    printf("\n=== Allocating D(80) - should reuse B's freed space ===\n");
    char *d = (char *)my_malloc(80);
    strcpy(d, "Block D data");
    print_heap();

    // Free A and D - since they are adjacent in the list (A, D, leftover-of-B,
    // C), freeing them shows coalescing merging adjacent free blocks.
    printf("\n=== Freeing A, then D (watch coalescing merge free blocks) ===\n");
    my_free(a);
    print_heap();
    my_free(d);
    print_heap();

    // Try to allocate something too big to fit.
    printf("\n=== Attempting to allocate 5,000,000 bytes (should fail) ===\n");
    void *too_big = my_malloc(5000000);
    if (too_big == NULL) {
        printf("my_malloc correctly returned NULL - not enough space.\n");
    }

    // Free C to clean up.
    printf("\n=== Freeing C ===\n");
    my_free(c);
    print_heap();

    return 0;
}
