#include "app/cmd_build.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "build/entry_renderer.h"
#include "build/manifest_builder.h"
#include "build/page_renderer.h"
#include "build/render_job.h"
#include "build/site_writer.h"
#include "core/error.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/manifest.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "runtime/pool.h"
#include "shared/string_buffer.h"

/** Mutable state owned by one `cmd_build_execute` invocation. */
struct BuildState {
  /** Loaded site configuration. */
  struct SiteConfig site_config;

  /** Discovered content entry source paths. */
  struct PathList source_paths;

  /** Intended output paths for this build, populated before any file is written. */
  struct Manifest manifest;

  /** Render job result slots, one per source path, paired with their count. */
  struct RenderJobSet render_jobs;

  /** Rendered non-draft content entries used by templates, sorted newest-first. */
  struct ContentEntry** content_entries;

  /** Number of content entries in `content_entries`. */
  size_t content_entry_count;

  /**
   * Site last-updated timestamp exposed to every template as `site.updated`. Derived from
   * `content_entries` once they are sorted, so it is unset until then.
   */
  const char* site_updated;

  /** Resolved worker count for parallel content rendering. */
  size_t worker_count;

  /** Whether phase progress and status messages are printed to `stderr`. */
  bool is_verbose;
};

/**
 * @brief Prints a formatted status line to `stderr` when the build is verbose.
 *
 * @param state Build state whose verbosity gates the output. Must not be `NULL`.
 * @param fmt   `printf`-style format string. Must not be `NULL`.
 * @param ...   Arguments for `fmt`.
 */
static void build_verbose(const struct BuildState* state, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Initializes every owned field in a build state so cleanup is safe after partial setup.
 *
 * @param state Build state to prepare. Must not be `NULL`.
 */
static void build_state_init(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Releases all allocations held by a build state.
 *
 * @param state Build state to release. Must not be `NULL`.
 */
static void build_state_free(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Loads `sosig.toml`, creates the output directory, discovers content entry sources, and
 *        prepares result storage.
 *
 * @param state   Build state that receives the config, source paths, and result slots. Must not be
 *                `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on a config, directory, listing, or allocation failure.
 */
static int load_build_inputs(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Parses every discovered content entry source, dispatching jobs across worker threads.
 *
 * @param state     Build state holding the sources and result slots. Must not be `NULL`.
 * @param error_out Growable buffer that receives the collected render diagnostics. Must not be
 *                  `NULL`.
 * @return `0` when every job succeeded, or `-1` when any render job failed.
 */
static int render_content_entries(struct BuildState* state, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Compacts rendered non-draft content entries, orders them, and derives `site.updated`.
 *
 * @param state   Build state whose `content_entries` array is filled and sorted. Must not be
 *                `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int collect_content_entries(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Compacts every rendered non-draft entry into one contiguous array.
 *
 * Sizes the array in a first pass and fills it in a second, so the two never disagree. Writes both
 * `content_entries` and `content_entry_count` on the state, which the sort and the `site.updated`
 * derivation in the caller then read.
 *
 * @param state   Build state whose `content_entries` array is allocated and filled. Must not be
 *                `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int collect_content_entries_compact(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Records every intended output path, rejecting a collision before anything is rendered.
 *
 * @param state   Build state whose manifest is populated from the collected entries. Must not be
 *                `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` when all output paths are unique, or `-1` on a collision or allocation failure.
 */
static int populate_output_manifest(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Renders every content entry's content template, dispatching jobs across worker threads.
 *
 * Runs after the entries are collected and sorted, so a content template sees the whole entry set
 * and the site's last-updated timestamp.
 *
 * @param state     Build state holding the sorted entries and result slots. Must not be `NULL`.
 * @param error_out Growable buffer that receives the collected render diagnostics. Must not be
 *                  `NULL`.
 * @return `0` when every job succeeded, or `-1` when any render job failed.
 */
static int render_content_pages(struct BuildState* state, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes content entry outputs, then configured template outputs.
 *
 * @param state   Build state holding rendered results and collected entries. Must not be `NULL`.
 * @param err     Destination buffer for a failure diagnostic.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on a write or render failure.
 */
static int write_generated_site(struct BuildState* state, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Frees rendered content entry HTML buffers once they have been written.
 *
 * @param render_jobs Result set holding rendered HTML. Each slot's `rendered_html` is freed and
 *                    reset to `NULL`, so `build_state_free`, which frees the same field on every
 *                    path, cannot double-free it. Its `items` may be `NULL` when its `count` is 0,
 *                    which an empty content directory produces through a zero-sized `calloc`. The
 *                    count then bounds the loop away from that `NULL`. Must not be `NULL`.
 */
static void free_rendered_html(const struct RenderJobSet* render_jobs) __attribute__((nonnull(1)));

enum ExitCode cmd_build_run(const struct BuildOptions* options) {
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  enum ExitCode rc = EXIT_CODE_FAILURE;
  if (cmd_build_execute(options, &error_buffer) != 0) {
    // `error_out` comes back empty only when recording the diagnostic itself ran out of memory, so
    // the fallback keeps the failure visible. The fallback also makes the four discarded
    // `(void)string_buffer_append(...)` results in `cmd_build_execute` safe. An append that fails
    // degrades the message to this line instead of reporting a silent success.
    fprintf(stderr, "%s\n", error_buffer.data != NULL ? error_buffer.data : "build failed");
    goto cleanup;
  }
  rc = EXIT_CODE_OK;

cleanup:
  string_buffer_free(&error_buffer);
  return rc;
}

int cmd_build_execute(const struct BuildOptions* options, struct StringBuffer* error_out) {
  struct BuildState state;
  build_state_init(&state);
  state.worker_count =
      options->worker_count != 0 ? options->worker_count : pool_resolve_worker_count();
  state.is_verbose = options->is_verbose;

  // Single-message phases report through the fixed `err`/`err_len` convention and are bridged into
  // the growable buffer here. The render phases aggregate many failures and append directly.
  //
  // The order below follows the data: a content template can read `site.updated` and iterate
  // `content_entries`. Parsing every source has to finish, and the entries have to be collected and
  // sorted, before any page is rendered.
  char err[ERROR_MESSAGE_SIZE];
  // Terminated before the first phase runs, so a phase that returns `-1` without formatting a
  // diagnostic cannot hand `string_buffer_append` an indeterminate `char[512]`. Reading one is
  // undefined behavior, and in practice appends stack garbage to the build's error output.
  err[0] = '\0';
  int rc = -1;
  if (load_build_inputs(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (render_content_entries(&state, error_out) != 0) {
    goto cleanup;
  }
  if (collect_content_entries(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (populate_output_manifest(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }
  if (render_content_pages(&state, error_out) != 0) {
    goto cleanup;
  }
  if (write_generated_site(&state, err, sizeof(err)) != 0) {
    (void)string_buffer_append(error_out, err);
    goto cleanup;
  }

  build_verbose(&state, "build complete");
  rc = 0;

cleanup:
  build_state_free(&state);
  return rc;
}

// The `vfprintf` and `fputc` calls run on the main thread between phases, so they do not need the
// `flockfile`/`funlockfile` pair that `print_progress_dot` uses. That is temporal separation, not a
// guarantee. `pool_run` joins every worker before the next status line prints, so a progress dot
// cannot land between a line and its newline. Add the lock if anything ever writes `stderr` while a
// pool is running.
//
// Both writes are unchecked, and nothing calls `ferror(stderr)`. These lines are status, not
// diagnostics, and a build that otherwise succeeded should not fail because `stderr` was a closed
// pipe. The cost is that an `EPIPE` here goes unnoticed, unlike `site_config_print`, which reports
// it.
static void build_verbose(const struct BuildState* state, const char* fmt, ...) {
  if (!state->is_verbose) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  (void)vfprintf(stderr, fmt, ap);
  va_end(ap);
  (void)fputc('\n', stderr);
}

static void build_state_init(struct BuildState* state) {
  // This is one literal, so a field added to `BuildState` cannot be left out of its defaults.
  // `path_list_init` and `manifest_init` only restate that zeroing. The code calls them for
  // symmetry. `site_config_init` does more, though: it installs the optional-key defaults, which
  // `site_config_load` leaves untouched when a key is absent.
  *state = (struct BuildState){0};
  site_config_init(&state->site_config);
  path_list_init(&state->source_paths);
  manifest_init(&state->manifest);
}

// Releases content entries through their module API before freeing result storage.
//
// `content_entries` holds borrowed pointers into `render_jobs`, so only its array is freed here.
// Each entry is owned by the slot it was rendered into and is released with that slot. The loop
// bound is the set's own `count`, stored with the slots, so do not assume the array has one slot
// per discovered source path. A `count` of 0 leaves `items` `NULL`, and both the loop and the
// trailing `free` handle that.
static void build_state_free(struct BuildState* state) {
  free(state->content_entries);
  for (size_t i = 0; i < state->render_jobs.count; i++) {
    free(state->render_jobs.items[i].rendered_html);
    if (state->render_jobs.items[i].entry != NULL) {
      content_entry_free(state->render_jobs.items[i].entry);
      free(state->render_jobs.items[i].entry);
    }
  }
  free(state->render_jobs.items);
  manifest_free(&state->manifest);
  path_list_free(&state->source_paths);
  site_config_free(&state->site_config);
}

static int load_build_inputs(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "loading config");
  if (site_config_load(&state->site_config, SITE_CONFIG_PATH_DEFAULT, err, err_len) != 0) {
    return -1;
  }
  char reason[FS_REASON_SIZE];
  if (fs_mkdir_p(state->site_config.output_dir, reason, sizeof(reason)) != 0) {
    // The reason names the component that failed, which is more precise than the configured root,
    // so the root is not repeated here.
    return error_report(err, err_len, "failed to prepare output directory: %s", reason);
  }
  build_verbose(state, "discovering content");
  if (fs_list_files_with_suffix(&state->source_paths, state->site_config.content_dir, ".md", reason,
                                sizeof(reason)) != 0) {
    // The reason names the directory or entry that failed, which is more precise than the
    // configured root, so the root is not repeated here, as for `fs_mkdir_p` above.
    return error_report(err, err_len, "failed to list Markdown files: %s", reason);
  }

  // `calloc` fails rather than wrapping when the product overflows, so it carries the bound. The
  // count check belongs here, not at the `arena_calloc` sites. `calloc(0, n)` may return `NULL` for
  // an empty request, while the arena hands back a distinct one-byte allocation.
  state->render_jobs.items = calloc(state->source_paths.count, sizeof(*state->render_jobs.items));
  if (state->render_jobs.items == NULL && state->source_paths.count > 0) {
    return error_report(err, err_len, "out of memory allocating content entry render results");
  }
  // Set the count only past the failure check, so `count > 0` always implies a non-`NULL` `items`.
  // `build_state_free` leans on that: it bounds its loop by `count` with no separate `NULL`
  // guard.
  state->render_jobs.count = state->source_paths.count;
  return 0;
}

static int render_content_entries(struct BuildState* state, struct StringBuffer* error_out) {
  build_verbose(state, "parsing content with %zu workers", state->worker_count);
  return entry_renderer_render_entries(&state->site_config, &state->source_paths,
                                       state->worker_count, state->is_verbose, &state->render_jobs,
                                       error_out);
}

static int collect_content_entries(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "collecting content entries");
  if (collect_content_entries_compact(state, err, err_len) != 0) {
    return -1;
  }

  // Every template render below reads the entries in this order, and `site.updated` is the newest
  // date among them, so the code settles both here rather than per render.
  content_entry_sort(state->content_entries, state->content_entry_count);
  // C has no implicit qualification conversion for pointer-to-pointer, so handing the entry array
  // to a read-only callee needs this cast spelled out. It recurs at all five such call sites in
  // this file. It only adds `const`, at both levels, and the deep `const` is a thread-safety
  // requirement. A `TemplateContext` with only `const` pointers prevents one render job from
  // allocating into another job's arena.
  state->site_updated = content_entry_latest_date(
      (const struct ContentEntry* const*)state->content_entries, state->content_entry_count);
  return 0;
}

static int collect_content_entries_compact(struct BuildState* state, char* err, size_t err_len) {
  for (size_t i = 0; i < state->render_jobs.count; i++) {
    if (state->render_jobs.items[i].entry != NULL) {
      state->content_entry_count++;
    }
  }

  state->content_entries = calloc(state->content_entry_count, sizeof(*state->content_entries));
  if (state->content_entries == NULL && state->content_entry_count > 0) {
    return error_report(err, err_len, "out of memory collecting content entries");
  }
  size_t j = 0;
  for (size_t i = 0; i < state->render_jobs.count; i++) {
    if (state->render_jobs.items[i].entry != NULL) {
      state->content_entries[j++] = state->render_jobs.items[i].entry;
    }
  }
  return 0;
}

static int populate_output_manifest(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "building output manifest");
  // This runs ahead of the page renders, so a duplicate output path fails before rendering is
  // wasted. It also runs ahead of every write, so an output aimed at one of this build's own inputs
  // is refused before it can destroy the file.
  return manifest_builder_populate(&state->manifest, &state->site_config, SITE_CONFIG_PATH_DEFAULT,
                                   &state->source_paths,
                                   (const struct ContentEntry* const*)state->content_entries,
                                   state->content_entry_count, err, err_len);
}

static int render_content_pages(struct BuildState* state, struct StringBuffer* error_out) {
  build_verbose(state, "rendering content with %zu workers", state->worker_count);
  return page_renderer_render_pages(&state->render_jobs, &state->site_config,
                                    (const struct ContentEntry* const*)state->content_entries,
                                    state->content_entry_count, state->site_updated,
                                    state->worker_count, state->is_verbose, error_out);
}

static int write_generated_site(struct BuildState* state, char* err, size_t err_len) {
  build_verbose(state, "writing content pages");
  if (site_writer_write_content_entries(&state->render_jobs, err, err_len) != 0) {
    return -1;
  }
  free_rendered_html(&state->render_jobs);

  build_verbose(state, "rendering aggregate templates");
  if (site_writer_write_aggregates(
          &state->site_config, (const struct ContentEntry* const*)state->content_entries,
          state->content_entry_count, state->site_updated, err, err_len) != 0) {
    return -1;
  }
  build_verbose(state, "rendering feed templates");
  return site_writer_write_feeds(&state->site_config,
                                 (const struct ContentEntry* const*)state->content_entries,
                                 state->content_entry_count, state->site_updated, err, err_len);
}

// This frees the buffers eagerly after writes to reduce peak retained memory during the build. It
// resets each slot to `NULL`, so `build_state_free`, which frees the same field on every path,
// stays correct. Without the reset this eager free turns that one into a double free.
static void free_rendered_html(const struct RenderJobSet* render_jobs) {
  for (size_t i = 0; i < render_jobs->count; i++) {
    free(render_jobs->items[i].rendered_html);
    render_jobs->items[i].rendered_html = NULL;
  }
}
