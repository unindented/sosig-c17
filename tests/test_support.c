// `TEST_NO_MAIN` tells acutest not to define `main` in this file. Each test executable defines
// `main` and the acutest run state in its own translation unit. This file has only the declarations
// of `acutest_check_` and `acutest_abort_`. The linker connects both declarations to the
// definitions in the test executable.
//
// A helper here can therefore call `TEST_CHECK` and `TEST_ASSERT`. Acutest reports a failure at
// this file and line. The test that called the helper then fails.
#define TEST_NO_MAIN

#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "test_support.h"

#include <acutest.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path.h"
#include "runtime/fs.h"
#include "shared/arena.h"

const char* init_fixture_dir(char root_dir[static 1]) {
  char* created_root_dir = mkdtemp(root_dir);
  TEST_ASSERT(created_root_dir != NULL);
  if (created_root_dir == NULL) {
    return NULL;
  }
  return created_root_dir;
}

/**
 * @brief Removes one entry a fixture-tree walk visits.
 *
 * @param path      Path of the visited entry.
 * @param st        Stat buffer `nftw` filled. Unused.
 * @param type_flag Entry type `nftw` determined.
 * @param ftw       Traversal state `nftw` maintains. Unused.
 * @return `0` to continue the walk.
 */
static int remove_fixture_tree_entry(const char* path,
                                     const struct stat* st,
                                     int type_flag,
                                     struct FTW* ftw) {
  (void)st;
  (void)ftw;
  TEST_CHECK((type_flag == FTW_DP ? rmdir(path) : unlink(path)) == 0);
  return 0;
}

void remove_fixture_tree(const char* root_dir) {
  enum { FIXTURE_TREE_FD_MAX = 8 };
  TEST_CHECK(nftw(root_dir, remove_fixture_tree_entry, FIXTURE_TREE_FD_MAX, FTW_DEPTH | FTW_PHYS) ==
             0);
}

int write_fixture_file(const char* root_dir, const char* relative_path, const char* contents) {
  struct Arena arena;
  arena_init(&arena);
  char* fixture_path = path_join(root_dir, relative_path, &arena);
  const int rc =
      fixture_path == NULL ? -1 : fs_write_file(fixture_path, contents, strlen(contents), NULL, 0);
  arena_free(&arena);
  return rc;
}

int read_capture(FILE* capture, char* text_out, size_t text_out_len) {
  TEST_ASSERT(text_out_len > 0);
  if (fseek(capture, 0, SEEK_SET) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  const size_t text_len = fread(text_out, 1, text_out_len - 1, capture);
  text_out[text_len] = '\0';
  if (text_len == text_out_len - 1) {
    const int trailing = fgetc(capture);
    TEST_CHECK(trailing == EOF);
    if (trailing != EOF) {
      return -1;
    }
  }
  if (ferror(capture) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}
