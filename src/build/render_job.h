#ifndef SOSIG_RENDER_JOB_H
#define SOSIG_RENDER_JOB_H

#include <stdbool.h>
#include <stddef.h>

#include "core/error.h"
#include "runtime/pool.h"

struct ContentEntry;
struct StringBuffer;

/**
 * Result slot for one content entry render job, shared by the two render passes and by the writer.
 *
 * `entry_renderer_render_entries` fills the slot's `entry`, `page_renderer_render_pages` reads that
 * `entry` and writes its `rendered_html`, and either pass records `error_message` on failure. The
 * slot is the sole coupling between the passes: neither module needs the other's header, only this
 * one.
 */
struct RenderJob {
  /**
   * Rendered content entry the caller takes ownership of on success, or `NULL` for a draft or
   * unrendered slot. Heap-allocated: release with `content_entry_free` then `free`.
   */
  struct ContentEntry* entry;

  /**
   * Rendered content entry HTML owned until written to disk, or `NULL` when unset. Release with
   * `free`.
   */
  char* rendered_html;

  /** Render job error reported after the workers finish. */
  char error_message[ERROR_MESSAGE_SIZE];
};

/**
 * The full set of render result slots for one build, one slot per discovered source path. Pairs the
 * slot array with its length so a caller that only touches results carries no separate count.
 * `count` equals the source-path count.
 */
struct RenderJobSet {
  /**
   * Result slots, one per source path. Its length is `count`. `NULL` only when `count` is 0, which
   * a zero-sized `calloc` may return.
   */
  struct RenderJob* items;

  /** Number of result slots in `items`. */
  size_t count;
};

/**
 * @brief Runs one render pass across the worker pool and collects its buffered diagnostics.
 *
 * Both passes run one job per result slot, so a job index is also its slot index. The pass runs
 * `render_jobs->count` jobs. `job_fn` writes only its own slot. This function reads every slot's
 * `error_message` after the pool joins.
 *
 * @param render_jobs  Result slot set whose `count` is the job count and whose buffered
 *                     diagnostics are collected. Its `items` is `NULL` only for a `count` of 0.
 * Must not be `NULL`.
 * @param worker_count Worker threads used for rendering. `0` is treated as `1`.
 * @param job_fn       Job run once per result slot. Must not be `NULL`.
 * @param userdata     Shared job context forwarded to `job_fn`. Must not be `NULL`.
 * @param is_verbose   Whether the pass closes its progress dot line on `stderr`.
 * @param error_out    Growable buffer that receives the collected render diagnostics. Must not be
 *                     `NULL`.
 * @return `0` when every job succeeded, or `-1` when a job failed, when the worker pool could not
 *         start, or when a diagnostic could not be buffered.
 */
int render_job_run(const struct RenderJobSet* render_jobs,
                   size_t worker_count,
                   PoolJobFn job_fn,
                   void* userdata,
                   bool is_verbose,
                   struct StringBuffer* error_out) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Stores an error message in a render result slot, keeping the first.
 *
 * Safe to call from worker threads because it writes only the job's own result slot. It is
 * first-wins, like the other two error latches in this codebase (`record_error` and `render_fail`).
 * The first message names the root cause. A job records one diagnostic and returns immediately
 * today, so the policy is not yet observable. This pins the policy here so a job that grows a
 * second failure path cannot silently overwrite the cause with a consequence.
 *
 * @param result Result slot that receives the message. Must not be `NULL`.
 * @param fmt    `printf`-style format string. Must not be `NULL`.
 * @param ...    Arguments for `fmt`.
 */
void render_job_set_error(struct RenderJob* result, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Prints one progress dot for a finished job when the build is verbose.
 *
 * Safe to call from worker threads. `stderr`'s lock serializes the write and its flush.
 *
 * This is best-effort. It deliberately ignores the write and flush results, so a failed progress
 * dot cannot fail the render job it reports on. Returning `-1` from a `PoolJobFn` marks the whole
 * pass failed, which would let a full `stderr` fail a build whose content rendered perfectly.
 *
 * @param is_verbose Whether the dot is printed at all.
 */
void render_job_progress_dot(bool is_verbose);

#endif
