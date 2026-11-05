#ifndef SOSIG_ENTRY_RENDERER_H
#define SOSIG_ENTRY_RENDERER_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "formats/markdown.h"
#include "formats/org.h"
#include "shared/arena.h"

struct ContentEntry;
struct JobArenas;
struct Log;
struct Os;
struct PathList;
struct SiteConfig;

/**
 * Largest content source file read, in bytes, metadata included.
 *
 * The body alone must not exceed `MARKDOWN_INPUT_LEN_MAX` or `ORG_INPUT_LEN_MAX`, and a file is
 * never smaller than its body, so this cap is what keeps either converter's input in range.
 * Checking the whole file at the read rejects it before it is resident. Content jobs read in
 * parallel, so each worker would otherwise hold a full oversize file before it failed. It also
 * bounds the frontmatter and the Org keywords, which have no smaller cap of their own.
 */
enum { CONTENT_FILE_LEN_MAX = MARKDOWN_INPUT_LEN_MAX };

// An Org source is converted whole, so the one read limit must also be the Org converter's.
_Static_assert((uintmax_t)CONTENT_FILE_LEN_MAX <= (uintmax_t)ORG_INPUT_LEN_MAX,
               "a content file must fit the Org converter's input limit");

// `frontmatter_parse` hands tomlc17 the frontmatter behind one added line, and tomlc17 takes the
// document length as an `int`.
_Static_assert(CONTENT_FILE_LEN_MAX < INT_MAX,
               "frontmatter plus its added line must fit the int length tomlc17 takes");

/**
 * @brief Parses every discovered content source into a `ContentEntry`.
 *
 * This is the first of the three parallel phases. When `source_paths->count` is 0, it runs no job
 * and returns `0`. Each job reads its source, parses its frontmatter or Org keywords, keeps the
 * body for `entry_renderer_convert_bodies`, and finalizes the entry's URL and output paths. It
 * leaves the conversion to the next phase, because a link to another content file needs that file's
 * URL, and it leaves page rendering to `page_renderer_render_pages`, because a content template can
 * read `site.modified` and `content_entries`, and neither is known until every entry is parsed and
 * sorted.
 *
 * One job runs per source path and fills that path's slot in `source_entries` with a parsed
 * `ContentEntry` on success. The entry, its source, and everything it points at live in the `perm`
 * arena of the worker that ran the job, which it commits only then. It parses draft entries but
 * does not publish them, so their slot stays `NULL` and their `draft_flags` slot is set. The slot
 * of a failed job stays `NULL` too, and neither leaves anything in its worker's `perm`. Each
 * finished job reports progress when `is_verbose`. Failing jobs' errors go to `log` in source
 * order, as `job_run` collects them, so a run reports every source that failed and all of each
 * one's errors.
 *
 * @param site_config    Site configuration shared by every parse job. Must not be `NULL`.
 * @param source_paths   Content entry source paths. One job runs per path. Each must be spelled
 *                       with `site_config->content_dir` as its literal prefix, because the
 *                       remainder is what becomes the entry's section. A path from outside that
 *                       root never arrives: the build's walk of `content_dir` guarantees this, and
 *                       the parse asserts it. Must not be `NULL`.
 * @param source_len_max Largest source a job reads, in bytes, at most `CONTENT_FILE_LEN_MAX`. A
 *                       larger one fails its own job before it takes any memory.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param worker_arenas  One `JobArenas` per requested worker, as for `job_run`. Each worker's
 *                       `perm` must outlive every use of the entries. Must not be `NULL`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param os             Host the sources are read from and progress printed on, by every worker at
 *                       once. Must not be `NULL`.
 * @param scratch        Arena for the phase's own bookkeeping, as for `job_run`.
 * @param source_entries One slot per source path, each `NULL`-initialized by the caller. A
 *                       successful non-draft job stores its entry there. May be `NULL` only when
 *                       `source_paths->count` is 0.
 * @param draft_flags    One flag per source path, each `false`-initialized by the caller. A job
 *                       that parses a draft sets its own. May be `NULL` only when
 *                       `source_paths->count` is 0.
 * @param log            Log that receives the collected parse errors. Its arena must be none of the
 *                       others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a parse job failed or the worker pool could
 *         not start.
 */
int entry_renderer_parse_entries(const struct SiteConfig* site_config,
                                 const struct PathList* source_paths,
                                 ptrdiff_t source_len_max,
                                 ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 bool is_verbose,
                                 struct Os* os,
                                 struct Arena scratch,
                                 struct ContentEntry** source_entries,
                                 bool* draft_flags,
                                 struct Log* log) __attribute__((nonnull(1, 2, 5, 7, 11)));

/**
 * @brief Converts the body of every parsed entry to HTML, with each link to another content file
 *        pointing at that file's page, or at its copy for an asset.
 *
 * This is the second of the three parallel phases. When `source_paths->count` is 0, it runs no job
 * and returns `0`. It runs after the parse phase, when every entry has its URL. Before the jobs
 * start, it finds the denote identifier in the name of every source and asset: a leading
 * `YYYYMMDDTHHMMSS`, or else the text after `@@` up to the next `==`, `--`, `__`, `@@` or `.`. When
 * two files have one identifier, a source comes before an asset, then the first path in sorted
 * order.
 *
 * One job runs per source path, and converts the body of the entry in that slot into the `perm` of
 * the worker that runs it, which keeps the HTML. A `NULL` slot has nothing to convert. The URL of a
 * link is relative to the directory of the linking page. A path link is relative to the directory
 * of the linking source, with its `.` and `..` segments resolved. Each link that names no file, or
 * names a draft, is one error, with the target and `(in '<source>')`:
 * `link '<path>' names no content file`, `link '<path>' names draft '<source>'`,
 * `denote link '<identifier>' names no file`, or
 * `denote link '<identifier>' names draft '<source>'`. A link to a source whose parse failed
 * reports nothing, because that source's own error fails the build. A link that fails keeps the
 * HTML that a conversion without links to content files gives it. Each job sets its entry's
 * `has_entry_link` and `has_asset_link`.
 *
 * @param source_entries Entry slots the parse phase filled, one per source path. Each job writes
 *                       the body and the link flags of its own entry. May be `NULL` only when
 *                       `source_paths->count` is 0.
 * @param draft_flags    Draft flags the parse phase set, one per source path. May be `NULL` only
 *                       when `source_paths->count` is 0.
 * @param site_config    Site configuration, whose `content_dir` the paths are below. Must not be
 *                       `NULL`.
 * @param source_paths   Content entry source paths, sorted, as for `entry_renderer_parse_entries`.
 *                       Must not be `NULL`.
 * @param asset_paths    Every other file below `content_dir`, which a denote link can name. Must
 *                       not be `NULL`.
 * @param worker_count   Requested worker threads, as for `pool_run`.
 * @param worker_arenas  One `JobArenas` per requested worker, as for `job_run`. Each worker's
 *                       `perm` must outlive every use of the entries. Must not be `NULL`.
 * @param is_verbose     Whether progress is printed to `stderr`.
 * @param os             Host progress is printed on, by every worker at once. Must not be `NULL`.
 * @param scratch        Arena for the list of identifiers and the phase's own bookkeeping, as for
 *                       `job_run`.
 * @param log            Log that receives the collected link errors. Its arena must be none of the
 *                       others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a link failed or the worker pool could not
 *         start.
 */
int entry_renderer_convert_bodies(struct ContentEntry* const* source_entries,
                                  const bool* draft_flags,
                                  const struct SiteConfig* site_config,
                                  const struct PathList* source_paths,
                                  const struct PathList* asset_paths,
                                  ptrdiff_t worker_count,
                                  struct JobArenas* worker_arenas,
                                  bool is_verbose,
                                  struct Os* os,
                                  struct Arena scratch,
                                  struct Log* log) __attribute__((nonnull(3, 4, 5, 7, 9, 11)));

#endif
