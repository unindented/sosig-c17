#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/cli_dispatch.h"
#include "app/exit_code.h"
#include "app/sosig_version.h"
#include "core/error.h"
#include "core/path.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/**
 * Captured output of one `cli_dispatch` call. Both streams are sized for the whole usage text,
 * which is the longest thing any dispatch path writes.
 */
struct DispatchOutput {
  /** Everything written to `stdout`, terminated. */
  char stdout_out[8192];

  /** Everything written to `stderr`, terminated. */
  char stderr_out[4096];
};

/**
 * @brief Runs CLI dispatch while capturing both standard streams.
 *
 * Redirects descriptors so both streams can be restored after the call. Clears stream errors left
 * by write-failure paths before returning.
 *
 * @param argc         Number of arguments in `argv`.
 * @param argv         Argument vector passed to `cli_dispatch`.
 * @param dispatch_out Captured, terminated `stdout` and `stderr` text.
 * @return The dispatch exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode dispatch_capturing(int argc,
                                        char** argv,
                                        struct DispatchOutput* dispatch_out) {
  dispatch_out->stdout_out[0] = '\0';
  dispatch_out->stderr_out[0] = '\0';

  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  FILE* stdout_capture = NULL;
  FILE* stderr_capture = NULL;

  const int stdout_flush_rc = fflush(stdout);
  const int stderr_flush_rc = fflush(stderr);
  TEST_CHECK(stdout_flush_rc == 0);
  TEST_CHECK(stderr_flush_rc == 0);
  if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
    clearerr(stdout);
    clearerr(stderr);
    return rc;
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

  {
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

    rc = cli_dispatch(argc, argv);
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
    has_plumbing_failed = read_capture(stdout_capture, dispatch_out->stdout_out,
                                       sizeof(dispatch_out->stdout_out)) != 0 ||
                          has_plumbing_failed;
    const int close_rc = fclose(stdout_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (stderr_capture != NULL) {
    has_plumbing_failed = read_capture(stderr_capture, dispatch_out->stderr_out,
                                       sizeof(dispatch_out->stderr_out)) != 0 ||
                          has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  return has_plumbing_failed ? (enum ExitCode)EXIT_CODE_VALUE_MAX : rc;
}

/**
 * @brief Creates and enters a minimal buildable site fixture.
 *
 * Cleans partial setup before returning a failure.
 *
 * @param root_dir            Writable `mkdtemp` template. Receives the fixture root.
 * @param working_dir_out     Buffer that receives the original working directory.
 * @param working_dir_out_len Size of `working_dir_out` in bytes.
 * @return `root_dir` on success, or `NULL` after recording a test-plumbing failure.
 */
static const char* enter_site_fixture(char root_dir[static 1],
                                      char* working_dir_out,
                                      size_t working_dir_out_len) {
  if (getcwd(working_dir_out, working_dir_out_len) == NULL) {
    TEST_CHECK(false);
    return NULL;
  }
  char* created_root_dir = mkdtemp(root_dir);
  TEST_ASSERT(created_root_dir != NULL);
  if (created_root_dir == NULL) {
    return NULL;
  }

  const char* result = NULL;
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
  const bool written = fs_write_file(path_join(created_root_dir, "sosig.toml", &arena), config,
                                     sizeof(config) - 1, NULL, 0) == 0 &&
                       fs_write_file(path_join(created_root_dir, "content/hello.md", &arena), entry,
                                     sizeof(entry) - 1, NULL, 0) == 0 &&
                       fs_write_file(path_join(created_root_dir, "templates/content.html", &arena),
                                     "{{{body}}}\n", strlen("{{{body}}}\n"), NULL, 0) == 0;
  TEST_CHECK(written);
  if (!written) {
    goto cleanup;
  }
  {
    const int chdir_rc = chdir(created_root_dir);
    TEST_CHECK(chdir_rc == 0);
    if (chdir_rc == 0) {
      result = created_root_dir;
    }
  }

cleanup:
  arena_free(&arena);
  if (result == NULL) {
    remove_fixture_tree(created_root_dir);
  }
  return result;
}

/**
 * @brief Restores the working directory and removes a site fixture.
 *
 * @param root_dir    Fixture root directory to remove.
 * @param working_dir Original working directory to restore.
 */
static void leave_site_fixture(const char* root_dir, const char* working_dir) {
  const int restore_rc = chdir(working_dir);
  TEST_CHECK(restore_rc == 0);
  remove_fixture_tree(root_dir);
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"sosig", "--version", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  char expected[64];
  const int n = snprintf(expected, sizeof(expected), "sosig %s\n", sosig_version_string());
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stdout_out, expected) == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// `--help` writes the usage text to `stdout` and reports success. The program name comes from
// `argv[0]`. Using a name that is deliberately not `"sosig"` proves the function substitutes it
// rather than hard-coding it. `test_cli.c` pins the text itself, so this test asserts only the
// plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/mysosig", "--help", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(strstr(dispatch_out.stdout_out, "/opt/bin/mysosig") != NULL);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// The `config` command is dispatched to `cmd_config_run`, not to a build: the resolved
// configuration lands on `stdout` and no `public/` directory appears. Dispatching it to the wrong
// command would otherwise still exit `0` in a valid site.
static void test_config_command_is_dispatched(void) {
  char root_dir_template[] = "/tmp/sosig-dispatch-site.XXXXXX";
  char working_dir[PATH_MAX];
  const char* root_dir = enter_site_fixture(root_dir_template, working_dir, sizeof(working_dir));
  if (root_dir == NULL) {
    return;
  }

  char* argv[] = {"sosig", "config", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(strcmp(dispatch_out.stdout_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Site\"\n"
                    "author = \"Author\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = []\n"
                    "feed_templates = []\n"
                    "feed_count = 10\n") == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
  // No page was written, which is what separates `config` from `build`.
  TEST_CHECK(access("public/hello.html", F_OK) != 0);

  leave_site_fixture(root_dir, working_dir);
}

// The `build` command is dispatched to `cmd_build_run` with the parsed options attached, so the
// worker count and the verbose flag reach the build rather than being dropped on the way. The
// verbose phase lines naming the requested count are the only externally visible evidence that both
// fields were carried across.
static void test_build_command_receives_parsed_options(void) {
  char root_dir_template[] = "/tmp/sosig-dispatch-site.XXXXXX";
  char working_dir[PATH_MAX];
  const char* root_dir = enter_site_fixture(root_dir_template, working_dir, sizeof(working_dir));
  if (root_dir == NULL) {
    return;
  }

  char* argv[] = {"sosig", "build", "--verbose", "--workers=3", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
  TEST_CHECK(strcmp(dispatch_out.stderr_out,
                    "loading config\n"
                    "discovering content\n"
                    "parsing content with 3 workers\n"
                    ".\n"
                    "collecting content entries\n"
                    "building output manifest\n"
                    "rendering content with 3 workers\n"
                    ".\n"
                    "writing content pages\n"
                    "rendering aggregate templates\n"
                    "rendering feed templates\n"
                    "build complete\n") == 0);
  TEST_CHECK(access("public/hello.html", F_OK) == 0);

  leave_site_fixture(root_dir, working_dir);
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. The exit code is what nothing else pins: `2` is what a
// shell or CI wrapper branches on to tell a bad invocation from a failed build, and `test_cli.c`
// asserts only that `cli_parse` produced the message.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"sosig", "--bogus", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_USAGE);
  TEST_CHECK(strcmp(dispatch_out.stderr_out, "unknown option '--bogus'\n") == 0);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
}

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and names
// both the subject and the stream's own reason. Nothing else in the suite reaches this arm: every
// other path writes to a working descriptor, so nothing else pins the mapping from a printer's
// non-zero result to an exit code. A read-only descriptor is the deterministic way there, since the
// write fails inside the printer's `fflush` rather than at the `fprintf`.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"sosig", "--version", NULL};
  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  int unwritable = -1;
  int sink = -1;
  FILE* stderr_capture = NULL;

  const int stdout_flush_rc = fflush(stdout);
  const int stderr_flush_rc = fflush(stderr);
  TEST_CHECK(stdout_flush_rc == 0);
  TEST_CHECK(stderr_flush_rc == 0);
  if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
    clearerr(stdout);
    clearerr(stderr);
    return;
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
  unwritable = open("/dev/null", O_RDONLY);
  TEST_CHECK(unwritable >= 0);
  if (unwritable < 0) {
    goto cleanup;
  }
  sink = open("/dev/null", O_WRONLY);
  TEST_CHECK(sink >= 0);
  if (sink < 0) {
    goto cleanup;
  }
  stderr_capture = tmpfile();
  TEST_ASSERT(stderr_capture != NULL);
  if (stderr_capture == NULL) {
    goto cleanup;
  }

  {
    const int stdout_redirect_rc = dup2(unwritable, STDOUT_FILENO);
    TEST_CHECK(stdout_redirect_rc == STDOUT_FILENO);
    if (stdout_redirect_rc != STDOUT_FILENO) {
      goto cleanup;
    }
    const int stderr_redirect_rc = dup2(fileno(stderr_capture), STDERR_FILENO);
    TEST_CHECK(stderr_redirect_rc == STDERR_FILENO);
    if (stderr_redirect_rc != STDERR_FILENO) {
      goto cleanup;
    }

    rc = cli_dispatch(2, argv);
    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stderr_flush_rc != 0;

    // The failed write left the version text in `stdout`'s buffer. Drain it into a writable sink
    // before restoring the real descriptor, or the next successful flush prints it to the harness.
    const int sink_redirect_rc = dup2(sink, STDOUT_FILENO);
    TEST_CHECK(sink_redirect_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || sink_redirect_rc != STDOUT_FILENO;
    if (sink_redirect_rc == STDOUT_FILENO) {
      clearerr(stdout);
      const int drain_rc = fflush(stdout);
      TEST_CHECK(drain_rc == 0);
      has_plumbing_failed = has_plumbing_failed || drain_rc != 0;
    }
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
  if (unwritable >= 0) {
    const int close_rc = close(unwritable);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (sink >= 0) {
    const int close_rc = close(sink);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }

  char stderr_out[4096] = "";
  if (stderr_capture != NULL) {
    has_plumbing_failed =
        read_capture(stderr_capture, stderr_out, sizeof(stderr_out)) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }

  TEST_CHECK(!has_plumbing_failed);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write version to 'stdout': %s\n",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
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
