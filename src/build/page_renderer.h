#ifndef SOSIG_PAGE_RENDERER_H
#define SOSIG_PAGE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

struct ContentEntry;
struct RenderJobSet;
struct SiteConfig;
struct StringBuffer;

/**
 * @brief Renders each parsed content entry through its content template and writes its page.
 *
 * This is the second of the two render passes. It runs only after `entry_renderer_render_entries`
 * succeeds and the entries are collected and sorted. Every entry's template sees the whole sorted
 * entry set and the site's last-updated timestamp, so a content template resolves `site.updated`
 * and `{{#content_entries}}` exactly as an aggregate template does.
 *
 * Each job writes its entry's page to the entry's `output_path` and frees the HTML before the next
 * job starts, so at most one page per worker is held in memory. Draft slots are skipped. A failed
 * write is that entry's diagnostic, like a failed render, and leaves the other pages written. Each
 * finished job reports progress when `is_verbose`. Failing jobs' diagnostics go to `error_out` one
 * per line, each distinct message once, up to `RENDER_JOB_ERROR_REPORT_COUNT_MAX` of them plus a
 * count of the rest, so the caller reports them at a single boundary.
 *
 * Call this only after `manifest_builder_populate` accepts every output path. Jobs write
 * concurrently, and the manifest is what guarantees that no two of them target the same file.
 *
 * @param render_jobs         Result set filled by `entry_renderer_render_entries`. A failing slot
 *                            receives its diagnostic. Must not be `NULL`.
 * @param site_config         Site configuration supplying `templates_dir` and the default content
 *                            template. Must not be `NULL`.
 * @param content_entries     Non-draft entries, sorted newest-first, visible to every page. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param site_updated        Site last-updated timestamp exposed as `site.updated`. Must not be
 *                            `NULL`.
 * @param worker_count        Requested worker threads, as for `pool_run`.
 * @param is_verbose          Whether progress is printed to `stderr`.
 * @param error_out           Growable buffer that receives the collected render diagnostics. Must
 *                            not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a page failed to render or write, the worker
 *         pool could not start, or a diagnostic could not be appended.
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
