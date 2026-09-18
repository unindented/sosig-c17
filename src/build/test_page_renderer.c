#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build/entry_renderer.h"
#include "build/page_renderer.h"
#include "build/render_job.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/**
 * @brief Releases render jobs and all storage owned by their result slots.
 *
 * @param render_jobs      Render-job array to release. May be `NULL` when the count is zero.
 * @param render_job_count Number of entries in `render_jobs`.
 */
static void free_render_jobs(struct RenderJob* render_jobs, size_t render_job_count) {
  for (size_t i = 0; i < render_job_count; i++) {
    free(render_jobs[i].rendered_html);
    if (render_jobs[i].entry != NULL) {
      content_entry_free(render_jobs[i].entry);
      free(render_jobs[i].entry);
    }
  }
  free(render_jobs);
}

/**
 * @brief Runs the page-render pass over filled render-job slots.
 *
 * @param site_config Configuration used for rendering.
 * @param render_jobs Filled render-job array.
 * @param count       Number of entries in `render_jobs`.
 * @param error_out   Buffer that receives any render diagnostic.
 * @return `0` on success, `-1` on render failure, or `1` on test-plumbing failure.
 */
static int render_pages(const struct SiteConfig* site_config,
                        struct RenderJob* render_jobs,
                        size_t count,
                        struct StringBuffer* error_out) {
  struct ContentEntry** entries = calloc(count > 0 ? count : 1, sizeof(*entries));
  TEST_ASSERT(entries != NULL);
  if (entries == NULL) {
    return 1;
  }
  size_t entry_count = 0;
  for (size_t i = 0; i < count; i++) {
    if (render_jobs[i].entry != NULL) {
      entries[entry_count++] = render_jobs[i].entry;
    }
  }
  content_entry_sort(entries, entry_count);
  const char* site_updated =
      content_entry_latest_date((const struct ContentEntry* const*)entries, entry_count);
  struct RenderJobSet result_set = {.items = render_jobs, .count = count};
  const int rc = page_renderer_render_pages(&result_set, site_config,
                                            (const struct ContentEntry* const*)entries, entry_count,
                                            site_updated, 1, false, error_out);
  free(entries);
  return rc;
}

/**
 * @brief Renders one fixture source through both worker-pool passes.
 *
 * @param root_dir             Fixture root used as the working directory.
 * @param source_relative_path Source path relative to `root_dir`.
 * @param permalink_override   Replacement permalink, or `NULL` to keep the configured value.
 * @param site_config          Configuration populated from the fixture.
 * @param source_paths         Path list populated with the source.
 * @param render_jobs_out      Receives the allocated render-job array.
 * @param error_out            Buffer that receives any render diagnostic.
 * @return `0` on success, `-1` on render failure, or `1` on test-plumbing failure.
 */
static int render_single_source(const char* root_dir,
                                const char* source_relative_path,
                                const char* permalink_override,
                                struct SiteConfig* site_config,
                                struct PathList* source_paths,
                                struct RenderJob** render_jobs_out,
                                struct StringBuffer* error_out) {
  char working_dir[PATH_MAX];
  if (getcwd(working_dir, sizeof(working_dir)) == NULL || chdir(root_dir) != 0) {
    TEST_CHECK(false);
    return 1;
  }

  char config_err[ERROR_MESSAGE_SIZE];
  config_err[0] = '\0';
  int rc = 1;
  if (site_config_load(site_config, "sosig.toml", config_err, sizeof(config_err)) == 0 &&
      path_list_push(source_paths, source_relative_path) == 0) {
    if (permalink_override != NULL) {
      site_config->permalink = permalink_override;
    }
    struct RenderJob* render_jobs = calloc(source_paths->count, sizeof(*render_jobs));
    TEST_ASSERT(render_jobs != NULL);
    if (render_jobs != NULL) {
      rc = entry_renderer_render_entries(
          site_config, source_paths, 1, false,
          &(struct RenderJobSet){.items = render_jobs, .count = source_paths->count}, error_out);
      if (rc == 0) {
        rc = render_pages(site_config, render_jobs, source_paths->count, error_out);
      }
      *render_jobs_out = render_jobs;
    }
  }

  const int restored = chdir(working_dir);
  TEST_CHECK(restored == 0);
  return rc;
}

/**
 * @brief Renders named fixture sources through both worker-pool passes.
 *
 * @param root_dir            Fixture root used as the working directory.
 * @param site_config         Configuration populated from the fixture.
 * @param relative_paths      Source paths relative to `root_dir`.
 * @param relative_path_count Number of entries in `relative_paths`.
 * @param source_paths        Path list populated with the sources.
 * @param render_jobs_out     Receives the allocated render-job array.
 * @param error_out           Buffer that receives any render diagnostic.
 * @return `0` on success, `-1` on render failure, or `1` on test-plumbing failure.
 */
static int render_sources(const char* root_dir,
                          struct SiteConfig* site_config,
                          const char* const* relative_paths,
                          size_t relative_path_count,
                          struct PathList* source_paths,
                          struct RenderJob** render_jobs_out,
                          struct StringBuffer* error_out) {
  char working_dir[PATH_MAX];
  if (getcwd(working_dir, sizeof(working_dir)) == NULL || chdir(root_dir) != 0) {
    TEST_CHECK(false);
    return 1;
  }

  char config_err[ERROR_MESSAGE_SIZE];
  config_err[0] = '\0';
  int rc = 1;
  if (site_config_load(site_config, "sosig.toml", config_err, sizeof(config_err)) == 0) {
    rc = 0;
    for (size_t i = 0; rc == 0 && i < relative_path_count; i++) {
      rc = path_list_push(source_paths, relative_paths[i]);
    }
    struct RenderJob* render_jobs =
        rc != 0 ? NULL : calloc(source_paths->count, sizeof(*render_jobs));
    if (source_paths->count > 0) {
      TEST_ASSERT(render_jobs != NULL);
    }
    if (render_jobs == NULL) {
      rc = 1;
    } else {
      rc = entry_renderer_render_entries(
          site_config, source_paths, 1, false,
          &(struct RenderJobSet){.items = render_jobs, .count = source_paths->count}, error_out);
      if (rc == 0) {
        rc = render_pages(site_config, render_jobs, source_paths->count, error_out);
      }
      *render_jobs_out = render_jobs;
    }
  }

  const int restored = chdir(working_dir);
  TEST_CHECK(restored == 0);
  return rc;
}

/** * @brief Removes a fixture tree entry.
 *
 * @param path      Path of the visited entry.
 * @param st        Stat buffer `nftw` filled. Unused.
 * @param type_flag Entry type `nftw` determined.
 * @param ftw       Traversal state `nftw` maintains. Unused.
 * @return `0` to continue the walk.
 */
/**
 * @brief Writes the shared multi-source page-renderer fixture.
 *
 * @param root_dir         Temporary site root directory.
 * @param content_template Terminated content-template text to write.
 */
static void write_multi_source_fixture(const char* root_dir, const char* content_template) {
  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
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
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/newer.md", newer) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/older.md", older) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/draft.md", draft) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", content_template) == 0);
}

// A content template resolves `site.updated` to the newest entry's date, not to nothing. The page
// pass runs after the build parses and sorts every source, so the value can exist: the date belongs
// to a different entry than the one being rendered. The end-to-end assertion lives in the golden
// test suite.
static void test_renders_site_updated_in_content_template(void) {
  char root_dir_template[] = "/tmp/sosig-render-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  write_multi_source_fixture(root_dir, "[{{site.updated}}]\n");

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  // The draft is listed last, so the newest date belongs to an entry that is never published.
  static const char* const sources[] = {"content/older.md", "content/newer.md", "content/draft.md"};
  TEST_CHECK(render_sources(root_dir, &site_config, sources, sizeof(sources) / sizeof(sources[0]),
                            &source_paths, &render_jobs, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  if (render_jobs != NULL) {
    TEST_CHECK(render_jobs[0].rendered_html != NULL &&
               strcmp(render_jobs[0].rendered_html, "[2026-07-02T00:00:00Z]\n") == 0);
    TEST_CHECK(render_jobs[1].rendered_html != NULL &&
               strcmp(render_jobs[1].rendered_html, "[2026-07-02T00:00:00Z]\n") == 0);
    // The draft is parsed but never rendered, so its slot stays empty.
    TEST_CHECK(render_jobs[2].entry == NULL);
    TEST_CHECK(render_jobs[2].rendered_html == NULL);
  }

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  remove_fixture_tree(root_dir);
}

// A content template iterates `content_entries` newest-first, and a draft is absent from it. Both
// follow from the page pass running after the collect-and-sort step.
static void test_iterates_content_entries_in_content_template(void) {
  char root_dir_template[] = "/tmp/sosig-render-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  write_multi_source_fixture(root_dir, "{{#content_entries}}[{{title}}]{{/content_entries}}\n");

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  static const char* const sources[] = {"content/older.md", "content/newer.md", "content/draft.md"};
  TEST_CHECK(render_sources(root_dir, &site_config, sources, sizeof(sources) / sizeof(sources[0]),
                            &source_paths, &render_jobs, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  if (render_jobs != NULL) {
    TEST_CHECK(render_jobs[0].rendered_html != NULL &&
               strcmp(render_jobs[0].rendered_html, "[Newer][Older]\n") == 0);
  }

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  remove_fixture_tree(root_dir);
}

// A missing content template surfaces a per-entry diagnostic in the collected error buffer.
static void test_reports_missing_template(void) {
  char root_dir_template[] = "/tmp/sosig-render-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/hello.md", content) == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(root_dir, "content/hello.md", NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) == -1);
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

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"renders site updated in content template", test_renders_site_updated_in_content_template},
    {"iterates content entries in content template",
     test_iterates_content_entries_in_content_template},
    {"reports missing template", test_reports_missing_template},
    {NULL, NULL}};
