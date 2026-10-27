CC      ?= gcc
BUILD   := build
WARN    := -Wall -Wextra -Werror -std=c11
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test bench clean

# make: build the allocator object, the sanitized test binary and the benchmark
all: $(BUILD)/allocator.o $(BUILD)/test $(BUILD)/compare

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/allocator.o: src/allocator.c src/allocator.h | $(BUILD)
	$(CC) $(WARN) -O2 -c src/allocator.c -o $@

# The test includes allocator.c directly (white-box), so it depends on it.
$(BUILD)/test: test/test.c src/allocator.c src/allocator.h | $(BUILD)
	$(CC) $(WARN) $(SAN) -g -O1 test/test.c -o $@

$(BUILD)/compare: bench/compare.c $(BUILD)/allocator.o src/allocator.h | $(BUILD)
	$(CC) $(WARN) -O2 bench/compare.c $(BUILD)/allocator.o -o $@

# make test: build with -Wall -Wextra -Werror -std=c11 plus ASan/UBSan, then run
test: $(BUILD)/test
	./$(BUILD)/test

# make bench: build and run the first-fit vs best-fit comparison
bench: $(BUILD)/compare
	./$(BUILD)/compare

clean:
	rm -rf $(BUILD)
