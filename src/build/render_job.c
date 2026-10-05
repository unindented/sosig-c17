// `flockfile`/`funlockfile` are POSIX rather than C17, so a strict `-std=c17` compile hides them
// unless the platform's feature-test macro asks for them: `_DARWIN_C_SOURCE` on macOS,
// `_DEFAULT_SOURCE` on glibc. Both must precede every `#include`, because a libc header resolves
// its own visibility guards the first time it is included.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "build/render_job.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "runtime/pool.h"
#include "shared/string_buffer.h"

/** One running render pass: the caller's job and the progress state wrapped around it. */
struct RenderJobRun {
  /** Number of jobs. Printed as the denominator of the progress line. */
  size_t count;

  /** Caller's job, invoked once per index. Read-only during the run. */
  PoolJobFn job_fn;

  /** Opaque context handed to every `job_fn` call, whose sharing is the caller's to make safe. */
  void* userdata;

  /** Pass name leading each progress line, and named when the pass fails without a diagnostic. */
  const char* phase_label;

  /** Whether each finished job prints a progress line to `stderr`. */
  bool is_verbose;

  /**
   * Jobs finished so far. Every worker increments it, so it is atomic. The only other memory a
   * worker writes is its own result slot.
   */
  atomic_size_t completed_count;
};

/**
 * @brief Runs one job and prints its progress line. This is the `PoolJobFn` behind
 *        `render_job_run`.
 *
 * @param index    Job index to run.
 * @param userdata `struct RenderJobRun*` of the running pass. Must not be `NULL`.
 * @return The job's own result: `0` on success, or `-1` on failure.
 */
static int run_one_job(size_t index, void* userdata) __attribute__((nonnull(2)));

/**
 * @brief Appends every distinct job diagnostic, the remainder count, and a fallback diagnostic.
 *
 * Runs after `pool_run` has joined every worker, so the slots are read unsynchronized.
 *
 * @param render_jobs Finished result slot set. Must not be `NULL`.
 * @param phase_label Pass name used by the fallback diagnostic. Must not be `NULL`.
 * @param pool_rc     Result of `pool_run`.
 * @param error_out   Growable buffer receiving the diagnostics. Must not be `NULL`.
 * @return `0` when the pass succeeded, or `-1` when it failed or a diagnostic could not be
 *         appended.
 */
static int run_report_errors(const struct RenderJobSet* render_jobs,
                             const char* phase_label,
                             int pool_rc,
                             struct StringBuffer* error_out) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Reports whether a message was already appended to this pass's report.
 *
 * @param reported       Borrowed messages appended so far. Must not be `NULL`.
 * @param reported_count Number of entries in `reported`.
 * @param message        Candidate message. Must not be `NULL`.
 * @return `true` when an identical message was already reported.
 */
static bool is_reported_message(const char* const* reported,
                                size_t reported_count,
                                const char* message) __attribute__((nonnull(1, 3)));

/**
 * @brief Appends one diagnostic to a pass's collected error text as its own line.
 *
 * A newline is written only when `buffer` already holds text, so the collection never starts with a
 * blank line.
 *
 * @param buffer  Growable collection of pass diagnostics. Must not be `NULL`.
 * @param message Terminated message to append. Must not be `NULL`.
 * @return `0` when the message was appended, or `-1` on allocation failure, leaving `buffer`
 *         unchanged.
 */
static int append_error(struct StringBuffer* buffer, const char* message)
    __attribute__((nonnull(1, 2)));

int render_job_run(const struct RenderJobSet* render_jobs,
                   size_t worker_count,
                   PoolJobFn job_fn,
                   void* userdata,
                   const char* phase_label,
                   bool is_verbose,
                   struct StringBuffer* error_out) {
  struct RenderJobRun run = {
      .count = render_jobs->count,
      .job_fn = job_fn,
      .userdata = userdata,
      .phase_label = phase_label,
      .is_verbose = is_verbose,
  };
  atomic_init(&run.completed_count, 0);
  const int pool_rc = pool_run(run.count, worker_count, run_one_job, &run);

  // `pool_run` joins every worker before returning, and that join is the happens-before edge that
  // publishes each job's writes to its own `render_jobs` slot. Everything below therefore runs with
  // no worker alive. The slot reads need no atomics. The `stderr` write needs no `flockfile`, and a
  // version of this that read the slots before the join would be a data race.

  // Close the progress line before any later status lines.
  if (is_verbose && run.count > 0) {
    (void)fputc('\n', stderr);
  }
  return run_report_errors(render_jobs, phase_label, pool_rc, error_out);
}

void render_job_set_error(struct RenderJob* result, const char* fmt, ...) {
  if (result->error_message[0] != '\0') {
    return;
  }

  // This is first-wins, like the other error latches (`record_error` and `render_fail`). The first
  // message names the cause. A later one is generally a consequence and must not replace it.
  va_list ap;
  va_start(ap, fmt);
  // This goes through `error_report_va` rather than `vsnprintf` directly, so it marks an over-long
  // message as truncated. These messages compose a callee's whole diagnostic with a source path
  // into one `ERROR_MESSAGE_SIZE` buffer, so they are the ones most likely to be cut. An unmarked
  // cut leaves a path that reads as complete and names no existing file.
  error_report_va(result->error_message, sizeof(result->error_message), fmt, ap);
  va_end(ap);
}

void render_job_set_free(struct RenderJobSet* render_jobs) {
  free(render_jobs->items);
}

static int run_one_job(size_t index, void* userdata) {
  struct RenderJobRun* run = userdata;
  const int rc = run->job_fn(index, run->userdata);
  const size_t completed =
      atomic_fetch_add_explicit(&run->completed_count, 1, memory_order_relaxed) + 1;
  if (run->is_verbose) {
    // Progress is best-effort. A full or closed `stderr` must not fail a pass whose jobs succeeded,
    // so both write results are discarded. The flush is required because a progress line ends in
    // `\r` rather than a newline and `stderr` may be line-buffered. It stays inside the lock, so no
    // other worker's line can land between the write and its flush.
    flockfile(stderr);
    (void)fprintf(stderr, "\r%s %zu/%zu", run->phase_label, completed, run->count);
    (void)fflush(stderr);
    funlockfile(stderr);
  }
  return rc;
}

static int run_report_errors(const struct RenderJobSet* render_jobs,
                             const char* phase_label,
                             int pool_rc,
                             struct StringBuffer* error_out) {
  // Report each distinct message once rather than the first twenty slots. Most render diagnostics
  // name their own entry and so differ, but one that does not would repeat across every job, and
  // capping by slot would spend the whole budget on twenty copies of one line. Deduplicating means
  // the report names every different message it saw, and the remainder below still carries the true
  // scale.
  const char* reported[RENDER_JOB_ERROR_REPORT_COUNT_MAX] = {NULL};
  size_t failure_count = 0;
  size_t reported_count = 0;
  for (size_t i = 0; i < render_jobs->count; i++) {
    const char* message = render_jobs->items[i].error_message;
    if (message[0] == '\0') {
      continue;
    }
    failure_count++;
    if (is_reported_message(reported, reported_count, message)) {
      continue;
    }
    if (reported_count == RENDER_JOB_ERROR_REPORT_COUNT_MAX) {
      continue;
    }
    if (append_error(error_out, message) != 0) {
      return -1;
    }
    // The slots are stable after the join, so borrowing each message is enough to compare against.
    reported[reported_count] = message;
    reported_count++;
  }

  if (failure_count > reported_count) {
    char summary[ERROR_MESSAGE_SIZE];
    error_report(summary, sizeof(summary), "\xE2\x80\xA6 and %zu more failures",
                 failure_count - reported_count);
    if (append_error(error_out, summary) != 0) {
      return -1;
    }
  }

  if (pool_rc != 0) {
    // `pool_run` can fail before any job records a per-slot diagnostic (e.g. no worker thread could
    // be started), so append a generic message rather than failing silently at the command
    // boundary.
    if (failure_count == 0) {
      char message[ERROR_MESSAGE_SIZE];
      error_report(message, sizeof(message), "%s failed without a diagnostic", phase_label);
      if (append_error(error_out, message) != 0) {
        return -1;
      }
    }
    return -1;
  }
  return 0;
}

static bool is_reported_message(const char* const* reported,
                                size_t reported_count,
                                const char* message) {
  for (size_t i = 0; i < reported_count; i++) {
    if (strcmp(reported[i], message) == 0) {
      return true;
    }
  }
  return false;
}

static int append_error(struct StringBuffer* buffer, const char* message) {
  // Reserve the separator and the message together so a failure leaves the buffer unchanged rather
  // than ending it with a dangling newline. Both appends below then fit the reserved capacity.
  const size_t separator_len = buffer->len > 0 ? 1 : 0;
  if (string_buffer_reserve(buffer, separator_len + strlen(message)) != 0) {
    return -1;
  }
  if (separator_len > 0 && string_buffer_append_char(buffer, '\n') != 0) {
    return -1;
  }
  return string_buffer_append(buffer, message);
}
