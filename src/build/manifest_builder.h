#ifndef SOSIG_MANIFEST_BUILDER_H
#define SOSIG_MANIFEST_BUILDER_H

#include <stddef.h>

struct ContentEntry;
struct Manifest;
struct PathList;
struct SiteConfig;

/**
 * @brief Records every intended output path in the build manifest, rejecting duplicates and any
 *        output that would overwrite one of this build's own input files.
 *
 * Registers content entry and template output paths before writing any file, so it catches a
 * collision between two sources up front. It rejects three kinds of collision:
 * - two producers that claim one output path
 * - an output path that names a build input
 * - an output path that is a `/`-delimited directory prefix of another output path
 *
 * The last case requires one path to be both a file and a directory.
 * `manifest_find_prefix_collision` checks for it after all paths are recorded.
 *
 * The identity check compares filesystem identity, not path text, because the two spellings need
 * not match. An output path is rooted at `output_dir` and a source path at `content_dir`, so
 * `output_dir = "."` with a permalink aiming into the content tree produces `./content/x.md`
 * against a source `content/x.md`. They can also come out byte-equal, because nothing rejects
 * `output_dir` naming the same directory as `content_dir`, so a text comparison is neither
 * sufficient on its own nor useful as a first check. Comparing `(device, inode)` covers both, and
 * also rejects a collision created by a symlink, a hard link, or a case-insensitive filesystem.
 * This does not cover partials, because `template_render_file` resolves them lazily from names
 * found inside template bytes. No pass before the first write can enumerate them.
 *
 * @param manifest            Manifest that receives the output paths. Must not be `NULL`.
 * @param site_config         Configuration supplying `output_dir`, `templates_dir` and the template
 *                            lists. Must not be `NULL`.
 * @param config_path         Path the configuration itself was loaded from. Claimed as an input
 *                            like any other, so a build cannot overwrite the file that
 * configured it. Must not be `NULL`.
 * @param source_paths        Every discovered content source path, drafts included, whose files
 *                            must not be overwritten. Must not be `NULL`.
 * @param content_entries     Rendered non-draft content entries whose output paths are recorded.
 *                            May be `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param err                 Destination buffer for a failure diagnostic.
 * @param err_len             Size of `err` in bytes.
 * @return `0` when every output path is unique, overwrites no input, and nests under no other, or
 *         `-1` on a duplicate, a prefix collision, an input overwrite, or an allocation failure.
 */
int manifest_builder_populate(struct Manifest* manifest,
                              const struct SiteConfig* site_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct ContentEntry* const* content_entries,
                              size_t content_entry_count,
                              char* err,
                              size_t err_len) __attribute__((nonnull(1, 2, 3, 4)));

#endif
