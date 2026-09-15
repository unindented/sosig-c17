#include <acutest.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "domain/manifest.h"

// `manifest_free` leaves the manifest reusable without a second `manifest_init`, which is the
// contract at `manifest.h`. The test checks two parts of this contract. First, it checks the reset
// field state. Second, it checks that re-adding a previous path reports *inserted*, not duplicate.
// This result proves that the function dropped the bucket index. Without the reset the stale bucket
// table is read on the next add, which is a use-after-free. The second free would release every
// entry string twice. The sibling container with the same contract pins this in
// `test_free_allows_reuse` in `src/core/test_path_list.c`.
static void test_free_allows_reuse(void) {
  struct Manifest manifest;
  manifest_init(&manifest);
  TEST_ASSERT(manifest_add(&manifest, "public/index.html", "index.md", NULL) ==
              MANIFEST_ADD_INSERTED);

  manifest_free(&manifest);
  TEST_CHECK(manifest.count == 0);
  TEST_CHECK(manifest.entries == NULL);
  TEST_CHECK(manifest.capacity == 0);
  TEST_CHECK(manifest.buckets == NULL);
  TEST_CHECK(manifest.bucket_count == 0);

  // Inserted, not duplicate: the earlier claim on this exact path is gone.
  TEST_CHECK(manifest_add(&manifest, "public/index.html", "index.md", NULL) ==
             MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest.count == 1);

  manifest_free(&manifest);
}

// A new path is inserted. Re-adding it is reported as a duplicate that leaves the manifest
// unchanged and surfaces the earlier source label, while a distinct path is still accepted. The
// final re-add takes the documented `NULL` form of `source_label_existing_out`, which is the only
// call in the suite that reaches the `!= NULL` guard's false arm.
static void test_inserts_then_detects_duplicate(void) {
  struct Manifest manifest;
  manifest_init(&manifest);

  TEST_CHECK(manifest_add(&manifest, "public/index.html", "index.md", NULL) ==
             MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest.count == 1);

  const char* source_label_existing = NULL;
  TEST_CHECK(manifest_add(&manifest, "public/index.html", "index.html", &source_label_existing) ==
             MANIFEST_ADD_DUPLICATE);
  TEST_CHECK(source_label_existing != NULL && strcmp(source_label_existing, "index.md") == 0);
  TEST_CHECK(manifest.count == 1);

  TEST_CHECK(manifest_add(&manifest, "public/about.html", "about.md", NULL) ==
             MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest.count == 2);

  TEST_CHECK(manifest_add(&manifest, "public/index.html", "index.html", NULL) ==
             MANIFEST_ADD_DUPLICATE);
  TEST_CHECK(manifest.count == 2);

  manifest_free(&manifest);
}

// Growth past the initial entry and bucket capacities rehashes correctly. Every earlier path is
// still found through the rehashed index (it appears as a duplicate on re-add) and maps back to its
// original source label. The test reads the label captured before the growth again after it. That
// exercises the borrowed-pointer promise `manifest.h` makes: growth reallocates the entry array but
// never the strings it points at. Reading a label on the line after the call that set it, as the
// loop does, cannot distinguish that promise from a scratch-buffer API.
static void test_grows_and_rehashes(void) {
  struct Manifest manifest;
  manifest_init(&manifest);

  enum { ENTRY_COUNT = 200, ENTRIES_CAPACITY_MIN_ASSUMED = 16, BUCKETS_CAPACITY_MIN_ASSUMED = 32 };
  TEST_CHECK(manifest_add(&manifest, "public/0.html", "0.md", NULL) == MANIFEST_ADD_INSERTED);
  // The test reads both seeds on the first insert, while they are still the allocated sizes. The
  // post-growth totals below cannot pin them: by 200 entries a larger seed doubles to the same
  // total. Both constants are file-local to `src/domain/manifest.c`, so they are spelled out here.
  // Changing either must update this test.
  TEST_CHECK(manifest.capacity == ENTRIES_CAPACITY_MIN_ASSUMED);
  TEST_CHECK(manifest.bucket_count == BUCKETS_CAPACITY_MIN_ASSUMED);

  // Captured while the entry array still holds its first allocation, then read again after the loop
  // below has forced several reallocations of it and several rehashes. Capturing after the growth
  // would test nothing: a duplicate re-add never grows the array.
  const char* source_label_held = NULL;
  TEST_CHECK(manifest_add(&manifest, "public/0.html", "again.md", &source_label_held) ==
             MANIFEST_ADD_DUPLICATE);
  TEST_ASSERT(source_label_held != NULL);

  for (size_t i = 1; i < ENTRY_COUNT; i++) {
    char output_path[32];
    char source_label[32];
    TEST_ASSERT(snprintf(output_path, sizeof(output_path), "public/%zu.html", i) > 0);
    TEST_ASSERT(snprintf(source_label, sizeof(source_label), "%zu.md", i) > 0);
    TEST_CHECK(manifest_add(&manifest, output_path, source_label, NULL) == MANIFEST_ADD_INSERTED);
  }
  TEST_CHECK(manifest.count == ENTRY_COUNT);
  // Both arrays doubled to their expected totals, as `src/core/test_path_list.c` pins for the
  // sibling container. Finding the entries still proves neither, since a correct lookup survives
  // any capacity. The bucket total reveals the load factor. That factor has a correctness role
  // rather than only a performance one: a table that reaches full load never terminates the linear
  // probes in `manifest_bucket_place` and `manifest_lookup`, so a factor widened to 100% hangs the
  // suite rather than failing it. This assertion turns that hang into a failure. It does not
  // separate every factor below 100%: at this entry count a 1/2 factor also lands on 512.
  TEST_CHECK(manifest.capacity == ENTRIES_CAPACITY_MIN_ASSUMED * 16);
  TEST_CHECK(manifest.bucket_count == BUCKETS_CAPACITY_MIN_ASSUMED * 16);

  for (size_t i = 0; i < ENTRY_COUNT; i++) {
    char output_path[32];
    char source_label[32];
    TEST_ASSERT(snprintf(output_path, sizeof(output_path), "public/%zu.html", i) > 0);
    TEST_ASSERT(snprintf(source_label, sizeof(source_label), "%zu.md", i) > 0);
    const char* source_label_existing = NULL;
    TEST_CHECK(manifest_add(&manifest, output_path, "again.md", &source_label_existing) ==
               MANIFEST_ADD_DUPLICATE);
    TEST_CHECK(source_label_existing != NULL && strcmp(source_label_existing, source_label) == 0);
  }
  TEST_CHECK(manifest.count == ENTRY_COUNT);
  TEST_CHECK(strcmp(source_label_held, "0.md") == 0);

  manifest_free(&manifest);
}

// `manifest_add` records `a/b` and `a/b/c` as distinct, because duplicate means byte-equal, but the
// two cannot coexist on disk: `a/b` is a file that `a/b/c` needs to be a directory.
// `manifest_find_prefix_collision` catches that pair, naming the shorter path as the ancestor (the
// file) and the longer as the descendant.
static void test_detects_prefix_collision(void) {
  struct Manifest manifest;
  manifest_init(&manifest);

  TEST_CHECK(manifest_add(&manifest, "a/b", "p.md", NULL) == MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest_add(&manifest, "a/b/c", "q.md", NULL) == MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest.count == 2);

  struct ManifestPrefixCollision collision;
  TEST_CHECK(manifest_find_prefix_collision(&manifest, &collision));
  TEST_CHECK(strcmp(collision.ancestor_path, "a/b") == 0);
  TEST_CHECK(strcmp(collision.ancestor_label, "p.md") == 0);
  TEST_CHECK(strcmp(collision.descendant_path, "a/b/c") == 0);
  TEST_CHECK(strcmp(collision.descendant_label, "q.md") == 0);

  manifest_free(&manifest);
}

// The scan examines each path's ancestor prefixes, so it reports the pair whichever order the two
// were added in: the shorter path is always an ancestor of the longer, and here the longer is added
// first, so nothing but the ancestor walk finds it. The shorter path is still named as the
// ancestor.
static void test_detects_prefix_collision_regardless_of_order(void) {
  struct Manifest manifest;
  manifest_init(&manifest);

  TEST_CHECK(manifest_add(&manifest, "a/b/c", "q.md", NULL) == MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest_add(&manifest, "a/b", "p.md", NULL) == MANIFEST_ADD_INSERTED);

  struct ManifestPrefixCollision collision;
  TEST_CHECK(manifest_find_prefix_collision(&manifest, &collision));
  TEST_CHECK(strcmp(collision.ancestor_path, "a/b") == 0);
  TEST_CHECK(strcmp(collision.descendant_path, "a/b/c") == 0);

  manifest_free(&manifest);
}

// Distinct sibling paths that merely share a directory prefix do not collide: `public` and
// `public/blog` are directory components, not recorded outputs, so no recorded path is a prefix of
// another. A leading-run match alone is not enough; the exact-length lookup is what keeps this from
// a false positive.
static void test_no_prefix_collision_among_distinct_files(void) {
  struct Manifest manifest;
  manifest_init(&manifest);

  TEST_CHECK(manifest_add(&manifest, "public/index.html", "index.md", NULL) ==
             MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest_add(&manifest, "public/blog/index.html", "blog.md", NULL) ==
             MANIFEST_ADD_INSERTED);
  TEST_CHECK(manifest_add(&manifest, "public/about.html", "about.md", NULL) ==
             MANIFEST_ADD_INSERTED);

  struct ManifestPrefixCollision collision;
  TEST_CHECK(!manifest_find_prefix_collision(&manifest, &collision));

  manifest_free(&manifest);
}

TEST_LIST = {
    {"free allows reuse", test_free_allows_reuse},
    {"inserts then detects duplicate", test_inserts_then_detects_duplicate},
    {"grows and rehashes", test_grows_and_rehashes},
    {"detects prefix collision", test_detects_prefix_collision},
    {"detects prefix collision regardless of order",
     test_detects_prefix_collision_regardless_of_order},
    {"no prefix collision among distinct files", test_no_prefix_collision_among_distinct_files},
    {NULL, NULL}};
