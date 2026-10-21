#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build/entry_renderer.h"
#include "build/job.h"
#include "build/page_renderer.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Allocates one `JobArenas` per worker, each region a fresh `test_arena` reservation.
 *
 * @param worker_count Number of workers to allocate arenas for.
 * @param perm         Arena that owns the array. Must not be `NULL`.
 * @return The array of `worker_count` arenas, none overlapping another or `perm`.
 */
static struct JobArenas* test_job_arenas(ptrdiff_t worker_count, struct Arena* perm) {
  struct JobArenas* arenas = arena_new(perm, worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    arenas[i].perm = test_arena();
    arenas[i].scratch = test_arena();
  }
  return arenas;
}

/**
 * @brief Runs the page phase over entry slots the parse phase filled.
 *
 * Collects, sorts and dates the published entries the way the build does before its page phase.
 *
 * @param site_config        Configuration used for rendering. Must not be `NULL`.
 * @param source_entries     Entry slots filled by the parse phase. Must not be `NULL`.
 * @param source_entry_count Number of slots in `source_entries`.
 * @param worker_count       Worker threads the page phase runs on.
 * @param worker_arenas      One `JobArenas` per worker, holding the parsed entries. Must not be
 *                           `NULL`.
 * @param log                Log that receives every error. Its arena also holds the collected entry
 *                           array. Must not be `NULL`.
 * @return `0` on success, or `-1` on render failure.
 */
static int render_pages(const struct SiteConfig* site_config,
                        struct ContentEntry* const* source_entries,
                        ptrdiff_t source_entry_count,
                        ptrdiff_t worker_count,
                        struct JobArenas* worker_arenas,
                        struct Log* log) {
  struct ContentEntry** entries =
      arena_new(log->buf.arena, source_entry_count, struct ContentEntry*);
  ptrdiff_t entry_count = 0;
  for (ptrdiff_t i = 0; i < source_entry_count; i++) {
    if (source_entries[i] != NULL) {
      entries[entry_count++] = source_entries[i];
    }
  }
  content_entry_sort(entries, entry_count);
  const struct Str site_updated =
      content_entry_date_latest((const struct ContentEntry* const*)entries, entry_count);
  return page_renderer_render_pages(site_config, (const struct ContentEntry* const*)source_entries,
                                    source_entry_count, (const struct ContentEntry* const*)entries,
                                    entry_count, site_updated, worker_count, worker_arenas, false,
                                    os_posix(), test_arena(), log);
}

/**
 * @brief Renders named fixture sources through both parallel phases on one worker.
 *
 * Loads `sosig.toml` from `root_dir` and runs with `root_dir` as the working directory, restoring
 * the previous one before returning. The configuration, the path list, and the entry arrays live in
 * `log`'s arena, and the entries in worker arenas of their own, so all of them outlive the call.
 *
 * @param root_dir            Fixture root used as the working directory. Must not be `NULL`.
 * @param relative_paths      Source paths relative to `root_dir`. Must not be `NULL`.
 * @param relative_path_count Number of entries in `relative_paths`.
 * @param site_config         Configuration populated from the fixture. Must not be `NULL`.
 * @param source_paths        Path list populated with the sources. Must not be `NULL`.
 * @param source_entries      One entry slot per path in `relative_paths`, each `NULL`-initialized.
 *                            Must not be `NULL`.
 * @param log                 Log that receives every error. Its arena also holds the configuration
 *                            and the path list, which only ever reach it by pointer. Must not be
 *                            `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
static int render_sources(const char* root_dir,
                          const char* const* relative_paths,
                          ptrdiff_t relative_path_count,
                          struct SiteConfig* site_config,
                          struct PathList* source_paths,
                          struct ContentEntry** source_entries,
                          struct Log* log) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return TEST_PLUMBING_FAILED;
  }

  int rc = TEST_PLUMBING_FAILED;
  const bool is_config_loaded = site_config_load(site_config, S("sosig.toml"), os_posix(),
                                                 log->buf.arena, test_arena(), log) == 0 &&
                                log->errors == 0;
  const struct Str config_errors = buf_str(&log->buf);
  TEST_CHECK(is_config_loaded);
  TEST_MSG("config: %.*s", STR_FMT(config_errors));
  if (is_config_loaded) {
    for (ptrdiff_t i = 0; i < relative_path_count; i++) {
      path_list_push(source_paths, str_from_cstr(relative_paths[i]), log->buf.arena);
    }
    struct JobArenas* worker_arenas = test_job_arenas(1, log->buf.arena);
    rc = entry_renderer_render_entries(site_config, source_paths, CONTENT_FILE_LEN_MAX, 1,
                                       worker_arenas, false, os_posix(), test_arena(),
                                       source_entries, log);
    if (rc == 0) {
      rc = render_pages(site_config, source_entries, relative_path_count, 1, worker_arenas, log);
    }
  }

  return working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
}

/**
 * @brief Renders one fixture source through both parallel phases on one worker.
 *
 * Equivalent to `render_sources` with a single path.
 *
 * @param root_dir             Fixture root used as the working directory. Must not be `NULL`.
 * @param source_relative_path Source path relative to `root_dir`. Must not be `NULL`.
 * @param site_config          Configuration populated from the fixture. Must not be `NULL`.
 * @param source_paths         Path list populated with the source. Must not be `NULL`.
 * @param source_entries       One `NULL`-initialized entry slot. Must not be `NULL`.
 * @param log                  Log that receives every error, as for `render_sources`. Must not be
 *                             `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
static int render_single_source(const char* root_dir,
                                const char* source_relative_path,
                                struct SiteConfig* site_config,
                                struct PathList* source_paths,
                                struct ContentEntry** source_entries,
                                struct Log* log) {
  return render_sources(root_dir, &source_relative_path, 1, site_config, source_paths,
                        source_entries, log);
}

/**
 * @brief Reads a page the page phase wrote below a fixture root.
 *
 * Pages in these tests are short, so a page over 4096 bytes fails the read like a missing one.
 *
 * @param root_dir      Fixture root the output path is relative to. Must not be `NULL`.
 * @param relative_path Output path relative to `root_dir`. Must not be `NULL`.
 * @param arena         Arena that owns the page. Must not be `NULL`.
 * @param page_out      Receives the page on success. Must not be `NULL`.
 * @return `0` when the page was read, or `-1` when it cannot be read.
 */
static int read_output(const char* root_dir,
                       const char* relative_path,
                       struct Arena* arena,
                       struct Str* page_out) {
  enum { OUTPUT_LEN_MAX = 4096 };
  const struct Str path = path_join(str_from_cstr(root_dir), str_from_cstr(relative_path), arena);
  // A missing page is an outcome some tests expect, so the read's diagnostic goes unread.
  struct Log log = test_log();
  return os_read(os_posix(), path, OUTPUT_LEN_MAX, arena, page_out, S("failed to read output"),
                 &log);
}

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 1024 };

/** Config most fixtures share. It turns off the aggregate and feed phases. */
static const char* const SITE_CONFIG =
    "base_url = \"https://example.com\"\n"
    "title = \"Site\"\n"
    "author = \"Author\"\n"
    "aggregate_templates = []\n"
    "feed_templates = []\n";

/**
 * @brief Runs the parse phase while capturing standard error.
 *
 * Restores standard error before returning.
 *
 * @param site_config    Configuration used for rendering.
 * @param source_paths   Source paths to render.
 * @param worker_count   Number of worker threads to request.
 * @param source_entries One `NULL`-initialized entry slot per source path.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @param log            Log that receives render errors.
 * @return The renderer result, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static int render_entries_capturing_stderr(const struct SiteConfig* site_config,
                                           const struct PathList* source_paths,
                                           ptrdiff_t worker_count,
                                           struct ContentEntry** source_entries,
                                           char* stderr_out,
                                           ptrdiff_t stderr_out_len,
                                           struct Log* log) {
  stderr_out[0] = '\0';
  struct StreamCapture stderr_capture;
  if (capture_begin(stderr, &stderr_capture) != 0) {
    return TEST_PLUMBING_FAILED;
  }
  const int rc =
      entry_renderer_render_entries(site_config, source_paths, CONTENT_FILE_LEN_MAX, worker_count,
                                    test_job_arenas(worker_count, log->buf.arena), true, os_posix(),
                                    test_arena(), source_entries, log);
  return capture_end(&stderr_capture, stderr_out, stderr_out_len) == 0 ? rc : TEST_PLUMBING_FAILED;
}

/**
 * @brief Runs the parse phase over one fixture source on one worker, from the fixture root.
 *
 * Uses the default configuration, whose `content_dir` is `content`, and restores the previous
 * working directory before returning.
 *
 * @param root_dir       Fixture root used as the working directory. Must not be `NULL`.
 * @param relative_path  Source path relative to `root_dir`. Must not be `NULL`.
 * @param source_len_max Largest source the job reads, in bytes.
 * @param worker_arenas  Arenas of the one worker. Must not be `NULL`.
 * @param entry_out      Receives the entry slot the job fills. Must not be `NULL`.
 * @param log            Log that receives the parse errors. Must not be `NULL`.
 * @return The renderer result, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static int parse_source_in_dir(const char* root_dir,
                               const char* relative_path,
                               ptrdiff_t source_len_max,
                               struct JobArenas* worker_arenas,
                               struct ContentEntry** entry_out,
                               struct Log* log) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return TEST_PLUMBING_FAILED;
  }
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  path_list_push(&source_paths, str_from_cstr(relative_path), log->buf.arena);
  *entry_out = NULL;
  const int rc =
      entry_renderer_render_entries(&site_config, &source_paths, source_len_max, 1, worker_arenas,
                                    false, os_posix(), test_arena(), entry_out, log);
  return working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
}

// A single source renders its frontmatter metadata, body HTML, page output, and output paths.
static void test_renders_entry_metadata_and_html(void) {
  char root_dir[] = "/tmp/sosig-entry-metadata.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  const struct ContentEntry* entry = NULL;
  struct Str page = {0};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html",
                                     "<main>{{title}} {{{body}}}</main>\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  entry = source_entries[0];
  TEST_CHECK(entry != NULL);
  if (entry != NULL) {
    TEST_CHECK(str_eq(entry->title, S("Hello")));
    TEST_CHECK(str_eq(entry->slug, S("hello")));
    TEST_CHECK(strstr(str_cstr(entry->body_html, &perm), "<p>Body</p>") != NULL);
    TEST_CHECK(str_eq(entry->url_path, S("/hello.html")));
    TEST_CHECK(str_eq(entry->output_path, S("public/hello.html")));
  }

  TEST_CHECK(read_output(root_dir, "public/hello.html", &perm, &page) == 0);
  TEST_CHECK(str_eq(page, S("<main>Hello <p>Body</p>\n</main>\n")));

cleanup:
  remove_fixture_tree(root_dir);
}

// A draft source is parsed but leaves its entry slot `NULL` and writes no page.
static void test_skips_draft_entry(void) {
  char root_dir[] = "/tmp/sosig-entry-draft.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "draft = true\n"
      "+++\n"
      "Body\n";
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  struct Str page = {0};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(source_entries[0] == NULL);

  TEST_CHECK(read_output(root_dir, "public/hello.html", &perm, &page) == -1);

cleanup:
  remove_fixture_tree(root_dir);
}

// A job advances its worker's `perm` only when it publishes an entry. A draft and a source that
// fails to parse leave it where it was, so neither keeps memory for the rest of the build.
static void test_keeps_perm_only_for_a_published_entry(void) {
  char root_dir[] = "/tmp/sosig-entry-commit.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  struct JobArenas* worker_arenas = test_job_arenas(1, &perm);
  struct ContentEntry* entry = NULL;
  char* perm_beg = NULL;
  if (!TEST_CHECK(write_fixture_file(
                      root_dir, "content/draft.md",
                      "+++\ntitle = \"Draft\"\ndate = 2026-07-01\ndraft = true\n+++\nBody\n") ==
                  0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/broken.md", "no fence\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md",
                                     "+++\ntitle = \"Hello\"\ndate = 2026-07-01\n+++\nBody\n") ==
                  0)) {
    goto cleanup;
  }

  perm_beg = worker_arenas[0].perm.beg;
  TEST_CHECK(parse_source_in_dir(root_dir, "content/draft.md", CONTENT_FILE_LEN_MAX, worker_arenas,
                                 &entry, &log) == 0);
  TEST_CHECK(entry == NULL && worker_arenas[0].perm.beg == perm_beg);

  TEST_CHECK(parse_source_in_dir(root_dir, "content/broken.md", CONTENT_FILE_LEN_MAX, worker_arenas,
                                 &entry, &log) == -1);
  TEST_CHECK(entry == NULL && worker_arenas[0].perm.beg == perm_beg);
  TEST_CHECK(
      test_log_is_one(&log, "missing opening '+++' frontmatter fence (in 'content/broken.md')"));

  TEST_CHECK(parse_source_in_dir(root_dir, "content/hello.md", CONTENT_FILE_LEN_MAX, worker_arenas,
                                 &entry, &log) == 0);
  TEST_CHECK(entry != NULL && worker_arenas[0].perm.beg > perm_beg);

cleanup:
  remove_fixture_tree(root_dir);
}

// A verbose phase prints one progress line per finished job, drafts included, then closes the line.
// The lines are printed from worker threads under the lock `job.c` keeps for the progress line.
// This drives it with more workers than one so the lock is contended, and the `tsan` test preset
// runs the same path under ThreadSanitizer.
static void test_verbose_prints_one_progress_line_per_job(void) {
  char root_dir[] = "/tmp/sosig-entry-verbose.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  const char page[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char draft[] =
      "+++\n"
      "title = \"Draft\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "draft = true\n"
      "+++\n"
      "Body\n";
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[3] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  char stderr_out[128] = "";
  int rc = -1;
  size_t progress_len = 0;
  int saved_dir_fd = -1;
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", page) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/second.md", page) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/draft.md", draft) == 0)) {
    goto cleanup;
  }
  path_list_push(&source_paths, S("content/hello.md"), &perm);
  path_list_push(&source_paths, S("content/second.md"), &perm);
  path_list_push(&source_paths, S("content/draft.md"), &perm);
  if (!TEST_CHECK(working_dir_enter(root_dir, &saved_dir_fd) == 0)) {
    goto cleanup;
  }

  if (TEST_CHECK(site_config_load(&site_config, S("sosig.toml"), os_posix(), &perm, test_arena(),
                                  &log) == 0 &&
                 log.errors == 0)) {
    rc = render_entries_capturing_stderr(&site_config, &source_paths, 4, source_entries, stderr_out,
                                         sizeof(stderr_out), &log);
  }
  TEST_CHECK(working_dir_leave(saved_dir_fd) == 0);

  TEST_CHECK(rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // Three jobs, so three progress lines even though one entry is a draft that produces no output,
  // and one trailing newline closing the line before any later status message. Workers finish in
  // any order, so each count is asserted present once rather than the whole text in sequence.
  static const char* const progress_lines[] = {"\rparsing content 1/3", "\rparsing content 2/3",
                                               "\rparsing content 3/3"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(progress_lines) / sizeof(progress_lines[0])); i++) {
    const char* found = strstr(stderr_out, progress_lines[i]);
    TEST_CHECK(found != NULL && strstr(found + 1, progress_lines[i]) == NULL);
    progress_len += strlen(progress_lines[i]);
  }
  TEST_CHECK(strlen(stderr_out) == progress_len + 1);
  TEST_CHECK(stderr_out[progress_len] == '\n');
  TEST_MSG("stderr: '%s'", stderr_out);

cleanup:
  remove_fixture_tree(root_dir);
}

// A source nested two directories deep publishes under both slugified section segments, joined by a
// single `/`. One level of nesting cannot show this. The join loop runs once, so a test with one
// level cannot observe the separator insertion or the per-segment slugification of anything beyond
// the first segment. Both directory names here need slugifying, so a join that skipped the
// separator would publish `my-sectionsub-dir` as one directory.
static void test_nests_output_under_slugified_sections(void) {
  char root_dir[] = "/tmp/sosig-entry-sections.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char* const source_relative_path = "content/My Section/Sub Dir/hello.md";
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  const struct ContentEntry* entry = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, source_relative_path, content) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, source_relative_path, &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  entry = source_entries[0];
  TEST_CHECK(entry != NULL);
  if (entry != NULL) {
    TEST_CHECK(str_eq(entry->url_path, S("/my-section/sub-dir/hello.html")));
    TEST_CHECK(str_eq(entry->output_path, S("public/my-section/sub-dir/hello.html")));
  }

cleanup:
  remove_fixture_tree(root_dir);
}

// The parse phase accepts an empty site with a `NULL` entry slot array, which its contract allows
// for a count of 0, and runs no job. `test_accepts_empty_entry_set` in `test_page_renderer.c` pins
// the same for the page phase.
static void test_accepts_empty_site(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(entry_renderer_render_entries(&site_config, &source_paths, CONTENT_FILE_LEN_MAX, 1,
                                           test_job_arenas(1, &perm), false, os_posix(),
                                           test_arena(), NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A source that exists but cannot be read is reported with the system cause and the path it
// happened on, rather than as a generic parse failure.
static void test_reports_unreadable_source(void) {
  // Root bypasses the permission bits, so the source would read fine and the assertion below would
  // fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read content: %s ('content/hello.md')",
               error_system_message(reason, sizeof(reason), EACCES));
  TEST_ASSERT(expected_len > 0 && expected_len < (int)sizeof(expected));

  char root_dir[] = "/tmp/sosig-entry-unreadable.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  struct Arena arena = test_arena();
  char* source_path = NULL;
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  int rc = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }
  source_path = str_cstr(path_join(str_from_cstr(root_dir), S("content/hello.md"), &arena), &arena);
  TEST_CHECK(chmod(source_path, 0) == 0);

  rc = render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                            source_entries, &log);
  // Restore the mode right after the call, so the unreadable source never outlives what it tests.
  (void)chmod(source_path, 0644);

  TEST_CHECK(rc == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log, "%s\n", expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A source larger than `CONTENT_FILE_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size, rather than read whole and turned away by the converter. The file is sparse,
// so the fixture costs no disk and a read that loaded it would show only as the wrong diagnostic.
static void test_rejects_oversize_source(void) {
  const off_t source_len = (off_t)CONTENT_FILE_LEN_MAX + 1;
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "failed to read content: exceeds max file size (%d bytes) at "
                                    "%jd bytes ('content/hello.md')",
                                    CONTENT_FILE_LEN_MAX, (intmax_t)source_len);
  TEST_ASSERT(expected_len > 0 && expected_len < (int)sizeof(expected));

  char root_dir[] = "/tmp/sosig-entry-oversize-source.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  char* source_path = NULL;
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(
          write_fixture_file(root_dir, "content/hello.md", "+++\ntitle = \"Hello\"\n+++\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }
  source_path = str_cstr(path_join(str_from_cstr(root_dir), S("content/hello.md"), &arena), &arena);
  if (!TEST_CHECK(truncate(source_path, source_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s\n", expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A source larger than the limit the build passes, such as one that grew since the walk sized the
// worker's memory, fails its own job at the read and leaves the worker's `perm` untouched.
static void test_rejects_source_over_job_limit(void) {
  char root_dir[] = "/tmp/sosig-entry-job-limit.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  enum { SOURCE_LEN_MAX = 8 };
  static const char content[] = "+++\ntitle = \"Hello\"\n+++\nBody\n";
  char expected[EXPECTED_SIZE];
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  struct JobArenas* worker_arenas = test_job_arenas(1, &perm);
  char* perm_beg = worker_arenas[0].perm.beg;
  struct ContentEntry* entry = NULL;
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "failed to read content: exceeds max file size (%d bytes) at "
                                    "%zu bytes ('content/hello.md')",
                                    SOURCE_LEN_MAX, sizeof(content) - 1);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(parse_source_in_dir(root_dir, "content/hello.md", SOURCE_LEN_MAX, worker_arenas,
                                 &entry, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s\n", expected));
  TEST_CHECK(entry == NULL && worker_arenas[0].perm.beg == perm_beg);

cleanup:
  remove_fixture_tree(root_dir);
}

// Two failing sources each append one line ending in a newline, with nothing before or between
// them. `entry_renderer.h` states this as errors in source order, as `job_run` collects them, and a
// single-source failure cannot observe it at all. `test_reports_write_failure_per_entry` in
// `src/build/test_page_renderer.c` pins the same for failures in the page phase, which runs only
// after this phase passes. This one pins it at the module boundary, where the contract is
// documented.
static void test_appends_one_error_line_per_failing_source(void) {
  char root_dir[] = "/tmp/sosig-entry-error-lines.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  // Both sources omit the required `title`, so each fails in the parse phase with its own message.
  const char first[] =
      "+++\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char second[] =
      "+++\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "Body\n";
  static const char* const sources[] = {"content/a.md", "content/b.md"};
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[2] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/a.md", first) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/b.md", second) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_sources(root_dir, sources, sizeof(sources) / sizeof(sources[0]), &site_config,
                            &source_paths, source_entries, &log) == -1);
  // Compared whole, with the separator in the middle: a dropped separator, a lost line, or a
  // leading or trailing newline each change these bytes.
  TEST_CHECK(test_log_is(&log,
                         "missing required frontmatter key 'title' (in 'content/a.md')\n"
                         "missing required frontmatter key 'title' (in 'content/b.md')\n"));

cleanup:
  remove_fixture_tree(root_dir);
}

// The whole expanded path is capped, not only each segment, so a pattern that is legal segment by
// segment still fails here. Config load does not measure a pattern, so this per-entry check is the
// one limit on the expanded path.
static void test_rejects_oversize_output_path(void) {
  // A long but URL-safe literal segment clears `path_is_safe_relative` and reaches the length cap.
  char pattern[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(pattern, 'a', sizeof(pattern) - 1);
  pattern[sizeof(pattern) - 1] = '\0';
  pattern[0] = '/';
  memcpy(pattern + sizeof(pattern) - sizeof("/{slug}.html"), "/{slug}.html",
         sizeof("/{slug}.html"));
  // The relative output path is the pattern without its leading `/`, with the slug of
  // `content/hello.md` in place of `{slug}`.
  char expected_path[sizeof(pattern)];
  const int expected_path_len =
      snprintf(expected_path, sizeof(expected_path), "%.*s/hello.html",
               (int)(sizeof(pattern) - sizeof("/{slug}.html") - 1), pattern + 1);
  TEST_ASSERT(expected_path_len > 0 && expected_path_len < (int)sizeof(expected_path));
  // Config load accepts the pattern, because only an expanded path is measured.
  char config[OUTPUT_PATH_RELATIVE_LEN_MAX + 256];
  const int config_len =
      snprintf(config, sizeof(config), "%spermalink = \"%s\"\n", SITE_CONFIG, pattern);
  TEST_ASSERT(config_len > 0 && config_len < (int)sizeof(config));
  char root_dir[] = "/tmp/sosig-entry-oversize-path.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'content/hello.md' exceeds max output path length (%d "
                         "bytes) at %d bytes: '%s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, expected_path_len, expected_path));
  TEST_CHECK(log.errors == 1);

cleanup:
  remove_fixture_tree(root_dir);
}

// A segment that overflows once a slug replaces its token is rejected per entry, with a diagnostic
// naming the limit and the segment, even though the whole path is under the output-path cap.
static void test_rejects_oversize_path_segment(void) {
  // A maximum-length slug plus the pattern's literal suffix, which together exceed
  // `FILENAME_LEN_MAX` while the total path stays inside `OUTPUT_PATH_RELATIVE_LEN_MAX`, so only
  // the per-segment check can reject this.
  static const char suffix[] = "-overflow.html";
  enum { SLUG_LEN = FILENAME_LEN_MAX - (int)(sizeof(".html") - 1) };
  char slug[SLUG_LEN + 1];
  memset(slug, 'a', sizeof(slug) - 1);
  slug[sizeof(slug) - 1] = '\0';
  char segment[SLUG_LEN + sizeof(suffix)];
  const int segment_len = snprintf(segment, sizeof(segment), "%s%s", slug, suffix);
  TEST_ASSERT(segment_len > 0 && segment_len < (int)sizeof(segment));
  _Static_assert(SLUG_LEN + sizeof(suffix) - 1 > FILENAME_LEN_MAX,
                 "the slug plus the pattern suffix must exceed the filename limit");
  _Static_assert(SLUG_LEN + sizeof(suffix) - 1 < OUTPUT_PATH_RELATIVE_LEN_MAX,
                 "the oversize segment must stay within the total output path limit");
  char content[SLUG_LEN + 128];
  const int content_len = snprintf(content, sizeof(content),
                                   "+++\n"
                                   "title = \"Hello\"\n"
                                   "date = 2026-07-01T00:00:00Z\n"
                                   "slug = \"%s\"\n"
                                   "+++\n"
                                   "Body\n",
                                   slug);
  TEST_ASSERT(content_len > 0 && content_len < (int)sizeof(content));
  char root_dir[] = "/tmp/sosig-entry-oversize-segment.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "permalink = \"/{slug}-overflow.html\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                                  source_entries, &log) == -1);
  // It is asserted in full, including the offending segment it names: a length alone would leave
  // the user no way to find which segment of the path was too long.
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'content/hello.md' exceeds max filename length "
                         "(%d bytes) at %zu bytes: '%s'\n",
                         FILENAME_LEN_MAX, strlen(segment), segment));

cleanup:
  remove_fixture_tree(root_dir);
}

// A source with no frontmatter fence fails in `frontmatter_split`, which names the source after the
// cause, like the `frontmatter_parse` errors.
static void test_reports_missing_frontmatter_fence(void) {
  char root_dir[] = "/tmp/sosig-entry-fence.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  if (!TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", "no fence here\n") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/content.html",
                                     "<main>{{title}}</main>\n") == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "missing opening '+++' frontmatter fence (in 'content/hello.md')\n"));

cleanup:
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"renders entry metadata and html", test_renders_entry_metadata_and_html},
    {"skips draft entry", test_skips_draft_entry},
    {"keeps perm only for a published entry", test_keeps_perm_only_for_a_published_entry},
    {"verbose prints one progress line per job", test_verbose_prints_one_progress_line_per_job},
    {"nests output under slugified sections", test_nests_output_under_slugified_sections},
    {"accepts empty site", test_accepts_empty_site},
    {"reports unreadable source", test_reports_unreadable_source},
    {"rejects oversize source", test_rejects_oversize_source},
    {"rejects source over job limit", test_rejects_source_over_job_limit},
    {"appends one error line per failing source", test_appends_one_error_line_per_failing_source},
    {"rejects oversize output path", test_rejects_oversize_output_path},
    {"rejects oversize path segment", test_rejects_oversize_path_segment},
    {"reports missing frontmatter fence", test_reports_missing_frontmatter_fence},
    {NULL, NULL},
};
