#ifndef SOSIG_SITE_WRITER_H
#define SOSIG_SITE_WRITER_H

#include <stddef.h>

struct ContentEntry;
struct RenderJobSet;
struct SiteConfig;

// These are declared in the order `cmd_build` runs them, which keeps the three
// `site_writer_write_*` names adjacent. The manifest pass in `manifest_builder` claims every
// intended output path before this module writes anything. Ordering the entries and deriving
// `site.updated` happen earlier still, in `content_entry` (`content_entry_sort`,
// `content_entry_latest_date`).

/**
 * @brief Writes each rendered content entry output to its configured output path.
 *
 * Call this only once `page_renderer_render_pages` returns `0`. Every slot with a non-`NULL`
 * `entry` must also have its `rendered_html` set, which that return guarantees. A slot with an
 * entry but no rendered HTML is a caller error, not a skipped entry.
 *
 * @param render_jobs Result slots holding rendered entries and HTML. Its `items` may be `NULL`
 *                    only when its `count` is 0. Must not be `NULL`.
 * @param err         Destination buffer for a failure diagnostic.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first write failure.
 */
int site_writer_write_content_entries(const struct RenderJobSet* render_jobs,
                                      char* err,
                                      size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Renders and writes configured aggregate templates with all non-draft entries visible.
 *
 * @param site_config         Configuration supplying `output_dir`, `templates_dir` and the
 *                            aggregate template list. Must not be `NULL`.
 * @param content_entries     Content entries, sorted newest-first, visible to the templates. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param site_updated        Site last-updated timestamp exposed as `site.updated`. Must not be
 *                            `NULL`.
 * @param err                 Destination buffer for a failure diagnostic.
 * @param err_len             Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first template that fails to render or write.
 */
int site_writer_write_aggregates(const struct SiteConfig* site_config,
                                 const struct ContentEntry* const* content_entries,
                                 size_t content_entry_count,
                                 const char* site_updated,
                                 char* err,
                                 size_t err_len) __attribute__((nonnull(1, 4)));

/**
 * @brief Renders and writes configured feed templates with the newest `feed_count` entries.
 *
 * @param site_config         Configuration supplying `output_dir`, `templates_dir`, `feed_count`,
 *                            and the feed template list. Must not be `NULL`.
 * @param content_entries     Content entries, sorted newest-first, the feed draws from. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param site_updated        Site last-updated timestamp exposed as `site.updated`. Must not be
 *                            `NULL`.
 * @param err                 Destination buffer for a failure diagnostic.
 * @param err_len             Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first template that fails to render or write.
 */
int site_writer_write_feeds(const struct SiteConfig* site_config,
                            const struct ContentEntry* const* content_entries,
                            size_t content_entry_count,
                            const char* site_updated,
                            char* err,
                            size_t err_len) __attribute__((nonnull(1, 4)));

#endif
