// Fragmentation comparison: first-fit vs best-fit.
//
// Each workload is deterministic: srand(SEED) is called before every run, so
// both policies see exactly the same sequence of random numbers. (The numbers
// come from the C library's rand(), so they can differ between libc
// implementations; they are identical from run to run on one machine.)
//
// Everything printed is measured from the live allocator via heap_stats().
//
// Metrics (all taken at the END of the workload):
//   utilization = used_bytes / HEAP_SIZE          (payload bytes in use)
//   ext. frag   = 1 - largest_free / total_free   (0 when no free memory)
//   failed      = my_malloc calls that returned NULL during the workload

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/allocator.h"

#define SEED 42u

typedef struct {
    heap_stats_t stats;
    size_t failed;       // allocation failures during the whole workload
    size_t attempts;     // allocation attempts during the whole workload
} result_t;

static size_t rand_range(size_t lo, size_t hi) {   // inclusive
    return lo + (size_t)rand() % (hi - lo + 1);
}

// --- (a) random mixed sizes, random frees ---------------------------------
// 3500 slots. 60000 steps: pick a slot; if it holds a block free it, else
// allocate. Sizes: 70% small (8-128), 25% medium (129-1024), 5% large (1025-8192).
static result_t workload_random(void) {
    enum { SLOTS = 3500, STEPS = 60000 };
    static void *slot[SLOTS];
    memset(slot, 0, sizeof slot);
    result_t r = {0};

    for (int i = 0; i < STEPS; i++) {
        size_t k = (size_t)rand() % SLOTS;
        if (slot[k] != NULL) {
            my_free(slot[k]);
            slot[k] = NULL;
            continue;
        }
        int roll = rand() % 100;
        size_t size = roll < 70 ? rand_range(8, 128)
                    : roll < 95 ? rand_range(129, 1024)
                                : rand_range(1025, 8192);
        r.attempts++;
        slot[k] = my_malloc(size);
        if (slot[k] == NULL) r.failed++;
    }
    r.stats = heap_stats();
    return r;
}

// --- (b) many small, free every other one, then allocate larger ones ------
// 5000 small blocks of random size 16-256; free every other (leaving 2500
// holes of mixed sizes); then try to allocate 3000 larger blocks of random
// size 128-512 and keep them. Some of these fit in a hole, some do not.
static result_t workload_holes(void) {
    enum { SMALL = 5000, LARGE = 3000 };
    static void *small[SMALL];
    result_t r = {0};

    for (int i = 0; i < SMALL; i++) {
        r.attempts++;
        small[i] = my_malloc(rand_range(16, 256));
        if (small[i] == NULL) r.failed++;
    }
    for (int i = 0; i < SMALL; i += 2) my_free(small[i]);   // swiss cheese
    for (int i = 0; i < LARGE; i++) {
        r.attempts++;
        if (my_malloc(rand_range(128, 512)) == NULL) r.failed++;
    }
    r.stats = heap_stats();
    return r;
}

// --- (c) steady state: constant number of live blocks, replaced FIFO ------
// 1000 live blocks in a ring. Each step frees the oldest block and allocates a
// new one of a random size class (32, 64, 128, 256, 512, 1024, 2048).
static result_t workload_steady(void) {
    enum { LIVE = 1000, STEPS = 60000 };
    static const size_t classes[] = {32, 64, 128, 256, 512, 1024, 2048};
    static void *ring[LIVE];
    memset(ring, 0, sizeof ring);
    result_t r = {0};

    for (int i = 0; i < STEPS; i++) {
        int k = i % LIVE;
        if (ring[k] != NULL) my_free(ring[k]);
        r.attempts++;
        ring[k] = my_malloc(classes[(size_t)rand() % (sizeof classes / sizeof classes[0])]);
        if (ring[k] == NULL) r.failed++;
    }
    r.stats = heap_stats();
    return r;
}

typedef result_t (*workload_fn)(void);

static void run(const char *name, workload_fn fn) {
    static const struct { fit_policy_t p; const char *label; } policies[] = {
        {FIT_FIRST, "first-fit"}, {FIT_BEST, "best-fit"},
    };

    printf("\nWorkload: %s\n", name);
    printf("%-10s | %11s | %9s | %11s | %12s | %11s | %8s\n", "policy",
           "utilization", "ext. frag", "free blocks", "largest free", "total free",
           "failed");
    printf("-----------+-------------+-----------+-------------+--------------+"
           "-------------+---------\n");
    for (size_t i = 0; i < 2; i++) {
        srand(SEED);
        heap_init();
        set_fit_policy(policies[i].p);
        result_t r = fn();
        double util = (double)r.stats.used_bytes / HEAP_SIZE * 100.0;
        double frag = r.stats.total_free == 0
                          ? 0.0
                          : 1.0 - (double)r.stats.largest_free / (double)r.stats.total_free;
        printf("%-10s | %10.2f%% | %8.2f%% | %11zu | %12zu | %11zu | %3zu/%-4zu\n",
               policies[i].label, util, frag * 100.0, r.stats.free_blocks,
               r.stats.largest_free, r.stats.total_free, r.failed, r.attempts);
    }
}

int main(void) {
    printf("Heap size: %d bytes, seed: %u\n", HEAP_SIZE, SEED);
    printf("utilization = used payload bytes / heap size; "
           "ext. frag = 1 - largest_free / total_free\n");
    printf("failed = allocation failures / allocation attempts\n");

    run("(a) random mixed sizes, random frees", workload_random);
    run("(b) small blocks, free every other, then larger allocations", workload_holes);
    run("(c) steady state: FIFO replacement of 1000 live blocks", workload_steady);
    return 0;
}
