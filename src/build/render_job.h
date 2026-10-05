#ifndef SOSIG_RENDER_JOB_H
#define SOSIG_RENDER_JOB_H

#include <stdbool.h>
#include <stddef.h>

#include "core/error.h"
#include "runtime/pool.h"

struct StringBuffer;

/**
 * Error slot for one render job. The caller allocates the slots, and each job records
 * `error_message` on failure.
 */
struct RenderJob {
  /** Render job error reported after the workers finish. */
  char error_message[ERROR_MESSAGE_SIZE];
};

/**
 * The full set of render result slots for one build, one slot per discovered source path. Pairs the
 * slot array with its length so a caller that only touches results carries no separate count.
 */
struct RenderJobSet {
  /** Result slots, one per source path. Its length is `count`. */
  struct RenderJob* items;

  /** Number of result slots in `items`. */
  size_t count;
};

/** Most distinct diagnostics one render pass reports before summarizing the rest as a count. */
enum { RENDER_JOB_ERROR_REPORT_COUNT_MAX = 20 };

/**
 * @brief Runs one render pass across the worker pool, reports progress, and collects its
 *        diagnostics.
 *
 * Each run executes one job per result slot, so a job index is also its slot index. It runs
 * `render_jobs->count` jobs. `job_fn` writes only its own slot. This function reads every slot's
 * `error_message` after the pool joins and appends each distinct one to `error_out` as its own
 * line, at most `RENDER_JOB_ERROR_REPORT_COUNT_MAX` of them, followed by a count of the remaining
 * failures. A newline is written only between lines, so the collection never starts with a blank
 * line.
 *
 * @param render_jobs  Result slot set whose `count` is the job count and whose buffered diagnostics
 *                     are collected. Must not be `NULL`.
 * @param worker_count Requested worker threads, as for `pool_run`.
 * @param job_fn       Job run once per result slot, concurrently from several threads, so it must
 *                     be thread-safe. Must not be `NULL`.
 * @param userdata     Shared job context forwarded to `job_fn`. Must not be `NULL`.
 * @param phase_label  Pass name leading each progress line, and named when the pass fails without a
 *                     diagnostic. Must not be `NULL`.
 * @param is_verbose   Whether each finished job prints a progress line to `stderr`.
 * @param error_out    Growable buffer that receives the collected render diagnostics. Must not be
 *                     `NULL`.
 * @return `0` when every job succeeded, or `-1` when a job failed, the worker pool could not start,
 *         or a diagnostic could not be appended.
 */
int render_job_run(const struct RenderJobSet* render_jobs,
                   size_t worker_count,
                   PoolJobFn job_fn,
                   void* userdata,
                   const char* phase_label,
                   bool is_verbose,
                   struct StringBuffer* error_out) __attribute__((nonnull(1, 3, 4, 5, 7)));

/**
 * @brief Records a job's first diagnostic in its result slot.
 *
 * Safe to call from worker threads because it writes only the job's own result slot. This is
 * first-wins: a later call for the same slot is ignored, because the first message names the cause
 * and a later one is generally a consequence of it.
 *
 * @param result Result slot that receives the message. Must not be `NULL`.
 * @param fmt    `printf` format for the diagnostic. Must not be `NULL`.
 * @param ...    Arguments for `fmt`.
 */
void render_job_set_error(struct RenderJob* result, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Releases the slot array.
 *
 * @param render_jobs Result slot set to release. Must not be `NULL`.
 */
void render_job_set_free(struct RenderJobSet* render_jobs) __attribute__((nonnull(1)));

#endif
