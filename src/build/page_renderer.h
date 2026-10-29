#ifndef SOSIG_PAGE_RENDERER_H
#define SOSIG_PAGE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct ContentEntry;
struct JobArenas;
struct Log;
struct Os;
struct OutputWriter;
struct SiteConfig;

/**
 * @brief Renders each parsed content entry through its content template and writes its page.
 *
 * This is the second of the two parallel phases. When `source_entry_count` is 0, it runs no job and
 * returns `0`. It runs only after `entry_renderer_render_entries` succeeds and the entries are
 * collected and sorted. Every entry's template sees the whole sorted entry set and the site's
 * last-updated timestamp, so a content template resolves `site.updated` and `{{#content_entries}}`
 * exactly as an aggregate template does.
 *
 * One job runs per slot of `source_entries`. Each job renders its entry's page into a copy of its
 * worker's `perm` and its temporaries into a copy of its worker's scratch, writes the page to the
 * entry's `output_path` through `writer`, and leaves both regions as it found them, so at most one
 * page per worker is held in memory. `NULL` slots are skipped. A failed write is that entry's
 * diagnostic, like a failed render, and leaves the other pages written. Each finished job reports
 * progress when `is_verbose`. Failing jobs' errors go to `log` in entry order, as `job_run`
 * collects them.
 *
 * Call this only after `manifest_builder_populate` accepts every output path. Jobs write
 * concurrently, and the manifest is what guarantees that no two of them target the same file, so
 * each job marks only its own path in it.
 *
 * @param writer              Writer every page is written through, by every worker at once. Must
 *                            not be `NULL`.
 * @param site_config         Site configuration supplying `templates_dir` and the default content
 *                            template. Must not be `NULL`.
 * @param source_entries      Slot array filled by `entry_renderer_render_entries`, one slot per
 *                            source path, where a draft or a failed source left `NULL`. Read-only
 *                            during the run. May be `NULL` only when `source_entry_count` is 0.
 * @param source_entry_count  Number of slots in `source_entries`, and therefore of jobs.
 * @param content_entries     Non-draft entries, sorted newest-first, visible to every page. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param site_updated        Site last-updated timestamp exposed as `site.updated`.
 * @param worker_count        Requested worker threads, as for `pool_run`.
 * @param worker_arenas       One `JobArenas` per requested worker, as for `job_run`. Must not be
 *                            `NULL`.
 * @param is_verbose          Whether progress is printed to `stderr`.
 * @param os                  Host the templates are read from and progress printed on, by every
 *                            worker at once. Must not be `NULL`.
 * @param scratch             Arena for the phase's own bookkeeping, as for `job_run`.
 * @param log                 Log that receives the collected render errors. Its arena must be none
 *                            of the others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a page failed to render or write or the worker
 *         pool could not start.
 */
int page_renderer_render_pages(const struct OutputWriter* writer,
                               const struct SiteConfig* site_config,
                               const struct ContentEntry* const* source_entries,
                               ptrdiff_t source_entry_count,
                               const struct ContentEntry* const* content_entries,
                               ptrdiff_t content_entry_count,
                               struct Str site_updated,
                               ptrdiff_t worker_count,
                               struct JobArenas* worker_arenas,
                               bool is_verbose,
                               struct Os* os,
                               struct Arena scratch,
                               struct Log* log) __attribute__((nonnull(1, 2, 9, 11, 13)));

#endif
