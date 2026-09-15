#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/cmd_build.h"
#include "app/exit_code.h"
#include "core/arena.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/string_buffer.h"
#include "runtime/fs.h"
#include "test_support.h"

/**
 * @brief Reads one generated file relative to a temporary site root.
 *
 * @param root_dir               Temporary site root directory.
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
  const int rc = fixture_path == NULL ? -1
                                      : fs_read_file(fixture_path, generated_file_out,
                                                     generated_file_len_out, NULL, 0);
  arena_free(&arena);
  return rc;
}

/**
 * @brief Runs a default build in a temporary fixture directory.
 *
 * Use `execute_build_in_dir` when a test must inspect the returned diagnostic.
 *
 * @param root_dir Fixture directory in which to run the build.
 * @return The command exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode run_build_in_dir(const char* root_dir) {
  char working_dir[PATH_MAX];
  if (getcwd(working_dir, sizeof(working_dir)) == NULL) {
    TEST_CHECK(false);
    return (enum ExitCode)EXIT_CODE_VALUE_MAX;
  }
  const int chdir_rc = chdir(root_dir);
  TEST_CHECK(chdir_rc == 0);
  if (chdir_rc != 0) {
    return (enum ExitCode)EXIT_CODE_VALUE_MAX;
  }
  const struct BuildOptions options = {0};
  const enum ExitCode rc = cmd_build_run(&options);
  const int restore_rc = chdir(working_dir);
  TEST_CHECK(restore_rc == 0);
  return restore_rc == 0 ? rc : (enum ExitCode)EXIT_CODE_VALUE_MAX;
}

/**
 * @brief Executes a default build and collects its diagnostic.
 *
 * @param root_dir  Fixture directory in which to execute the build.
 * @param error_out Buffer that receives any build diagnostic.
 * @return `0` on success, `-1` on build failure, or `EXIT_CODE_VALUE_MAX` on plumbing failure.
 */
static int execute_build_in_dir(const char* root_dir, struct StringBuffer* error_out) {
  char working_dir[PATH_MAX];
  if (getcwd(working_dir, sizeof(working_dir)) == NULL) {
    TEST_CHECK(false);
    return EXIT_CODE_VALUE_MAX;
  }
  const int chdir_rc = chdir(root_dir);
  TEST_CHECK(chdir_rc == 0);
  if (chdir_rc != 0) {
    return EXIT_CODE_VALUE_MAX;
  }
  const struct BuildOptions options = {0};
  const int rc = cmd_build_execute(&options, error_out);
  const int restore_rc = chdir(working_dir);
  TEST_CHECK(restore_rc == 0);
  return restore_rc == 0 ? rc : EXIT_CODE_VALUE_MAX;
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
 * @return The command exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode run_build_capturing(const char* root_dir,
                                         const struct BuildOptions* options,
                                         char* stdout_out,
                                         size_t stdout_out_len,
                                         char* stderr_out,
                                         size_t stderr_out_len) {
  stdout_out[0] = '\0';
  stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  bool has_changed_dir = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  FILE* stdout_capture = NULL;
  FILE* stderr_capture = NULL;
  char working_dir[PATH_MAX];
  if (getcwd(working_dir, sizeof(working_dir)) == NULL) {
    TEST_CHECK(false);
    goto cleanup;
  }
  {
    const int chdir_rc = chdir(root_dir);
    TEST_CHECK(chdir_rc == 0);
    if (chdir_rc != 0) {
      goto cleanup;
    }
    has_changed_dir = true;

    const int stdout_flush_rc = fflush(stdout);
    const int stderr_flush_rc = fflush(stderr);
    TEST_CHECK(stdout_flush_rc == 0);
    TEST_CHECK(stderr_flush_rc == 0);
    if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
      goto cleanup;
    }
    saved_stdout = dup(STDOUT_FILENO);
    TEST_CHECK(saved_stdout >= 0);
    if (saved_stdout < 0) {
      goto cleanup;
    }
    saved_stderr = dup(STDERR_FILENO);
    TEST_CHECK(saved_stderr >= 0);
    if (saved_stderr < 0) {
      goto cleanup;
    }
    stdout_capture = tmpfile();
    TEST_ASSERT(stdout_capture != NULL);
    if (stdout_capture == NULL) {
      goto cleanup;
    }
    stderr_capture = tmpfile();
    TEST_ASSERT(stderr_capture != NULL);
    if (stderr_capture == NULL) {
      goto cleanup;
    }

    const int stdout_redirect_rc = dup2(fileno(stdout_capture), STDOUT_FILENO);
    TEST_CHECK(stdout_redirect_rc == STDOUT_FILENO);
    if (stdout_redirect_rc != STDOUT_FILENO) {
      goto cleanup;
    }
    const int stderr_redirect_rc = dup2(fileno(stderr_capture), STDERR_FILENO);
    TEST_CHECK(stderr_redirect_rc == STDERR_FILENO);
    if (stderr_redirect_rc != STDERR_FILENO) {
      goto cleanup;
    }

    rc = cmd_build_run(options);
    const int captured_stdout_flush_rc = fflush(stdout);
    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stdout_flush_rc == 0);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stdout_flush_rc != 0 || captured_stderr_flush_rc != 0;
  }

cleanup:
  if (saved_stdout >= 0) {
    const int restore_rc = dup2(saved_stdout, STDOUT_FILENO);
    TEST_CHECK(restore_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDOUT_FILENO;
    const int close_rc = close(saved_stdout);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (saved_stderr >= 0) {
    const int restore_rc = dup2(saved_stderr, STDERR_FILENO);
    TEST_CHECK(restore_rc == STDERR_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDERR_FILENO;
    const int close_rc = close(saved_stderr);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  clearerr(stdout);
  clearerr(stderr);
  if (stdout_capture != NULL) {
    has_plumbing_failed =
        read_capture(stdout_capture, stdout_out, stdout_out_len) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stdout_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (stderr_capture != NULL) {
    has_plumbing_failed =
        read_capture(stderr_capture, stderr_out, stderr_out_len) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (has_changed_dir) {
    const int restore_dir_rc = chdir(working_dir);
    TEST_CHECK(restore_dir_rc == 0);
    has_plumbing_failed = has_plumbing_failed || restore_dir_rc != 0;
  }
  return has_plumbing_failed ? (enum ExitCode)EXIT_CODE_VALUE_MAX : rc;
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

// A configured content template name is used instead of the default.
static void test_honors_configured_content_template(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "content_template = \"content-entry.html\"\n"
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
  TEST_CHECK(write_fixture_file(root_dir, "templates/content-entry.html",
                                "<main>{{title}} {{{body}}}</main>\n") == 0);
  TEST_CHECK(run_build_in_dir(root_dir) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(
      read_fixture_file(root_dir, "public/hello.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  TEST_CHECK(generated_file != NULL && strstr(generated_file, "<main>Hello <p>Body</p>") != NULL);
  free(generated_file);

  remove_fixture_tree(root_dir);
}

// A build writes exactly the manifest's intended outputs: content pages, aggregate and feed
// outputs, and nothing for drafts.
static void test_writes_exactly_manifest_outputs(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

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
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/alpha.md", alpha) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/beta.md", beta) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/draft.md", draft) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/index.html", "index\n") == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/feed.xml", "feed\n") == 0);
  TEST_CHECK(run_build_in_dir(root_dir) == 0);

  static const char* expected[] = {"public/alpha.html", "public/beta.html", "public/feed.xml",
                                   "public/index.html"};
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

  struct Arena arena;
  arena_init(&arena);
  char* public_dir = path_join(root_dir, "public", &arena);
  struct PathList outputs;
  path_list_init(&outputs);
  TEST_CHECK(public_dir != NULL &&
             fs_list_files_with_suffix(&outputs, public_dir, "", NULL, 0) == 0);

  // Exactly the expected set: matching count plus membership rules out extras and the draft.
  TEST_CHECK(outputs.count == expected_count);
  for (size_t i = 0; i < expected_count; i++) {
    TEST_CHECK(has_output_path(&outputs, root_dir, expected[i]));
  }

  path_list_free(&outputs);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The default permalink mirrors the source subdirectory into the output tree. The golden test
// compares the generated bytes end to end with `tests/expected/site_file_permalink/nested`. What
// this adds is that the build creates the nested output directory at all, so the assertion here
// stops at the file being readable.
static void test_mirrors_nested_source_tree(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
      "title = \"Post\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/nested/post.md", content) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(root_dir) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(read_fixture_file(root_dir, "public/nested/post.html", &generated_file,
                               &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  free(generated_file);

  remove_fixture_tree(root_dir);
}

// A trailing slash on `content_dir` does not leak the directory name into the output tree. The
// prefix strip that derives an entry's section compares `<content_dir>/`, so an unnormalized
// `"content/"` would miss and publish `public/content/post.html`.
static void test_tolerates_trailing_slash_on_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "content_dir = \"content/\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  const char content[] =
      "+++\n"
      "title = \"Post\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/post.md", content) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(root_dir) == 0);

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
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "permalink = \"/{slug}/\"\n"
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
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(root_dir) == 0);

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
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  const char a[] =
      "+++\n"
      "title = \"A\"\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "A body\n";
  const char b[] =
      "+++\n"
      "title = \"B\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "B body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/a/post.md", a) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/b/post.md", b) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(root_dir) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(
      read_fixture_file(root_dir, "public/a/post.html", &generated_file, &generated_file_len) == 0);
  free(generated_file);
  generated_file = NULL;
  TEST_CHECK(
      read_fixture_file(root_dir, "public/b/post.html", &generated_file, &generated_file_len) == 0);
  free(generated_file);

  remove_fixture_tree(root_dir);
}

// A caller-supplied `worker_count` reaches the pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose progress line, which is the only
// place the module reports it.
static void test_honors_requested_worker_count(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "<html>{{{body}}}</html>\n") ==
             0);

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
                              "parsing content with %d workers\n"
                              ".\n"
                              "collecting content entries\n"
                              "building output manifest\n"
                              "rendering content with %d workers\n"
                              ".\n"
                              "writing content pages\n"
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
                          "parsing content with %d workers\n"
                          ".\n"
                          "collecting content entries\n"
                          "building output manifest\n"
                          "rendering content with %d workers\n"
                          ".\n"
                          "writing content pages\n"
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
  if (generated != NULL) {
    TEST_CHECK(strcmp(generated, "<html><p>Body</p>\n</html>\n") == 0);
  }
  free(generated);

  remove_fixture_tree(root_dir);
}

// A build that succeeds leaves the collected diagnostic buffer empty, which is the other half of
// `cmd_build_execute`'s `error_out` contract. Without this a phase that appended to `error_out` on
// the success path would go unnoticed, since every other test that inspects the buffer is a failure
// test.
static void test_leaves_error_buffer_empty_on_success(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "<html>{{{body}}}</html>\n") ==
             0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == 0);
  TEST_CHECK(error_buffer.len == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// `stderr` exactly once, which is the whole of what it adds over `cmd_build_execute`. Every other
// test routes failures through `cmd_build_execute` to inspect the message, so without this the
// boundary itself is unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  const int n = snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')\n",
                         error_system_message(reason, sizeof(reason), ENOENT));
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  remove_fixture_tree(root_dir);
}

// A permalink that would expand to a path escaping the output directory is rejected at config load,
// so the build reports it once rather than once per content entry.
static void test_rejects_unsafe_permalink(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "permalink = \"/{slug}/../evil.html\"\n"
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
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);

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
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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

// An absent content directory is reported through the returned diagnostic.
static void test_reports_absent_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to list Markdown files: cannot inspect directory: %s ('content')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // Compared exactly, not by substring: the walk's reason already names the path, so the only way
  // to catch the caller appending it a second time is to assert nothing trails the message.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A configured `output_dir` that already exists as a regular file is reported from the first build
// phase, before any content is read. The reason names the component that failed and the caller does
// not repeat the configured root, so the message is compared whole.
static void test_reports_unusable_output_dir(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  // `public` is the default `output_dir`, so a file there is what makes `fs_mkdir_p` fail.
  TEST_CHECK(write_fixture_file(root_dir, "public", "not a directory") == 0);

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

// A content entry that cannot be parsed fails the build at the parse phase and its diagnostic
// reaches the command boundary. This is a different wiring from
// `test_reports_one_line_per_failing_entry`, which fails at the *page* phase: the two failures
// travel through separate entry points, `entry_renderer_render_entries` and
// `page_renderer_render_pages`, so neither test covers the other's propagation. Without this,
// `cmd_build_execute`'s parse-phase arm never runs and a parse failure that incorrectly returns
// success would still pass the suite.
static void test_reports_unparsable_content(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  // No closing fence, so `frontmatter_split` rejects the file before any key is read.
  const char unterminated[] =
      "+++\n"
      "title = \"X\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/a.md", unterminated) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);

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
// collision described in the returned diagnostic.
static void test_rejects_duplicate_output(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = [\"index.html\"]\n"
      "feed_templates = []\n";
  const char content[] =
      "+++\n"
      "title = \"Index\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/index.md", content) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/index.html", "index\n") == 0);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "content/index.md", "aggregate_templates[0]", "public/index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Exact, not by substring: `expected` is the whole message, so a substring check could not tell
  // it from the same message with something appended.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// A content entry whose content template cannot be rendered surfaces a per-entry diagnostic through
// the collected build error.
static void test_reports_bad_template(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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

  // No `templates/content.html` exists, so the page render fails for the entry. The diagnostic
  // names the failing entry and the specific file that could not be read, rather than a generic
  // failure message that cannot be distinguished from an unrelated error.
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "failed to read template: %s ('templates/content.html') (while rendering "
                         "'content/hello.md')",
                         error_system_message(reason, sizeof(reason), ENOENT));
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Exact: `expected` is the whole message.
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// Two content entries that both fail to render append one diagnostic line each, so neither is lost
// to the other. This is the postcondition `cmd_build_execute` states and the reason `error_out` is
// growable rather than a fixed buffer: an `append_render_error` that overwrote, or that dropped the
// separator and ran two messages together, would still satisfy every single-entry test.
static void test_reports_one_line_per_failing_entry(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  const char first[] =
      "+++\n"
      "title = \"First\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char second[] =
      "+++\n"
      "title = \"Second\"\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/a.md", first) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/b.md", second) == 0);

  // No `templates/content.html`, so the page render fails for both entries.
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  TEST_CHECK(execute_build_in_dir(root_dir, &error_buffer) == -1);
  char reason[FS_REASON_SIZE];
  error_system_message(reason, sizeof(reason), ENOENT);
  char expected[ERROR_MESSAGE_SIZE * 2];
  // Sources are walked in sorted order, so `content/a.md` precedes `content/b.md`. Compared whole,
  // with the separator in the middle, so a dropped newline or a lost line both fail here.
  const int n = snprintf(expected, sizeof(expected),
                         "failed to read template: %s ('templates/content.html') (while rendering "
                         "'content/a.md')\n"
                         "failed to read template: %s ('templates/content.html') (while rendering "
                         "'content/b.md')",
                         reason, reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);

  remove_fixture_tree(root_dir);
}

// An output path that cannot be opened as a file fails the build at the write phase, after every
// entry has rendered successfully. That is the last of `cmd_build_execute`'s phase arms and the
// only one reached with a complete set of rendered pages in hand, so a write failure reported as
// success would otherwise pass the suite with the pages silently missing.
static void test_reports_unwritable_output(void) {
  char root_dir_template[] = "/tmp/sosig-build-test.XXXXXX";
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
  const char entry[] =
      "+++\n"
      "title = \"X\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(root_dir, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/a.md", entry) == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "{{{body}}}\n") == 0);

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
  const int n =
      snprintf(expected, sizeof(expected),
               "failed to write output: %s (for 'content/a.md', to 'public/a.html')", reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  string_buffer_free(&error_buffer);
  arena_free(&arena);

  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"honors configured content template", test_honors_configured_content_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"mirrors nested source tree", test_mirrors_nested_source_tree},
    {"tolerates trailing slash on content_dir", test_tolerates_trailing_slash_on_content_dir},
    {"honors custom permalink", test_honors_custom_permalink},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"leaves error buffer empty on success", test_leaves_error_buffer_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"rejects unsafe permalink", test_rejects_unsafe_permalink},
    {"reports missing config", test_reports_missing_config},
    {"reports absent content dir", test_reports_absent_content_dir},
    {"reports unusable output dir", test_reports_unusable_output_dir},
    {"reports unparsable content", test_reports_unparsable_content},
    {"rejects duplicate output", test_rejects_duplicate_output},
    {"reports bad template", test_reports_bad_template},
    {"reports one line per failing entry", test_reports_one_line_per_failing_entry},
    {"reports unwritable output", test_reports_unwritable_output},
    {NULL, NULL}};
