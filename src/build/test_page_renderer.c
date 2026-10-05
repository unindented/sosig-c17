#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/page_renderer.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/** Largest page a test reads back, in bytes. Pages in these tests are a few bytes. */
enum { TEST_PAGE_LEN_MAX = 4096 };

/**
 * @brief Initializes a parsed, non-draft content entry the page phase can render.
 *
 * Copies every string into the entry's arena except `body_html`, which is a heap copy because
 * `content_entry_free` releases it.
 *
 * @param entry       Entry to initialize. Must not be `NULL`.
 * @param source_path Source path the diagnostics name. Must not be `NULL`.
 * @param output_path Output path the page is written to, relative to the working directory. Must
 *                    not be `NULL`.
 * @param title       Entry title. Must not be `NULL`.
 * @param date        RFC 3339 date. Must not be `NULL`.
 * @param date_epoch  Same instant as `date` in Unix epoch seconds.
 * @return `0` on success, or `-1` after recording a test-plumbing failure. The entry is initialized
 *         either way, so `content_entry_free` is safe on it.
 */
static int init_page_entry(struct ContentEntry* entry,
                           const char* source_path,
                           const char* output_path,
                           const char* title,
                           const char* date,
                           int64_t date_epoch) {
  content_entry_init(entry);
  entry->source_path = arena_strdup(&entry->arena, source_path);
  entry->output_path = arena_strdup(&entry->arena, output_path);
  entry->title = arena_strdup(&entry->arena, title);
  entry->date = arena_strdup(&entry->arena, date);
  entry->date_epoch = date_epoch;
  entry->body_html = text_strdup("<p>Body</p>\n");
  return TEST_CHECK(entry->source_path != NULL && entry->output_path != NULL &&
                    entry->title != NULL && entry->date != NULL && entry->body_html != NULL)
             ? 0
             : -1;
}

/**
 * @brief Initializes the two published entries most tests share.
 *
 * `newer` is dated a day after `older`, so the sorted entry set and `site.updated` both follow
 * `newer`.
 *
 * @param older Entry dated `2026-07-01`. Must not be `NULL`.
 * @param newer Entry dated `2026-07-02`. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure. Both entries are
 *         initialized either way, so `content_entry_free` is safe on each.
 */
static int init_older_and_newer(struct ContentEntry* older, struct ContentEntry* newer) {
  const int older_rc = init_page_entry(older, "content/older.md", "public/older.html", "Older",
                                       "2026-07-01T00:00:00Z", 1782864000);
  const int newer_rc = init_page_entry(newer, "content/newer.md", "public/newer.html", "Newer",
                                       "2026-07-02T00:00:00Z", 1782950400);
  return older_rc == 0 && newer_rc == 0 ? 0 : -1;
}

/**
 * @brief Runs the page phase over entry slots with the default site configuration.
 *
 * Collects, sorts and dates the published entries the way the build does before its page phase, and
 * runs with `root_dir` as the working directory, restoring the previous one before returning.
 *
 * @param root_dir       Fixture root holding `templates/`. Must not be `NULL`.
 * @param source_entries Entry slots, each a parsed entry or `NULL` for a draft. Must not be `NULL`.
 * @param slot_count     Number of slots in `source_entries`.
 * @param worker_count   Worker threads the page phase runs on.
 * @param error_out      Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
static int render_entry_slots(const char* root_dir,
                              struct ContentEntry* const* source_entries,
                              size_t slot_count,
                              size_t worker_count,
                              struct StringBuffer* error_out) {
  struct ContentEntry** entries = calloc(slot_count > 0 ? slot_count : 1, sizeof(*entries));
  int rc = TEST_PLUMBING_FAILED;
  if (TEST_CHECK(entries != NULL)) {
    size_t entry_count = 0;
    for (size_t i = 0; i < slot_count; i++) {
      if (source_entries[i] != NULL) {
        entries[entry_count++] = source_entries[i];
      }
    }
    content_entry_sort(entries, entry_count);
    const char* site_updated =
        content_entry_latest_date((const struct ContentEntry* const*)entries, entry_count);

    int saved_dir_fd = -1;
    if (working_dir_enter(root_dir, &saved_dir_fd) == 0) {
      struct SiteConfig site_config;
      site_config_init(&site_config);
      rc = page_renderer_render_pages(&site_config,
                                      (const struct ContentEntry* const*)source_entries, slot_count,
                                      (const struct ContentEntry* const*)entries, entry_count,
                                      site_updated, worker_count, false, error_out);
      site_config_free(&site_config);
      rc = working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
    }
  }
  free(entries);
  return rc;
}

/**
 * @brief Frees every entry an entry slot array holds.
 *
 * @param source_entries Entry slots, each an initialized entry or `NULL`. Must not be `NULL`.
 * @param slot_count     Number of slots in `source_entries`.
 */
static void free_entry_slots(struct ContentEntry* const* source_entries, size_t slot_count) {
  for (size_t i = 0; i < slot_count; i++) {
    if (source_entries[i] != NULL) {
      content_entry_free(source_entries[i]);
    }
  }
}

/**
 * @brief Checks that a page the page phase wrote holds exactly the expected text.
 *
 * @param root_dir      Fixture root the output path is relative to.
 * @param relative_path Output path relative to `root_dir`.
 * @param expected      Terminated text the page must hold.
 */
static void check_output(const char* root_dir, const char* relative_path, const char* expected) {
  struct Arena arena;
  arena_init(&arena);
  const char* path = path_join(root_dir, relative_path, &arena);
  char* data = NULL;
  size_t data_len = 0;
  const int rc =
      path != NULL ? fs_read_file(path, TEST_PAGE_LEN_MAX, &data, &data_len, NULL, 0) : -1;
  arena_free(&arena);
  TEST_CHECK(rc == 0 && data != NULL && strcmp(data, expected) == 0);
  TEST_MSG("output '%s': %s", relative_path, data != NULL ? data : "(unreadable)");
  free(data);
}

/**
 * @brief Counts the files below a fixture root's `public/` directory.
 *
 * @param root_dir Fixture root holding `public/`.
 * @return The number of files, or `SIZE_MAX` when the directory cannot be listed.
 */
static size_t count_outputs(const char* root_dir) {
  struct Arena arena;
  arena_init(&arena);
  const char* output_dir = path_join(root_dir, "public", &arena);
  struct PathList outputs;
  path_list_init(&outputs);
  static const char* const all_suffixes[] = {""};
  const bool is_listed =
      output_dir != NULL &&
      fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL, 0) == 0;
  const size_t count = is_listed ? outputs.count : SIZE_MAX;
  path_list_free(&outputs);
  arena_free(&arena);
  return count;
}

// A content template resolves `site.updated` to the newest entry's date, not to nothing. The page
// phase runs after the build parses and sorts every source, so the value can exist: the date
// belongs to a different entry than the one being rendered. The end-to-end assertion lives in the
// golden test suite.
static void test_renders_site_updated_in_content_template(void) {
  char root_dir_template[] = "/tmp/sosig-page-site-updated.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "[{{site.updated}}]\n") == 0);
  struct ContentEntry older;
  struct ContentEntry newer;
  // The last slot is a draft's, which the parse phase leaves `NULL`.
  struct ContentEntry* source_entries[] = {&older, &newer, NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (init_older_and_newer(&older, &newer) != 0) {
    goto cleanup;
  }

  TEST_CHECK(render_entry_slots(root_dir, source_entries, 3, 1, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  check_output(root_dir, "public/older.html", "[2026-07-02T00:00:00Z]\n");
  check_output(root_dir, "public/newer.html", "[2026-07-02T00:00:00Z]\n");
  // The `NULL` slot is skipped, so it writes no page.
  TEST_CHECK(count_outputs(root_dir) == 2);

cleanup:
  string_buffer_free(&error_buffer);
  free_entry_slots(source_entries, 3);
  remove_fixture_tree(root_dir);
}

// A content template iterates `content_entries` newest-first, and a draft is absent from it. Both
// follow from the page phase running after the collect-and-sort step.
static void test_iterates_content_entries_in_content_template(void) {
  char root_dir_template[] = "/tmp/sosig-page-entries.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html",
                                "{{#content_entries}}[{{title}}]{{/content_entries}}\n") == 0);
  struct ContentEntry older;
  struct ContentEntry newer;
  struct ContentEntry* source_entries[] = {&older, &newer, NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (init_older_and_newer(&older, &newer) != 0) {
    goto cleanup;
  }

  TEST_CHECK(render_entry_slots(root_dir, source_entries, 3, 1, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  check_output(root_dir, "public/older.html", "[Newer][Older]\n");

cleanup:
  string_buffer_free(&error_buffer);
  free_entry_slots(source_entries, 3);
  remove_fixture_tree(root_dir);
}

// Pages that share parent directories are written concurrently, each job creating the parents its
// own page needs. Every job races its siblings to create `public/section` and `public/section/sub`,
// so a job that took a sibling's `EEXIST` for a failure would drop its page. The `tsan` test preset
// runs the same path under ThreadSanitizer.
static void test_writes_pages_sharing_parents_concurrently(void) {
  char root_dir_template[] = "/tmp/sosig-page-shared-parents.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "[{{title}}]\n") == 0);
  enum { SOURCE_COUNT = 32 };
  struct ContentEntry entries[SOURCE_COUNT];
  struct ContentEntry* source_entries[SOURCE_COUNT] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  for (int i = 0; i < SOURCE_COUNT; i++) {
    char source_path[64];
    char output_path[64];
    char title[16];
    (void)snprintf(source_path, sizeof(source_path), "content/section/sub/post-%02d.md", i);
    (void)snprintf(output_path, sizeof(output_path), "public/section/sub/post-%02d/index.html", i);
    (void)snprintf(title, sizeof(title), "Post %02d", i);
    source_entries[i] = &entries[i];
    if (init_page_entry(&entries[i], source_path, output_path, title, "2026-07-01T00:00:00Z",
                        1782864000) != 0) {
      goto cleanup;
    }
  }

  TEST_CHECK(render_entry_slots(root_dir, source_entries, SOURCE_COUNT, 8, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");
  for (int i = 0; i < SOURCE_COUNT; i++) {
    char output_path[64];
    char expected[32];
    (void)snprintf(output_path, sizeof(output_path), "public/section/sub/post-%02d/index.html", i);
    (void)snprintf(expected, sizeof(expected), "[Post %02d]\n", i);
    check_output(root_dir, output_path, expected);
  }

cleanup:
  string_buffer_free(&error_buffer);
  free_entry_slots(source_entries, SOURCE_COUNT);
  remove_fixture_tree(root_dir);
}

// The page phase accepts an empty entry set. Its slot and entry arrays may be `NULL`, which its
// contract allows for a count of 0, and `job_run` allocates error slots whose count is 0, for which
// `calloc(0, ...)` may also return `NULL`. Neither may be reported as an allocation failure.
static void test_accepts_empty_entry_set(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(page_renderer_render_pages(&site_config, NULL, 0, NULL, 0, "1970-01-01T00:00:00Z", 1,
                                        false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);

  string_buffer_free(&error_buffer);
  site_config_free(&site_config);
}

// A missing content template surfaces a per-entry diagnostic in the collected error buffer.
static void test_reports_missing_template(void) {
  // The diagnostic names the failing entry, the template file that could not be read, and why.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read template: %s ('templates/content.html') (while rendering "
               "'content/hello.md')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  char root_dir_template[] = "/tmp/sosig-page-missing-template.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct ContentEntry entry;
  struct ContentEntry* source_entries[] = {&entry};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  if (init_page_entry(&entry, "content/hello.md", "public/hello.html", "Hello",
                      "2026-07-01T00:00:00Z", 1782864000) != 0) {
    goto cleanup;
  }

  TEST_CHECK(render_entry_slots(root_dir, source_entries, 1, 1, &error_buffer) == -1);
  // Exact, not by substring: `expected` is the whole message, so a substring check would also pass
  // for that message with something appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

cleanup:
  string_buffer_free(&error_buffer);
  free_entry_slots(source_entries, 1);
  remove_fixture_tree(root_dir);
}

// A page that cannot be written fails only its own job. Its diagnostic is one line of the phase's
// collected errors, and every other page is still written. A directory at the page path cannot be
// opened as a file even by a privileged process.
static void test_reports_write_failure_per_entry(void) {
  char root_dir_template[] = "/tmp/sosig-page-write-failure.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "[{{title}}]\n") == 0);
  struct ContentEntry older;
  struct ContentEntry newer;
  struct ContentEntry third;
  struct ContentEntry* source_entries[] = {&older, &newer, &third};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  struct Arena arena;
  arena_init(&arena);
  const int older_and_newer_rc = init_older_and_newer(&older, &newer);
  const int third_rc = init_page_entry(&third, "content/third.md", "public/third.html", "Third",
                                       "2026-06-30T00:00:00Z", 1782777600);
  const char* older_page = path_join(root_dir, "public/older.html", &arena);
  const char* newer_page = path_join(root_dir, "public/newer.html", &arena);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  int expected_len = 0;
  if (older_and_newer_rc != 0 || third_rc != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(older_page != NULL && newer_page != NULL)) {
    goto cleanup;
  }
  TEST_CHECK(fs_mkdir_p(older_page, NULL, 0) == 0);
  TEST_CHECK(fs_mkdir_p(newer_page, NULL, 0) == 0);

  TEST_CHECK(render_entry_slots(root_dir, source_entries, 3, 1, &error_buffer) == -1);
  error_system_message(reason, sizeof(reason), EISDIR);
  // Diagnostics are collected in job index order, so the two lines follow `source_entries`.
  expected_len =
      snprintf(expected, sizeof(expected),
               "failed to write output: %s (for 'content/older.md', to 'public/older.html')\n"
               "failed to write output: %s (for 'content/newer.md', to 'public/newer.html')",
               reason, reason);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");
  check_output(root_dir, "public/third.html", "[Third]\n");

cleanup:
  arena_free(&arena);
  string_buffer_free(&error_buffer);
  free_entry_slots(source_entries, 3);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"renders site updated in content template", test_renders_site_updated_in_content_template},
    {"iterates content entries in content template",
     test_iterates_content_entries_in_content_template},
    {"writes pages sharing parents concurrently", test_writes_pages_sharing_parents_concurrently},
    {"accepts empty entry set", test_accepts_empty_entry_set},
    {"reports missing template", test_reports_missing_template},
    {"reports write failure per entry", test_reports_write_failure_per_entry},
    {NULL, NULL},
};
