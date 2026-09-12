#ifndef SOSIG_ENTRY_RENDERER_H
#define SOSIG_ENTRY_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

struct PathList;
struct RenderJobSet;
struct SiteConfig;
struct StringBuffer;

/**
 * @brief Turns every discovered content source into a rendered `ContentEntry`.
 *
 * This is the first of the two render passes. Each job reads its source, splits and parses the
 * frontmatter, converts the Markdown body to HTML, and finalizes the entry's URL and output paths.
 * It deliberately leaves page rendering to `page_renderer_render_pages`, because a content template
 * can read `site.updated` and `content_entries`, and neither is known until every entry is parsed
 * and sorted.
 *
 * Fills one caller-allocated result slot per source path: a rendered `ContentEntry` on success, or
 * a buffered diagnostic on failure. It parses draft entries but does not publish them, so their
 * slot stays empty. Each finished job prints a progress dot when `is_verbose`. Each failing job's
 * buffered diagnostic goes to `error_out`, one per line, so the caller reports them at a single
 * boundary.
 *
 * @param site_config     Site configuration shared by every render job. Must not be `NULL`.
 * @param source_paths    Content entry source paths. One job runs per path. Each must be spelled
 *                        with `site_config->content_dir` as its literal prefix, because the
 *                        remainder is what becomes the entry's section. A path from outside that
 *                        root fails its own job rather than being reinterpreted, which would put
 *                        the content directory's own name into the entry's URL. Must not be `NULL`.
 * @param worker_count    Worker threads used for rendering. `0` is treated as `1`.
 * @param is_verbose      Whether each finished job prints a progress dot to `stderr`.
 * @param render_jobs_out Caller-allocated result set with one slot per source path; its `count`
 *                        must equal `source_paths->count`. Initialized and filled by this function
 *                        (the caller need not zero the slots). Its `items` may be `NULL` only when
 *                        its `count` is 0. Must not be `NULL`.
 * @param error_out       Growable buffer that receives the collected render diagnostics. Must not
 *                        be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a render job failed, when the worker pool
 *         could not start, or when a diagnostic could not be buffered.
 */
int entry_renderer_render_entries(const struct SiteConfig* site_config,
                                  const struct PathList* source_paths,
                                  size_t worker_count,
                                  bool is_verbose,
                                  struct RenderJobSet* render_jobs_out,
                                  struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2, 5, 6)));

#endif
