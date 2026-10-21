#ifndef SOSIG_OUTPUT_PATH_H
#define SOSIG_OUTPUT_PATH_H

#include <stddef.h>

#include "shared/str.h"

struct Log;

/**
 * @brief Rejects a generated output path that exceeds either output-path limit.
 *
 * Every producer of an output path reports a limit failure through this one function, so the
 * content entry and configured template paths share one wording. The limits themselves are applied
 * by `path_check_output_limits`.
 *
 * @param relative_path Output path relative to the output directory.
 * @param source_label  Producer the path was generated for, such as a content source or a config
 *                      list entry, named in the diagnostic.
 * @param log           Log that receives an error naming the limit that was exceeded. Must not be
 *                      `NULL`.
 * @return `0` when both limits hold, or `-1` when one is exceeded.
 */
int output_path_check_limits(struct Str relative_path, struct Str source_label, struct Log* log)
    __attribute__((nonnull(3)));

#endif
