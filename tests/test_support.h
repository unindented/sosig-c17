#ifndef SOSIG_TEST_SUPPORT_H
#define SOSIG_TEST_SUPPORT_H

#include <stddef.h>
#include <stdio.h>

/**
 * @brief Creates a temporary fixture root.
 *
 * The returned pointer aliases the caller's writable template and must not be freed.
 *
 * @param root_dir Writable `mkdtemp` template. Receives the created directory path.
 * @return `root_dir` on success, or `NULL` after recording a test-plumbing failure.
 */
const char* init_fixture_dir(char root_dir[static 1]);

/**
 * @brief Removes a fixture root and everything below it.
 *
 * Walking the tree keeps cleanup complete without a per-test inventory of the paths a fixture
 * writes or a build generates, and it needs no caller to classify a path as a file or a directory.
 * Each removal is asserted, so a fixture a test leaves behind fails that test rather than
 * accumulating under `/tmp`.
 *
 * @param root_dir Fixture root directory to remove.
 */
void remove_fixture_tree(const char* root_dir);

/**
 * @brief Writes one file relative to a fixture root, creating parent directories as needed.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative fixture path below `root_dir`.
 * @param contents      Terminated text to write.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
int write_fixture_file(const char* root_dir, const char* relative_path, const char* contents);

/**
 * @brief Reads an anonymous capture stream.
 *
 * @param capture      Stream to rewind and read. Must not be `NULL`.
 * @param text_out     Buffer that receives terminated captured text.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
int read_capture(FILE* capture, char* text_out, size_t text_out_len);

#endif  // SOSIG_TEST_SUPPORT_H
