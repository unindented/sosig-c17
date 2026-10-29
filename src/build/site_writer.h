#ifndef SOSIG_SITE_WRITER_H
#define SOSIG_SITE_WRITER_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct ContentEntry;
struct Log;
struct Os;
struct OutputWriter;
struct PathList;
struct SiteConfig;

// The manifest phase in `manifest_builder` claims every intended output path before this module
// writes anything. Content pages are written earlier, by `page_renderer_render_pages`. Ordering the
// entries and deriving `site.updated` happen earlier still, in `content_entry`
// (`content_entry_sort`, `content_entry_date_latest`).

/**
 * @brief Renders each of `template_names` with `content_entries` visible and writes it through
 *        `writer` to its matching path under `output_dir`.
 *
 * `cmd_build` calls this once for the aggregate templates, with every non-draft entry, and once for
 * the feed templates, with the newest `feed_count`. When `is_inputs_fresh`, an output that exists
 * is kept through `writer` instead, with no render.
 *
 * @param writer              Writer every output is written through. Must not be `NULL`.
 * @param site_config         Configuration supplying `output_dir` and `templates_dir`. Must not be
 *                            `NULL`.
 * @param template_names      Array of `template_count` safe relative template names. May be `NULL`
 *                            only when `template_count` is 0.
 * @param template_count      Number of entries in `template_names`.
 * @param content_entries     Content entries, sorted newest-first, visible to the templates. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param site_updated        Site last-updated timestamp exposed as `site.updated`.
 * @param is_inputs_fresh     Whether the build cache vouches that every template and every entry,
 *                            and the page recipe, are as they were when the outputs were made.
 * @param os                  Host the templates are read from and the outputs inspected on. Must
 *                            not be `NULL`.
 * @param output              Arena whose free space holds each rendered output until it is written.
 *                            Nothing in it outlives the call. Must not overlap `scratch`.
 * @param scratch             Arena for each render's temporaries.
 * @param log                 Log that receives the first failure. Its arena must be none of the
 *                            others. Must not be `NULL`.
 * @return `0` on success, including when `template_count` is 0, or `-1` on the first template that
 *         fails to render or write.
 */
int site_writer_write_templates(const struct OutputWriter* writer,
                                const struct SiteConfig* site_config,
                                const struct Str* template_names,
                                ptrdiff_t template_count,
                                const struct ContentEntry* const* content_entries,
                                ptrdiff_t content_entry_count,
                                struct Str site_updated,
                                bool is_inputs_fresh,
                                struct Os* os,
                                struct Arena output,
                                struct Arena scratch,
                                struct Log* log) __attribute__((nonnull(1, 2, 9, 12)));

/**
 * @brief Copies every enumerated static file below `output_dir` through `writer`.
 *
 * Each static file keeps its path relative to `static_dir` below `output_dir`, the path
 * `manifest_builder_populate` registered.
 *
 * @param writer       Writer every copy is made through. Must not be `NULL`.
 * @param site_config  Configuration supplying `output_dir` and `static_dir`. Must not be `NULL`.
 * @param static_paths Every enumerated file below `static_dir`, each already accepted by
 *                     `manifest_builder_populate`. Must not be `NULL`.
 * @param scratch      Arena for each output path.
 * @param log          Log that receives the first failure, which names the path that could not be
 *                     read or written. Must not be `NULL`.
 * @return `0` on success, or `-1` on the first copy failure.
 */
int site_writer_copy_static_files(const struct OutputWriter* writer,
                                  const struct SiteConfig* site_config,
                                  const struct PathList* static_paths,
                                  struct Arena scratch,
                                  struct Log* log) __attribute__((nonnull(1, 2, 3, 5)));

#endif
