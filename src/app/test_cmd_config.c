#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
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
 * @return The command exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode run_config_capturing(const char* root_dir,
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
      rc = cmd_config_run();
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, stderr_out_len) != 0;
    }
    has_plumbing_failed =
        capture_end(&stdout_capture, stdout_out, stdout_out_len) != 0 || has_plumbing_failed;
  }
  has_plumbing_failed = working_dir_leave(saved_dir_fd) != 0 || has_plumbing_failed;
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

/**
 * @brief Runs the config command with an unwritable standard output stream.
 *
 * Captures the diagnostic and restores both streams and the working directory before returning.
 *
 * @param root_dir       Fixture directory in which to run the command.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode run_config_with_unwritable_stdout(const char* root_dir,
                                                       char* stderr_out,
                                                       size_t stderr_out_len) {
  stderr_out[0] = '\0';
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return (enum ExitCode)TEST_PLUMBING_FAILED;
  }

  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin_unwritable(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cmd_config_run();
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, stderr_out_len) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, NULL, 0) != 0 || has_plumbing_failed;
  }
  has_plumbing_failed = working_dir_leave(saved_dir_fd) != 0 || has_plumbing_failed;
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
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
// failure mode, and the reason it reports is what lets the user tell a closed pipe from a full
// disk. A read-only `/dev/null` on `STDOUT_FILENO` is the deterministic way to reach that failure:
// the writes fail, `site_config_print` reports it. The diagnostic still reaches the captured
// `stderr`. Runs its own redirection rather than `run_config_capturing`, which needs a working
// `stdout` to capture.
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

TEST_LIST = {
    {"prints loaded config", test_prints_loaded_config},
    {"prints empty template arrays", test_prints_empty_template_arrays},
    {"reports missing config", test_reports_missing_config},
    {"reports missing required key", test_reports_missing_required_key},
    {"reports unwritable stdout", test_reports_unwritable_stdout},
    {NULL, NULL},
};
