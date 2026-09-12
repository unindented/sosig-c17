#ifndef SOSIG_PATH_H
#define SOSIG_PATH_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;

/**
 * Upper bound on a single filename, excluding the terminator. 255 is the per-component limit on
 * every filesystem this targets. ext4, APFS and HFS+ all cap one name at 255 bytes, and POSIX
 * exposes it as `NAME_MAX`. Generated names are pure ASCII, because `text_slugify` folds each
 * non-ASCII byte to hex digits, so a byte count and a character count are the same limit here.
 */
enum { FILENAME_LEN_MAX = 255 };

_Static_assert(FILENAME_LEN_MAX > (sizeof(".html") - 1),
               "filename limit must leave room for the '.html' suffix");

/**
 * Upper bound on a generated output path relative to the output directory, excluding the
 * terminator. 1024 is the smallest `PATH_MAX` across the targets (macOS caps a whole path at 1024.
 * Linux allows 4096), so the bound holds everywhere rather than being tuned per platform. It covers
 * the relative path only, because `output_dir` is joined on afterwards. An accepted path can
 * therefore still exceed the system limit. The check exists to fail with a diagnostic naming the
 * limit and the path rather than to guarantee the write will succeed. This bounds every generated
 * relative output path: a content entry's permalink-expanded `<section>/<slug>.html`, and an
 * aggregate or feed template's own name used as its output path.
 */
enum { OUTPUT_PATH_RELATIVE_LEN_MAX = 1024 };

_Static_assert((size_t)OUTPUT_PATH_RELATIVE_LEN_MAX >= (size_t)FILENAME_LEN_MAX,
               "output path limit must fit at least a bare '<slug>.html' filename");

/** Measurements taken from an output path while checking it against the output-path limits. */
struct PathOutputMetrics {
  /** Whole path length in bytes. */
  size_t len;

  /** Longest `/`-delimited segment, borrowed into the checked path and not terminated. */
  const char* segment;

  /** Length of `segment` in bytes. */
  size_t segment_len;
};

/** Which output-path limit a relative path exceeded, if any. */
enum PathOutputVerdict {
  /** Within both the whole-path and the per-segment limit. */
  PATH_OUTPUT_OK,

  /** Longer than `OUTPUT_PATH_RELATIVE_LEN_MAX` in total. */
  PATH_OUTPUT_TOO_LONG,

  /** Within the total limit, but one segment exceeds `FILENAME_LEN_MAX`. */
  PATH_OUTPUT_SEGMENT_TOO_LONG,
};

/**
 * @brief Checks a generated output path against both output-path limits.
 *
 * This is the single place that applies the two limits, so every generated output path routes
 * through one check rather than each producer repeating it. A limit that each producer applied for
 * itself would drift between them. A limit on a whole path does not bound its individual names, so
 * both are needed. Each segment becomes one filesystem entry. An overlong one has to fail here with
 * a diagnostic naming the limit rather than later as an opaque write error from the OS.
 *
 * It reports the measurements whatever the verdict, including the longest segment itself, so a
 * per-segment failure can name which segment rather than only its length.
 *
 * @param relative_path Output path relative to the output directory. Must not be `NULL`.
 * @param metrics_out   Receives the measurements, whatever the verdict, so the caller can name the
 *                      offending value. Must not be `NULL`.
 * @return The first limit exceeded, or `PATH_OUTPUT_OK`.
 */
enum PathOutputVerdict path_check_output_limits(const char* relative_path,
                                                struct PathOutputMetrics* metrics_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the final path component with its last extension removed, in arena-owned storage.
 *
 * A leading dot marks a hidden file (e.g. `.gitignore`), so this keeps it as part of the name
 * rather than treating it as an extension separator.
 *
 * @param file_path Terminated path to take the basename of. Must not be `NULL`.
 * @param arena     Arena that owns the returned name. Must not be `NULL`.
 * @return Terminated basename without a trailing extension, or `NULL` on allocation failure.
 */
char* path_basename_without_extension(const char* file_path, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Joins two path components into arena-owned storage.
 *
 * Inserts a single `/` between the components only when `base_path` does not already end with one.
 *
 * @param base_path     Leading path component. Must not be `NULL`.
 * @param relative_path Trailing path component to append. Must not be `NULL`.
 * @param arena         Arena that owns the joined path. Must not be `NULL`.
 * @return Terminated joined path owned by the arena, or `NULL` on allocation failure.
 */
char* path_join(const char* base_path, const char* relative_path, struct Arena* arena)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reports whether `path` is a safe non-absolute relative path.
 *
 * A safe path is non-empty, does not start with `/`, contains only ASCII alphanumerics, `_`, `-`,
 * `.`, and `/`, has no empty segments, and includes no `.` or `..` segment, so it cannot escape its
 * configured root.
 *
 * This is the traversal boundary for every caller-supplied name that becomes a path. It is a
 * whitelist, so it rejects any byte outside the allowed set. It decides on the bytes alone and
 * never touches the filesystem, so it does not stop a symlink inside the root from redirecting a
 * read or write outside it. It does not stop two accepted names from colliding on a
 * case-insensitive filesystem. Rejecting every byte above 0x7F also avoids Unicode normalization,
 * because no accepted name has a second spelling.
 *
 * @param path Candidate relative path. `NULL` is treated as unsafe.
 * @return `true` when `path` is a safe relative path, `false` otherwise.
 */
bool path_is_safe_relative(const char* path);

#endif
