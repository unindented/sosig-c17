#ifndef SOSIG_ENTRY_RENDERER_H
#define SOSIG_ENTRY_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

struct ContentEntry;
struct PathList;
struct SiteConfig;
struct StringBuffer;

/**
 * @brief Turns every discovered content source into a rendered `ContentEntry`.
 *
 * This is the first of the two parallel phases. When `source_paths->count` is 0, it runs no job and
 * returns `0`. Each job reads its source, splits and parses the frontmatter, converts the Markdown
 * body to HTML, and finalizes the entry's URL and output paths. It deliberately leaves page
 * rendering to `page_renderer_render_pages`, because a content template can read `site.updated` and
 * `content_entries`, and neither is known until every entry is parsed and sorted.
 *
 * One job runs per source path and fills that path's slot in `source_entries` with a rendered
 * `ContentEntry` on success. It parses draft entries but does not publish them, so their slot stays
 * `NULL`, as does the slot of a failed job. Each finished job reports progress when `is_verbose`.
 * Failing jobs' diagnostics go to `error_out` one per line, each distinct message once, up to
 * `JOB_ERROR_REPORT_COUNT_MAX` of them plus a count of the rest, so the caller reports them at a
 * single boundary.
 *
 * @param site_config    Site configuration shared by every parse job. Must not be `NULL`.
 * @param source_paths   Content entry source paths. One job runs per path. Each must be spelled
 *                       with `site_config->content_dir` as its literal prefix, because the
 *                       remainder is what becomes the entry's section. A path from outside that
 *                       root fails its own job rather than being reinterpreted, which would put the
 *                       content directory's own name into the entry's URL. Must not be `NULL`.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param source_entries One slot per source path, each `NULL`-initialized by the caller. A
 *                       successful non-draft job stores its entry there, and the caller must
 *                       release every slot through `entry_renderer_free_entries`, including when
 *                       another job failed. May be `NULL` only when `source_paths->count` is 0.
 * @param error_out      Growable buffer that receives the collected parse diagnostics. Must not be
 *                       `NULL`.
 * @return `0` when every job succeeded, or `-1` when a parse job failed, the error slots could not
 *         be allocated, the worker pool could not start, or a diagnostic could not be appended.
 */
int entry_renderer_render_entries(const struct SiteConfig* site_config,
                                  const struct PathList* source_paths,
                                  size_t worker_count,
                                  bool is_verbose,
                                  struct ContentEntry** source_entries,
                                  struct StringBuffer* error_out) __attribute__((nonnull(1, 2, 6)));

/**
 * @brief Frees every entry `entry_renderer_render_entries` stored, then clears its slot.
 *
 * The slot array itself stays the caller's to free.
 *
 * @param source_entries     Slot array the parse phase filled. May be `NULL` with any
 *                           `source_entry_count`, which frees nothing, so a caller whose slots were
 *                           never allocated can still release them unconditionally.
 * @param source_entry_count Number of slots in `source_entries`.
 */
void entry_renderer_free_entries(struct ContentEntry** source_entries, size_t source_entry_count);

#endif
