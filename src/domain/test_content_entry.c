#include <acutest.h>
#include <stddef.h>

#include "domain/content_entry.h"
#include "shared/str.h"

// Only `date_epoch` and output path drive ordering, so the sort fixtures below are built as bare
// structs.

// Sorting orders entries newest-first by `date_epoch`, breaking ties by output path ascending. The
// slugs deliberately imply the opposite tie order so they cannot influence the result.
static void test_sort_orders_by_date_then_output_path(void) {
  struct ContentEntry older = {.date_epoch = 100, .output_path = STR_INIT("public/o.html")};
  struct ContentEntry newer = {.date_epoch = 300, .output_path = STR_INIT("public/n.html")};
  struct ContentEntry tie_bravo = {
      .date_epoch = 200, .slug = STR_INIT("bravo"), .output_path = STR_INIT("public/a.html")};
  struct ContentEntry tie_alpha = {
      .date_epoch = 200, .slug = STR_INIT("alpha"), .output_path = STR_INIT("public/b.html")};

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
  struct ContentEntry in_b = {.date_epoch = 200, .output_path = STR_INIT("public/b/post.html")};
  struct ContentEntry in_a = {.date_epoch = 200, .output_path = STR_INIT("public/a/post.html")};

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

  struct ContentEntry only = {
      .date_epoch = 42, .slug = STR_INIT("only"), .output_path = STR_INIT("public/only.html")};
  struct ContentEntry* one[] = {&only};
  content_entry_sort(one, 1);
  TEST_CHECK(one[0] == &only);
}

// Without `modified` values, the latest change is the newest entry's own `date` bytes, not a copy,
// and the empty set falls back to the Unix epoch. Neither is reachable from the golden test suite,
// whose fixture sites always have entries. Without this test, `MODIFIED_LATEST_FALLBACK` could
// change without a failure. An invalid `<updated>` value would then appear in every feed for an
// empty site. Both expected values came from a run.
static void test_modified_latest_uses_newest_or_epoch(void) {
  TEST_CHECK(str_eq(content_entry_modified_latest(NULL, 0), S("1970-01-01T00:00:00Z")));

  struct ContentEntry newer = {.date = STR_INIT("2026-07-01T00:00:00Z"),
                               .date_epoch = 1782864000,
                               .modified_epoch = 1782864000};
  struct ContentEntry older = {.date = STR_INIT("2020-01-01T00:00:00Z"),
                               .date_epoch = 1577836800,
                               .modified_epoch = 1577836800};
  const struct ContentEntry* entries[] = {&newer, &older};
  const struct Str modified = content_entry_modified_latest(entries, 2);
  TEST_CHECK(str_eq(modified, S("2026-07-01T00:00:00Z")));
  // The entry's own string, borrowed rather than copied: every render holds it for the build.
  TEST_CHECK(modified.data == newer.date.data);

  // A single entry, which is the boundary between the two cases above and the shape a new site has
  // on its first post. A count guard of `> 1` instead of `> 0` still satisfies both assertions
  // above while handing that site the 1970 fallback as its feed's `<updated>`.
  const struct ContentEntry* only[] = {&newer};
  TEST_CHECK(content_entry_modified_latest(only, 1).data == newer.date.data);
}

// The latest change is the newest `modified`, even on an entry that sorts after a newer `date`, and
// an entry without `modified` competes with its `date`. On a tie, the entry that sorts first wins,
// as it does for a site without `modified` values.
static void test_modified_latest_picks_newest_change(void) {
  struct ContentEntry newer = {.date = STR_INIT("2026-07-01T00:00:00Z"),
                               .date_epoch = 1782864000,
                               .modified_epoch = 1782864000};
  struct ContentEntry older = {.date = STR_INIT("2020-01-01T00:00:00Z"),
                               .date_epoch = 1577836800,
                               .modified = STR_INIT("2026-07-03T00:00:00Z"),
                               .modified_epoch = 1783036800};
  const struct ContentEntry* entries[] = {&newer, &older};
  const struct Str modified = content_entry_modified_latest(entries, 2);
  TEST_CHECK(str_eq(modified, S("2026-07-03T00:00:00Z")));
  TEST_CHECK(modified.data == older.modified.data);

  // An earlier `modified` loses to the newer entry's `date`.
  older.modified = S("2025-01-01T00:00:00Z");
  older.modified_epoch = 1735689600;
  TEST_CHECK(content_entry_modified_latest(entries, 2).data == newer.date.data);

  // The same instant on both entries resolves to the first in sorted order.
  older.modified = S("2026-07-01T00:00:00Z");
  older.modified_epoch = newer.modified_epoch;
  TEST_CHECK(content_entry_modified_latest(entries, 2).data == newer.date.data);
}

TEST_LIST = {
    {"sort orders by date then output path", test_sort_orders_by_date_then_output_path},
    {"sort output path tie break is deterministic",
     test_sort_output_path_tie_break_is_deterministic},
    {"sort handles empty and single", test_sort_handles_empty_and_single},
    {"modified latest uses newest or epoch", test_modified_latest_uses_newest_or_epoch},
    {"modified latest picks newest change", test_modified_latest_picks_newest_change},
    {NULL, NULL},
};
