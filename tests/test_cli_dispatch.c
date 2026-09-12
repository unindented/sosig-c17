#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cli_dispatch.h"
#include "app/exit_code.h"
#include "core/arena.h"
#include "core/path.h"
#include "runtime/fs.h"

/**
 * Captured output of one `cli_dispatch` call. Both streams are sized for the whole usage text,
 * which is the longest thing any dispatch path writes.
 */
struct DispatchOutput {
  /** Everything written to `stdout`, terminated. */
  char out[8192];

  /** Everything written to `stderr`, terminated. */
  char err[4096];
};

// Reads a capture file into `captured_out` as a terminated string, truncating if it does not fit.
static void read_capture(const char* path, char* captured_out, size_t captured_out_len) {
  captured_out[0] = '\0';
  FILE* stream = fopen(path, "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  const size_t n = fread(captured_out, 1, captured_out_len - 1, stream);
  captured_out[n] = '\0';
  (void)fclose(stream);
}

// Runs `cli_dispatch` with `argv`, capturing both streams into `captured_out`. Redirected at the
// descriptor rather than with `freopen` so the originals can be restored from a `dup`: a test that
// left them redirected would silently swallow the harness's own output for every later test.
// `clearerr` is required because the write-failure test deliberately leaves an error latched on
// `stdout`. Without it, every later write from this process would fail. Returns the exit code, or
// `EXIT_CODE_VALUE_MAX` when the plumbing itself failed, which no dispatch path returns.
static enum ExitCode dispatch_capturing(int argc,
                                        char** argv,
                                        struct DispatchOutput* captured_out) {
  captured_out->out[0] = '\0';
  captured_out->err[0] = '\0';

  char out_path[] = "/tmp/sosig-dispatch-out.XXXXXX";
  char err_path[] = "/tmp/sosig-dispatch-err.XXXXXX";
  const int out_fd = mkstemp(out_path);
  const int err_fd = mkstemp(err_path);
  TEST_ASSERT(out_fd >= 0 && err_fd >= 0);
  if (out_fd < 0 || err_fd < 0) {
    return (enum ExitCode)EXIT_CODE_VALUE_MAX;
  }
  const int saved_out = dup(STDOUT_FILENO);
  const int saved_err = dup(STDERR_FILENO);
  TEST_ASSERT(saved_out >= 0 && saved_err >= 0);
  if (saved_out < 0 || saved_err < 0) {
    return (enum ExitCode)EXIT_CODE_VALUE_MAX;
  }

  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  fflush(stdout);
  fflush(stderr);
  if (dup2(out_fd, STDOUT_FILENO) >= 0 && dup2(err_fd, STDERR_FILENO) >= 0) {
    rc = cli_dispatch(argc, argv);
    fflush(stdout);
    fflush(stderr);
  }
  (void)dup2(saved_out, STDOUT_FILENO);
  (void)dup2(saved_err, STDERR_FILENO);
  clearerr(stdout);
  clearerr(stderr);
  (void)close(saved_out);
  (void)close(saved_err);
  (void)close(out_fd);
  (void)close(err_fd);

  read_capture(out_path, captured_out->out, sizeof(captured_out->out));
  read_capture(err_path, captured_out->err, sizeof(captured_out->err));
  (void)unlink(out_path);
  (void)unlink(err_path);
  return rc;
}

// Creates a minimal buildable site in a fresh temporary directory and makes it the current
// directory, returning the previous one in `cwd_out` so the caller can restore it. Returns `NULL`
// as a failure sentinel every caller branches on. The `TEST_CHECK` has already failed the test.
static const char* enter_site_fixture(char root_dir[static 1], char* cwd_out, size_t cwd_out_len) {
  if (getcwd(cwd_out, cwd_out_len) == NULL) {
    TEST_CHECK(false);
    return NULL;
  }
  char* tmp = mkdtemp(root_dir);
  TEST_CHECK(tmp != NULL);
  if (tmp == NULL) {
    return NULL;
  }

  struct Arena arena;
  arena_init(&arena);
  static const char config[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Site\"\n"
      "author = \"Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  static const char entry[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const bool written = fs_write_file(path_join(tmp, "sosig.toml", &arena), config,
                                     sizeof(config) - 1, NULL, 0) == 0 &&
                       fs_write_file(path_join(tmp, "content/hello.md", &arena), entry,
                                     sizeof(entry) - 1, NULL, 0) == 0 &&
                       fs_write_file(path_join(tmp, "templates/content.html", &arena),
                                     "{{{body}}}\n", strlen("{{{body}}}\n"), NULL, 0) == 0;
  arena_free(&arena);
  TEST_CHECK(written);
  if (!written || chdir(tmp) != 0) {
    TEST_CHECK(false);
    return NULL;
  }
  return tmp;
}

// Removes the fixture tree and returns to the directory the test started in.
static void leave_site_fixture(const char* root_dir, const char* cwd) {
  TEST_CHECK(chdir(cwd) == 0);
  struct Arena arena;
  arena_init(&arena);
  static const char* files[] = {"public/hello.html", "content/hello.md", "templates/content.html",
                                "sosig.toml"};
  static const char* dirs[] = {"public", "content", "templates"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    (void)unlink(path_join(root_dir, files[i], &arena));
  }
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    (void)rmdir(path_join(root_dir, dirs[i], &arena));
  }
  arena_free(&arena);
  (void)rmdir(root_dir);
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {(char*)"sosig", (char*)"--version", NULL};
  struct DispatchOutput captured;
  TEST_CHECK(dispatch_capturing(2, argv, &captured) == EXIT_CODE_OK);
  // Composed rather than concatenated with the macro: `"sosig " SOSIG_VERSION` is a string
  // concatenation cppcheck cannot parse without the build's `-DSOSIG_VERSION`.
  char expected[64];
  const int n = snprintf(expected, sizeof(expected), "sosig %s\n", SOSIG_VERSION);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(captured.out, expected) == 0);
  TEST_CHECK(captured.err[0] == '\0');
}

// `--help` writes the usage text to `stdout` and reports success. The program name comes from
// `argv[0]`. Using a name that is deliberately not `"sosig"` proves the function substitutes it
// rather than hard-coding it. `test_cli.c` pins the text itself, so this test asserts only the
// plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {(char*)"/opt/bin/mysosig", (char*)"--help", NULL};
  struct DispatchOutput captured;
  TEST_CHECK(dispatch_capturing(2, argv, &captured) == EXIT_CODE_OK);
  TEST_CHECK(strstr(captured.out, "/opt/bin/mysosig") != NULL);
  TEST_CHECK(captured.err[0] == '\0');
}

// The `config` command is dispatched to `cmd_config_run`, not to a build: the resolved
// configuration lands on `stdout` and no `public/` directory appears. Dispatching it to the wrong
// command would otherwise still exit `0` in a valid site.
static void test_config_command_is_dispatched(void) {
  char dir[] = "/tmp/sosig-dispatch-site.XXXXXX";
  char cwd[PATH_MAX];
  const char* tmp = enter_site_fixture(dir, cwd, sizeof(cwd));
  if (tmp == NULL) {
    return;
  }

  char* argv[] = {(char*)"sosig", (char*)"config", NULL};
  struct DispatchOutput captured;
  TEST_CHECK(dispatch_capturing(2, argv, &captured) == EXIT_CODE_OK);
  TEST_CHECK(strstr(captured.out, "https://example.com") != NULL);
  // No page was written, which is what separates `config` from `build`.
  TEST_CHECK(access("public/hello.html", F_OK) != 0);

  leave_site_fixture(dir, cwd);
}

// The `build` command is dispatched to `cmd_build_run` with the parsed options attached, so the
// worker count and the verbose flag reach the build rather than being dropped on the way. The
// verbose phase lines naming the requested count are the only externally visible evidence that both
// fields were carried across.
static void test_build_command_receives_parsed_options(void) {
  char dir[] = "/tmp/sosig-dispatch-site.XXXXXX";
  char cwd[PATH_MAX];
  const char* tmp = enter_site_fixture(dir, cwd, sizeof(cwd));
  if (tmp == NULL) {
    return;
  }

  char* argv[] = {(char*)"sosig", (char*)"build", (char*)"--verbose", (char*)"--workers=3", NULL};
  struct DispatchOutput captured;
  TEST_CHECK(dispatch_capturing(4, argv, &captured) == EXIT_CODE_OK);
  TEST_CHECK(strstr(captured.err, "parsing content with 3 workers\n") != NULL);
  TEST_CHECK(access("public/hello.html", F_OK) == 0);

  leave_site_fixture(dir, cwd);
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. The exit code is what nothing else pins: `2` is what a
// shell or CI wrapper branches on to tell a bad invocation from a failed build, and `test_cli.c`
// asserts only that `cli_parse` produced the message.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {(char*)"sosig", (char*)"--bogus", NULL};
  struct DispatchOutput captured;
  TEST_CHECK(dispatch_capturing(2, argv, &captured) == EXIT_CODE_USAGE);
  TEST_CHECK(strcmp(captured.err, "unknown option '--bogus'\n") == 0);
  TEST_CHECK(captured.out[0] == '\0');
}

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and names
// both the subject and the stream's own reason. Nothing else in the suite reaches this arm: every
// other path writes to a working descriptor, so nothing else pins the mapping from a printer's
// non-zero result to an exit code. A read-only descriptor is the deterministic way there, since the
// write fails inside the printer's `fflush` rather than at the `fprintf`.
static void test_version_write_failure_reports_failure(void) {
  const int devnull = open("/dev/null", O_RDONLY);
  TEST_ASSERT(devnull >= 0);
  if (devnull < 0) {
    return;
  }
  const int saved_out = dup(STDOUT_FILENO);
  const int saved_err = dup(STDERR_FILENO);
  TEST_ASSERT(saved_out >= 0 && saved_err >= 0);
  if (saved_out < 0 || saved_err < 0) {
    (void)close(devnull);
    return;
  }
  char err_path[] = "/tmp/sosig-dispatch-err.XXXXXX";
  const int err_fd = mkstemp(err_path);
  TEST_ASSERT(err_fd >= 0);
  if (err_fd < 0) {
    (void)close(devnull);
    return;
  }

  char* argv[] = {(char*)"sosig", (char*)"--version", NULL};
  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  fflush(stdout);
  fflush(stderr);
  if (dup2(devnull, STDOUT_FILENO) >= 0 && dup2(err_fd, STDERR_FILENO) >= 0) {
    rc = cli_dispatch(2, argv);
    fflush(stderr);
  }
  // The failed write left the version text in `stdout`'s buffer. Drain it into a writable sink
  // before restoring the real descriptor, or the next successful flush from this process prints it
  // to the harness's output.
  const int sink = open("/dev/null", O_WRONLY);
  if (sink >= 0) {
    (void)dup2(sink, STDOUT_FILENO);
    clearerr(stdout);
    fflush(stdout);
    (void)close(sink);
  }
  (void)dup2(saved_out, STDOUT_FILENO);
  (void)dup2(saved_err, STDERR_FILENO);
  clearerr(stdout);
  clearerr(stderr);
  (void)close(saved_out);
  (void)close(saved_err);
  (void)close(devnull);
  (void)close(err_fd);

  char captured[4096];
  read_capture(err_path, captured, sizeof(captured));
  (void)unlink(err_path);

  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  // The subject is named, so a diagnostic that omits the subject fails here.
  TEST_CHECK(strncmp(captured, "failed to write version to 'stdout': ",
                     strlen("failed to write version to 'stdout': ")) == 0);
  // A reason follows the colon rather than an empty tail.
  TEST_CHECK(strlen(captured) > strlen("failed to write version to 'stdout': ") + 1);
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"config command is dispatched", test_config_command_is_dispatched},
    {"build command receives parsed options", test_build_command_receives_parsed_options},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"version write failure reports failure", test_version_write_failure_reports_failure},
    {NULL, NULL},
};
