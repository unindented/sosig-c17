#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cmd_config.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "runtime/fs.h"
#include "test_support.h"

/**
 * @brief Writes text to a fixture file.
 *
 * @param file_path Path of the file to write.
 * @param contents  Terminated text to write.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_text_file(const char* file_path, const char* contents) {
  FILE* stream = fopen(file_path, "wb");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return -1;
  }
  int rc = fputs(contents, stream) == EOF ? -1 : 0;
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);
  if (close_rc != 0) {
    rc = -1;
  }
  return rc;
}

/**
 * @brief Runs the config command in a fixture while capturing both standard streams.
 *
 * Restores both streams and the working directory before returning.
 *
 * @param root_dir       Fixture directory in which to run the command.
 * @param stdout_out     Buffer that receives terminated standard output.
 * @param stdout_out_len Size of `stdout_out` in bytes. Must be non-zero.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode run_config_capturing(const char* root_dir,
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

    rc = cmd_config_run();
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
 * @brief Runs the config command with an unwritable standard output stream.
 *
 * Captures the diagnostic and restores both streams and the working directory before returning.
 *
 * @param root_dir       Fixture directory in which to run the command.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode run_config_with_unwritable_stdout(const char* root_dir,
                                                       char* stderr_out,
                                                       size_t stderr_out_len) {
  stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  bool has_changed_dir = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  int unwritable = -1;
  int sink = -1;
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

    rc = cmd_config_run();
    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stderr_flush_rc != 0;

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

// A valid `sosig.toml` loads and prints, and the command reports success.
static void test_prints_loaded_config(void) {
  char root_dir_template[] = "/tmp/sosig-cmd-config.XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", root_dir_template);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n"
                              "author = \"Author Name\"\n") == 0);

  char stdout_out[8192];
  char stderr_out[8192];
  const enum ExitCode rc = run_config_capturing(root_dir, stdout_out, sizeof(stdout_out),
                                                stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_OK);
  // The whole capture, not two per-line searches: exact comparison catches a missing, duplicated or
  // reordered key. The empty `stderr_out` below is the routing half: the configuration goes to
  // `stdout`, and the success path says nothing on `stderr`.
  TEST_CHECK(stderr_out[0] == '\0');
  TEST_CHECK(strcmp(stdout_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Test Site\"\n"
                    "author = \"Author Name\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = [\"index.html\"]\n"
                    "feed_templates = [\"atom.xml\"]\n"
                    "feed_count = 10\n") == 0);

  TEST_CHECK(unlink(toml_path) == 0);
  TEST_CHECK(rmdir(root_dir) == 0);
}

// A config whose template arrays are empty prints and reports success. A loader that accepted `[]`
// and left the array `NULL` would hand that null to a `nonnull` parameter and abort here. This is
// the end-to-end half of the pair: `test_print_empty_template_arrays` covers the same composition
// at the module boundary.
static void test_prints_empty_template_arrays(void) {
  char root_dir_template[] = "/tmp/sosig-cmd-config-empty.XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", root_dir_template);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n"
                              "author = \"Author Name\"\n"
                              "aggregate_templates = []\n"
                              "feed_templates = []\n") == 0);

  char stdout_out[8192];
  char stderr_out[8192];
  const enum ExitCode rc = run_config_capturing(root_dir, stdout_out, sizeof(stdout_out),
                                                stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_OK);
  // Compared whole, and `stderr` empty, for the reasons at `test_prints_loaded_config`.
  TEST_CHECK(stderr_out[0] == '\0');
  TEST_CHECK(strcmp(stdout_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Test Site\"\n"
                    "author = \"Author Name\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = []\n"
                    "feed_templates = []\n"
                    "feed_count = 10\n") == 0);

  TEST_CHECK(unlink(toml_path) == 0);
  TEST_CHECK(rmdir(root_dir) == 0);
}

// A missing `sosig.toml` is reported with the read failure that caused it, naming the path, so this
// cannot pass on an unrelated failure such as a `chdir` or a template error. The reason is derived
// from the running libc rather than hardcoded, so the assertion is the whole claim and stays
// portable.
static void test_reports_missing_config(void) {
  char root_dir_template[] = "/tmp/sosig-cmd-config-missing.XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }

  char stdout_out[8192];
  char stderr_out[8192];
  const enum ExitCode rc = run_config_capturing(root_dir, stdout_out, sizeof(stdout_out),
                                                stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')\n",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  // The diagnostic goes to `stderr`. A failed run prints nothing on `stdout`, so a pipeline
  // redirecting `stdout` to a file does not capture the error into it.
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
  TEST_CHECK(stdout_out[0] == '\0');

  TEST_CHECK(rmdir(root_dir) == 0);
}

// A well-formed `sosig.toml` that omits a required key fails, and fails with the missing-key
// message rather than the wrong-type one. An exact line assertion preserves that distinction.
static void test_reports_missing_required_key(void) {
  char root_dir_template[] = "/tmp/sosig-cmd-config-invalid.XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", root_dir_template);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  // Missing the required `author` key.
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n") == 0);

  char stdout_out[8192];
  char stderr_out[8192];
  const enum ExitCode rc = run_config_capturing(root_dir, stdout_out, sizeof(stdout_out),
                                                stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(strcmp(stderr_out, "missing required config key 'author'\n") == 0);
  TEST_CHECK(stdout_out[0] == '\0');

  TEST_CHECK(unlink(toml_path) == 0);
  TEST_CHECK(rmdir(root_dir) == 0);
}

// A `sosig.toml` that loads fine but cannot be written out fails with the stream's own reason,
// rather than exiting 0 when nobody received the output. This is the command's second documented
// failure mode and the one `main` relies on to tell a closed pipe from a full disk. A read-only
// `/dev/null` on `STDOUT_FILENO` is the deterministic way to reach that failure: the writes fail,
// `site_config_print` reports it. The diagnostic still reaches the captured `stderr`. Runs its own
// redirection rather than `run_config_capturing`, which needs a working `stdout` to capture.
static void test_reports_unwritable_stdout(void) {
  char root_dir_template[] = "/tmp/sosig-cmd-config-unwritable.XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", root_dir_template);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n"
                              "author = \"Author Name\"\n") == 0);

  char stderr_out[8192];
  const enum ExitCode rc =
      run_config_with_unwritable_stdout(root_dir, stderr_out, sizeof(stderr_out));

  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  // `EBADF`: the writes go to a descriptor opened read-only. The reason is derived from the running
  // libc rather than hardcoded, and the whole line is compared so a reworded prefix fails too.
  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write resolved config to 'stdout': %s\n",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  TEST_CHECK(unlink(toml_path) == 0);
  TEST_CHECK(rmdir(root_dir) == 0);
}

TEST_LIST = {{"prints loaded config", test_prints_loaded_config},
             {"prints empty template arrays", test_prints_empty_template_arrays},
             {"reports missing config", test_reports_missing_config},
             {"reports missing required key", test_reports_missing_required_key},
             {"reports unwritable stdout", test_reports_unwritable_stdout},
             {NULL, NULL}};
