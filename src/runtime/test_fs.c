#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path.h"
#include "core/path_list.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/**
 * @brief Formats the system reason an `fs` action reports.
 *
 * Derives the text from the running libc so exact assertions remain portable.
 *
 * @param buf          Buffer that receives the terminated reason.
 * @param error_number System error number to describe.
 * @return `buf` containing the system message.
 */
static const char* expected_errno_reason(char buf[static FS_REASON_SIZE], int error_number) {
  const int reason_len = snprintf(buf, FS_REASON_SIZE, "%s", strerror(error_number));
  TEST_ASSERT(reason_len > 0 && (size_t)reason_len < FS_REASON_SIZE);
  return buf;
}

// The `NULL, 0` reason arguments throughout this file are the documented option, not forgotten
// assertions. `fs`'s contract lets `reason` be `NULL` when `reason_len` is 0, and a fixture write
// that fails is a broken test rather than behavior under test. This is the one suite whose subject
// *is* that diagnostic, so the calls that do assert a reason pass a real buffer. No test here takes
// the `NULL`-reason path on a *failing* call. That is deliberate rather than a gap: the pair
// reaches `error_report`, pinned once at that boundary by `test_report_error_accepts_null_buffer`
// instead of once per `fs` action.

// Suffix discovery returns only matching files, walks into subdirectories, and orders results
// deterministically. A successful listing also leaves the reason buffer untouched.
static void test_list_files_matches_suffix(void) {
  char root_dir_template[] = "/tmp/sosig-fs-suffix-match.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* nested_md = path_join(root_dir, "nested/b.md", &arena);
  char* root_md = path_join(root_dir, "a.md", &arena);
  char* ignored = path_join(root_dir, "nested/c.txt", &arena);
  TEST_CHECK(fs_write_file(nested_md, "nested", strlen("nested"), NULL, 0) == 0);
  TEST_CHECK(fs_write_file(root_md, "root", strlen("root"), NULL, 0) == 0);
  TEST_CHECK(fs_write_file(ignored, "ignored", strlen("ignored"), NULL, 0) == 0);

  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "untouched";
  TEST_CHECK(fs_list_files_with_suffix(&paths, root_dir, ".md", reason, sizeof(reason)) == 0);
  TEST_CHECK(paths.count == 2);
  TEST_CHECK(strcmp(paths.items[0], root_md) == 0);
  TEST_CHECK(strcmp(paths.items[1], nested_md) == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  path_list_free(&paths);

  (void)unlink(ignored);
  (void)unlink(nested_md);
  (void)unlink(root_md);
  (void)rmdir(path_join(root_dir, "nested", &arena));
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// An empty suffix matches every regular file in the tree.
static void test_list_files_empty_suffix_matches_all(void) {
  char root_dir_template[] = "/tmp/sosig-fs-suffix.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* md_path = path_join(root_dir, "a.md", &arena);
  char* txt_path = path_join(root_dir, "b.txt", &arena);
  TEST_CHECK(fs_write_file(md_path, "a", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(txt_path, "b", 1, NULL, 0) == 0);

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffix(&paths, root_dir, "", NULL, 0) == 0);
  TEST_CHECK(paths.count == 2);
  path_list_free(&paths);

  (void)unlink(md_path);
  (void)unlink(txt_path);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// An empty directory is a successful listing with no matches, not a failure.
static void test_list_files_accepts_empty_dir(void) {
  char root_dir_template[] = "/tmp/sosig-fs-list-empty.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffix(&paths, root_dir, ".md", NULL, 0) == 0);
  TEST_CHECK(paths.count == 0);
  path_list_free(&paths);

  (void)rmdir(root_dir);
}

// A symlinked directory that loops back into its own ancestry is skipped, so the walk completes and
// still finds the real files instead of recursing until the path overflows.
static void test_list_files_skips_symlink_cycle(void) {
  char root_dir_template[] = "/tmp/sosig-fs-list.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* page = path_join(root_dir, "page.md", &arena);
  char* loop = path_join(root_dir, "loop", &arena);
  TEST_CHECK(fs_write_file(page, "x", 1, NULL, 0) == 0);
  // `loop -> .` resolves to its own directory, so descending into it is a cycle.
  TEST_ASSERT(symlink(".", loop) == 0);

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffix(&paths, root_dir, ".md", NULL, 0) == 0);
  TEST_CHECK(paths.count == 1);
  TEST_CHECK(strcmp(paths.items[0], page) == 0);
  path_list_free(&paths);

  (void)unlink(loop);
  (void)unlink(page);
  arena_free(&arena);
  (void)rmdir(root_dir);
}

// A symlinked directory whose target is an outer ancestor, rather than the directory holding it, is
// skipped too. This is the case that distinguishes the ancestor chain from a single device/inode
// pair. The matching crumb is the second in the chain, so it is found only by walking past the
// innermost one.
static void test_list_files_skips_symlink_cycle_to_ancestor(void) {
  char root_dir_template[] = "/tmp/sosig-fs-list-ancestor.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* page = path_join(root_dir, "sub/page.md", &arena);
  char* up = path_join(root_dir, "sub/up", &arena);
  TEST_CHECK(fs_write_file(page, "x", 1, NULL, 0) == 0);
  // `sub/up -> ..` resolves to the walk's root, which is `sub`'s parent rather than `sub` itself,
  // so the cycle is one crumb further out than the directory being scanned.
  TEST_ASSERT(symlink("..", up) == 0);

  struct PathList paths;
  path_list_init(&paths);
  TEST_CHECK(fs_list_files_with_suffix(&paths, root_dir, ".md", NULL, 0) == 0);
  TEST_CHECK(paths.count == 1);
  TEST_CHECK(strcmp(paths.items[0], page) == 0);
  path_list_free(&paths);

  (void)unlink(up);
  (void)unlink(page);
  (void)rmdir(path_join(root_dir, "sub", &arena));
  arena_free(&arena);
  (void)rmdir(root_dir);
}

// A missing directory fails and names the root it could not reach, so the caller does not have to
// repeat it.
static void test_list_files_rejects_missing_dir(void) {
  char root_dir_template[] = "/tmp/sosig-fs-list-missing.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* missing = path_join(root_dir, "does-not-exist", &arena);
  struct PathList missing_paths;
  path_list_init(&missing_paths);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_list_files_with_suffix(&missing_paths, missing, ".md", reason, sizeof(reason)) ==
             -1);
  // Every walk failure names its own path, so the reason is self-contained.
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "cannot inspect directory: %s ('%s')",
               expected_errno_reason(message, ENOENT), missing);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);
  path_list_free(&missing_paths);

  arena_free(&arena);
  (void)rmdir(root_dir);
}

// A regular file passed as the root is rejected by the directory open rather than by the `stat`
// that seeds the ancestor chain, which deliberately does not check the type. The directory open
// prevents a walk when `content_dir` names a file instead of a tree.
static void test_list_files_rejects_file_root(void) {
  char root_dir_template[] = "/tmp/sosig-fs-list-file-root.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* plain = path_join(root_dir, "plain.txt", &arena);
  TEST_CHECK(fs_write_file(plain, "x", 1, NULL, 0) == 0);

  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_list_files_with_suffix(&paths, plain, ".md", reason, sizeof(reason)) == -1);
  TEST_CHECK(paths.count == 0);
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected), "cannot open directory: %s ('%s')",
                                    expected_errno_reason(message, ENOTDIR), plain);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);
  path_list_free(&paths);

  (void)unlink(plain);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// An entry that exists but cannot be inspected fails the walk and names that entry, rather than
// being dropped from the listing while the walk still reports success. A directory that is readable
// but not searchable is the reachable way to produce it: `readdir` lists the entry, and `stat` on
// the entry then fails with `EACCES`.
static void test_list_files_rejects_unstatable_entry(void) {
  // Root bypasses the permission bits, so the sealed directory would be walked like any other and
  // the assertions below would fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir_template[] = "/tmp/sosig-fs-list-unstatable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* sealed_dir = path_join(root_dir, "sealed", &arena);
  char* sealed_md = path_join(root_dir, "sealed/a.md", &arena);
  TEST_CHECK(fs_write_file(sealed_md, "hidden", strlen("hidden"), NULL, 0) == 0);
  TEST_CHECK(chmod(sealed_dir, 0400) == 0);

  struct PathList paths;
  path_list_init(&paths);
  char reason[FS_REASON_SIZE] = "";
  const int rc = fs_list_files_with_suffix(&paths, root_dir, ".md", reason, sizeof(reason));
  // Restore the mode before asserting: a failing assertion aborts the test, and neither the cleanup
  // below nor the harness can remove a directory it is not allowed to search.
  (void)chmod(sealed_dir, 0700);

  TEST_CHECK(rc == -1);
  TEST_CHECK(paths.count == 0);
  char expected_entry[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected_entry, sizeof(expected_entry), "cannot inspect entry: %s ('%s')",
               expected_errno_reason(message, EACCES), sealed_md);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected_entry));
  TEST_CHECK(strcmp(reason, expected_entry) == 0);
  path_list_free(&paths);

  (void)unlink(sealed_md);
  (void)rmdir(sealed_dir);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// An empty file reads back as zero bytes with a valid terminator.
static void test_read_file_accepts_empty(void) {
  char root_dir_template[] = "/tmp/sosig-fs-empty.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* empty_path = path_join(root_dir, "empty.md", &arena);
  TEST_CHECK(fs_write_file(empty_path, "", 0, NULL, 0) == 0);

  char* file_data = NULL;
  size_t file_len = 123;
  TEST_CHECK(fs_read_file(empty_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == 0);
  TEST_CHECK(file_data != NULL && file_data[0] == '\0');
  free(file_data);

  (void)unlink(empty_path);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_read_file` rejects a missing path and a directory, leaving outputs untouched, and reports the
// two as different reasons rather than one indistinguishable failure.
static void test_read_file_rejects_missing_and_non_regular(void) {
  char root_dir_template[] = "/tmp/sosig-fs-read.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* missing = path_join(root_dir, "missing.md", &arena);

  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(missing, &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  char expected[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, ENOENT)) == 0);

  // A directory is not a regular file. This is a first-party rejection, not a system error, so it
  // carries its own wording.
  file_data = sentinel;
  file_len = 999;
  reason[0] = '\0';
  TEST_CHECK(fs_read_file(root_dir, &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);

  arena_free(&arena);
  (void)rmdir(root_dir);
}

// A file carrying an embedded `NUL` is rejected, leaving outputs untouched, and says so. This is
// the boundary that establishes the `NUL`-free text invariant every downstream `strlen` relies on.
// Accepting it would silently truncate the rendered output at the `NUL`.
static void test_read_file_rejects_embedded_nul(void) {
  char root_dir_template[] = "/tmp/sosig-fs-nul.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* nul_path = path_join(root_dir, "nul.md", &arena);
  TEST_CHECK(fs_write_file(nul_path, "before\0after", sizeof("before\0after") - 1, NULL, 0) == 0);

  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(nul_path, &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  // The reason names the policy. No system error occurred and the file reads fine otherwise.
  TEST_CHECK(strcmp(reason, "contains an embedded NUL byte") == 0);

  (void)unlink(nul_path);
  arena_free(&arena);
  (void)rmdir(root_dir);
}

// A written file reads back byte-for-byte, and a successful write and a successful read each leave
// the reason untouched.
static void test_write_then_read_round_trips(void) {
  char root_dir_template[] = "/tmp/sosig-fs-roundtrip.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* root_md = path_join(root_dir, "a.md", &arena);
  char reason[FS_REASON_SIZE] = "untouched";
  TEST_CHECK(fs_write_file(root_md, "root", strlen("root"), reason, sizeof(reason)) == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(root_md, &file_data, &file_len, reason, sizeof(reason)) == 0);
  TEST_CHECK(file_len == strlen("root"));
  TEST_CHECK(strcmp(file_data, "root") == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  free(file_data);

  (void)unlink(root_md);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_write_file` fails when the parent directory cannot be created and when the target is itself a
// directory, and reports the two as different reasons. The first case is what pins that the
// parent-directory failure is propagated rather than discarded, which the return value alone cannot
// show, since a write that never needed a parent directory returns 0 either way.
static void test_write_file_rejects_file_parent_and_dir_target(void) {
  char root_dir_template[] = "/tmp/sosig-fs-write-fail.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  // `plain.txt` is a regular file, so it cannot hold the child the write asks for. The reason comes
  // from the parent-directory pass and names the component that blocked it.
  char* blocking_file = path_join(root_dir, "plain.txt", &arena);
  char* through_file = path_join(root_dir, "plain.txt/child.html", &arena);
  TEST_CHECK(fs_write_file(blocking_file, "x", 1, NULL, 0) == 0);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_write_file(through_file, "x", 1, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  const int blocking_expected_len =
      snprintf(expected, sizeof(expected), "exists and is not a directory ('%s')", blocking_file);
  TEST_ASSERT(blocking_expected_len > 0 && (size_t)blocking_expected_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);

  // An existing directory as the target has a parent that is already there, so the walk gets past
  // it and the open is what refuses. That is a system error, so it carries the system message.
  char* dir_target = path_join(root_dir, "sub", &arena);
  TEST_CHECK(fs_mkdir_p(dir_target, NULL, 0) == 0);
  reason[0] = '\0';
  TEST_CHECK(fs_write_file(dir_target, "x", 1, reason, sizeof(reason)) == -1);
  char message[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(message, EISDIR)) == 0);

  (void)unlink(blocking_file);
  (void)rmdir(dir_target);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_mkdir_p` creates a deep path, treats an empty path as a no-op, and is idempotent.
static void test_mkdir_p_creates_nested_and_is_idempotent(void) {
  char root_dir_template[] = "/tmp/sosig-fs-mkdir.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(fs_mkdir_p("", NULL, 0) == 0);

  char* nested = path_join(root_dir, "x/y/z", &arena);
  TEST_CHECK(fs_mkdir_p(nested, NULL, 0) == 0);
  struct stat st;
  TEST_CHECK(stat(nested, &st) == 0 && S_ISDIR(st.st_mode));

  // Re-creating an existing tree is not an error.
  TEST_CHECK(fs_mkdir_p(nested, NULL, 0) == 0);

  (void)rmdir(path_join(root_dir, "x/y/z", &arena));
  (void)rmdir(path_join(root_dir, "x/y", &arena));
  (void)rmdir(path_join(root_dir, "x", &arena));
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_mkdir_p` fails when a path component is an existing regular file. The reason names the
// component rather than the whole path the caller asked for.
static void test_mkdir_p_rejects_file_component(void) {
  char root_dir_template[] = "/tmp/sosig-fs-mkdir-fail.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  // `file` is a regular file, so it cannot be the target of `fs_mkdir_p`.
  char* existing_file = path_join(root_dir, "file", &arena);
  TEST_CHECK(fs_write_file(existing_file, "x", 1, NULL, 0) == 0);
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_mkdir_p(existing_file, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  const int file_expected_len =
      snprintf(expected, sizeof(expected), "exists and is not a directory ('%s')", existing_file);
  TEST_ASSERT(file_expected_len > 0 && (size_t)file_expected_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);

  // `file` is a regular file, so it cannot serve as an intermediate path component. The walk stops
  // at that component, so the reason names it rather than the deeper path the caller asked for.
  char* path_through_file = path_join(root_dir, "file/child", &arena);
  reason[0] = '\0';
  TEST_CHECK(fs_mkdir_p(path_through_file, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, expected) == 0);

  (void)unlink(existing_file);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_mkdir_p` fails when a component exists but cannot be inspected, which is a distinct branch
// from the regular-file rejection above. `mkdir` reports `EEXIST`, so the walk falls through to a
// `stat` that then fails on its own. A dangling symlink is the reachable way to get there. The link
// itself exists, so `mkdir` refuses, and resolving it fails with `ENOENT`.
static void test_mkdir_p_rejects_unstatable_component(void) {
  char root_dir_template[] = "/tmp/sosig-fs-mkdir-unstatable.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);

  char* dangling = path_join(root_dir, "dangling", &arena);
  TEST_ASSERT(dangling != NULL);
  if (dangling == NULL) {
    arena_free(&arena);
    return;
  }
  TEST_CHECK(symlink("/sosig-nonexistent-symlink-target", dangling) == 0);

  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_mkdir_p(dangling, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "cannot inspect directory: %s ('%s')",
               expected_errno_reason(message, ENOENT), dangling);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);

  (void)unlink(dangling);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_mkdir_p` fails when `mkdir` itself is refused, a third branch again. The failure is neither
// the first-party type rejection nor the `EEXIST` fallthrough, but the system error reported
// straight from the create. A parent that is readable but not writable is the way to produce it.
static void test_mkdir_p_rejects_uncreatable_component(void) {
  // Root bypasses the permission bits, so `mkdir` would succeed and the assertions below would fail
  // for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir_template[] = "/tmp/sosig-fs-mkdir-refused.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* sealed_dir = path_join(root_dir, "sealed", &arena);
  char* child_dir = path_join(root_dir, "sealed/child", &arena);
  TEST_CHECK(fs_mkdir_p(sealed_dir, NULL, 0) == 0);
  TEST_CHECK(chmod(sealed_dir, 0500) == 0);

  char reason[FS_REASON_SIZE] = "";
  const int rc = fs_mkdir_p(child_dir, reason, sizeof(reason));
  // Restore the mode before asserting, as `test_list_files_rejects_unstatable_entry` does: a
  // failing assertion aborts the test. The cleanup below cannot remove a directory it may not
  // write.
  (void)chmod(sealed_dir, 0700);

  TEST_CHECK(rc == -1);
  char expected[FS_REASON_SIZE];
  char message[FS_REASON_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "cannot create directory: %s ('%s')",
               expected_errno_reason(message, EACCES), child_dir);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(reason, expected) == 0);

  (void)rmdir(sealed_dir);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

// `fs_identify` answers which file a path names rather than what the path spells: two spellings of
// one file share an identity, two files do not. A path that cannot be inspected is rejected with
// the caller's struct left alone. The last of those is what `register_output_path` relies on to
// tell an output that does not exist yet from one that collides with a build input.
static void test_identify_distinguishes_files_and_rejects_missing(void) {
  char root_dir_template[] = "/tmp/sosig-fs-identify.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }

  struct Arena arena;
  arena_init(&arena);
  char* file_path = path_join(root_dir, "a.md", &arena);
  char* dotted_path = path_join(root_dir, "./a.md", &arena);
  char* other_path = path_join(root_dir, "b.md", &arena);
  TEST_CHECK(fs_write_file(file_path, "a", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(other_path, "b", 1, NULL, 0) == 0);

  struct FsIdentity identity;
  struct FsIdentity dotted_identity;
  TEST_CHECK(fs_identify(file_path, &identity) == 0);
  TEST_CHECK(fs_identify(dotted_path, &dotted_identity) == 0);
  TEST_CHECK(identity.device == dotted_identity.device);
  TEST_CHECK(identity.inode == dotted_identity.inode);

  // Two files in one directory share a device, so here it is the inode that has to tell them apart.
  // Neither number is predictable, so the assertion is relational rather than exact.
  struct FsIdentity other_identity;
  TEST_CHECK(fs_identify(other_path, &other_identity) == 0);
  TEST_CHECK(other_identity.device == identity.device);
  TEST_CHECK(other_identity.inode != identity.inode);

  // A path that names nothing has no identity, and the caller's struct is not written.
  struct FsIdentity unwritten = {.device = 7, .inode = 11};
  TEST_CHECK(fs_identify(path_join(root_dir, "missing.md", &arena), &unwritten) == -1);
  TEST_CHECK(unwritten.device == 7);
  TEST_CHECK(unwritten.inode == 11);

  (void)unlink(file_path);
  (void)unlink(other_path);
  (void)rmdir(root_dir);
  arena_free(&arena);
}

TEST_LIST = {
    {"list files matches suffix", test_list_files_matches_suffix},
    {"list files empty suffix matches all", test_list_files_empty_suffix_matches_all},
    {"list files accepts empty dir", test_list_files_accepts_empty_dir},
    {"list files skips symlink cycle", test_list_files_skips_symlink_cycle},
    {"list files skips symlink cycle to ancestor", test_list_files_skips_symlink_cycle_to_ancestor},
    {"list files rejects missing dir", test_list_files_rejects_missing_dir},
    {"list files rejects file root", test_list_files_rejects_file_root},
    {"list files rejects unstatable entry", test_list_files_rejects_unstatable_entry},
    {"read file accepts empty", test_read_file_accepts_empty},
    {"read file rejects missing and non-regular", test_read_file_rejects_missing_and_non_regular},
    {"read file rejects embedded nul", test_read_file_rejects_embedded_nul},
    {"write then read round trips", test_write_then_read_round_trips},
    {"write file rejects file parent and dir target",
     test_write_file_rejects_file_parent_and_dir_target},
    {"mkdir_p creates nested and is idempotent", test_mkdir_p_creates_nested_and_is_idempotent},
    {"mkdir_p rejects file component", test_mkdir_p_rejects_file_component},
    {"mkdir_p rejects unstatable component", test_mkdir_p_rejects_unstatable_component},
    {"mkdir_p rejects uncreatable component", test_mkdir_p_rejects_uncreatable_component},
    {"identify distinguishes files and rejects missing",
     test_identify_distinguishes_files_and_rejects_missing},
    {NULL, NULL}};
