#include <acutest.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/string_buffer.h"

// A reserved-but-not-yet-appended buffer is already `NUL`-terminated at `len`, so it reads and
// steals as a valid (empty) C string. Guards the reserve terminator invariant.
static void test_reserve_terminates_before_append(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);

  TEST_ASSERT(string_buffer_reserve(&buffer, 8) == 0);
  TEST_ASSERT(buffer.data != NULL);
  TEST_CHECK(buffer.len == 0);
  TEST_CHECK(buffer.data[buffer.len] == '\0');
  TEST_CHECK(strcmp(buffer.data, "") == 0);

  char* stolen = string_buffer_steal(&buffer);
  TEST_ASSERT(stolen != NULL);
  TEST_CHECK(strcmp(stolen, "") == 0);
  free(stolen);
}

// `string_buffer_reserve` rejects a request that overflows the size arithmetic and leaves the
// buffer untouched. Both an empty and a non-empty buffer are checked. The guard subtracts the
// current length. On an empty buffer that term is 0, so `SIZE_MAX` alone would also fail a guard
// that ignored the length entirely. The non-empty case is the one that pins the subtraction.
// Without it `needed` wraps to a small value, `reserve` reports success, and the next append tries
// to copy `SIZE_MAX - 3` bytes into the existing allocation. Neither request reaches `realloc`, so
// neither trips an allocation-size abort.
static void test_reserve_overflow_is_rejected(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);

  TEST_CHECK(string_buffer_reserve(&buffer, SIZE_MAX) == -1);
  TEST_CHECK(buffer.data == NULL);
  TEST_CHECK(buffer.len == 0);
  TEST_CHECK(buffer.capacity == 0);

  TEST_ASSERT(string_buffer_append(&buffer, "foo") == 0);
  TEST_CHECK(string_buffer_reserve(&buffer, SIZE_MAX - 3) == -1);
  TEST_CHECK(buffer.len == 3);
  TEST_CHECK(buffer.data != NULL && strcmp(buffer.data, "foo") == 0);

  string_buffer_free(&buffer);
}

// The three append variants compose into one run of bytes with a terminator at `len`, and
// `string_buffer_append_len` copies exactly its length: the `!!` past `"bar"` is never appended.
static void test_append_builds_terminated_bytes(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);

  TEST_CHECK(string_buffer_append(&buffer, "foo") == 0);
  TEST_CHECK(string_buffer_append_len(&buffer, "bar!!", 3) == 0);
  TEST_CHECK(string_buffer_append_char(&buffer, '!') == 0);

  TEST_ASSERT(buffer.data != NULL);
  TEST_CHECK(buffer.len == 7);
  TEST_CHECK(strcmp(buffer.data, "foobar!") == 0);
  TEST_CHECK(buffer.data[buffer.len] == '\0');

  string_buffer_free(&buffer);
}

// A zero-length append is a no-op that accepts a `NULL` source, which is the contract
// `string_buffer_append_len`'s length guard exists for rather than for speed. Template and Markdown
// callbacks may forward zero-length chunks, while this test directly covers the documented `NULL`
// form. Without the guard the call reaches `memcpy(dst, NULL, 0)`, which is undefined even for a
// zero length and which no sanitizer here reports.
static void test_append_len_accepts_null_when_empty(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);

  TEST_CHECK(string_buffer_append_len(&buffer, NULL, 0) == 0);
  TEST_ASSERT(buffer.data != NULL);
  TEST_CHECK(buffer.len == 0);
  TEST_CHECK(strcmp(buffer.data, "") == 0);

  // A zero-length append must also preserve existing content.
  TEST_ASSERT(string_buffer_append(&buffer, "ab") == 0);
  TEST_CHECK(string_buffer_append_len(&buffer, NULL, 0) == 0);
  TEST_CHECK(buffer.len == 2);
  TEST_CHECK(strcmp(buffer.data, "ab") == 0);

  string_buffer_free(&buffer);
}

// Growth past the initial capacity preserves earlier bytes and follows the doubling policy.
// `APPEND_COUNT` must stay above `STRING_BUFFER_CAPACITY_MIN` (256, file-local to
// `src/core/string_buffer.c` and so not reachable from here). Raising that constant means raising
// this count, or the test stops forcing a growth. The test asserts the exact final capacity rather
// than a lower bound, as `src/core/test_path_list.c` does for the sibling container. Content
// surviving is true under any growth factor, so without the exact assertion neither the seed nor
// the doubling is pinned and a buffer that grew one byte at a time would pass.
static void test_growth_preserves_contents(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);

  enum { CAPACITY_MIN_ASSUMED = 256, APPEND_COUNT = 1000 };
  for (size_t i = 0; i < APPEND_COUNT; i++) {
    TEST_ASSERT(string_buffer_append_char(&buffer, 'x') == 0);
  }

  TEST_CHECK(buffer.len == APPEND_COUNT);
  // 256 -> 512 once the terminator no longer fits, then 512 -> 1024, which covers all 1000 bytes
  // plus the terminator.
  TEST_CHECK(buffer.capacity == CAPACITY_MIN_ASSUMED * 4);
  TEST_ASSERT(buffer.data != NULL);
  for (size_t i = 0; i < APPEND_COUNT; i++) {
    TEST_CHECK(buffer.data[i] == 'x');
  }
  TEST_CHECK(buffer.data[buffer.len] == '\0');

  string_buffer_free(&buffer);
}

// Stealing hands the allocation to the caller and resets the buffer to empty.
static void test_steal_transfers_and_resets(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  TEST_ASSERT(string_buffer_append(&buffer, "owned") == 0);

  char* stolen = string_buffer_steal(&buffer);
  TEST_ASSERT(stolen != NULL);
  TEST_CHECK(strcmp(stolen, "owned") == 0);
  TEST_CHECK(buffer.data == NULL);
  TEST_CHECK(buffer.len == 0);

  free(stolen);
}

// A buffer that never grew still steals as a terminated empty string, so callers do not have to
// distinguish "nothing was appended" from an allocation failure.
static void test_steal_from_never_grown_buffer(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  TEST_ASSERT(buffer.data == NULL);

  char* stolen = string_buffer_steal(&buffer);
  TEST_ASSERT(stolen != NULL);
  TEST_CHECK(strcmp(stolen, "") == 0);
  TEST_CHECK(buffer.data == NULL);
  TEST_CHECK(buffer.len == 0);
  TEST_CHECK(buffer.capacity == 0);

  free(stolen);
}

// `string_buffer_free` leaves the buffer reusable without a second `string_buffer_init`, which is
// the contract in `core/string_buffer.h`. Three things rest on the reset: an append after the free
// rebuilds from empty rather than reallocating the freed pointer, a second free is not a double
// free, and the field state is what `string_buffer_init` would have written. The arena carries the
// same contract and pins it in `src/core/test_arena.c`.
static void test_free_allows_reuse(void) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  TEST_ASSERT(string_buffer_append(&buffer, "first") == 0);

  string_buffer_free(&buffer);
  TEST_CHECK(buffer.data == NULL);
  TEST_CHECK(buffer.len == 0);
  TEST_CHECK(buffer.capacity == 0);

  TEST_ASSERT(string_buffer_append(&buffer, "second") == 0);
  TEST_ASSERT(buffer.data != NULL);
  TEST_CHECK(strcmp(buffer.data, "second") == 0);
  TEST_CHECK(buffer.len == strlen("second"));

  string_buffer_free(&buffer);
  string_buffer_free(&buffer);
}

TEST_LIST = {
    {"reserve terminates before append", test_reserve_terminates_before_append},
    {"reserve overflow is rejected", test_reserve_overflow_is_rejected},
    {"append builds terminated bytes", test_append_builds_terminated_bytes},
    {"append len accepts null when empty", test_append_len_accepts_null_when_empty},
    {"growth preserves contents", test_growth_preserves_contents},
    {"steal transfers and resets", test_steal_transfers_and_resets},
    {"steal from never grown buffer", test_steal_from_never_grown_buffer},
    {"free allows reuse", test_free_allows_reuse},
    {NULL, NULL},
};
