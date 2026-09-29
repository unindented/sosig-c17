#ifndef SOSIG_OUTPUT_PATH_H
#define SOSIG_OUTPUT_PATH_H

#include <stddef.h>

/**
 * @brief Rejects a generated output path that exceeds either output-path limit.
 *
 * Every producer of an output path reports a limit failure through this one function, so the
 * content entry and configured template paths share one wording. The limits themselves are applied
 * by `path_check_output_limits`.
 *
 * @param relative_path Output path relative to the output directory. Must not be `NULL`.
 * @param label         Producer the path was generated for, such as a content source or a config
 *                      key, named in the diagnostic. Must not be `NULL`.
 * @param err           Destination for a diagnostic. May be `NULL` only when `err_len` is 0.
 * @param err_len       Size of `err` in bytes.
 * @return `0` when both limits hold, or `-1` naming the limit that was exceeded.
 */
int output_path_check_limits(const char* relative_path,
                             const char* label,
                             char* err,
                             size_t err_len) __attribute__((nonnull(1, 2)));

#endif
