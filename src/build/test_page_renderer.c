#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build/entry_renderer.h"
#include "build/page_renderer.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_render_support.h"
#include "test_support.h"

/** Config most fixtures share. It turns off the aggregate and feed phases. */
static const char* const SITE_CONFIG =
    "base_url = \"https://example.com\"\n"
    "title = \"Site\"\n"
    "author = \"Author\"\n"
    "aggregate_templates = []\n"
    "feed_templates = []\n";

/**
 * @brief Checks that a page the page phase wrote holds exactly the expected text.
 *
 * @param root_dir      Fixture root the output path is relative to.
 * @param relative_path Output path relative to `root_dir`.
 * @param expected      Terminated text the page must hold.
 */
static void check_output(const char* root_dir, const char* relative_path, const char* expected) {
  char* data = read_output(root_dir, relative_path);
  TEST_CHECK(data != NULL && strcmp(data, expected) == 0);
  TEST_MSG("output '%s': %s", relative_path, data != NULL ? data : "(unreadable)");
  free(data);
}

/**
 * @brief Writes the shared multi-source page-renderer fixture.
 *
 * @param root_dir         Temporary site root directory.
 * @param content_template Terminated content-template text to write.
 */
static void write_multi_source_fixture(const char* root_dir, const char* content_template) {
  const char newer[] =
      "+++\n"
      "title = \"Newer\"\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "Newer body\n";
  const char older[] =
      "+++\n"
      "title = \"Older\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Older body\n";
  const char draft[] =
      "+++\n"
      "title = \"Draft\"\n"
      "date = 2026-07-03T00:00:00Z\n"
      "draft = true\n"
      "+++\n"
      "Draft body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/newer.md", newer) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/older.md", older) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/draft.md", draft) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", content_template) == 0);
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
  write_multi_source_fixture(root_dir, "[{{site.updated}}]\n");

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct ContentEntry* source_entries[3] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  // The draft is listed last, so the newest date belongs to an entry that is never published.
  static const char* const sources[] = {"content/older.md", "content/newer.md", "content/draft.md"};
  TEST_CHECK(render_sources(root_dir, sources, sizeof(sources) / sizeof(sources[0]), NULL,
                            &site_config, &source_paths, source_entries, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  check_output(root_dir, "public/older.html", "[2026-07-02T00:00:00Z]\n");
  check_output(root_dir, "public/newer.html", "[2026-07-02T00:00:00Z]\n");
  // The draft is parsed but never rendered, so its slot stays `NULL` and it writes no page.
  TEST_CHECK(source_entries[2] == NULL);

  char* draft_page = read_output(root_dir, "public/draft.html");
  TEST_CHECK(draft_page == NULL);
  free(draft_page);

  entry_renderer_free_entries(source_entries, sizeof(source_entries) / sizeof(source_entries[0]));
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
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
  write_multi_source_fixture(root_dir, "{{#content_entries}}[{{title}}]{{/content_entries}}\n");

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct ContentEntry* source_entries[3] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  static const char* const sources[] = {"content/older.md", "content/newer.md", "content/draft.md"};
  TEST_CHECK(render_sources(root_dir, sources, sizeof(sources) / sizeof(sources[0]), NULL,
                            &site_config, &source_paths, source_entries, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  check_output(root_dir, "public/older.html", "[Newer][Older]\n");

  entry_renderer_free_entries(source_entries, sizeof(source_entries) / sizeof(source_entries[0]));
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
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

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "permalink = \"/{section}/{slug}/\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "[{{title}}]\n") == 0);
  enum { SOURCE_COUNT = 32 };
  struct PathList source_paths;
  path_list_init(&source_paths);
  for (int i = 0; i < SOURCE_COUNT; i++) {
    char source_path[64];
    char source[128];
    (void)snprintf(source_path, sizeof(source_path), "content/section/sub/post-%02d.md", i);
    (void)snprintf(source, sizeof(source),
                   "+++\ntitle = \"Post %02d\"\ndate = 2026-07-01T00:00:00Z\n+++\nBody\n", i);
    TEST_CHECK(write_fixture_file(root_dir, source_path, source) == 0);
    TEST_CHECK(path_list_push(&source_paths, source_path) == 0);
  }

  int saved_dir_fd = -1;
  TEST_ASSERT(working_dir_enter(root_dir, &saved_dir_fd) == 0);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  char config_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&site_config, "sosig.toml", config_err, sizeof(config_err)) == 0);
  struct ContentEntry* source_entries[SOURCE_COUNT] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(entry_renderer_render_entries(&site_config, &source_paths, 8, false, source_entries,
                                           &error_buffer) == 0);
  TEST_CHECK(render_pages(&site_config, source_entries, SOURCE_COUNT, 8, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(working_dir_leave(saved_dir_fd) == 0);

  for (int i = 0; i < SOURCE_COUNT; i++) {
    char output_path[64];
    char expected[32];
    (void)snprintf(output_path, sizeof(output_path), "public/section/sub/post-%02d/index.html", i);
    (void)snprintf(expected, sizeof(expected), "[Post %02d]\n", i);
    check_output(root_dir, output_path, expected);
  }

  entry_renderer_free_entries(source_entries, sizeof(source_entries) / sizeof(source_entries[0]));
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  remove_fixture_tree(root_dir);
}

// A missing content template surfaces a per-entry diagnostic in the collected error buffer.
static void test_reports_missing_template(void) {
  char root_dir_template[] = "/tmp/sosig-page-missing-template.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct ContentEntry* source_entries[1] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", NULL, &site_config, &source_paths,
                                  source_entries, &error_buffer) == -1);
  // The diagnostic names the failing entry, the template file that could not be read, and why.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read template: %s ('templates/content.html') (while rendering "
               "'content/hello.md')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact, not by substring: `expected` is the whole message, so a substring check would also pass
  // for that message with something appended to it.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");

  entry_renderer_free_entries(source_entries, sizeof(source_entries) / sizeof(source_entries[0]));
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
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
  write_multi_source_fixture(root_dir, "[{{title}}]\n");
  struct Arena arena;
  arena_init(&arena);
  const char* older_page = path_join(root_dir, "public/older.html", &arena);
  const char* newer_page = path_join(root_dir, "public/newer.html", &arena);
  TEST_ASSERT(older_page != NULL && newer_page != NULL);
  TEST_CHECK(fs_mkdir_p(older_page, NULL, 0) == 0);
  TEST_CHECK(fs_mkdir_p(newer_page, NULL, 0) == 0);
  TEST_CHECK(
      write_fixture_file(root_dir, "content/third.md",
                         "+++\ntitle = \"Third\"\ndate = 2026-06-30T00:00:00Z\n+++\nBody\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct ContentEntry* source_entries[3] = {NULL};
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  static const char* const sources[] = {"content/older.md", "content/newer.md", "content/third.md"};
  TEST_CHECK(render_sources(root_dir, sources, sizeof(sources) / sizeof(sources[0]), NULL,
                            &site_config, &source_paths, source_entries, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  error_system_message(reason, sizeof(reason), EISDIR);
  char expected[ERROR_MESSAGE_SIZE * 2];
  // Diagnostics are collected in job index order, so the two lines follow `sources`.
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to write output: %s (for 'content/older.md', to 'public/older.html')\n"
               "failed to write output: %s (for 'content/newer.md', to 'public/newer.html')",
               reason, reason);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("errors: %s", error_buffer.data != NULL ? error_buffer.data : "");
  check_output(root_dir, "public/third.html", "[Third]\n");

  entry_renderer_free_entries(source_entries, sizeof(source_entries) / sizeof(source_entries[0]));
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"renders site updated in content template", test_renders_site_updated_in_content_template},
    {"iterates content entries in content template",
     test_iterates_content_entries_in_content_template},
    {"writes pages sharing parents concurrently", test_writes_pages_sharing_parents_concurrently},
    {"reports missing template", test_reports_missing_template},
    {"reports write failure per entry", test_reports_write_failure_per_entry},
    {NULL, NULL},
};
