#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cmd_config.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "runtime/fs.h"

// Writes `contents` to `path`, returning 0 on success. Only `fclose`'s result is checked, so a
// short `fputs` surfaces as a close failure or not at all. That is a deliberate shortcut,
// acceptable because every caller writes a few dozen bytes. A failure here is a broken test rather
// than behavior under test.
static int write_text_file(const char* path, const char* contents) {
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    return -1;
  }
  fputs(contents, fp);
  return fclose(fp);
}

// Runs `cmd_config_run` with the working directory set to `root_dir`, capturing `stdout` and
// `stderr` into *separate* buffers and restoring both streams and the working directory afterward.
// Separate captures rather than one: the command's contract routes success output to `stdout` and
// diagnostics to `stderr`, so a merged buffer proves a message was printed but not which stream
// carried it. Each test verifies the routing by checking both buffers.
static enum ExitCode run_in_dir(const char* root_dir,
                                char* stdout_out,
                                size_t stdout_out_len,
                                char* stderr_out,
                                size_t stderr_out_len) {
  char cwd[PATH_MAX];
  TEST_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL);
  TEST_ASSERT(chdir(root_dir) == 0);

  fflush(stdout);
  fflush(stderr);
  int saved_out = dup(STDOUT_FILENO);
  int saved_err = dup(STDERR_FILENO);
  FILE* out_capture = tmpfile();
  FILE* err_capture = tmpfile();
  TEST_ASSERT(saved_out >= 0 && saved_err >= 0 && out_capture != NULL && err_capture != NULL);
  // Unreachable, since the assert above aborts. This code lets static analysis see three facts. A
  // null capture never reaches `fileno`. If only one capture opens, the code closes it. Both output
  // parameters are terminated. The caller reads them whatever this returns, and two of its tests
  // expect `EXIT_CODE_FAILURE`, so it cannot tell a failed capture apart from a failed command by
  // the return value alone. The saved descriptors are deliberately not closed here: an aborted test
  // leaves them to process exit either way.
  if (out_capture == NULL || err_capture == NULL) {
    if (out_capture != NULL) {
      fclose(out_capture);
    }
    if (err_capture != NULL) {
      fclose(err_capture);
    }
    stdout_out[0] = '\0';
    stderr_out[0] = '\0';
    return EXIT_CODE_FAILURE;
  }
  dup2(fileno(out_capture), STDOUT_FILENO);
  dup2(fileno(err_capture), STDERR_FILENO);

  const enum ExitCode rc = cmd_config_run();

  fflush(stdout);
  fflush(stderr);
  dup2(saved_out, STDOUT_FILENO);
  dup2(saved_err, STDERR_FILENO);
  close(saved_out);
  close(saved_err);

  rewind(out_capture);
  const size_t out_n = fread(stdout_out, 1, stdout_out_len - 1, out_capture);
  stdout_out[out_n] = '\0';
  fclose(out_capture);
  rewind(err_capture);
  const size_t err_n = fread(stderr_out, 1, stderr_out_len - 1, err_capture);
  stderr_out[err_n] = '\0';
  fclose(err_capture);

  TEST_ASSERT(chdir(cwd) == 0);
  return rc;
}

// A valid `sosig.toml` loads and prints, and the command reports success.
static void test_prints_loaded_config(void) {
  char dir[] = "/tmp/sosig-cmd-config.XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n"
                              "author = \"Author Name\"\n") == 0);

  char out[8192];
  char err[8192];
  const enum ExitCode rc = run_in_dir(dir, out, sizeof(out), err, sizeof(err));
  TEST_CHECK(rc == EXIT_CODE_OK);
  // The whole capture, not two per-line searches: exact comparison catches a missing, duplicated or
  // reordered key. The empty `err` below is the routing half: the configuration goes to `stdout`,
  // and the success path says nothing on `stderr`.
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(out,
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

  unlink(toml_path);
  rmdir(dir);
}

// A config whose template arrays are empty prints and reports success. A loader that accepted `[]`
// and left the array `NULL` would hand that null to a `nonnull` parameter and abort here. This is
// the end-to-end half of the pair: `test_print_empty_template_arrays` covers the same composition
// at the module boundary.
static void test_prints_empty_template_arrays(void) {
  char dir[] = "/tmp/sosig-cmd-config-empty.XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n"
                              "author = \"Author Name\"\n"
                              "aggregate_templates = []\n"
                              "feed_templates = []\n") == 0);

  char out[8192];
  char err[8192];
  const enum ExitCode rc = run_in_dir(dir, out, sizeof(out), err, sizeof(err));
  TEST_CHECK(rc == EXIT_CODE_OK);
  // Compared whole, and `stderr` empty, for the reasons at `test_prints_loaded_config`.
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(out,
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

  unlink(toml_path);
  rmdir(dir);
}

// A missing `sosig.toml` is reported with the read failure that caused it, naming the path, so this
// cannot pass on an unrelated failure such as a `chdir` or a template error. The reason is derived
// from the running libc rather than hardcoded, so the assertion is the whole claim and stays
// portable.
static void test_reports_missing_config(void) {
  char dir[] = "/tmp/sosig-cmd-config-missing.XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);

  char out[8192];
  char err[8192];
  const enum ExitCode rc = run_in_dir(dir, out, sizeof(out), err, sizeof(err));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
  char expected[ERROR_MESSAGE_SIZE];
  (void)snprintf(expected, sizeof(expected), "failed to read config: %s ('sosig.toml')\n", reason);
  // The diagnostic goes to `stderr`. A failed run prints nothing on `stdout`, so a pipeline
  // redirecting `stdout` to a file does not capture the error into it.
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_CHECK(out[0] == '\0');

  rmdir(dir);
}

// A well-formed `sosig.toml` that omits a required key fails, and fails with the missing-key
// message rather than the wrong-type one. An exact line assertion preserves that distinction.
static void test_reports_missing_required_key(void) {
  char dir[] = "/tmp/sosig-cmd-config-invalid.XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  // Missing the required `author` key.
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n") == 0);

  char out[8192];
  char err[8192];
  const enum ExitCode rc = run_in_dir(dir, out, sizeof(out), err, sizeof(err));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(strcmp(err, "missing required config key 'author'\n") == 0);
  TEST_CHECK(out[0] == '\0');

  unlink(toml_path);
  rmdir(dir);
}

// A `sosig.toml` that loads fine but cannot be written out fails with the stream's own reason,
// rather than exiting 0 when nobody received the output. This is the command's second documented
// failure mode and the one `main` relies on to tell a closed pipe from a full disk. A read-only
// `/dev/null` on `STDOUT_FILENO` is the deterministic way to reach that failure: the writes fail,
// `site_config_print` reports it. The diagnostic still reaches the captured `stderr`. Runs its own
// redirection rather than `run_in_dir`, which needs a working `stdout` to capture.
static void test_reports_unwritable_stdout(void) {
  char dir[] = "/tmp/sosig-cmd-config-unwritable.XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);
  char toml_path[PATH_MAX];
  const int n = snprintf(toml_path, sizeof(toml_path), "%s/sosig.toml", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_path));
  TEST_ASSERT(write_text_file(toml_path,
                              "base_url = \"https://example.com\"\n"
                              "title = \"Test Site\"\n"
                              "author = \"Author Name\"\n") == 0);

  char cwd[PATH_MAX];
  TEST_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL);
  TEST_ASSERT(chdir(dir) == 0);

  fflush(stdout);
  fflush(stderr);
  const int saved_out = dup(STDOUT_FILENO);
  const int saved_err = dup(STDERR_FILENO);
  const int unwritable = open("/dev/null", O_RDONLY);
  FILE* err_capture = tmpfile();
  TEST_ASSERT(saved_out >= 0 && saved_err >= 0 && unwritable >= 0 && err_capture != NULL);
  if (err_capture == NULL) {
    return;
  }
  dup2(unwritable, STDOUT_FILENO);
  dup2(fileno(err_capture), STDERR_FILENO);

  const enum ExitCode rc = cmd_config_run();

  fflush(stdout);
  fflush(stderr);
  dup2(saved_out, STDOUT_FILENO);
  dup2(saved_err, STDERR_FILENO);
  close(saved_out);
  close(saved_err);
  close(unwritable);

  char err[8192];
  rewind(err_capture);
  const size_t err_n = fread(err, 1, sizeof(err) - 1, err_capture);
  err[err_n] = '\0';
  fclose(err_capture);
  TEST_ASSERT(chdir(cwd) == 0);

  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  // `EBADF`: the writes go to a descriptor opened read-only. The reason is derived from the running
  // libc rather than hardcoded, and the whole line is compared so a reworded prefix fails too.
  char reason[ERROR_MESSAGE_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(EBADF));
  char expected[ERROR_MESSAGE_SIZE * 2];
  (void)snprintf(expected, sizeof(expected), "failed to write resolved config to 'stdout': %s\n",
                 reason);
  TEST_CHECK(strcmp(err, expected) == 0);

  unlink(toml_path);
  rmdir(dir);
}

TEST_LIST = {{"prints loaded config", test_prints_loaded_config},
             {"prints empty template arrays", test_prints_empty_template_arrays},
             {"reports missing config", test_reports_missing_config},
             {"reports missing required key", test_reports_missing_required_key},
             {"reports unwritable stdout", test_reports_unwritable_stdout},
             {NULL, NULL}};
