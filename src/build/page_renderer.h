#ifndef SOSIG_PAGE_RENDERER_H
#define SOSIG_PAGE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

struct ContentEntry;
struct RenderJobSet;
struct SiteConfig;
struct StringBuffer;

/**
 * @brief Renders each parsed content entry through its content template.
 *
 * This is the second of the two render passes. It runs only after `entry_renderer_render_entries`
 * succeeds and the entries are collected and sorted. Every entry's template sees the whole sorted
 * entry set and the site's last-updated timestamp, so a content template resolves `site.updated`
 * and `{{#content_entries}}` exactly as an aggregate template does.
 *
 * Fills the `rendered_html` of each result slot holding an entry and leaves draft slots untouched.
 * Each finished job prints a progress dot when `is_verbose`. Each failing job's buffered diagnostic
 * goes to `error_out`, one per line.
 *
 * The result slots lead as this pass's subject rather than trailing as an output. Unlike
 * `entry_renderer_render_entries`, which fills each slot exactly once, this pass reads the `entry`
 * out of a slot and writes its `rendered_html` back, so the set is an in/out subject. That
 * asymmetry is why only the other function's parameter carries `_out`.
 *
 * @param render_jobs         Result set filled by `entry_renderer_render_entries`. Each slot
 *                            holding an entry receives its rendered HTML. Its `items` may be `NULL`
 *                            only when its `count` is 0. Must not be `NULL`.
 * @param site_config         Site configuration supplying `templates_dir` and the default content
 *                            template. Must not be `NULL`.
 * @param content_entries     Non-draft entries, sorted newest-first, visible to every page. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param site_updated        Site last-updated timestamp exposed as `site.updated`. Must not be
 *                            `NULL`.
 * @param worker_count        Worker threads used for rendering. `0` is treated as `1`.
 * @param is_verbose          Whether each finished job prints a progress dot to `stderr`.
 * @param error_out           Growable buffer that receives the collected render diagnostics. Must
 *                            not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a render job failed, when the worker pool
 *         could not start, or when a diagnostic could not be buffered.
 */
int page_renderer_render_pages(struct RenderJobSet* render_jobs,
                               const struct SiteConfig* site_config,
                               const struct ContentEntry* const* content_entries,
                               size_t content_entry_count,
                               const char* site_updated,
                               size_t worker_count,
                               bool is_verbose,
                               struct StringBuffer* error_out) __attribute__((nonnull(1, 2, 5, 8)));

#endif
