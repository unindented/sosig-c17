#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "build/job.h"
#include "build/page_renderer.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host: the page phase reads templates and writes pages
// only through `runtime/os.h`.

/**
 * @brief Initializes a parsed, non-draft content entry the page phase can render.
 *
 * Copies every string, `body_html` included, into `arena`, as the parse phase copies them into its
 * worker's long-lived arena.
 *
 * @param entry       Entry to initialize. Must not be `NULL`.
 * @param source_path Source path the diagnostics name. Must not be `NULL`.
 * @param output_path Output path the page is written to. Must not be `NULL`.
 * @param title       Entry title. Must not be `NULL`.
 * @param date        RFC 3339 date. Must not be `NULL`.
 * @param date_epoch  Same instant as `date` in Unix epoch seconds.
 * @param arena       Arena that owns the copied strings. Must not be `NULL`.
 */
static void init_page_entry(struct ContentEntry* entry,
                            const char* source_path,
                            const char* output_path,
                            const char* title,
                            const char* date,
                            int64_t date_epoch,
                            struct Arena* arena) {
  *entry = (struct ContentEntry){0};
  entry->source_path = str_clone(str_from_cstr(source_path), arena);
  entry->output_path = str_clone(str_from_cstr(output_path), arena);
  entry->title = str_clone(str_from_cstr(title), arena);
  entry->date = str_clone(str_from_cstr(date), arena);
  entry->date_epoch = date_epoch;
  entry->body_html = str_clone(S("<p>Body</p>\n"), arena);
}

/**
 * @brief Initializes the two published entries most tests share.
 *
 * `newer` is dated a day after `older`, so the sorted entry set and `site.updated` both follow
 * `newer`.
 *
 * @param older Entry dated `2026-07-01`. Must not be `NULL`.
 * @param newer Entry dated `2026-07-02`. Must not be `NULL`.
 * @param arena Arena that owns both entries' strings. Must not be `NULL`.
 */
static void init_older_and_newer(struct ContentEntry* older,
                                 struct ContentEntry* newer,
                                 struct Arena* arena) {
  init_page_entry(older, "content/older.md", "public/older.html", "Older", "2026-07-01T00:00:00Z",
                  1782864000, arena);
  init_page_entry(newer, "content/newer.md", "public/newer.html", "Newer", "2026-07-02T00:00:00Z",
                  1782950400, arena);
}

/**
 * @brief Runs the page phase over entry slots with the default site configuration.
 *
 * Collects, sorts and dates the published entries the way the build does before its page phase.
 *
 * @param source_entries     Entry slots, each a parsed entry or `NULL` for a draft. Must not be
 *                           `NULL`.
 * @param source_entry_count Number of slots in `source_entries`.
 * @param worker_count       Worker threads the page phase runs on.
 * @param os                 Host holding `templates/`, which receives the pages. Must not be
 *                           `NULL`.
 * @param log                Log that receives every render and write error. Must not be `NULL`.
 * @return `0` on success, or `-1` when a page fails to render or write.
 */
static int render_entry_slots(struct ContentEntry* const* source_entries,
                              ptrdiff_t source_entry_count,
                              ptrdiff_t worker_count,
                              struct Os* os,
                              struct Log* log) {
  // Every worker gets arenas of its own, as `cmd_build_execute` carves them, so no two concurrent
  // jobs share one.
  struct Arena perm = test_arena();
  struct JobArenas* worker_arenas = arena_new(&perm, worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    worker_arenas[i].perm = test_arena();
    worker_arenas[i].scratch = test_arena();
  }
  struct ContentEntry** entries =
      arena_new(&perm, source_entry_count > 0 ? source_entry_count : 1, struct ContentEntry*);
  ptrdiff_t entry_count = 0;
  for (ptrdiff_t i = 0; i < source_entry_count; i++) {
    if (source_entries[i] != NULL) {
      entries[entry_count++] = source_entries[i];
    }
  }
  content_entry_sort(entries, entry_count);
  const struct Str site_updated =
      content_entry_date_latest((const struct ContentEntry* const*)entries, entry_count);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  return page_renderer_render_pages(&site_config, (const struct ContentEntry* const*)source_entries,
                                    source_entry_count, (const struct ContentEntry* const*)entries,
                                    entry_count, site_updated, worker_count, worker_arenas, false,
                                    os, test_arena(), log);
}

// A content template resolves `site.updated` to the newest entry's date, not to nothing. The page
// phase runs after the build parses and sorts every source, so the value can exist: the date
// belongs to a different entry than the one being rendered. The end-to-end assertion lives in the
// golden test suite.
static void test_renders_site_updated_in_content_template(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/content.html", "[{{site.updated}}]\n");
  struct ContentEntry older;
  struct ContentEntry newer;
  // The last slot is a draft's, which the parse phase leaves `NULL`.
  struct ContentEntry* source_entries[] = {&older, &newer, NULL};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  init_older_and_newer(&older, &newer, &arena);

  TEST_CHECK(render_entry_slots(source_entries, 3, 1, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/older.html", "[2026-07-02T00:00:00Z]\n"));
  TEST_CHECK(os_memory_is_file(os, "public/newer.html", "[2026-07-02T00:00:00Z]\n"));
  // The `NULL` slot is skipped, so it writes no page.
  TEST_CHECK(os_memory_files(os, "public").count == 2);
}

// A content template iterates `content_entries` newest-first, and a draft is absent from it. Both
// follow from the page phase running after the collect-and-sort step.
static void test_iterates_content_entries_in_content_template(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/content.html",
                  "{{#content_entries}}[{{title}}]{{/content_entries}}\n");
  struct ContentEntry older;
  struct ContentEntry newer;
  struct ContentEntry* source_entries[] = {&older, &newer, NULL};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  init_older_and_newer(&older, &newer, &arena);

  TEST_CHECK(render_entry_slots(source_entries, 3, 1, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/older.html", "[Newer][Older]\n"));
}

// Pages that share parent directories are written concurrently, each job creating the parents its
// own page needs, and every page lands. The `tsan` test preset runs the same path under
// ThreadSanitizer. On a real filesystem a sibling's `EEXIST` must not count as a failure either,
// which the POSIX `os_mkdirs` handles by creating first and accepting an existing directory.
static void test_writes_pages_sharing_parents_concurrently(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/content.html", "[{{title}}]\n");
  enum { SOURCE_COUNT = 32 };
  struct ContentEntry entries[SOURCE_COUNT];
  struct ContentEntry* source_entries[SOURCE_COUNT] = {NULL};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  for (int i = 0; i < SOURCE_COUNT; i++) {
    char source_path[64];
    char output_path[64];
    char title[24];
    (void)snprintf(source_path, sizeof(source_path), "content/section/sub/post-%02d.md", i);
    (void)snprintf(output_path, sizeof(output_path), "public/section/sub/post-%02d/index.html", i);
    (void)snprintf(title, sizeof(title), "Post %02d", i);
    source_entries[i] = &entries[i];
    init_page_entry(&entries[i], source_path, output_path, title, "2026-07-01T00:00:00Z",
                    1782864000, &arena);
  }

  TEST_CHECK(render_entry_slots(source_entries, SOURCE_COUNT, 8, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  for (int i = 0; i < SOURCE_COUNT; i++) {
    char output_path[64];
    char expected[32];
    (void)snprintf(output_path, sizeof(output_path), "public/section/sub/post-%02d/index.html", i);
    (void)snprintf(expected, sizeof(expected), "[Post %02d]\n", i);
    TEST_CHECK(os_memory_is_file(os, output_path, expected));
  }
}

// The page phase accepts an empty entry set. Its slot and entry arrays may be `NULL`, which its
// contract allows for a count of 0, and `job_run` allocates a zero-count log array.
static void test_accepts_empty_entry_set(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  struct JobArenas worker_arenas = {.perm = test_arena(), .scratch = test_arena()};

  TEST_CHECK(page_renderer_render_pages(&site_config, NULL, 0, NULL, 0, S("1970-01-01T00:00:00Z"),
                                        1, &worker_arenas, false, os_memory(), test_arena(),
                                        &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A missing content template surfaces a per-entry error in the collected log. The diagnostic names
// the failing entry, the template file that could not be read, and why.
static void test_reports_missing_template(void) {
  struct Os* os = os_memory();
  struct ContentEntry entry;
  struct ContentEntry* source_entries[] = {&entry};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  init_page_entry(&entry, "content/hello.md", "public/hello.html", "Hello", "2026-07-01T00:00:00Z",
                  1782864000, &arena);

  TEST_CHECK(render_entry_slots(source_entries, 1, 1, os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: %s ('templates/content.html') (while rendering "
                         "'content/hello.md')\n",
                         strerror(ENOENT)));
}

// A page that cannot be written fails only its own job. Its error is one line of the phase's
// collected errors, and every other page is still written. One page path is an existing directory,
// and the other fails with an injected `EACCES`, as an unwritable directory would refuse it.
static void test_reports_write_failure_per_entry(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/content.html", "[{{title}}]\n");
  os_memory_mkdir(os, "public/older.html");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/newer.html", EACCES);
  struct ContentEntry older;
  struct ContentEntry newer;
  struct ContentEntry third;
  struct ContentEntry* source_entries[] = {&older, &newer, &third};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  init_older_and_newer(&older, &newer, &arena);
  init_page_entry(&third, "content/third.md", "public/third.html", "Third", "2026-06-30T00:00:00Z",
                  1782777600, &arena);

  TEST_CHECK(render_entry_slots(source_entries, 3, 1, os, &log) == -1);
  // Errors are collected in job index order, so the two lines follow `source_entries`.
  TEST_CHECK(
      test_log_is(&log,
                  "failed to write output for 'content/older.md': %s ('public/older.html')\nfailed "
                  "to write output for 'content/newer.md': %s ('public/newer.html')\n",
                  strerror(EISDIR), strerror(EACCES)));
  TEST_CHECK(log.errors == 2);
  TEST_CHECK(os_memory_is_file(os, "public/third.html", "[Third]\n"));
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
