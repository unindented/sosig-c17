#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/cmd_build.h"
#include "core/arena.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/string_buffer.h"
#include "runtime/fs.h"

// Creates a temporary site root for build tests. Returns `NULL` once `TEST_CHECK` has failed the
// test, so a caller's `return` only suppresses cascading noise. The result aliases the caller's
// `dir` rather than being owned.
static const char* init_build_fixture(char root_dir[static 1]) {
  char* tmp = mkdtemp(root_dir);
  TEST_CHECK(tmp != NULL);
  return tmp;
}

// Writes one fixture file relative to the temporary site root. The `NULL, 0` reason arguments are
// the option `fs_write_file`'s contract allows: a failed fixture write is a broken test, not
// behavior under test. Callers assert the non-zero return. `read_fixture_file` below does the same
// for `fs_read_file`.
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

// Reads one generated file relative to the temporary site root.
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

// Runs a default build with the temporary fixture as the current directory. Goes through
// `cmd_build_run`, which prints any diagnostic to `stderr` and hands back only an exit code, so a
// test that needs to assert the message must use `execute_build_in_dir` below instead. Using this
// function when the diagnostic matters silently loses that assertion.
static int run_build_in_dir(const char* root_dir) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    TEST_CHECK(false);
    return 1;
  }
  if (chdir(root_dir) != 0) {
    return 1;
  }
  const struct BuildOptions options = {0};
  const int rc = cmd_build_run(&options);
  const int restored = chdir(cwd);
  TEST_CHECK(restored == 0);
  return restored == 0 ? rc : 1;
}

// Runs a default build through the message-returning boundary, collecting any diagnostic.
static int execute_build_in_dir(const char* root_dir, struct StringBuffer* error_out) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    TEST_CHECK(false);
    return 1;
  }
  if (chdir(root_dir) != 0) {
    return 1;
  }
  const struct BuildOptions options = {0};
  const int rc = cmd_build_execute(&options, error_out);
  const int restored = chdir(cwd);
  TEST_CHECK(restored == 0);
  return restored == 0 ? rc : 1;
}

// Runs a build with caller-chosen `options` through `cmd_build_run`, capturing everything it writes
// to `stderr` into `captured_out` as a terminated string. Capturing is the only way to assert
// `cmd_build_run`'s half of the boundary contract, since it hands back an exit code and nothing
// else. `stderr` is redirected at the descriptor rather than with `freopen` so the original can be
// restored from a `dup` afterwards. A test that left it redirected would silently swallow the
// harness's own output for every later test. Returns the exit code, or `1` if the plumbing itself
// failed.
static int run_build_capturing_stderr(const char* root_dir,
                                      const struct BuildOptions* options,
                                      char* captured_out,
                                      size_t captured_out_len) {
  captured_out[0] = '\0';
  char capture_path[] = "/tmp/sosig-build-stderr.XXXXXX";
  const int capture_fd = mkstemp(capture_path);
  TEST_ASSERT(capture_fd >= 0);
  if (capture_fd < 0) {
    return 1;
  }
  const int saved_stderr = dup(STDERR_FILENO);
  TEST_ASSERT(saved_stderr >= 0);
  if (saved_stderr < 0) {
    close(capture_fd);
    (void)unlink(capture_path);
    return 1;
  }

  char cwd[PATH_MAX];
  int rc = 1;
  if (getcwd(cwd, sizeof(cwd)) != NULL && chdir(root_dir) == 0) {
    fflush(stderr);
    if (dup2(capture_fd, STDERR_FILENO) >= 0) {
      rc = cmd_build_run(options);
      fflush(stderr);
    }
    (void)dup2(saved_stderr, STDERR_FILENO);
    const int restored = chdir(cwd);
    TEST_CHECK(restored == 0);
  } else {
    (void)dup2(saved_stderr, STDERR_FILENO);
    TEST_CHECK(false);
  }
  close(saved_stderr);

  FILE* capture = fdopen(capture_fd, "rb");
  TEST_ASSERT(capture != NULL);
  if (capture == NULL) {
    close(capture_fd);
    (void)unlink(capture_path);
    return 1;
  }
  rewind(capture);
  const size_t n = fread(captured_out, 1, captured_out_len - 1, capture);
  captured_out[n] = '\0';
  fclose(capture);
  (void)unlink(capture_path);
  return rc;
}

// Removes the fixture paths shared by the two tests that write the default `content`, `templates`
// and `public` layout. A test writing anything else must use `cleanup_named_paths`, since an
// unlisted file makes the final `rmdir` fail and leaves the temporary root behind.
static void cleanup_build_fixture(const char* root_dir) {
  static const char* files[] = {
      "public/hello.html",
      "public/index.html",
      "content/hello.md",
      "content/index.md",
      "templates/content.html",
      "templates/index.html",
      "templates/content-entry.html",
      "sosig.toml",
  };
  static const char* dirs[] = {"public", "content", "templates"};

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

// Removes the fixture paths used by the exact-outputs test.
static void cleanup_exact_outputs_fixture(const char* root_dir) {
  static const char* files[] = {
      "public/alpha.html",    "public/beta.html",   "public/index.html", "public/feed.xml",
      "content/alpha.md",     "content/beta.md",    "content/draft.md",  "templates/content.html",
      "templates/index.html", "templates/feed.xml", "sosig.toml",
  };
  static const char* dirs[] = {"public", "content", "templates"};

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

// Removes the given files then the given directories (which must be listed deepest-first).
static void cleanup_named_paths(const char* root_dir,
                                const char* const* files,
                                size_t file_count,
                                const char* const* dirs,
                                size_t dir_count) {
  struct Arena arena;
  arena_init(&arena);
  for (size_t i = 0; i < file_count; i++) {
    (void)unlink(path_join(root_dir, files[i], &arena));
  }
  for (size_t i = 0; i < dir_count; i++) {
    (void)rmdir(path_join(root_dir, dirs[i], &arena));
  }
  arena_free(&arena);
  (void)rmdir(root_dir);
}

// Reports whether the path list holds an entry equal to `root_dir` joined with `relative_path`.
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
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content-entry.html",
                                "<main>{{title}} {{{body}}}</main>\n") == 0);
  TEST_CHECK(run_build_in_dir(tmp) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(read_fixture_file(tmp, "public/hello.html", &generated_file, &generated_file_len) ==
             0);
  TEST_CHECK(generated_file_len > 0);
  TEST_CHECK(generated_file != NULL && strstr(generated_file, "<main>Hello <p>Body</p>") != NULL);
  free(generated_file);

  cleanup_build_fixture(tmp);
}

// A build writes exactly the manifest's intended outputs: content pages, aggregate and feed
// outputs, and nothing for drafts.
static void test_writes_exactly_manifest_outputs(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/alpha.md", alpha) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/beta.md", beta) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/draft.md", draft) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/index.html", "index\n") == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/feed.xml", "feed\n") == 0);
  TEST_CHECK(run_build_in_dir(tmp) == 0);

  static const char* expected[] = {"public/alpha.html", "public/beta.html", "public/feed.xml",
                                   "public/index.html"};
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

  struct Arena arena;
  arena_init(&arena);
  char* public_dir = path_join(tmp, "public", &arena);
  struct PathList outputs;
  path_list_init(&outputs);
  TEST_CHECK(public_dir != NULL &&
             fs_list_files_with_suffix(&outputs, public_dir, "", NULL, 0) == 0);

  // Exactly the expected set: matching count plus membership rules out extras and the draft.
  TEST_CHECK(outputs.count == expected_count);
  for (size_t i = 0; i < expected_count; i++) {
    TEST_CHECK(has_output_path(&outputs, tmp, expected[i]));
  }

  path_list_free(&outputs);
  arena_free(&arena);
  cleanup_exact_outputs_fixture(tmp);
}

// The default permalink mirrors the source subdirectory into the output tree. The generated bytes
// are diffed end to end by `tests/expected/site-file-permalink/nested`. What this adds is that the
// build creates the nested output directory at all, so the assertion here stops at the file being
// readable.
static void test_mirrors_nested_source_tree(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
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
      "title = \"Post\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/nested/post.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(tmp) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(
      read_fixture_file(tmp, "public/nested/post.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  free(generated_file);

  static const char* files[] = {"public/nested/post.html", "content/nested/post.md",
                                "templates/content.html", "sosig.toml"};
  static const char* dirs[] = {"public/nested", "content/nested", "public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A trailing slash on `content_dir` does not leak the directory name into the output tree. The
// prefix strip that derives an entry's section compares `<content_dir>/`, so an unnormalized
// `"content/"` would miss and publish `public/content/post.html`.
static void test_tolerates_trailing_slash_on_content_dir(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/post.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(tmp) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(read_fixture_file(tmp, "public/post.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  free(generated_file);

  // The path an unnormalized `content_dir` would publish instead.
  char* leaked_file = NULL;
  size_t leaked_file_len = 0;
  TEST_CHECK(read_fixture_file(tmp, "public/content/post.html", &leaked_file, &leaked_file_len) ==
             -1);

  static const char* files[] = {"public/post.html", "content/post.md", "templates/content.html",
                                "sosig.toml"};
  static const char* dirs[] = {"public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A custom pretty permalink publishes `<slug>/index.html` instead of `<slug>.html`. That output
// shape is diffed end to end by `tests/expected/site-index-permalink`. What this adds is the
// assertion at the command boundary, so it stops at the file being readable.
static void test_honors_custom_permalink(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(tmp) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(
      read_fixture_file(tmp, "public/hello/index.html", &generated_file, &generated_file_len) == 0);
  TEST_CHECK(generated_file_len > 0);
  free(generated_file);

  static const char* files[] = {"public/hello/index.html", "content/hello.md",
                                "templates/content.html", "sosig.toml"};
  static const char* dirs[] = {"public/hello", "public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// Same-named sources in different directories publish to distinct, non-colliding outputs under the
// default permalink.
static void test_distinguishes_same_name_in_different_dirs(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/a/post.md", a) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/b/post.md", b) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(run_build_in_dir(tmp) == 0);

  char* generated_file = NULL;
  size_t generated_file_len = 0;
  TEST_CHECK(read_fixture_file(tmp, "public/a/post.html", &generated_file, &generated_file_len) ==
             0);
  free(generated_file);
  generated_file = NULL;
  TEST_CHECK(read_fixture_file(tmp, "public/b/post.html", &generated_file, &generated_file_len) ==
             0);
  free(generated_file);

  static const char* files[] = {"public/a/post.html", "public/b/post.html",     "content/a/post.md",
                                "content/b/post.md",  "templates/content.html", "sosig.toml"};
  static const char* dirs[] = {"public/a", "public/b", "content/a", "content/b",
                               "public",   "content",  "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A caller-supplied `worker_count` reaches the pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose progress line, which is the only
// place the module reports it.
static void test_honors_requested_worker_count(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
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
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "<html>{{{body}}}</html>\n") == 0);

  const struct BuildOptions options = {.worker_count = 3, .is_verbose = true};
  char captured[ERROR_MESSAGE_SIZE * 8];
  TEST_CHECK(run_build_capturing_stderr(tmp, &options, captured, sizeof(captured)) == EXIT_CODE_OK);
  // Both pool phases must name the requested count, not the detected default. An implementation
  // that ignored the option would print the machine's core count here, which is not 3 on any CI
  // runner this project targets, and one that inverted the ternary would print 0.
  TEST_CHECK(strstr(captured, "parsing content with 3 workers\n") != NULL);
  TEST_CHECK(strstr(captured, "rendering content with 3 workers\n") != NULL);

  // One worker, requested explicitly, must be honored rather than read as unset. A guard spelled
  // `worker_count != 1` instead of `!= 0` swallows this request and falls back to the detected core
  // count. Asking for `3` above cannot see that. It is also the only way to ask for a serial build.
  // Like the `3` case, this assumes the host reports more than one core.
  const struct BuildOptions single_worker = {.worker_count = 1, .is_verbose = true};
  TEST_CHECK(run_build_capturing_stderr(tmp, &single_worker, captured, sizeof(captured)) ==
             EXIT_CODE_OK);
  TEST_CHECK(strstr(captured, "parsing content with 1 workers\n") != NULL);
  TEST_CHECK(strstr(captured, "rendering content with 1 workers\n") != NULL);

  // The requested count changes only how the work is scheduled, never the output.
  char* generated = NULL;
  size_t generated_len = 0;
  TEST_CHECK(read_fixture_file(tmp, "public/hello.html", &generated, &generated_len) == 0);
  if (generated != NULL) {
    TEST_CHECK(strcmp(generated, "<html><p>Body</p>\n</html>\n") == 0);
  }
  free(generated);

  static const char* files[] = {"public/hello.html", "content/hello.md", "templates/content.html",
                                "sosig.toml"};
  static const char* dirs[] = {"public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// `stderr` exactly once, which is the whole of what it adds over `cmd_build_execute`. Every other
// test routes failures through `cmd_build_execute` to inspect the message, so without this the
// boundary itself is unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // No `sosig.toml`, the earliest failure, so the diagnostic is a single known line.
  const struct BuildOptions options = {0};
  char captured[ERROR_MESSAGE_SIZE * 4];
  const int rc = run_build_capturing_stderr(tmp, &options, captured, sizeof(captured));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);

  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')\n", reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  TEST_CHECK(strcmp(captured, expected) == 0);

  (void)rmdir(tmp);
}

// A build that succeeds leaves the collected diagnostic buffer empty, which is the other half of
// `cmd_build_execute`'s `error_out` contract. Without this a phase that appended to `error_out` on
// the success path would go unnoticed, since every other test that inspects the buffer is a failure
// test.
static void test_leaves_error_buffer_empty_on_success(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
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
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "<html>{{{body}}}</html>\n") == 0);

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) == 0);
  TEST_CHECK(err.len == 0);
  string_buffer_free(&err);

  static const char* files[] = {"public/hello.html", "content/hello.md", "templates/content.html",
                                "sosig.toml"};
  static const char* dirs[] = {"public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A permalink that would expand to a path escaping the output directory is rejected at config load,
// so the build reports it once rather than once per content entry.
static void test_rejects_unsafe_permalink(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/hello.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  // One config diagnostic, not one per entry: the message names the key, and the buffer holds a
  // single line. Compared exactly rather than by prefix, so that nothing trailing the message can
  // hide. A prefix needle stays green however the tail is corrupted.
  TEST_CHECK(err.data != NULL &&
             strcmp(err.data,
                    "config key 'permalink' must expand to a safe relative path using only "
                    "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                    "with no empty, '.' or '..' path segment: '/{slug}/../evil.html'") == 0);
  TEST_CHECK(err.data != NULL && strchr(err.data, '\n') == NULL);
  string_buffer_free(&err);

  static const char* files[] = {"content/hello.md", "templates/content.html", "sosig.toml"};
  static const char* dirs[] = {"public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A missing `sosig.toml` is reported at the command boundary rather than printed by a helper.
static void test_reports_missing_config(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  // The cause is part of the claim: a missing config must not read like an unreadable one.
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
  char expected[ERROR_MESSAGE_SIZE];
  (void)snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')", reason);
  // Exact: `expected` is the whole message.
  TEST_CHECK(err.data != NULL && strcmp(err.data, expected) == 0);
  string_buffer_free(&err);

  (void)rmdir(tmp);
}

// An absent content directory is reported through the returned diagnostic.
static void test_reports_absent_content_dir(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
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

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
  char expected[ERROR_MESSAGE_SIZE];
  (void)snprintf(expected, sizeof(expected),
                 "failed to list Markdown files: cannot inspect directory: %s ('content')", reason);
  // Compared exactly, not by substring: the walk's reason already names the path, so the only way
  // to catch the caller appending it a second time is to assert nothing trails the message.
  TEST_CHECK(err.data != NULL && strcmp(err.data, expected) == 0);
  string_buffer_free(&err);

  static const char* files[] = {"sosig.toml"};
  static const char* dirs[] = {"public"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A configured `output_dir` that already exists as a regular file is reported from the first build
// phase, before any content is read. The reason names the component that failed and the caller does
// not repeat the configured root, so the message is compared whole.
static void test_reports_unusable_output_dir(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
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
  // `public` is the default `output_dir`, so a file there is what makes `fs_mkdir_p` fail.
  TEST_CHECK(write_fixture_file(tmp, "public", "not a directory") == 0);

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  TEST_CHECK(
      err.data != NULL &&
      strcmp(err.data,
             "failed to prepare output directory: exists and is not a directory ('public')") == 0);
  string_buffer_free(&err);

  static const char* files[] = {"sosig.toml", "public"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), NULL, 0);
}

// A content entry that cannot be parsed fails the build at the parse phase and its diagnostic
// reaches the command boundary. This is a different wiring from
// `test_reports_one_line_per_failing_entry`, which fails at the *page* phase: the two failures
// travel through separate entry points, `entry_renderer_render_entries` and
// `page_renderer_render_pages`, so neither test covers the other's propagation. Without this,
// `cmd_build_execute`'s parse-phase arm never runs and a parse failure that incorrectly returns
// success would still pass the suite.
static void test_reports_unparsable_content(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/a.md", unterminated) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  TEST_CHECK(err.data != NULL &&
             strcmp(err.data, "missing closing '+++' frontmatter fence (in 'content/a.md')") == 0);
  string_buffer_free(&err);

  static const char* files[] = {"content/a.md", "templates/content.html", "sosig.toml"};
  static const char* dirs[] = {"public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// A configured aggregate output that collides with a content entry output is rejected, with the
// collision described in the returned diagnostic.
static void test_rejects_duplicate_output(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/index.md", content) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/index.html", "index\n") == 0);

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "content/index.md", "aggregate_templates[0]", "public/index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Exact, not by substring: `expected` is the whole message, so a substring check could not tell
  // it from the same message with something appended.
  TEST_CHECK(err.data != NULL && strcmp(err.data, expected) == 0);
  string_buffer_free(&err);

  cleanup_build_fixture(tmp);
}

// A content entry whose content template cannot be rendered surfaces a per-entry diagnostic through
// the collected build error.
static void test_reports_bad_template(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
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

  // No `templates/content.html` exists, so the page render fails for the entry. The diagnostic
  // names the failing entry and the specific file that could not be read, rather than a generic
  // failure message that cannot be distinguished from an unrelated error.
  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "failed to read template: %s ('templates/content.html') (while rendering "
                         "'content/hello.md')",
                         reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  // Exact: `expected` is the whole message.
  TEST_CHECK(err.data != NULL && strcmp(err.data, expected) == 0);
  string_buffer_free(&err);

  static const char* files[] = {"content/hello.md", "sosig.toml"};
  static const char* dirs[] = {"public", "content"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// Two content entries that both fail to render append one diagnostic line each, so neither is lost
// to the other. This is the postcondition `cmd_build_execute` states and the reason `error_out` is
// growable rather than a fixed buffer: an `append_render_error` that overwrote, or that dropped the
// separator and ran two messages together, would still satisfy every single-entry test.
static void test_reports_one_line_per_failing_entry(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/a.md", first) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/b.md", second) == 0);

  // No `templates/content.html`, so the page render fails for both entries.
  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
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
  TEST_CHECK(err.data != NULL && strcmp(err.data, expected) == 0);
  string_buffer_free(&err);

  static const char* files[] = {"content/a.md", "content/b.md", "sosig.toml"};
  static const char* dirs[] = {"public", "content"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

// An output path that cannot be opened as a file fails the build at the write phase, after every
// entry has rendered successfully. That is the last of `cmd_build_execute`'s phase arms and the
// only one reached with a complete set of rendered pages in hand, so a write failure reported as
// success would otherwise pass the suite with the pages silently missing.
static void test_reports_unwritable_output(void) {
  char dir[] = "/tmp/sosig-build-test.XXXXXX";
  const char* tmp = init_build_fixture(dir);
  if (tmp == NULL) {
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
  TEST_CHECK(write_fixture_file(tmp, "sosig.toml", config) == 0);
  TEST_CHECK(write_fixture_file(tmp, "content/a.md", entry) == 0);
  TEST_CHECK(write_fixture_file(tmp, "templates/content.html", "{{{body}}}\n") == 0);

  struct Arena arena;
  arena_init(&arena);
  char* output_path = path_join(tmp, "public/a.html", &arena);
  TEST_ASSERT(output_path != NULL);
  if (output_path == NULL) {
    arena_free(&arena);
    return;
  }
  // A directory at the page path cannot be opened as a file even by a privileged process.
  TEST_CHECK(fs_mkdir_p(output_path, NULL, 0) == 0);

  struct StringBuffer err;
  string_buffer_init(&err);
  TEST_CHECK(execute_build_in_dir(tmp, &err) != 0);
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(EISDIR));
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected),
               "failed to write output: %s (for 'content/a.md', to 'public/a.html')", reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(err.data != NULL && strcmp(err.data, expected) == 0);
  string_buffer_free(&err);
  arena_free(&arena);

  static const char* files[] = {"content/a.md", "templates/content.html", "sosig.toml"};
  static const char* dirs[] = {"public/a.html", "public", "content", "templates"};
  cleanup_named_paths(tmp, files, sizeof(files) / sizeof(files[0]), dirs,
                      sizeof(dirs) / sizeof(dirs[0]));
}

TEST_LIST = {
    {"honors configured content template", test_honors_configured_content_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"mirrors nested source tree", test_mirrors_nested_source_tree},
    {"tolerates trailing slash on content_dir", test_tolerates_trailing_slash_on_content_dir},
    {"honors custom permalink", test_honors_custom_permalink},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"leaves error buffer empty on success", test_leaves_error_buffer_empty_on_success},
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
