// Test suite for the custom allocator.
//
// This is a white-box test: it #includes allocator.c directly (instead of
// linking against it) so that the consistency checker below can see the real
// block_header_t and the static heap array. Build it with the Makefile:
//     make test
//
// Every check goes through CHECK(), which behaves like assert() but (a) is
// never compiled out by -DNDEBUG and (b) prints a FAIL line with the test
// name before exiting with a non-zero status.

#include "../src/allocator.c"

#include <stdlib.h>

static const char *current_test = "";

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("[FAIL] %s: %s (%s:%d)\n", current_test, #cond, __FILE__, \
                   __LINE__);                                                \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Consistency checker: walks the whole block list and verifies the invariants
//   * headers + payload sizes add up to exactly HEAP_SIZE (blocks tile the heap)
//   * prev/next pointers agree with each other and with memory layout
//   * no two adjacent blocks are both free
//   * every size is a multiple of 8 and every header carries the magic tag
// ---------------------------------------------------------------------------
static void check_heap(void) {
    CHECK(heap_start == (block_header_t *)heap);
    CHECK(heap_start->prev == NULL);

    size_t total = 0;
    block_header_t *prev = NULL;
    for (block_header_t *b = heap_start; b != NULL; b = b->next) {
        CHECK(b->magic == BLOCK_MAGIC);
        CHECK(b->is_free == 0 || b->is_free == 1);
        CHECK(b->size % ALIGNMENT == 0);
        CHECK(b->size > 0);
        CHECK(b->prev == prev);
        CHECK((uintptr_t)payload_of(b) % ALIGNMENT == 0);

        total += HEADER_SIZE + b->size;
        CHECK(total <= HEAP_SIZE);

        if (b->next != NULL) {
            // next block starts exactly where this one ends
            CHECK((uint8_t *)b->next == (uint8_t *)b + HEADER_SIZE + b->size);
            CHECK(b->next->prev == b);
            CHECK(!(b->is_free && b->next->is_free));   // no adjacent free blocks
        }
        prev = b;
    }
    CHECK(total == HEAP_SIZE);
}

static void fresh_heap(fit_policy_t p) {
    heap_init();
    set_fit_policy(p);
    check_heap();
}

static block_header_t *hdr(void *p) {
    return (block_header_t *)((uint8_t *)p - HEADER_SIZE);
}

static int all_bytes_equal(const void *p, unsigned char v, size_t n) {
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++)
        if (c[i] != v) return 0;
    return 1;
}

static int same_stats(heap_stats_t a, heap_stats_t b) {
    return a.total_free == b.total_free && a.largest_free == b.largest_free &&
           a.free_blocks == b.free_blocks && a.used_bytes == b.used_bytes &&
           a.total_blocks == b.total_blocks;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_basic_allocations(void) {
    fresh_heap(FIT_FIRST);
    char *a = my_malloc(100);
    char *b = my_malloc(200);
    char *c = my_malloc(50);
    CHECK(a && b && c);
    CHECK(a != b && b != c && a != c);
    check_heap();
    // payloads must not overlap (blocks are laid out in address order)
    CHECK(b >= a + 100 && c >= b + 200);
    CHECK(my_malloc(0) == NULL);
    heap_stats_t s = heap_stats();
    CHECK(s.total_blocks == 4);   // a, b, c, big free tail
    CHECK(s.free_blocks == 1);
}

static void test_data_integrity(void) {
    fresh_heap(FIT_FIRST);
    enum { N = 20 };
    unsigned char *p[N];
    size_t len[N];
    for (int i = 0; i < N; i++) {
        len[i] = 10 + (size_t)i * 37;
        p[i] = my_malloc(len[i]);
        CHECK(p[i] != NULL);
        memset(p[i], 0x30 + i, len[i]);
    }
    check_heap();
    for (int i = 0; i < N; i++) CHECK(all_bytes_equal(p[i], (unsigned char)(0x30 + i), len[i]));
}

static void test_free_and_reuse_hole(void) {
    fresh_heap(FIT_FIRST);
    void *a = my_malloc(100);
    void *b = my_malloc(200);
    void *c = my_malloc(50);
    CHECK(a && b && c);
    my_free(b);
    check_heap();
    void *d = my_malloc(200);   // exactly the hole
    CHECK(d == b);
    void *e = my_malloc(50);    // too big for any hole -> goes after c
    CHECK(e > c);
    check_heap();
}

static void test_splitting(void) {
    fresh_heap(FIT_FIRST);
    void *a = my_malloc(100);   // rounds up to 104
    CHECK(a != NULL);
    CHECK(hdr(a)->size == 104);
    heap_stats_t s = heap_stats();
    CHECK(s.total_blocks == 2);
    CHECK(s.used_bytes == 104);
    CHECK(s.total_free == HEAP_SIZE - 2 * HEADER_SIZE - 104);
    check_heap();

    // Splitting a hole: free a 300-byte block between guards, take 100 of it.
    void *b = my_malloc(300);
    void *g = my_malloc(8);
    CHECK(b && g);
    my_free(b);
    void *c = my_malloc(100);
    CHECK(c == b);
    CHECK(hdr(c)->size == 104);
    CHECK(hdr(c)->next->is_free);
    CHECK(hdr(c)->next->size == 304 - 104 - HEADER_SIZE);
    CHECK(hdr(c)->next->prev == hdr(c));
    check_heap();

    // A remainder too small for header + 8 bytes is NOT split off.
    fresh_heap(FIT_FIRST);
    void *x = my_malloc(64);
    void *y = my_malloc(8);
    CHECK(x && y);
    my_free(x);
    void *z = my_malloc(64 - 8);   // leaves 8 bytes: < HEADER_SIZE + 8
    CHECK(z == x);
    CHECK(hdr(z)->size == 64);
    check_heap();
}

static void test_forward_coalescing(void) {
    fresh_heap(FIT_FIRST);
    void *a = my_malloc(100);
    void *b = my_malloc(100);
    void *guard = my_malloc(100);
    CHECK(a && b && guard);
    heap_stats_t before = heap_stats();
    my_free(b);
    my_free(a);   // a's NEXT block (b) is free -> forward merge
    check_heap();
    heap_stats_t after = heap_stats();
    CHECK(after.total_blocks == before.total_blocks - 1);
    CHECK(hdr(a)->is_free);
    CHECK(hdr(a)->size == 104 + HEADER_SIZE + 104);
    CHECK(hdr(a)->next == hdr(guard));
}

static void test_backward_coalescing(void) {
    fresh_heap(FIT_FIRST);
    void *a = my_malloc(100);
    void *b = my_malloc(100);
    void *guard = my_malloc(100);
    CHECK(a && b && guard);
    heap_stats_t before = heap_stats();
    my_free(a);
    my_free(b);   // b's PREV block (a) is free -> backward merge into a
    check_heap();
    heap_stats_t after = heap_stats();
    CHECK(after.total_blocks == before.total_blocks - 1);
    CHECK(hdr(a)->is_free);
    CHECK(hdr(a)->size == 104 + HEADER_SIZE + 104);
    CHECK(hdr(a)->next == hdr(guard));
    CHECK(hdr(guard)->prev == hdr(a));
    CHECK(hdr(b)->magic == 0);   // b's header was merged away

    // The merged region is reusable as one block.
    void *big = my_malloc(104 + HEADER_SIZE + 104);
    CHECK(big == a);
    check_heap();
}

static void test_coalescing_both_sides(void) {
    fresh_heap(FIT_FIRST);
    void *a = my_malloc(100);
    void *b = my_malloc(100);
    void *c = my_malloc(100);
    void *guard = my_malloc(100);
    CHECK(a && b && c && guard);
    my_free(a);
    my_free(c);
    check_heap();
    CHECK(heap_stats().free_blocks == 3);   // a, c, tail
    my_free(b);   // prev (a) and next (c) both free -> one block
    check_heap();
    CHECK(heap_stats().free_blocks == 2);   // a+b+c, tail
    CHECK(hdr(a)->size == 3 * 104 + 2 * HEADER_SIZE);
}

static void test_coalesce_everything(void) {
    fresh_heap(FIT_FIRST);
    enum { N = 200 };
    void *p[N];
    for (int i = 0; i < N; i++) {
        p[i] = my_malloc(8 + (size_t)(i % 13) * 24);
        CHECK(p[i] != NULL);
    }
    // free evens first, then odds, checking invariants after every free
    for (int i = 0; i < N; i += 2) { my_free(p[i]); check_heap(); }
    for (int i = 1; i < N; i += 2) { my_free(p[i]); check_heap(); }
    heap_stats_t s = heap_stats();
    CHECK(s.total_blocks == 1);
    CHECK(s.free_blocks == 1);
    CHECK(s.used_bytes == 0);
    CHECK(s.total_free == HEAP_SIZE - HEADER_SIZE);
    CHECK(s.largest_free == HEAP_SIZE - HEADER_SIZE);

    // and in reverse order
    for (int i = 0; i < N; i++) p[i] = my_malloc(40);
    for (int i = N - 1; i >= 0; i--) my_free(p[i]);
    check_heap();
    CHECK(heap_stats().total_blocks == 1);
}

static void test_realloc_null_and_zero(void) {
    fresh_heap(FIT_FIRST);
    char *p = my_realloc(NULL, 64);   // behaves like malloc
    CHECK(p != NULL);
    CHECK(hdr(p)->size == 64 && !hdr(p)->is_free);
    check_heap();

    CHECK(my_realloc(p, 0) == NULL);  // behaves like free
    check_heap();
    CHECK(heap_stats().total_blocks == 1);
    CHECK(heap_stats().used_bytes == 0);

    CHECK(my_realloc(NULL, 0) == NULL);
}

static void test_realloc_shrink(void) {
    fresh_heap(FIT_FIRST);
    char *p = my_malloc(400);
    void *guard = my_malloc(16);
    CHECK(p && guard);
    memset(p, 0x5A, 400);
    heap_stats_t before = heap_stats();

    char *q = my_realloc(p, 100);
    CHECK(q == p);                          // in place
    CHECK(hdr(q)->size == 104);
    CHECK(all_bytes_equal(q, 0x5A, 100));   // data preserved
    check_heap();
    heap_stats_t after = heap_stats();
    CHECK(after.total_blocks == before.total_blocks + 1);   // tail was split off
    CHECK(after.used_bytes < before.used_bytes);

    // Shrinking when the right neighbour is free merges the freed tail with it.
    fresh_heap(FIT_FIRST);
    char *a = my_malloc(400);
    CHECK(a != NULL);                       // next block is the big free tail
    size_t blocks = heap_stats().total_blocks;
    char *a2 = my_realloc(a, 100);
    CHECK(a2 == a);
    CHECK(heap_stats().total_blocks == blocks);   // still [a][one free block]
    check_heap();
}

static void test_realloc_grow_in_place(void) {
    fresh_heap(FIT_FIRST);
    // (1) next block is the free tail
    char *a = my_malloc(100);
    CHECK(a != NULL);
    memset(a, 0x11, 100);
    char *a2 = my_realloc(a, 5000);
    CHECK(a2 == a);
    CHECK(hdr(a2)->size == 5000);
    CHECK(all_bytes_equal(a2, 0x11, 100));
    check_heap();

    // (2) next block is a small freed hole
    fresh_heap(FIT_FIRST);
    char *x = my_malloc(100);
    char *y = my_malloc(100);
    void *guard = my_malloc(100);
    CHECK(x && y && guard);
    memset(x, 0x22, 100);
    my_free(y);
    char *x2 = my_realloc(x, 200);    // 104 + 32 + 104 = 240 >= 200
    CHECK(x2 == x);
    CHECK(hdr(x2)->size == 200);      // 240 absorbed, surplus 40 = header + 8 is split back off
    CHECK(all_bytes_equal(x2, 0x22, 100));
    check_heap();
}

static void test_realloc_grow_by_move(void) {
    fresh_heap(FIT_FIRST);
    char *a = my_malloc(100);
    char *b = my_malloc(100);   // blocks a from growing in place
    CHECK(a && b);
    for (int i = 0; i < 100; i++) a[i] = (char)i;
    char *a2 = my_realloc(a, 1000);
    CHECK(a2 != NULL && a2 != a);
    for (int i = 0; i < 100; i++) CHECK(a2[i] == (char)i);   // data copied
    CHECK(hdr(a)->is_free);                                  // old block freed
    check_heap();
    CHECK(hdr(b)->is_free == 0);

    // old block is reusable
    CHECK(my_malloc(100) == a);
    check_heap();
}

static void test_realloc_failure_keeps_original(void) {
    fresh_heap(FIT_FIRST);
    char *p = my_malloc(100);
    char *guard = my_malloc(100);
    CHECK(p && guard);
    memset(p, 0x77, 100);

    // (1) request larger than the whole heap
    CHECK(my_realloc(p, HEAP_SIZE * 2) == NULL);
    CHECK(my_realloc(p, SIZE_MAX) == NULL);
    // (2) fits the heap size in principle, but no free block is big enough
    CHECK(my_realloc(p, HEAP_SIZE - 64) == NULL);
    // (3) heap completely full: fill the tail, then try to grow p by moving
    size_t room = heap_stats().largest_free;
    void *filler = my_malloc(room);
    CHECK(filler != NULL);
    CHECK(my_realloc(p, 200) == NULL);

    CHECK(!hdr(p)->is_free);
    CHECK(hdr(p)->size == 104);
    CHECK(all_bytes_equal(p, 0x77, 100));
    check_heap();

    // realloc on a bogus pointer fails without touching anything
    heap_stats_t before = heap_stats();
    int local;
    CHECK(my_realloc(&local, 16) == NULL);
    CHECK(same_stats(before, heap_stats()));
    check_heap();
}

static void test_calloc(void) {
    fresh_heap(FIT_FIRST);
    // Dirty a block, free it, then calloc must hand back zeros even though the
    // same bytes are reused.
    unsigned char *d = my_malloc(256);
    CHECK(d != NULL);
    memset(d, 0xAB, 256);
    my_free(d);
    unsigned char *z = my_calloc(16, 16);
    CHECK(z == d);
    CHECK(all_bytes_equal(z, 0, 256));
    check_heap();

    // overflow -> NULL and the heap is untouched
    heap_stats_t before = heap_stats();
    CHECK(my_calloc(SIZE_MAX / 2 + 1, 2) == NULL);
    CHECK(my_calloc(SIZE_MAX, SIZE_MAX) == NULL);
    CHECK(my_calloc(2, SIZE_MAX / 2 + 1) == NULL);
    CHECK(same_stats(before, heap_stats()));
    // no overflow but too big for the heap
    CHECK(my_calloc(HEAP_SIZE, 2) == NULL);
    // zero-sized request
    CHECK(my_calloc(0, 10) == NULL);
    CHECK(my_calloc(10, 0) == NULL);
    check_heap();
}

static void test_alignment(void) {
    fresh_heap(FIT_FIRST);
    CHECK((uintptr_t)heap % 8 == 0);
    CHECK(HEADER_SIZE % 8 == 0);

    void *p[300];
    int n = 0;
    for (size_t sz = 1; sz <= 100; sz++) {
        p[n] = my_malloc(sz);
        CHECK(p[n] != NULL);
        CHECK((uintptr_t)p[n] % 8 == 0);
        CHECK(hdr(p[n])->size % 8 == 0);
        CHECK(hdr(p[n])->size >= sz && hdr(p[n])->size < sz + 8);
        n++;
    }
    size_t odd[] = {127, 255, 1001, 4097, 12345};
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        p[n] = my_malloc(odd[i]);
        CHECK(p[n] != NULL && (uintptr_t)p[n] % 8 == 0);
        n++;
    }
    check_heap();
    CHECK(hdr(my_malloc(1))->size == 8);
    CHECK(hdr(my_malloc(9))->size == 16);
    // alignment also holds after frees, coalescing, splitting and realloc
    for (int i = 0; i < n; i += 3) my_free(p[i]);
    void *r = my_realloc(p[1], 333);
    CHECK(r != NULL && (uintptr_t)r % 8 == 0);
    for (size_t sz = 1; sz < 60; sz += 7) {
        void *q = my_malloc(sz);
        CHECK(q != NULL && (uintptr_t)q % 8 == 0);
    }
    check_heap();
}

static void test_out_of_memory(void) {
    fresh_heap(FIT_FIRST);
    CHECK(my_malloc(HEAP_SIZE) == NULL);          // header does not fit
    CHECK(my_malloc(HEAP_SIZE + 1) == NULL);
    CHECK(my_malloc(SIZE_MAX) == NULL);
    CHECK(my_malloc(SIZE_MAX - 3) == NULL);       // would overflow align_up
    check_heap();
    CHECK(heap_stats().total_blocks == 1);

    void *all = my_malloc(HEAP_SIZE - HEADER_SIZE);   // exactly everything
    CHECK(all != NULL);
    CHECK(heap_stats().free_blocks == 0);
    CHECK(my_malloc(1) == NULL);
    CHECK(my_malloc(8) == NULL);
    check_heap();

    my_free(all);
    CHECK(heap_stats().total_blocks == 1);
    CHECK(my_malloc(1) != NULL);                  // usable again

    // fill the heap with small blocks until it runs dry
    fresh_heap(FIT_FIRST);
    size_t count = 0;
    while (my_malloc(1000) != NULL) count++;
    CHECK(count > 900);   // ~1 MB / 1032 bytes
    check_heap();
}

static void test_invalid_and_double_free(void) {
    fresh_heap(FIT_FIRST);
    char *a = my_malloc(100);
    char *b = my_malloc(100);
    char *c = my_malloc(100);
    CHECK(a && b && c);
    memset(b, 0xFF, 100);
    heap_stats_t base = heap_stats();

    my_free(NULL);
    CHECK(same_stats(base, heap_stats()));

    // pointers outside the heap
    int local = 0;
    static int global_var = 0;
    my_free(&local);
    my_free(&global_var);
    my_free((void *)(uintptr_t)8);
    my_free((uint8_t *)heap - 8);
    my_free((uint8_t *)heap + HEAP_SIZE);
    my_free((uint8_t *)heap + HEAP_SIZE + 4096);
    // inside the heap but not a payload start: misaligned, interior, header
    my_free(b + 1);
    my_free(b + 8);
    my_free(b + 50);
    my_free((uint8_t *)heap);
    CHECK(same_stats(base, heap_stats()));
    check_heap();

    // double free
    my_free(b);
    heap_stats_t once = heap_stats();
    my_free(b);
    CHECK(same_stats(once, heap_stats()));
    check_heap();

    // double free after b was merged into a (its header is dead)
    my_free(a);          // a absorbs b via forward merge; b's header is dead
    heap_stats_t twice = heap_stats();
    my_free(a);
    my_free(b);
    CHECK(same_stats(twice, heap_stats()));
    check_heap();

    // free of a pointer that was already merged backwards
    fresh_heap(FIT_FIRST);
    char *x = my_malloc(64);
    char *y = my_malloc(64);
    char *z = my_malloc(64);
    CHECK(x && y && z);
    my_free(x);
    my_free(y);          // y merges into x; y's header is dead
    heap_stats_t s = heap_stats();
    my_free(y);
    CHECK(same_stats(s, heap_stats()));
    check_heap();

    // realloc on an already-freed pointer must not resurrect it
    fresh_heap(FIT_FIRST);
    char *f1 = my_malloc(64);
    char *f2 = my_malloc(64);
    CHECK(f1 && f2);
    my_free(f1);
    heap_stats_t fs = heap_stats();
    CHECK(my_realloc(f1, 128) == NULL);
    CHECK(my_realloc(f1, 0) == NULL);
    CHECK(same_stats(fs, heap_stats()));
    check_heap();

    // malloc still works after all the abuse
    CHECK(my_malloc(64) != NULL);
    check_heap();
}

static void test_fit_policies(void) {
    // Layout: [A 300][g][B 100][g][C 200][g][tail]; free A, B, C -> three
    // holes of 304, 104 and 200 bytes in address order.
    for (int pass = 0; pass < 2; pass++) {
        fit_policy_t p = pass == 0 ? FIT_FIRST : FIT_BEST;
        fresh_heap(p);
        void *A = my_malloc(300); void *g1 = my_malloc(8);
        void *B = my_malloc(100); void *g2 = my_malloc(8);
        void *C = my_malloc(200); void *g3 = my_malloc(8);
        CHECK(A && B && C && g1 && g2 && g3);
        my_free(A); my_free(B); my_free(C);
        check_heap();

        void *r = my_malloc(90);   // fits all three holes
        if (p == FIT_FIRST) CHECK(r == A);   // lowest address
        else                CHECK(r == B);   // smallest hole (104)
        check_heap();
    }

    // Best-fit picks the tightest hole for a mid-sized request...
    fresh_heap(FIT_BEST);
    {
        void *A = my_malloc(300); void *g1 = my_malloc(8);
        void *B = my_malloc(100); void *g2 = my_malloc(8);
        void *C = my_malloc(200); void *g3 = my_malloc(8);
        CHECK(A && B && C && g1 && g2 && g3);
        my_free(A); my_free(B); my_free(C);
        CHECK(my_malloc(150) == C);        // 200 is the smallest hole >= 150
        CHECK(my_malloc(104) == B);        // exact fit
        check_heap();
    }
    // ...and first-fit takes the first one that fits, even if it wastes more.
    fresh_heap(FIT_FIRST);
    {
        void *A = my_malloc(300); void *g1 = my_malloc(8);
        void *B = my_malloc(100); void *g2 = my_malloc(8);
        void *C = my_malloc(200); void *g3 = my_malloc(8);
        CHECK(A && B && C && g1 && g2 && g3);
        my_free(A); my_free(B); my_free(C);
        CHECK(my_malloc(150) == A);
        check_heap();
    }

    // Policy can be switched at runtime on a live heap; bad values are ignored.
    fresh_heap(FIT_FIRST);
    {
        void *A = my_malloc(300); void *g1 = my_malloc(8);
        void *B = my_malloc(100); void *g2 = my_malloc(8);
        CHECK(A && B && g1 && g2);
        my_free(A); my_free(B);
        set_fit_policy(FIT_BEST);
        set_fit_policy((fit_policy_t)99);   // ignored
        CHECK(my_malloc(90) == B);
        check_heap();
    }
}

static void test_stats_and_visualizer(void) {
    fresh_heap(FIT_FIRST);
    heap_stats_t s = heap_stats();
    CHECK(s.total_blocks == 1 && s.free_blocks == 1 && s.used_bytes == 0);
    CHECK(s.total_free == HEAP_SIZE - HEADER_SIZE);
    CHECK(s.largest_free == s.total_free);

    void *a = my_malloc(100);
    void *b = my_malloc(200);
    void *c = my_malloc(50);
    CHECK(a && b && c);
    my_free(b);
    s = heap_stats();
    CHECK(s.total_blocks == 4);
    CHECK(s.free_blocks == 2);
    CHECK(s.used_bytes == 104 + 56);
    CHECK(s.total_free == 200 + (HEAP_SIZE - 4 * HEADER_SIZE - 104 - 200 - 56));
    CHECK(s.largest_free > 200);
    // used + free + headers == heap
    CHECK(s.used_bytes + s.total_free + s.total_blocks * HEADER_SIZE == HEAP_SIZE);

    printf("       (print_heap output for this state:)\n");
    print_heap();
}

// ---------------------------------------------------------------------------

typedef void (*test_fn)(void);
static const struct { const char *name; test_fn fn; } tests[] = {
    {"basic allocations",               test_basic_allocations},
    {"data written is read back intact", test_data_integrity},
    {"free then reuse a hole",          test_free_and_reuse_hole},
    {"block splitting",                 test_splitting},
    {"forward coalescing",              test_forward_coalescing},
    {"backward coalescing",             test_backward_coalescing},
    {"coalescing on both sides",        test_coalescing_both_sides},
    {"free everything -> one block",    test_coalesce_everything},
    {"realloc NULL and size 0",         test_realloc_null_and_zero},
    {"realloc shrink in place",         test_realloc_shrink},
    {"realloc grow in place",           test_realloc_grow_in_place},
    {"realloc grow by move",            test_realloc_grow_by_move},
    {"realloc failure keeps original",  test_realloc_failure_keeps_original},
    {"calloc zeroing and overflow",     test_calloc},
    {"8-byte alignment",                test_alignment},
    {"out of memory returns NULL",      test_out_of_memory},
    {"invalid pointer / double free",   test_invalid_and_double_free},
    {"first-fit vs best-fit policies",  test_fit_policies},
    {"heap_stats and print_heap",       test_stats_and_visualizer},
};

int main(void) {
    size_t n = sizeof tests / sizeof tests[0];
    for (size_t i = 0; i < n; i++) {
        current_test = tests[i].name;
        tests[i].fn();
        printf("[PASS] %s\n", tests[i].name);
    }
    printf("\nAll %zu tests passed.\n", n);
    return 0;
}
