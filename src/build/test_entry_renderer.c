#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build/entry_renderer.h"
#include "build/page_renderer.h"
#include "build/render_job.h"
#include "core/arena.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/string_buffer.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"

// Creates a temporary site root for render tests. Returns `NULL` once `TEST_CHECK` has failed the
// test, so a caller's `return` only suppresses cascading noise. The result aliases the caller's
// `dir` rather than being owned.
static const char* init_render_fixture(char root_dir[static 1]) {
  char* tmp = mkdtemp(root_dir);
  TEST_CHECK(tmp != NULL);
  return tmp;
}

// Writes one fixture file relative to the temporary site root. The `NULL, 0` reason arguments are
// the option `fs_write_file`'s contract allows: a failed fixture write is a broken test, not
// behavior under test. Callers assert the non-zero return.
static int write_fixture_file(const char* root_dir,
                              const char* relative_path,
                              const char* contents) {
  struct Arena arena;
  arena_init(&arena);
  char* fixture_path = path_join(root_dir, relative_path, &arena);
  const int rc =
      fixture_path == NULL ? -1 : fs_write_file(fixture_path, contents, strlen(contents), NULL, 0);
  arena_free(&arena);
  return rc;
}

// Releases every result slot and its owned entry and HTML.
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

// Runs both render passes over already-filled result slots, mirroring the order `cmd_build` uses:
// collect the non-draft entries, sort them, derive `site.updated`, then render every page.
static int render_pages(const struct SiteConfig* site_config,
                        struct RenderJob* render_jobs,
                        size_t count,
                        struct StringBuffer* error_out) {
  struct ContentEntry** entries = calloc(count > 0 ? count : 1, sizeof(*entries));
  if (entries == NULL) {
    TEST_CHECK(false);
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

// Loads the fixture config and renders one content source through both worker-pool passes.
// `permalink_override` replaces the loaded `permalink` after validation, or is `NULL` to keep the
// configured one. The override exists so a test can reach the render phase's own path-safety checks
// with a pattern `site_config_load` would reject outright. Runs with the fixture as the current
// directory.
static int render_single_source(const char* root_dir,
                                const char* source_relative_path,
                                const char* permalink_override,
                                struct SiteConfig* site_config,
                                struct PathList* source_paths,
                                struct RenderJob** render_jobs_out,
                                struct StringBuffer* error_out) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL || chdir(root_dir) != 0) {
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

  const int restored = chdir(cwd);
  TEST_CHECK(restored == 0);
  return rc;
}

// Renders every named source under the fixture through both passes, with the fixture as the current
// directory.
static int render_sources(const char* root_dir,
                          struct SiteConfig* site_config,
                          const char* const* relative_paths,
                          size_t relative_path_count,
                          struct PathList* source_paths,
                          struct RenderJob** render_jobs_out,
                          struct StringBuffer* error_out) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL || chdir(root_dir) != 0) {
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

  const int restored = chdir(cwd);
  TEST_CHECK(restored == 0);
  return rc;
}

// Removes the fixture paths used by the single-source tests. The multi-source tests have their own
// list. A test writing any other path unlinks it itself before calling this, since an unlisted file
// makes the final `rmdir` fail and leaves the temporary root behind.
static void cleanup_render_fixture(const char* root_dir) {
  static const char* files[] = {
      "content/hello.md",
      "templates/content.html",
      "sosig.toml",
  };
  static const char* dirs[] = {"content", "templates"};

  struct Arena arena;
  arena_init(&arena);
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    (void)unlink(path_join(root_dir, files[i], &arena));
  }
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    (void)rmdir(path_join(root_dir, dirs[i], &arena));
  }
  arena_free(&arena);
  (void)rmdir(root_dir);
}

// A single source renders its frontmatter metadata, body HTML, page output, and output paths.
static void test_renders_entry_metadata_and_html(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html",
                                "<main>{{title}} {{{body}}}</main>\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, "content/hello.md", NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  if (render_jobs != NULL) {
    const struct ContentEntry* entry = render_jobs[0].entry;
    TEST_CHECK(entry != NULL);
    if (entry != NULL) {
      TEST_CHECK(strcmp(entry->title, "Hello") == 0);
      TEST_CHECK(strcmp(entry->slug, "hello") == 0);
      TEST_CHECK(strstr(entry->body_html, "<p>Body</p>") != NULL);
      TEST_CHECK(strcmp(entry->url_path, "/hello.html") == 0);
      TEST_CHECK(strcmp(entry->output_path, "public/hello.html") == 0);
      TEST_CHECK(strstr(render_jobs[0].rendered_html, "<main>Hello <p>Body</p>") != NULL);
    }
  }

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// A draft source is parsed but leaves an empty result slot with no rendered HTML.
static void test_skips_draft_entry(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
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
      "draft = true\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, "content/hello.md", NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  if (render_jobs != NULL) {
    TEST_CHECK(render_jobs[0].entry == NULL);
    TEST_CHECK(render_jobs[0].rendered_html == NULL);
  }

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// A verbose pass prints one progress dot per finished job, drafts included, then closes the line.
// The dots are written from worker threads under `stderr`'s lock, which is the only hand-written
// locking outside the pool. This drives it with more workers than one so the lock is contended, and
// the `tsan` test preset runs the same path under ThreadSanitizer.
static void test_verbose_prints_one_dot_per_job(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", page) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/second.md", page) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/draft.md", draft) == 0);

  struct PathList source_paths;
  path_list_init(&source_paths);
  TEST_CHECK(path_list_push(&source_paths, "content/hello.md") == 0);
  TEST_CHECK(path_list_push(&source_paths, "content/second.md") == 0);
  TEST_CHECK(path_list_push(&source_paths, "content/draft.md") == 0);
  // Allocated before the working directory changes so the guard below has nothing to unwind.
  struct RenderJob* render_jobs = calloc(source_paths.count, sizeof(*render_jobs));
  TEST_ASSERT(render_jobs != NULL);
  if (render_jobs == NULL) {
    path_list_free(&source_paths);
    return;
  }

  char cwd[PATH_MAX];
  TEST_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL);
  TEST_ASSERT(chdir(tmp) == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  char config_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&site_config, "sosig.toml", config_err, sizeof(config_err)) == 0);
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  // Redirect `stderr` to a file for the duration of the pass, then put the real one back. The dots
  // are flushed as they are written, so the capture is complete once the pass returns.
  (void)fflush(stderr);
  const int saved_stderr = dup(STDERR_FILENO);
  TEST_ASSERT(saved_stderr >= 0);
  TEST_ASSERT(freopen("progress.txt", "w", stderr) != NULL);
  struct RenderJobSet result_set = {.items = render_jobs, .count = source_paths.count};
  const int rc = entry_renderer_render_entries(&site_config, &source_paths, 4, true, &result_set,
                                               &error_buffer);
  (void)fflush(stderr);
  TEST_CHECK(dup2(saved_stderr, STDERR_FILENO) >= 0);
  (void)close(saved_stderr);
  clearerr(stderr);

  TEST_CHECK(rc == 0);
  char* progress = NULL;
  size_t progress_len = 0;
  TEST_CHECK(fs_read_file("progress.txt", &progress, &progress_len, NULL, 0) == 0);
  // Three jobs, so three dots even though one entry is a draft that produces no output, and one
  // trailing newline closing the line before any later status message.
  TEST_CHECK(progress != NULL && strcmp(progress, "...\n") == 0);
  free(progress);
  (void)unlink("progress.txt");

  const int restored = chdir(cwd);
  TEST_CHECK(restored == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  struct Arena arena;
  arena_init(&arena);
  (void)unlink(path_join(tmp, "content/second.md", &arena));
  (void)unlink(path_join(tmp, "content/draft.md", &arena));
  arena_free(&arena);
  cleanup_render_fixture(tmp);
}

// A source nested two directories deep publishes under both slugified section segments, joined by a
// single `/`. One level of nesting cannot show this. The join loop runs once, so a test with one
// level cannot observe the separator insertion or the per-segment slugification of anything beyond
// the first segment. Both directory names here need slugifying, so a join that skipped the
// separator would publish `my-sectionsub-dir` as one directory.
static void test_nests_output_under_slugified_sections(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
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
  const char* const source_relative_path = "content/My Section/Sub Dir/hello.md";
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, source_relative_path, content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, source_relative_path, NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  if (render_jobs != NULL) {
    const struct ContentEntry* entry = render_jobs[0].entry;
    TEST_CHECK(entry != NULL);
    if (entry != NULL) {
      TEST_CHECK(strcmp(entry->url_path, "/my-section/sub-dir/hello.html") == 0);
      TEST_CHECK(strcmp(entry->output_path, "public/my-section/sub-dir/hello.html") == 0);
    }
  }

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  struct Arena nested_arena;
  arena_init(&nested_arena);
  (void)unlink(path_join(tmp, source_relative_path, &nested_arena));
  (void)rmdir(path_join(tmp, "content/My Section/Sub Dir", &nested_arena));
  (void)rmdir(path_join(tmp, "content/My Section", &nested_arena));
  arena_free(&nested_arena);
  cleanup_render_fixture(tmp);
}

// Both render passes accept an empty site: a result set whose `count` is 0 and whose `items` is
// `NULL`. That is the shape `cmd_build` hands over when `calloc(0, ...)` returns `NULL` for a
// content directory with no sources. A count guard before `memset` makes this safe, because
// `memset` requires a non-`NULL` pointer even for zero bytes. Removing the guard leaves every other
// test green while an empty site becomes undefined behavior, so these two calls are the only thing
// pinning it. Under the sanitizer test build they fail loudly rather than silently.
static void test_accepts_empty_site(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  struct RenderJobSet empty = {.items = NULL, .count = 0};
  TEST_CHECK(entry_renderer_render_entries(&site_config, &source_paths, 1, false, &empty,
                                           &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_CHECK(page_renderer_render_pages(&empty, &site_config, NULL, 0, "1970-01-01T00:00:00Z", 1,
                                        false, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);

  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
}

// A source outside `<content_dir>/` fails its own job. Otherwise, the complete path would become
// its section and add the content directory name to the URL. The test uses two path forms. One
// shares no prefix with `content_dir`. The other shares the prefix but has no `/` boundary after
// it. A prefix-only test would accept the second form and produce an empty section. `cmd_build`
// cannot produce either form because its walk starts at `content_dir`. A caller that supplies a
// source list can produce them, so this function checks the prefix.
static void test_rejects_source_outside_content_dir(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  // Both sources parse cleanly, so each reaches the path-finalizing step where the check lives.
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "contentx/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  static const char* const sources[] = {"hello.md", "contentx/hello.md"};
  TEST_CHECK(render_sources(tmp, &site_config, sources, sizeof(sources) / sizeof(sources[0]),
                            &source_paths, &render_jobs, &error_buffer) != 0);
  // Compared whole, so a message that merely mentions the source path cannot pass for this one.
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "content source must be under the configured 'content_dir': 'hello.md'\n"
                    "content source must be under the configured 'content_dir': "
                    "'contentx/hello.md'") == 0);
  // No entry was published, so nothing downstream can read a section derived from a bad path.
  if (render_jobs != NULL) {
    TEST_CHECK(render_jobs[0].entry == NULL);
    TEST_CHECK(render_jobs[1].entry == NULL);
  }

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  struct Arena arena;
  arena_init(&arena);
  (void)unlink(path_join(tmp, "hello.md", &arena));
  (void)unlink(path_join(tmp, "contentx/hello.md", &arena));
  (void)rmdir(path_join(tmp, "contentx", &arena));
  arena_free(&arena);
  cleanup_render_fixture(tmp);
}

// A permalink that expands to a path escaping the output directory is rejected by the render phase.
// `site_config_load` rejects such a pattern outright, so this reaches the render phase's own check
// by overriding `permalink` after the load. The check is kept as defense in depth: it guards the
// expanded path, which config validation can only reason about indirectly. Rejection at load is
// covered by `test_load_rejects_unsafe_permalink` in the site config tests.
static void test_rejects_unsafe_output_path(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, "content/hello.md", "/../{slug}.html", &site_config,
                                  &source_paths, &render_jobs, &error_buffer) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "permalink expanded to an unsafe output path for '%s': '%s'",
                         "content/hello.md", "../hello.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Exact: `expected` is the whole message.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// The whole expanded path is capped, not only each segment, so a pattern that is legal segment by
// segment still fails here. This is the render phase's backstop: `site_config_load` rejects the
// same pattern up front, asserted by `test_load_rejects_oversize_permalink`.
static void test_rejects_oversize_output_path(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // A long but URL-safe literal segment clears `path_is_safe_relative` and reaches the length cap.
  // Supplied as the override rather than in the config, because `site_config_load` rejects a
  // pattern this long up front. This test covers the render phase's per-entry backstop, which stays
  // reachable through a long slug or a deep section.
  char pattern[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(pattern, 'a', sizeof(pattern) - 1);
  pattern[sizeof(pattern) - 1] = '\0';
  pattern[0] = '/';
  memcpy(pattern + sizeof(pattern) - sizeof("/{slug}.html"), "/{slug}.html",
         sizeof("/{slug}.html"));

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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, "content/hello.md", pattern, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) != 0);
  // The limit leads and the offending path trails, so the limit clause survives even though the
  // whole message is far longer than the diagnostic buffer. The measured length varies with the
  // pattern, so the head is asserted up to it and the entry is asserted separately. Between them
  // they pin every part of the message that is not the pathological value itself.
  char expected_head[128];
  const int expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "output path exceeds max output path length (%zu bytes) at ",
               (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(error_buffer.data != NULL &&
             strncmp(error_buffer.data, expected_head, (size_t)expected_head_len) == 0);
  TEST_CHECK(error_buffer.data != NULL &&
             strstr(error_buffer.data, " bytes for 'content/hello.md': '") != NULL);
  // The cut is marked, as in `test_reports_frontmatter_reason_before_long_source_path`. The exact
  // length proves that this message still truncates. If a reworded message fit, the marker check
  // could pass for the wrong reason. Text could also follow the message without detection because
  // the two checks above use a prefix and a floating fragment. `TRUNCATION_MARKER` is file-local,
  // so this test spells out `...`. A marker change must update this test.
  TEST_CHECK(error_buffer.len == ERROR_MESSAGE_SIZE - 1);
  TEST_CHECK(error_buffer.data != NULL && error_buffer.len >= 3 &&
             strcmp(error_buffer.data + error_buffer.len - 3, "...") == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// A single path segment past the filename limit is rejected even though the whole path is well
// under the output-path cap, so an overlong slug fails with a diagnostic naming the limit instead
// of an opaque write error from the filesystem later.
//
// Reached through a real slug rather than a long literal in the permalink, because a literal cannot
// get here at all: `site_config_load` runs the same per-segment check over the pattern's own
// expansions, so an overlong literal is a config error. What survives for this backstop is the part
// the pattern alone cannot settle: a segment that only overflows once a real slug replaces the
// six-byte sample. `<slug>.html` is exactly `FILENAME_LEN_MAX` by construction, so the overflow
// needs a literal suffix on top of a maximum-length slug.
static void test_rejects_oversize_path_segment(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // A maximum-length slug plus the pattern's literal suffix, which together exceed
  // `FILENAME_LEN_MAX` while the total path stays inside `OUTPUT_PATH_RELATIVE_LEN_MAX`, so only
  // the per-segment check can reject this.
  static const char suffix[] = "-overflow.html";
  char slug[SLUG_LEN_MAX + 1];
  memset(slug, 'a', sizeof(slug) - 1);
  slug[sizeof(slug) - 1] = '\0';
  char segment[SLUG_LEN_MAX + sizeof(suffix)];
  const int segment_len = snprintf(segment, sizeof(segment), "%s%s", slug, suffix);
  TEST_CHECK(segment_len > 0 && (size_t)segment_len < sizeof(segment));
  _Static_assert(SLUG_LEN_MAX + sizeof(suffix) - 1 > FILENAME_LEN_MAX,
                 "the slug plus the pattern suffix must exceed the filename limit");
  _Static_assert(SLUG_LEN_MAX + sizeof(suffix) - 1 < OUTPUT_PATH_RELATIVE_LEN_MAX,
                 "the oversize segment must stay within the total output path limit");

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "permalink = \"/{slug}-overflow.html\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  char content[SLUG_LEN_MAX + 128];
  const int n = snprintf(content, sizeof(content),
                         "+++\n"
                         "title = \"Hello\"\n"
                         "date = 2026-07-01T00:00:00Z\n"
                         "slug = \"%s\"\n"
                         "+++\n"
                         "Body\n",
                         slug);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(content));
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, "content/hello.md", NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) != 0);
  // The whole message fits, so it is asserted in full, including the offending segment it names: a
  // length alone would leave the user no way to find which segment of the path was too long.
  char expected[ERROR_MESSAGE_SIZE];
  const int n2 =
      snprintf(expected, sizeof(expected),
               "output path segment exceeds max filename length (%zu bytes) at %zu bytes "
               "for 'content/hello.md': '%s'",
               (size_t)FILENAME_LEN_MAX, strlen(segment), segment);
  TEST_CHECK(n2 > 0 && (size_t)n2 < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// A source with no frontmatter fence fails in `frontmatter_split`, and its reason is composed with
// the path behind the cause. That is the same ordering the `frontmatter_parse` branch below it
// already pins. The one the module comment requires so a long path cannot push the cause out of the
// buffer. Without this case only the parse branch is covered, and an inversion to
// `in '<path>': <reason>` breaks no test.
static void test_reports_missing_frontmatter_fence(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", "no fence here\n") == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "<main>{{title}}</main>\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, "content/hello.md", NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) != 0);
  TEST_ASSERT(error_buffer.data != NULL);
  TEST_CHECK(strcmp(error_buffer.data,
                    "missing opening '+++' frontmatter fence (in 'content/hello.md')") == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// The four `out of memory` render diagnostics in `entry_renderer.c` have no test here, and cannot.
// That gap is not local to this file: it covers every `out of memory` diagnostic in the tree, and
// is recorded once at `ERROR_MESSAGE_SIZE` in `core/error.h` with the reason.

// A frontmatter reason survives in full when a long source path overflows the diagnostic, and the
// overflow itself is marked rather than silent.
static void test_reports_frontmatter_reason_before_long_source_path(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // A stem one byte over `SLUG_LEN_MAX` gives a reason that already carries the path, so composing
  // it behind the path a second time is what overflows `ERROR_MESSAGE_SIZE` and cuts it.
  char stem[SLUG_LEN_MAX + 3];
  memset(stem, 'a', sizeof(stem) - 1);
  stem[sizeof(stem) - 1] = '\0';
  char source_relative_path[SLUG_LEN_MAX + 32];
  int n = snprintf(source_relative_path, sizeof(source_relative_path), "content/%s.md", stem);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(source_relative_path));
  _Static_assert(SLUG_LEN_MAX + 2 <= FILENAME_LEN_MAX,
                 "the oversize stem must still be a legal filename");

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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, source_relative_path, content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  TEST_CHECK(render_single_source(tmp, source_relative_path, NULL, &site_config, &source_paths,
                                  &render_jobs, &error_buffer) != 0);
  // The reason leads, so the clause naming the limit and the measured length is present even though
  // the composed message is longer than the buffer. The prefix assertion verifies this ordering.
  // The slug itself trails, so truncation removes the value rather than the reason. The source path
  // is appended once by the wrapping caller rather than twice: naming it here as well would push
  // this clause out of the buffer for any source path over roughly 465 bytes. The measured length
  // is the stem's, since every byte of the stem slugifies to itself.
  char expected_tail[80];
  n = snprintf(expected_tail, sizeof(expected_tail),
               "slug exceeds max slug length (%zu bytes) at %zu bytes: '", (size_t)SLUG_LEN_MAX,
               sizeof(stem) - 1);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_tail));
  TEST_CHECK(error_buffer.data != NULL && strstr(error_buffer.data, expected_tail) != NULL);

  // And the cut is marked. Filling the buffer exactly is what proves this case still truncates at
  // all: a reworded diagnostic that fits would leave the marker assertion below passing for the
  // wrong reason, so the length is asserted first and fails loudly instead. Without the marker a
  // cut path reads as a whole one and names a file that does not exist, which is the failure
  // `error_report_va` exists to prevent. A render path formatting with a bare `vsnprintf` would
  // reach exactly that. The `...` is spelled out because `TRUNCATION_MARKER` is file-local to
  // `core/error.c`. Changing it there must update this line.
  TEST_CHECK(error_buffer.len == ERROR_MESSAGE_SIZE - 1);
  TEST_CHECK(error_buffer.data != NULL && error_buffer.len >= 3 &&
             strcmp(error_buffer.data + error_buffer.len - 3, "...") == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  struct Arena arena;
  arena_init(&arena);
  (void)unlink(path_join(tmp, source_relative_path, &arena));
  arena_free(&arena);
  cleanup_render_fixture(tmp);
}

// A source that exists but cannot be read is reported with the system cause and the path it
// happened on, rather than as a generic parse failure.
static void test_reports_unreadable_source(void) {
  // Root bypasses the permission bits, so the source would read fine and the assertion below would
  // fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct Arena arena;
  arena_init(&arena);
  char* source_path = path_join(tmp, "content/hello.md", &arena);
  TEST_CHECK(source_path != NULL && chmod(source_path, 0) == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const int rc = render_single_source(tmp, "content/hello.md", NULL, &site_config, &source_paths,
                                      &render_jobs, &error_buffer);
  // Restore the mode before asserting: a failing assertion aborts the test, and the cleanup below
  // cannot unlink through a path it may not read.
  if (source_path != NULL) {
    (void)chmod(source_path, 0644);
  }
  arena_free(&arena);

  TEST_CHECK(rc != 0);
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(EACCES));
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "failed to read content: %s ('content/hello.md')", reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Exact: the path trails the cause, which is the shape an errno-sourced failure takes, and only a
  // whole comparison catches a reordering that puts the path first.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  cleanup_render_fixture(tmp);
}

// Two failing sources append one diagnostic line each, separated by exactly one newline, with none
// leading or trailing the buffer. That separator placement is what the "one per line" contract in
// `entry_renderer.h` means, and a single-source failure cannot observe it at all. The build-level
// counterpart is `test_reports_one_line_per_failing_entry` in `src/app/test_cmd_build.c`. This one
// pins it at the module boundary, where the contract is documented.
static void test_appends_one_error_line_per_failing_source(void) {
  char dir[] = "/tmp/sosig-render-test.XXXXXX";
  const char* tmp = init_render_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  // Both sources omit the required `title`, so each fails in the parse phase with its own
  // message.
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/a.md", first) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/b.md", second) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths;
  path_list_init(&source_paths);
  struct RenderJob* render_jobs = NULL;
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  static const char* const sources[] = {"content/a.md", "content/b.md"};
  TEST_CHECK(render_sources(tmp, &site_config, sources, sizeof(sources) / sizeof(sources[0]),
                            &source_paths, &render_jobs, &error_buffer) != 0);
  // Compared whole, with the separator in the middle: a dropped separator, a lost line, or a
  // leading or trailing newline each change these bytes.
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "missing required frontmatter key 'title' (in 'content/a.md')\n"
                    "missing required frontmatter key 'title' (in 'content/b.md')") == 0);

  free_render_jobs(render_jobs, source_paths.count);
  string_buffer_free(&error_buffer);
  path_list_free(&source_paths);
  site_config_free(&site_config);
  struct Arena arena;
  arena_init(&arena);
  (void)unlink(path_join(tmp, "content/a.md", &arena));
  (void)unlink(path_join(tmp, "content/b.md", &arena));
  arena_free(&arena);
  cleanup_render_fixture(tmp);
}

TEST_LIST = {
    {"renders entry metadata and html", test_renders_entry_metadata_and_html},
    {"skips draft entry", test_skips_draft_entry},
    {"verbose prints one dot per job", test_verbose_prints_one_dot_per_job},
    {"nests output under slugified sections", test_nests_output_under_slugified_sections},
    {"accepts empty site", test_accepts_empty_site},
    {"rejects source outside content dir", test_rejects_source_outside_content_dir},
    {"rejects unsafe output path", test_rejects_unsafe_output_path},
    {"rejects oversize output path", test_rejects_oversize_output_path},
    {"rejects oversize path segment", test_rejects_oversize_path_segment},
    {"reports missing frontmatter fence", test_reports_missing_frontmatter_fence},
    {"reports frontmatter reason before long source path",
     test_reports_frontmatter_reason_before_long_source_path},
    {"reports unreadable source", test_reports_unreadable_source},
    {"appends one error line per failing source", test_appends_one_error_line_per_failing_source},
    {NULL, NULL}};
