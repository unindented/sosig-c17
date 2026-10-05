#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cmd_build.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/** Largest generated file a test reads back, in bytes. Fixture outputs are a few kilobytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/** Config body every fixture shares. It turns off the aggregate and feed passes. */
static const char* const SITE_CONFIG =
    "base_url = \"https://example.com\"\n"
    "title = \"Site\"\n"
    "author = \"Author\"\n"
    "aggregate_templates = []\n"
    "feed_templates = []\n";

/** Minimal content template, which renders a page as its body alone. */
static const char* const CONTENT_TEMPLATE = "{{{body}}}\n";

/** One published content entry whose body renders as `<p>Body</p>`. */
static const char* const HELLO_ENTRY =
    "+++\n"
    "title = \"Hello\"\n"
    "date = 2026-07-01T00:00:00Z\n"
    "+++\n"
    "Body\n";

/**
 * @brief Reads one generated file below a fixture root.
 *
 * @param root_dir               Fixture root directory.
 * @param relative_path          Relative generated-file path below `root_dir`.
 * @param generated_file_out     Receives allocated file contents on success.
 * @param generated_file_len_out Receives the content length in bytes on success.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int read_fixture_file(const char* root_dir,
                             const char* relative_path,
                             char** generated_file_out,
                             size_t* generated_file_len_out) {
  struct Arena arena;
  arena_init(&arena);
  char* fixture_path = path_join(root_dir, relative_path, &arena);
  const int rc = fixture_path == NULL
                     ? -1
                     : fs_read_file(fixture_path, TEST_FILE_LEN_MAX, generated_file_out,
                                    generated_file_len_out, NULL, 0);
  arena_free(&arena);
  return rc;
}

/**
 * @brief Writes `sosig.toml` and the content template below a fixture root.
 *
 * @param root_dir     Fixture root directory.
 * @param config_extra Config lines placed before the shared body, or `NULL` for the body alone. It
 *                     must not repeat a key the body sets, because TOML rejects a duplicate key.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_site_fixture(const char* root_dir, const char* config_extra) {
  if (write_fixture_file(root_dir, "templates/content.html", CONTENT_TEMPLATE) != 0) {
    return -1;
  }
  if (config_extra == NULL) {
    return write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG);
  }
  struct StringBuffer config;
  string_buffer_init(&config);
  int rc = string_buffer_append(&config, config_extra) != 0 ||
                   string_buffer_append(&config, SITE_CONFIG) != 0
               ? -1
               : 0;
  if (rc == 0) {
    rc = write_fixture_file(root_dir, "sosig.toml", config.data);
  }
  string_buffer_free(&config);
  return rc;
}

/**
 * @brief Executes a build in a fixture directory and collects its diagnostic.
 *
 * The build resolves `sosig.toml` from the working directory, so this changes into the fixture and
 * restores the previous directory before returning.
 *
 * @param root_dir  Fixture directory in which to execute the build.
 * @param error_out Buffer that receives any build diagnostic.
 * @return `0` on success, `-1` on build failure, or `TEST_PLUMBING_FAILED` on plumbing failure.
 */
static int execute_build_in_dir(const char* root_dir, struct StringBuffer* error_out) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return TEST_PLUMBING_FAILED;
  }
  const struct BuildOptions options = {0};
  const int rc = cmd_build_execute(&options, error_out);
  return working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
}

/**
 * @brief Runs a build while capturing both standard streams.
 *
 * Restores both streams and the working directory before returning.
 *
 * @param root_dir       Fixture directory in which to run the build.
 * @param options        Build options passed to `cmd_build_run`.
 * @param stdout_out     Buffer that receives terminated standard output.
 * @param stdout_out_len Size of `stdout_out` in bytes. Must be non-zero.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode run_build_capturing(const char* root_dir,
                                         const struct BuildOptions* options,
                                         char* stdout_out,
                                         size_t stdout_out_len,
                                         char* stderr_out,
                                         size_t stderr_out_len) {
  stdout_out[0] = '\0';
  stderr_out[0] = '\0';
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return (enum ExitCode)TEST_PLUMBING_FAILED;
  }

  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cmd_build_run(options);
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, stderr_out_len) != 0;
    }
    has_plumbing_failed =
        capture_end(&stdout_capture, stdout_out, stdout_out_len) != 0 || has_plumbing_failed;
  }
  has_plumbing_failed = working_dir_leave(saved_dir_fd) != 0 || has_plumbing_failed;
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

/**
 * @brief Reports whether a path list contains a fixture-relative path.
 *
 * @param outputs       Path list to search.
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative path to join to `root_dir`.
 * @return `true` when the joined path is present, or `false` otherwise.
 */
static bool has_output_path(const struct PathList* outputs,
                            const char* root_dir,
                            const char* relative_path) {
  struct Arena arena;
  arena_init(&arena);
  char* want = path_join(root_dir, relative_path, &arena);
  bool found = false;
  for (size_t i = 0; want != NULL && !found && i < outputs->count; i++) {
    found = strcmp(outputs->items[i], want) == 0;
  }
  arena_free(&arena);
  return found;
}

/**
 * @brief Reports whether a path below a fixture root exists.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative path below `root_dir` to inspect.
 * @return `true` when the path exists, or `false` otherwise.
 */
static bool fixture_path_exists(const char* root_dir, const char* relative_path) {
  struct Arena arena;
  arena_init(&arena);
  char* path = path_join(root_dir, relative_path, &arena);
  const bool exists = path != NULL && access(path, F_OK) == 0;
  arena_free(&arena);
  return exists;
}

// A configured content template name is used instead of the default.
static void test_honors_configured_content_template(void) {
  char root_dir_template[] = "/tmp/sosig-build-content-template.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "content_template = \"content-entry.html\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/content-entry.html",
                                 "<main>{{title}} {{{body}}}</main>\n") == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(
      read_fixture_file(root_dir, "public/hello.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file != NULL &&
             strcmp(generated_file, "<main>Hello <p>Body</p>\n</main>\n") == 0);
  free(generated_file);

  remove_fixture_tree(root_dir);
}

// A build writes exactly the manifest's intended outputs: content pages, aggregate and feed
// outputs, and nothing for drafts.
static void test_writes_exactly_manifest_outputs(void) {
  char root_dir_template[] = "/tmp/sosig-build-manifest-outputs.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // The shared config turns off aggregates and feeds, so this fixture writes its own.
  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = [\"index.html\"]\n"
      "feed_templates = [\"feed.xml\"]\n";
  const char alpha[] =
      "+++\n"
      "title = \"Alpha\"\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "Alpha body\n";
  const char beta[] =
      "+++\n"
      "title = \"Beta\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Beta body\n";
  const char draft[] =
      "+++\n"
      "title = \"Draft\"\n"
      "date = 2026-07-03T00:00:00Z\n"
      "draft = true\n"
      "+++\n"
      "Draft body\n";
  TEST_ASSERT(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/alpha.md", alpha) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/beta.md", beta) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/draft.md", draft) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/content.html", CONTENT_TEMPLATE) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/index.html", "index\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/feed.xml", "feed\n") == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  static const char* expected[] = {"public/alpha.html", "public/beta.html", "public/feed.xml",
                                   "public/index.html"};
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

  struct Arena arena;
  arena_init(&arena);
  char* output_dir = path_join(root_dir, "public", &arena);
  TEST_ASSERT(output_dir != NULL);
  struct PathList outputs;
  path_list_init(&outputs);
  static const char* const all_suffixes[] = {""};
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);

  // Exactly the expected set: matching count plus membership rules out extras and the draft.
  TEST_CHECK(outputs.count == expected_count);
  for (size_t i = 0; i < expected_count; i++) {
    TEST_CHECK(has_output_path(&outputs, root_dir, expected[i]));
    TEST_MSG("missing: '%s'", expected[i]);
  }

  path_list_free(&outputs);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A trailing slash on `content_dir` does not leak the directory name into the output tree. The
// prefix strip that derives an entry's section compares `<content_dir>/`, so an unnormalized
// `"content/"` would miss and publish `public/content/post.html`.
static void test_tolerates_trailing_slash_on_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-trailing-slash.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "content_dir = \"content/\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/post.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(
      read_fixture_file(root_dir, "public/post.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  free(generated_file);

  // The path an unnormalized `content_dir` would publish instead.
  char* leaked_file = NULL;
  size_t leaked_file_len = 0;
  TEST_CHECK(read_fixture_file(root_dir, "public/content/post.html", &leaked_file,
                               &leaked_file_len) == -1);

  remove_fixture_tree(root_dir);
}

// A custom pretty permalink publishes `<slug>/index.html` instead of `<slug>.html`. The golden test
// compares that output shape end to end with `tests/expected/site_index_permalink`. What this adds
// is the assertion at the command boundary, so it stops at the file being readable.
static void test_honors_custom_permalink(void) {
  char root_dir_template[] = "/tmp/sosig-build-permalink.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "permalink = \"/{slug}/\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(read_fixture_file(root_dir, "public/hello/index.html", &generated_file,
                               &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  free(generated_file);

  remove_fixture_tree(root_dir);
}

// Same-named sources in different directories publish to distinct, non-colliding outputs under the
// default permalink.
static void test_distinguishes_same_name_in_different_dirs(void) {
  char root_dir_template[] = "/tmp/sosig-build-same-name.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a/post.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/b/post.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  TEST_CHECK(fixture_path_exists(root_dir, "public/a/post.html"));
  TEST_CHECK(fixture_path_exists(root_dir, "public/b/post.html"));
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A second build over a finished output tree succeeds and produces the same set of outputs.
static void test_rebuild_succeeds_and_repeats_its_outputs(void) {
  char root_dir_template[] = "/tmp/sosig-build-rebuild.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");

  struct Arena arena;
  arena_init(&arena);
  char* output_dir = path_join(root_dir, "public", &arena);
  TEST_ASSERT(output_dir != NULL);
  struct PathList outputs;
  path_list_init(&outputs);
  static const char* const all_suffixes[] = {""};
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);
  // The one content page: the second build adds nothing and removes nothing.
  TEST_CHECK(outputs.count == 1);
  TEST_CHECK(has_output_path(&outputs, root_dir, "public/hello.html"));
  path_list_free(&outputs);
  arena_free(&arena);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A `content_dir` inside `output_dir` builds, because no output lands inside it: `output_dir = "."`
// with `content_dir = "content"` puts the site beside its sources. A rebuild succeeds and repeats
// the same outputs, so the walk does not read the first build's pages back as sources.
static void test_builds_and_rebuilds_with_output_dir_holding_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-dot-output.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "output_dir = \".\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  static const char* const all_suffixes[] = {""};
  size_t output_counts[2] = {0, 0};
  for (size_t i = 0; i < 2; i++) {
    struct StringBuffer error_buffer;
    string_buffer_init(&error_buffer);
    TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
    TEST_CHECK(error_buffer.len == 0);
    TEST_MSG("build %zu: '%s'", i + 1, error_buffer.data != NULL ? error_buffer.data : "");
    string_buffer_free(&error_buffer);

    struct PathList outputs;
    path_list_init(&outputs);
    TEST_CHECK(fs_list_files_with_suffixes(&outputs, root_dir, NULL, all_suffixes, 1, false, NULL,
                                           0) == 0);
    output_counts[i] = outputs.count;
    path_list_free(&outputs);
  }
  // The config, the template, and the source, then the content page.
  TEST_CHECK(output_counts[0] == 4);
  TEST_CHECK(output_counts[1] == output_counts[0]);
  TEST_CHECK(fixture_path_exists(root_dir, "hello.html"));

  remove_fixture_tree(root_dir);
}

// An `output_dir` that a symlink inside `content_dir` reaches is left out of the content walk, so a
// rebuild does not read the first build's outputs back as sources. The aggregate here publishes a
// `.md` file, which the walk would otherwise parse as a content entry and claim as an input.
static void test_rebuild_skips_output_dir_linked_from_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-linked-output.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // The shared config turns off aggregates, so this fixture writes its own.
  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = [\"notes.md\"]\n"
      "feed_templates = []\n";
  TEST_ASSERT(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/content.html", CONTENT_TEMPLATE) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/notes.md", "notes\n") == 0);
  struct Arena arena;
  arena_init(&arena);
  char* link_path = path_join(root_dir, "content/published", &arena);
  TEST_ASSERT(link_path != NULL);
  if (link_path == NULL) {
    arena_free(&arena);
    return;
  }
  TEST_ASSERT(symlink("../public", link_path) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  char* output_dir = path_join(root_dir, "public", &arena);
  TEST_ASSERT(output_dir != NULL);
  struct PathList outputs;
  path_list_init(&outputs);
  static const char* const all_suffixes[] = {""};
  TEST_CHECK(fs_list_files_with_suffixes(&outputs, output_dir, NULL, all_suffixes, 1, false, NULL,
                                         0) == 0);
  // The page and the aggregate, and no page rendered from the aggregate's own output.
  TEST_CHECK(outputs.count == 2);
  TEST_CHECK(has_output_path(&outputs, root_dir, "public/hello.html"));
  TEST_CHECK(has_output_path(&outputs, root_dir, "public/notes.md"));
  path_list_free(&outputs);

  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A caller-supplied `worker_count` reaches every pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose phase lines, which are the only
// place the module reports it. Each parallel pass closes with its one-job progress line.
static void test_honors_requested_worker_count(void) {
  char root_dir_template[] = "/tmp/sosig-build-workers.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  const struct BuildOptions options = {.worker_count = 3, .is_verbose = true};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 8];
  TEST_CHECK(run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out), stderr_out,
                                 sizeof(stderr_out)) == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  char expected[ERROR_MESSAGE_SIZE * 2];
  int expected_len = snprintf(expected, sizeof(expected),
                              "loading config\n"
                              "discovering content\n"
                              "parsing content, workers: %d\n"
                              "\rparsing content 1/1\n"
                              "collecting content entries\n"
                              "building output manifest\n"
                              "rendering content, workers: %d\n"
                              "\rrendering content 1/1\n"
                              "rendering aggregate templates\n"
                              "rendering feed templates\n"
                              "build complete\n",
                              3, 3);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  // One worker, requested explicitly, must be honored rather than read as unset. A guard spelled
  // `worker_count != 1` instead of `!= 0` swallows this request and falls back to the detected core
  // count. Asking for `3` above cannot see that. It is also the only way to ask for a serial build.
  // Like the `3` case, this assumes the host reports more than one core.
  const struct BuildOptions single_worker = {.worker_count = 1, .is_verbose = true};
  TEST_CHECK(run_build_capturing(root_dir, &single_worker, stdout_out, sizeof(stdout_out),
                                 stderr_out, sizeof(stderr_out)) == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  expected_len = snprintf(expected, sizeof(expected),
                          "loading config\n"
                          "discovering content\n"
                          "parsing content, workers: %d\n"
                          "\rparsing content 1/1\n"
                          "collecting content entries\n"
                          "building output manifest\n"
                          "rendering content, workers: %d\n"
                          "\rrendering content 1/1\n"
                          "rendering aggregate templates\n"
                          "rendering feed templates\n"
                          "build complete\n",
                          1, 1);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  // The requested count changes only how the work is scheduled, never the output.
  char* generated = NULL;
  size_t generated_len = 0;
  TEST_CHECK(read_fixture_file(root_dir, "public/hello.html", &generated, &generated_len) == 0);
  TEST_CHECK(generated != NULL && strcmp(generated, "<p>Body</p>\n\n") == 0);
  free(generated);

  remove_fixture_tree(root_dir);
}

// A build that succeeds leaves the collected diagnostic buffer empty, which is the other half of
// `cmd_build_execute`'s `error_out` contract. Without this a phase that appended to `error_out` on
// the success path would go unnoticed, since every other test that inspects the buffer is a failure
// test.
static void test_leaves_error_buffer_empty_on_success(void) {
  char root_dir_template[] = "/tmp/sosig-build-clean.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// `stderr` exactly once, which is the whole of what it adds over `cmd_build_execute`. Every other
// test routes failures through `cmd_build_execute` to inspect the message, so without this the
// boundary itself is unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  char root_dir_template[] = "/tmp/sosig-build-exit.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // No `sosig.toml`, the earliest failure, so the diagnostic is a single known line.
  const struct BuildOptions options = {0};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 4];
  const enum ExitCode rc = run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out),
                                               stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');

  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')\n",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  remove_fixture_tree(root_dir);
}

// A permalink that would expand to a path escaping the output directory is rejected at config load,
// so the build reports it once rather than once per content entry.
static void test_rejects_unsafe_permalink(void) {
  char root_dir_template[] = "/tmp/sosig-build-unsafe-permalink.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "permalink = \"/{slug}/../evil.html\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  // One config diagnostic, not one per entry: the message names the key, and the buffer holds a
  // single line. Compared exactly rather than by prefix, so that nothing trailing the message can
  // hide. A prefix needle stays green however the tail is corrupted.
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "config key 'permalink' must expand to a safe relative path using only "
                    "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                    "with no empty, '.' or '..' path segment: '/{slug}/../evil.html'") == 0);
  TEST_CHECK(error_buffer.data != NULL && strchr(error_buffer.data, '\n') == NULL);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A missing `sosig.toml` is reported at the command boundary rather than printed by a helper.
static void test_reports_missing_config(void) {
  char root_dir_template[] = "/tmp/sosig-build-no-config.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  // The cause is part of the claim: a missing config must not read like an unreadable one.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact: `expected` is the whole message.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// `content_dir` is required, so an absent one is a failure, reported as a resolution failure
// carrying the system cause. `ENOENT` and `EACCES` need different fixes and nothing later would
// report either, so the cause has to appear here. Compared exactly, so a caller that appended the
// path a second time would fail.
static void test_reports_absent_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-no-content.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to resolve config directory 'content_dir': %s ('content')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// `templates_dir` is required the same way, and reports the key the user has to fix rather than the
// one that happened to be checked first.
static void test_reports_absent_templates_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-no-templates.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to resolve config directory 'templates_dir': %s ('templates')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A `templates_dir` that exists but is a regular file is reported as the wrong type, which is its
// own cause rather than an `errno`, so the message carries no system text.
static void test_reports_templates_dir_that_is_a_file(void) {
  char root_dir_template[] = "/tmp/sosig-build-templates-file.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_fixture_file(root_dir, "sosig.toml", SITE_CONFIG) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates", "not a directory\n") == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "failed to resolve config directory 'templates_dir': not a directory "
                    "('templates')") == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A configured `output_dir` that already exists as a regular file is reported once the manifest has
// passed, where the build creates `output_dir`, before any page is written. The reason names the
// component that failed and the caller does not repeat the configured root, so the message is
// compared whole.
static void test_reports_unusable_output_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-output-file.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);
  // `public` is the default `output_dir`, so a file there is what makes `fs_mkdir_p` fail.
  TEST_ASSERT(write_fixture_file(root_dir, "public", "not a directory") == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(
      error_buffer.data != NULL &&
      strcmp(error_buffer.data,
             "failed to prepare output directory: exists and is not a directory ('public')") == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// An `output_dir` below `content_dir` is refused before content is discovered, so no earlier
// build's output can be read back as a source, and it is refused although it does not exist yet.
// The verbose phase lines show that the build stopped before the content walk, and the refused
// build creates none of the nested directories.
static void test_rejects_output_dir_inside_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-nested-output.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "output_dir = \"content/generated/site\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  const struct BuildOptions options = {.is_verbose = true};
  char stdout_out[ERROR_MESSAGE_SIZE];
  char stderr_out[ERROR_MESSAGE_SIZE * 4];
  TEST_CHECK(run_build_capturing(root_dir, &options, stdout_out, sizeof(stdout_out), stderr_out,
                                 sizeof(stderr_out)) == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  TEST_CHECK(strcmp(stderr_out,
                    "loading config\n"
                    "output directory would write inside 'content_dir': "
                    "'content/generated/site'\n") == 0);
  TEST_MSG("actual: '%s'", stderr_out);
  TEST_CHECK(!fixture_path_exists(root_dir, "content/generated"));

  remove_fixture_tree(root_dir);
}

// A content entry that cannot be parsed fails the build at the parse phase and its diagnostic
// reaches the command boundary. This is a different wiring from
// `test_reports_one_line_per_failing_entry`, which fails at the *page* phase: the two failures
// travel through separate entry points, `entry_renderer_render_entries` and
// `page_renderer_render_pages`, so neither test covers the other's propagation. Without this,
// `cmd_build_execute`'s parse-phase arm never runs and a parse failure that incorrectly returns
// success would still pass the suite.
static void test_reports_unparsable_content(void) {
  char root_dir_template[] = "/tmp/sosig-build-unparsable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // No closing fence, so `frontmatter_split` rejects the file before any key is read.
  const char unterminated[] =
      "+++\n"
      "title = \"X\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "Body\n";
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", unterminated) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  TEST_CHECK(error_buffer.data != NULL &&
             strcmp(error_buffer.data,
                    "missing closing '+++' frontmatter fence (in 'content/a.md')") == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A configured aggregate output that collides with a content entry output is rejected, with the
// collision described in the returned diagnostic. The refused build creates no `output_dir`,
// because nothing is written before the manifest passes.
static void test_rejects_duplicate_output(void) {
  char root_dir_template[] = "/tmp/sosig-build-collision.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  // The shared config turns off aggregates, so this fixture writes its own.
  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = [\"index.html\"]\n"
      "feed_templates = []\n";
  TEST_ASSERT(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/index.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/content.html", CONTENT_TEMPLATE) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/index.html", "index\n") == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "content/index.md", "aggregate_templates[0]", "public/index.html");
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact, not by substring: `expected` is the whole message, so a substring check could not tell
  // it from the same message with something appended.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);
  TEST_CHECK(!fixture_path_exists(root_dir, "public"));

  remove_fixture_tree(root_dir);
}

// Slugging is lossy, so two different source names can claim one output path. The manifest reports
// the collision and names both sources instead of letting the second write win. A non-ASCII byte
// folds to hex, so `caf\xC3\xA9` produces the slug a literal `cafc3a9` also produces. The refused
// build creates no `output_dir`, because nothing is written before the manifest passes.
static void test_rejects_colliding_source_names(void) {
  char root_dir_template[] = "/tmp/sosig-build-source-collision.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/caf\xC3\xA9.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/cafc3a9.md", HELLO_ENTRY) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "content/cafc3a9.md", "content/caf\xC3\xA9.md", "public/cafc3a9.html");
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact, not by substring: `expected` is the whole message, so a substring check could not tell
  // it from the same message with something appended.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);
  TEST_CHECK(!fixture_path_exists(root_dir, "public"));

  remove_fixture_tree(root_dir);
}

// A content entry whose content template cannot be rendered surfaces a per-entry diagnostic through
// the collected build error.
static void test_reports_bad_template(void) {
  char root_dir_template[] = "/tmp/sosig-build-bad-template.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, "content_template = \"missing.html\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/hello.md", HELLO_ENTRY) == 0);

  // No `templates/missing.html` exists, so the page render fails for the entry. The diagnostic
  // names the failing entry and the specific file that could not be read, rather than a generic
  // failure message that cannot be distinguished from an unrelated error.
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read template: %s ('templates/missing.html') (while rendering "
               "'content/hello.md')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Exact: `expected` is the whole message.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  TEST_MSG("actual: '%s'", error_buffer.data != NULL ? error_buffer.data : "");
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// Two content entries that both fail to render append one diagnostic line each, so neither is lost
// to the other. This is the postcondition `cmd_build_execute` states and the reason `error_out` is
// growable rather than a fixed buffer: an `append_error` that overwrote, or that dropped the
// separator and ran two messages together, would still satisfy every single-entry test.
static void test_reports_one_line_per_failing_entry(void) {
  char root_dir_template[] = "/tmp/sosig-build-render-failures.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char second[] =
      "+++\n"
      "title = \"Second\"\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_ASSERT(write_site_fixture(root_dir, "content_template = \"missing.html\"\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/b.md", second) == 0);

  // No `templates/missing.html`, so the page render fails for both entries.
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  error_system_message(reason, sizeof(reason), ENOENT);
  char expected[ERROR_MESSAGE_SIZE * 2];
  // Sources are walked in sorted order, so `content/a.md` precedes `content/b.md`. Compared whole,
  // with the separator in the middle, so a dropped newline or a lost line both fail here.
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read template: %s ('templates/missing.html') (while rendering "
               "'content/a.md')\n"
               "failed to read template: %s ('templates/missing.html') (while rendering "
               "'content/b.md')",
               reason, reason);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// An output path that cannot be opened as a file fails the build at the page phase, where each job
// writes the page it rendered. The failure is that entry's own diagnostic, so the other entry's
// page is still written. A write failure reported as success would otherwise pass the suite with
// the page silently missing.
static void test_reports_unwritable_output(void) {
  char root_dir_template[] = "/tmp/sosig-build-unwritable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_site_fixture(root_dir, NULL) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", HELLO_ENTRY) == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/b.md", HELLO_ENTRY) == 0);

  struct Arena arena;
  arena_init(&arena);
  char* output_path = path_join(root_dir, "public/a.html", &arena);
  TEST_ASSERT(output_path != NULL);
  if (output_path == NULL) {
    arena_free(&arena);
    return;
  }
  // A directory at the page path cannot be opened as a file even by a privileged process.
  TEST_CHECK(fs_mkdir_p(output_path, NULL, 0) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  error_system_message(reason, sizeof(reason), EISDIR);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to write output: %s (for 'content/a.md', to 'public/a.html')", reason);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);
  arena_free(&arena);
  char* generated = NULL;
  size_t generated_len = 0;
  TEST_CHECK(read_fixture_file(root_dir, "public/b.html", &generated, &generated_len) == 0);
  TEST_CHECK(generated != NULL && strcmp(generated, "<p>Body</p>\n\n") == 0);
  free(generated);

  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"honors configured content template", test_honors_configured_content_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"tolerates trailing slash on content_dir", test_tolerates_trailing_slash_on_content_dir},
    {"honors custom permalink", test_honors_custom_permalink},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"rebuild succeeds and repeats its outputs", test_rebuild_succeeds_and_repeats_its_outputs},
    {"builds and rebuilds with output dir holding content dir",
     test_builds_and_rebuilds_with_output_dir_holding_content_dir},
    {"rebuild skips output dir linked from content dir",
     test_rebuild_skips_output_dir_linked_from_content_dir},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"leaves error buffer empty on success", test_leaves_error_buffer_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"rejects unsafe permalink", test_rejects_unsafe_permalink},
    {"reports missing config", test_reports_missing_config},
    {"reports absent content dir", test_reports_absent_content_dir},
    {"reports absent templates dir", test_reports_absent_templates_dir},
    {"reports templates dir that is a file", test_reports_templates_dir_that_is_a_file},
    {"reports unusable output dir", test_reports_unusable_output_dir},
    {"rejects output dir inside content dir", test_rejects_output_dir_inside_content_dir},
    {"reports unparsable content", test_reports_unparsable_content},
    {"rejects duplicate output", test_rejects_duplicate_output},
    {"rejects colliding source names", test_rejects_colliding_source_names},
    {"reports bad template", test_reports_bad_template},
    {"reports one line per failing entry", test_reports_one_line_per_failing_entry},
    {"reports unwritable output", test_reports_unwritable_output},
    {NULL, NULL},
};
