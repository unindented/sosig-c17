#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/arena.h"

/**
 * @brief Reports whether a pointer meets the maximum fundamental alignment.
 *
 * @param ptr Pointer to inspect. Must not be `NULL`.
 * @return `true` when `ptr` is maximally aligned, or `false` otherwise.
 */
static bool is_max_aligned(const void* ptr) {
  return ((uintptr_t)ptr % _Alignof(max_align_t)) == 0;
}

// Small allocations go into one shared chunk and stay distinct and aligned. The allocator rounds
// each 13-byte request up to 16 bytes. 512 such requests fill `ARENA_CHUNK_CAPACITY_MIN` (8192,
// defined only inside `src/core/arena.c`) exactly, so no second chunk is needed. Changing
// `ARENA_CHUNK_CAPACITY_MIN` requires updating `ALLOCATION_COUNT`.
// `test_large_allocation_spans_own_chunk` covers chunk chaining.
static void test_small_allocations_are_distinct_and_aligned(void) {
  struct Arena arena;
  arena_init(&arena);

  enum { ALLOCATION_COUNT = 512 };
  char* slots[ALLOCATION_COUNT];
  for (size_t i = 0; i < ALLOCATION_COUNT; i++) {
    slots[i] = arena_alloc(&arena, 13);
    TEST_ASSERT(slots[i] != NULL);
    TEST_CHECK(is_max_aligned(slots[i]));
    memset(slots[i], (int)(i & 0xFF), 13);
  }

  // Every earlier write must survive later allocations without clobbering.
  for (size_t i = 0; i < ALLOCATION_COUNT; i++) {
    for (size_t j = 0; j < 13; j++) {
      TEST_CHECK((unsigned char)slots[i][j] == (i & 0xFF));
    }
  }

  arena_free(&arena);
}

// A zero-byte request yields a distinct, usable, non-`NULL` pointer.
static void test_zero_byte_allocation_is_distinct(void) {
  struct Arena arena;
  arena_init(&arena);

  void* first = arena_alloc(&arena, 0);
  void* second = arena_alloc(&arena, 0);
  TEST_CHECK(first != NULL);
  TEST_CHECK(second != NULL);
  TEST_CHECK(first != second);

  arena_free(&arena);
}

// An allocation larger than a chunk succeeds and stays aligned and writable.
static void test_large_allocation_spans_own_chunk(void) {
  struct Arena arena;
  arena_init(&arena);

  const size_t large = 64 * 1024;
  unsigned char* block = arena_alloc(&arena, large);
  TEST_ASSERT(block != NULL);
  TEST_CHECK(is_max_aligned(block));
  memset(block, 0xAB, large);
  TEST_CHECK(block[0] == 0xAB);
  TEST_CHECK(block[large - 1] == 0xAB);

  // The allocator remains usable after an oversized request.
  char* tail = arena_alloc(&arena, 8);
  TEST_CHECK(tail != NULL);
  TEST_CHECK(is_max_aligned(tail));

  arena_free(&arena);
}

// Interleaving small and large allocations preserves each region. The arena splices an oversized
// chunk in *behind* the current head rather than making it the new head. That splice keeps the
// allocation after an oversized one adjacent to the one before it, because the head keeps its
// remaining space. The build relies on this. A rendered body routinely exceeds
// `ARENA_CHUNK_CAPACITY_MIN` and `arena_strdup` duplicates it between small allocations from the
// same arena. Losing that behavior would cost a `malloc` and most of a chunk per content entry.
static void test_interleaved_allocations_do_not_clobber(void) {
  struct Arena arena;
  arena_init(&arena);

  char* before = arena_strdup(&arena, "before");
  unsigned char* big = arena_alloc(&arena, 32 * 1024);
  char* after = arena_strdup(&arena, "after");
  TEST_ASSERT(before != NULL && big != NULL && after != NULL);
  memset(big, 0x5A, 32 * 1024);

  TEST_CHECK(strcmp(before, "before") == 0);
  TEST_CHECK(strcmp(after, "after") == 0);
  TEST_CHECK(big[0] == 0x5A && big[32 * 1024 - 1] == 0x5A);

  // Straddle the oversized request with two equal small allocations. Check that the second lands
  // one alignment unit past the first. That spacing is only possible when both came from the same
  // head chunk. Surviving content does not prove this: content survives under any chunk
  // arrangement.
  char* head_first = arena_alloc(&arena, 8);
  TEST_ASSERT(head_first != NULL);
  unsigned char* spliced = arena_alloc(&arena, 32 * 1024);
  TEST_ASSERT(spliced != NULL);
  char* head_second = arena_alloc(&arena, 8);
  TEST_CHECK(head_second == head_first + _Alignof(max_align_t));

  arena_free(&arena);
}

// `calloc` zeroes its storage. `strndup` copies a bounded slice and terminates it.
static void test_calloc_zeroes_and_strndup_copies(void) {
  struct Arena arena;
  arena_init(&arena);

  unsigned char* zeros = arena_calloc(&arena, 100, sizeof(*zeros));
  TEST_ASSERT(zeros != NULL);
  for (size_t i = 0; i < 100; i++) {
    TEST_CHECK(zeros[i] == 0);
  }

  // A zero element size must not reach the overflow guard's division. `count * 0` cannot overflow,
  // so the guard's `size != 0` arm skips the division. A guard spelled `size == 0` instead would
  // divide by zero. The result follows `arena_alloc`'s zero-byte rule: distinct and usable.
  void* no_elements = arena_calloc(&arena, 4, 0);
  TEST_CHECK(no_elements != NULL);

  char* prefix = arena_strndup(&arena, "hello world", 5);
  TEST_ASSERT(prefix != NULL);
  TEST_CHECK(strcmp(prefix, "hello") == 0);

  // One byte is the shortest copy that must actually happen. It also separates a `str_len > 0`
  // guard from a `str_len > 1` spelling: the wider guard skips the `memcpy` and terminates a
  // buffer whose single byte it never wrote.
  char* single = arena_strndup(&arena, "x", 1);
  TEST_ASSERT(single != NULL);
  TEST_CHECK(strcmp(single, "x") == 0);

  // This is the documented empty-range form. `memcpy` requires valid pointers even for a zero
  // length, so the `str_len > 0` guard in `arena_strndup` is the only thing that keeps this call
  // defined. This call is the only one that reaches the guard's skipping arm.
  char* empty = arena_strndup(&arena, NULL, 0);
  TEST_ASSERT(empty != NULL);
  TEST_CHECK(empty[0] == '\0');

  arena_free(&arena);
}

// Reusing an arena after `arena_free` starts from an empty chunk list.
static void test_reuse_after_free_starts_empty(void) {
  struct Arena arena;
  arena_init(&arena);
  TEST_CHECK(arena_strdup(&arena, "first") != NULL);
  arena_free(&arena);

  char* second = arena_strdup(&arena, "second");
  TEST_CHECK(second != NULL);
  TEST_CHECK(strcmp(second, "second") == 0);
  arena_free(&arena);
}

// The arena rejects requests that overflow size arithmetic with `NULL`, at both guards. The
// alignment-rounding guard stops `SIZE_MAX` before it reaches the chunk-header guard, so the second
// request below is the largest that clears the first. The header guard must catch it. Without that
// guard, `malloc(sizeof(struct ArenaChunk) + capacity)` wraps to a tiny block and the chunk header
// overruns it.
static void test_overflow_requests_are_rejected(void) {
  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(arena_alloc(&arena, SIZE_MAX) == NULL);
  TEST_CHECK(arena_alloc(&arena, SIZE_MAX - (_Alignof(max_align_t) - 1)) == NULL);
  TEST_CHECK(arena_calloc(&arena, SIZE_MAX, 2) == NULL);
  // `arena_calloc`'s own multiply check must catch a product that wraps to a *small* value. The
  // case above does not reach it. `SIZE_MAX * 2` wraps to a value still near the top of the range,
  // and `arena_alloc`'s rounding guard rejects it downstream, so that assertion passes even without
  // the multiply check. This pair wraps to exactly `0`. `arena_alloc` answers that with a usable
  // one-byte allocation the caller then overruns by `count` elements.
  TEST_CHECK(arena_calloc(&arena, (SIZE_MAX / 2) + 1, 2) == NULL);
  TEST_CHECK(arena_strndup(&arena, "", SIZE_MAX) == NULL);

  arena_free(&arena);
}

TEST_LIST = {
    {"small allocations are distinct and aligned", test_small_allocations_are_distinct_and_aligned},
    {"zero byte allocation is distinct", test_zero_byte_allocation_is_distinct},
    {"large allocation spans own chunk", test_large_allocation_spans_own_chunk},
    {"interleaved allocations do not clobber", test_interleaved_allocations_do_not_clobber},
    {"calloc zeroes and strndup copies", test_calloc_zeroes_and_strndup_copies},
    {"reuse after free starts empty", test_reuse_after_free_starts_empty},
    {"overflow requests are rejected", test_overflow_requests_are_rejected},
    {NULL, NULL},
};
