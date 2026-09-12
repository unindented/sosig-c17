// `flockfile`/`funlockfile` are POSIX rather than C17, so a strict `-std=c17` compile hides them
// unless the platform's feature-test macro asks for them: `_DARWIN_C_SOURCE` on macOS,
// `_DEFAULT_SOURCE` on glibc. Both must precede every `#include`, because a libc header resolves
// its own visibility guards the first time it is included.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "build/render_job.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/error.h"
#include "core/string_buffer.h"
#include "runtime/pool.h"

/**
 * @brief Appends one buffered worker diagnostic to the growable render error buffer.
 *
 * Preserves earlier lines and separates messages with a newline, so the single boundary reports
 * every failing content entry.
 *
 * @param buffer  Growable buffer that accumulates one diagnostic per line. Must not be `NULL`.
 * @param message Terminated diagnostic to append. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_render_error(struct StringBuffer* buffer, const char* message)
    __attribute__((nonnull(1, 2)));

int render_job_run(const struct RenderJobSet* render_jobs,
                   size_t worker_count,
                   PoolJobFn job_fn,
                   void* userdata,
                   bool is_verbose,
                   struct StringBuffer* error_out) {
  const size_t job_count = render_jobs->count;
  const int pool_rc = pool_run(job_count, worker_count, job_fn, userdata);

  // `pool_run` joins every worker before returning, and that join is the happens-before edge that
  // publishes each job's writes to its own `render_jobs` slot. Everything below therefore runs with
  // no worker alive. The slot reads need no atomics. The `stderr` write needs no `flockfile`, and a
  // version of this that read the slots before the join would be a data race.

  // Close the progress dot line before any later status lines.
  if (is_verbose && job_count > 0) {
    (void)fputc('\n', stderr);
  }
  // Translate every buffered worker diagnostic into the growable error buffer, so the command
  // boundary reports them once and keeps each failing entry visible.
  bool reported_any = false;
  for (size_t i = 0; i < job_count; i++) {
    if (render_jobs->items[i].error_message[0] == '\0') {
      continue;
    }
    if (append_render_error(error_out, render_jobs->items[i].error_message) != 0) {
      return -1;
    }
    reported_any = true;
  }
  if (pool_rc != 0) {
    // `pool_run` can fail before any job records a per-slot diagnostic (e.g. no worker thread could
    // be started), so append a generic message rather than failing silently at the command
    // boundary.
    if (!reported_any && append_render_error(error_out, "content rendering failed to start") != 0) {
      return -1;
    }
    return -1;
  }
  return 0;
}

void render_job_set_error(struct RenderJob* result, const char* fmt, ...) {
  // An empty `error_message` is this module's documented "no error" sentinel, so it doubles as the
  // "nothing recorded yet" test without a second flag.
  if (result->error_message[0] != '\0') {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  // This goes through `error_report_va` rather than `vsnprintf` directly, so it marks an over-long
  // message as truncated. These messages compose a callee's whole diagnostic with a source path
  // into one `ERROR_MESSAGE_SIZE` buffer, so they are the ones most likely to be cut. An unmarked
  // cut leaves a path that reads as complete and names no existing file.
  error_report_va(result->error_message, sizeof(result->error_message), fmt, ap);
  va_end(ap);
}

void render_job_progress_dot(bool is_verbose) {
  if (!is_verbose) {
    return;
  }
  // This deliberately discards both write results. It runs inside a `PoolJobFn`, so a failed
  // progress dot must not turn into a failed render pass. Do not reshape it to report errors. The
  // flush is required because a dot never ends a line and `stderr` may be line-buffered. It stays
  // inside the lock, so no other worker's dot can land between the write and its flush.
  flockfile(stderr);
  (void)fputc('.', stderr);
  (void)fflush(stderr);
  funlockfile(stderr);
}

static int append_render_error(struct StringBuffer* buffer, const char* message) {
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
