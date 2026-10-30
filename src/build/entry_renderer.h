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
 * @brief Turns every discovered content source into a rendered `ContentEntry`.
 *
 * This is the first of the two parallel phases. When `source_paths->count` is 0, it runs no job and
 * returns `0`. Each job reads its source, parses its frontmatter or Org keywords, converts the body
 * to HTML, and finalizes the entry's URL and output paths. It deliberately leaves page rendering to
 * `page_renderer_render_pages`, because a content template can read `site.updated` and
 * `content_entries`, and neither is known until every entry is parsed and sorted.
 *
 * One job runs per source path and fills that path's slot in `source_entries` with a rendered
 * `ContentEntry` on success. The entry and everything it points at live in the `perm` arena of the
 * worker that ran the job, which it commits only then. It parses draft entries but does not publish
 * them, so their slot stays `NULL`, as does the slot of a failed job, and neither leaves anything
 * in its worker's `perm`. Each finished job reports progress when `is_verbose`. Failing jobs'
 * errors go to `log` in source order, as `job_run` collects them, so a run reports every source
 * that failed and all of each one's errors.
 *
 * @param site_config    Site configuration shared by every parse job. Must not be `NULL`.
 * @param source_paths   Content entry source paths. One job runs per path. Each must be spelled
 *                       with `site_config->content_dir` as its literal prefix, because the
 *                       remainder is what becomes the entry's section. A path from outside that
 *                       root never arrives: the build's walk of `content_dir` guarantees this, and
 *                       the render asserts it. Must not be `NULL`.
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
 * @param log            Log that receives the collected parse errors. Its arena must be none of the
 *                       others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a parse job failed or the worker pool could
 *         not start.
 */
int entry_renderer_render_entries(const struct SiteConfig* site_config,
                                  const struct PathList* source_paths,
                                  ptrdiff_t source_len_max,
                                  ptrdiff_t worker_count,
                                  struct JobArenas* worker_arenas,
                                  bool is_verbose,
                                  struct Os* os,
                                  struct Arena scratch,
                                  struct ContentEntry** source_entries,
                                  struct Log* log) __attribute__((nonnull(1, 2, 5, 7, 10)));

#endif
