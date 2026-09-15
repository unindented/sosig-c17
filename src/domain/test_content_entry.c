#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "domain/content_entry.h"

// `description` is the one field that defaults to `""` rather than `NULL`, so a template can emit
// it without a presence check. Everything else starts empty.
static void test_init_sets_defaults(void) {
  struct ContentEntry entry;
  content_entry_init(&entry);

  TEST_CHECK(entry.source_path == NULL);
  TEST_CHECK(entry.title == NULL);
  TEST_CHECK(entry.date == NULL);
  TEST_CHECK(entry.date_epoch == 0);
  TEST_ASSERT(entry.description != NULL);
  TEST_CHECK(strcmp(entry.description, "") == 0);
  TEST_CHECK(entry.slug == NULL);
  TEST_CHECK(entry.tags == NULL);
  TEST_CHECK(entry.tag_count == 0);
  TEST_CHECK(entry.is_draft == false);
  TEST_CHECK(entry.template == NULL);
  TEST_CHECK(entry.body_html == NULL);
  TEST_CHECK(entry.url_path == NULL);
  TEST_CHECK(entry.output_path == NULL);

  content_entry_free(&entry);
}

// Freeing releases arena-owned data and leaves the entry *initialized*, not merely zeroed. That is
// the contract at `content_entry.h` that lets a caller reuse it without a second
// `content_entry_init`. `description` is what separates the two: it is the one field whose default
// is non-zero, so asserting it is the only way a zeroing implementation fails here. The arena
// append after the free exercises the reuse the header promises, with no intervening init.
static void test_free_clears_fields(void) {
  struct ContentEntry entry;
  content_entry_init(&entry);

  entry.title = arena_strdup(&entry.arena, "Example");
  entry.slug = arena_strdup(&entry.arena, "example");
  entry.tag_count = 3;
  entry.is_draft = true;
  TEST_ASSERT(entry.title != NULL);

  content_entry_free(&entry);

  TEST_CHECK(entry.title == NULL);
  TEST_CHECK(entry.slug == NULL);
  TEST_CHECK(entry.tag_count == 0);
  TEST_CHECK(entry.is_draft == false);
  TEST_ASSERT(entry.description != NULL);
  TEST_CHECK(strcmp(entry.description, "") == 0);

  char* reused = arena_strdup(&entry.arena, "reused");
  TEST_ASSERT(reused != NULL);
  TEST_CHECK(strcmp(reused, "reused") == 0);

  content_entry_free(&entry);
}

// Re-initializing after a free yields a usable arena again.
static void test_reinit_after_free_is_safe(void) {
  struct ContentEntry entry;
  content_entry_init(&entry);
  content_entry_free(&entry);

  content_entry_init(&entry);
  char* reused = arena_strdup(&entry.arena, "reused");
  TEST_ASSERT(reused != NULL);
  TEST_CHECK(strcmp(reused, "reused") == 0);

  content_entry_free(&entry);
}

// Only `date_epoch` and output path drive ordering, so the sort fixtures below are built as bare
// structs.

// Sorting orders entries newest-first by `date_epoch`, breaking ties by output path ascending. The
// slugs deliberately imply the opposite tie order so they cannot influence the result.
static void test_sort_orders_by_date_then_output_path(void) {
  struct ContentEntry older = {.date_epoch = 100, .output_path = "public/o.html"};
  struct ContentEntry newer = {.date_epoch = 300, .output_path = "public/n.html"};
  struct ContentEntry tie_bravo = {
      .date_epoch = 200, .slug = "bravo", .output_path = "public/a.html"};
  struct ContentEntry tie_alpha = {
      .date_epoch = 200, .slug = "alpha", .output_path = "public/b.html"};

  struct ContentEntry* entries[] = {&older, &tie_bravo, &newer, &tie_alpha};
  content_entry_sort(entries, 4);

  TEST_CHECK(entries[0] == &newer);
  TEST_CHECK(entries[1] == &tie_bravo);  // epoch 200, "public/a.html" < "public/b.html"
  TEST_CHECK(entries[2] == &tie_alpha);
  TEST_CHECK(entries[3] == &older);
}

// Equal-date entries have the same result regardless of input order, so generated output does not
// depend on how `qsort` happens to arrange equal-comparing elements.
static void test_sort_output_path_tie_break_is_deterministic(void) {
  struct ContentEntry in_b = {.date_epoch = 200, .output_path = "public/b/post.html"};
  struct ContentEntry in_a = {.date_epoch = 200, .output_path = "public/a/post.html"};

  struct ContentEntry* forward[] = {&in_b, &in_a};
  content_entry_sort(forward, 2);
  TEST_CHECK(forward[0] == &in_a);
  TEST_CHECK(forward[1] == &in_b);

  // The same result regardless of the order the entries arrived in.
  struct ContentEntry* reversed[] = {&in_a, &in_b};
  content_entry_sort(reversed, 2);
  TEST_CHECK(reversed[0] == &in_a);
  TEST_CHECK(reversed[1] == &in_b);
}

// Sorting an empty or single-element array is a no-op that does not read out of bounds.
static void test_sort_handles_empty_and_single(void) {
  content_entry_sort(NULL, 0);  // count 0: the array pointer must not be dereferenced

  struct ContentEntry only = {.date_epoch = 42, .slug = "only", .output_path = "public/only.html"};
  struct ContentEntry* one[] = {&only};
  content_entry_sort(one, 1);
  TEST_CHECK(one[0] == &only);
}

// The latest date is the newest entry's own `date` pointer, not a copy, and the empty set falls
// back to the Unix epoch. Neither is reachable from the golden test suite, whose fixture sites
// always have entries.
// Without this test, `LATEST_DATE_FALLBACK` could change without a failure. An invalid `<updated>`
// value would then appear in every feed for an empty site. Both expected values came from a run.
static void test_latest_date_uses_newest_or_epoch(void) {
  TEST_CHECK(strcmp(content_entry_latest_date(NULL, 0), "1970-01-01T00:00:00Z") == 0);

  struct ContentEntry newer = {.date = "2026-07-01T00:00:00Z"};
  struct ContentEntry older = {.date = "2020-01-01T00:00:00Z"};
  const struct ContentEntry* entries[] = {&newer, &older};
  const char* updated = content_entry_latest_date(entries, 2);
  TEST_CHECK(strcmp(updated, "2026-07-01T00:00:00Z") == 0);
  // The entry's own string, borrowed rather than copied: every render holds it for the build.
  TEST_CHECK(updated == newer.date);

  // A single entry, which is the boundary between the two cases above and the shape a new site has
  // on its first post. A count guard of `> 1` instead of `> 0` still satisfies both assertions
  // above while handing that site the 1970 fallback as its feed's `<updated>`.
  const struct ContentEntry* only[] = {&newer};
  TEST_CHECK(content_entry_latest_date(only, 1) == newer.date);
}

TEST_LIST = {
    {"init sets defaults", test_init_sets_defaults},
    {"free clears fields", test_free_clears_fields},
    {"reinit after free is safe", test_reinit_after_free_is_safe},
    {"sort orders by date then output path", test_sort_orders_by_date_then_output_path},
    {"sort output path tie break is deterministic",
     test_sort_output_path_tie_break_is_deterministic},
    {"sort handles empty and single", test_sort_handles_empty_and_single},
    {"latest date uses newest or epoch", test_latest_date_uses_newest_or_epoch},
    {NULL, NULL},
};
