#include "build/entry_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "build/render_job.h"
#include "core/arena.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "domain/frontmatter.h"
#include "domain/permalink.h"
#include "domain/site_config.h"
#include "formats/markdown.h"
#include "runtime/fs.h"

/**
 * Slugified `/`-separated path segments and their lengths, sized and filled in one pass.
 *
 * `item_lens` and `joined_len` come from the same measurements, so `join_segments` sizes its
 * allocation and fills it from one set of numbers. Recovering the lengths again with `strlen` at
 * the fill would make the allocation depend on two derivations agreeing. The allocation has no
 * slack to absorb a disagreement. It is exactly `joined_len + count`, which is the payload plus one
 * `/` between segments plus the terminator.
 */
struct SlugSegments {
  /** Arena-owned terminated slug for each segment, in path order. */
  const char** items;

  /** Length in bytes of each slug in `items`, excluding its terminator. */
  const size_t* item_lens;

  /** Number of segments, one more than the separator count. */
  size_t count;

  /**
   * Sum of `item_lens`: the payload `join_segments` writes, separators aside. Each slug is at most
   * twice its source segment or the 8-byte `untitled` fallback, whichever is larger. The segments
   * partition one allocated path, so the sum cannot overflow.
   */
  size_t joined_len;
};

/** Source bytes and frontmatter slices for one content entry render job. */
struct ContentEntryRenderSource {
  /** Complete Markdown source file contents owned by this struct. */
  char* markdown;

  /** Borrowed slices into `markdown` after frontmatter splitting. */
  struct FrontmatterSplit split;
};

/**
 * Worker context shared by all content entry render jobs. Every field is read-only to a job except
 * `render_jobs`, where each job writes its own slot.
 */
struct ContentEntryRenderContext {
  /** Site configuration shared by every render job. */
  const struct SiteConfig* site_config;

  /** Content entry source paths shared by every render job. */
  const struct PathList* source_paths;

  /** One result slot per source path. Each render job writes only its index. */
  struct RenderJob* render_jobs;

  /** Whether each finished job prints a progress dot to `stderr`. */
  bool is_verbose;
};

/**
 * @brief Parses one content entry source path as a worker-pool job.
 *
 * Owns all temporary resources for the job, transferring the parsed entry to the matching result
 * slot on success. Draft entries are parsed but leave the slot empty.
 *
 * @param index    Source path index this job parses.
 * @param userdata Pointer to the shared `struct ContentEntryRenderContext`. Must not be `NULL`.
 * @return `0` on success or a skipped draft, or `-1` on a parse failure.
 */
static int render_content_entry_job(size_t index, void* userdata) __attribute__((nonnull(2)));

/**
 * @brief Allocates and initializes a content entry, reporting allocation failure on the result.
 *
 * @param source_path Source path used in the failure diagnostic. Must not be `NULL`.
 * @param result      Result slot that receives an allocation error message on failure. Must not be
 *                    `NULL`.
 * @return The initialized entry the caller owns, or `NULL` on allocation failure.
 */
static struct ContentEntry* render_content_entry_create(const char* source_path,
                                                        struct RenderJob* result)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads a content entry source file and parses its frontmatter into the entry.
 *
 * @param entry       Entry that receives the parsed metadata. Must not be `NULL`.
 * @param source_path Path of the source file to read. Must not be `NULL`.
 * @param source_out  Receives the file bytes and frontmatter/body slices. Must not be `NULL`.
 * @param result      Result slot that receives an error message on failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read, split, or parse failure.
 */
static int render_content_entry_load_source(struct ContentEntry* entry,
                                            const char* source_path,
                                            struct ContentEntryRenderSource* source_out,
                                            struct RenderJob* result)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Converts a content entry's Markdown body into arena-owned HTML.
 *
 * @param entry       Entry that receives `body_html`. Must not be `NULL`.
 * @param source      Render source whose frontmatter split supplies the Markdown body. Must not be
 *                    `NULL`.
 * @param source_path Source path used in the failure diagnostic. Must not be `NULL`.
 * @param result      Result slot that receives an error message on failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a render or allocation failure.
 */
static int render_content_entry_body(struct ContentEntry* entry,
                                     const struct ContentEntryRenderSource* source,
                                     const char* source_path,
                                     struct RenderJob* result) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Computes the public URL path and filesystem output path by expanding the permalink.
 *
 * Requires the source to sit under `content_dir`. Then expands `site_config->permalink` with the
 * entry slug and the source's slugified section. Validates and length-checks the resulting relative
 * path before rooting it in `output_dir`.
 *
 * @param entry       Entry that receives `url_path` and `output_path`. Must not be `NULL`.
 * @param site_config Configuration supplying `permalink`, `content_dir`, and `output_dir`. Must not
 *                    be `NULL`.
 * @param source_path Source path used to derive the section and for failure diagnostics. Must be
 *                    spelled with `site_config->content_dir` as its literal prefix. Must not be
 * `NULL`.
 * @param result      Result slot that receives an error message on failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the source is not under `content_dir`, on an unsafe or
 *         oversize path, or on allocation failure.
 */
static int render_content_entry_finalize_paths(struct ContentEntry* entry,
                                               const struct SiteConfig* site_config,
                                               const char* source_path,
                                               struct RenderJob* result)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Returns the remainder of `source_path` after a required `content_dir/` prefix.
 *
 * @param source_path Source Markdown path that must be spelled with `content_dir` as its literal
 *                    prefix. Must not be `NULL`.
 * @param content_dir Configured content directory, with no trailing `/`. Must not be `NULL`.
 * @param result      Result slot that receives an error message when the prefix is missing. Must
 *                    not be `NULL`.
 * @return Pointer into `source_path` past `content_dir/`, or `NULL` after recording an error.
 */
static const char* render_content_entry_finalize_paths_relative(const char* source_path,
                                                                const char* content_dir,
                                                                struct RenderJob* result)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Derives an entry's URL-safe section from its source path relative to `content_dir`.
 *
 * Drops the filename, slugifies each remaining path segment, and rejoins them with `/`. A source
 * directly in the content root yields an empty string.
 *
 * Takes the already-relative path rather than stripping the `content_dir` prefix itself. A path
 * that does not carry that prefix is a contract violation the caller reports. This function cannot
 * derive a section from it.
 *
 * @param source_relative_path Source Markdown path with the `<content_dir>/` prefix removed. Must
 *                             not be `NULL`.
 * @param arena                Arena that owns the returned section string. Must not be `NULL`.
 * @return Terminated section owned by `arena`, or `NULL` on allocation failure.
 */
static char* render_content_entry_finalize_paths_section(const char* source_relative_path,
                                                         struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Slugifies each `/`-separated segment of `text` into arena storage.
 *
 * Counting the separators up front lets the code hold every slug at once. The count bounds the two
 * parallel arrays. The slugs' own lengths then size the joined result exactly, so the caller builds
 * directly in arena storage rather than in a heap buffer it copies.
 *
 * @param text         Segment bytes to slugify, without a trailing separator. Must not be `NULL`.
 * @param text_len     Number of bytes in `text`.
 * @param arena        Arena that owns every array and slug written to `segments_out`. Must not be
 *                     `NULL`.
 * @param segments_out Receives the slugs, their lengths, and their total. Written only on success.
 *                     Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int slugify_segments(const char* text,
                            size_t text_len,
                            struct Arena* arena,
                            struct SlugSegments* segments_out) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Joins slugified segments with `/` into one arena-owned string.
 *
 * @param segments Segments to join, as filled by `slugify_segments`. Must not be `NULL`.
 * @param arena    Arena that owns the returned string. Must not be `NULL`.
 * @return Terminated joined string owned by `arena`, or `NULL` on allocation failure.
 */
static char* join_segments(const struct SlugSegments* segments, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Roots an expanded permalink URL under the output directory as the entry's output path.
 *
 * Strips the leading `/` from the URL, validates the remainder as a safe relative path within the
 * output-path length limit, then joins it under `output_dir`.
 *
 * @param entry       Entry that receives `url_path` and `output_path`. Must not be `NULL`.
 * @param site_config Configuration supplying `output_dir`, and `permalink` for diagnostics. Must
 *                    not be `NULL`.
 * @param url_path    Expanded permalink URL with a leading `/`, allocated in `entry->arena`. The
 *                    entry stores it without copying, so it must outlive the entry. Must not be
 * `NULL`.
 * @param source_path Source path used in failure diagnostics. Must not be `NULL`.
 * @param result      Result slot that receives an error message on failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on an unsafe/oversize path or allocation failure.
 */
static int render_content_entry_finalize_paths_output(struct ContentEntry* entry,
                                                      const struct SiteConfig* site_config,
                                                      const char* url_path,
                                                      const char* source_path,
                                                      struct RenderJob* result)
    __attribute__((nonnull(1, 2, 3, 4, 5)));

/**
 * @brief Releases the source buffer held by a content entry render source.
 *
 * @param source Render source whose `markdown` buffer is freed. Must not be `NULL`.
 */
static void render_content_entry_free_source(struct ContentEntryRenderSource* source)
    __attribute__((nonnull(1)));

int entry_renderer_render_entries(const struct SiteConfig* site_config,
                                  const struct PathList* source_paths,
                                  size_t worker_count,
                                  bool is_verbose,
                                  struct RenderJobSet* render_jobs_out,
                                  struct StringBuffer* error_out) {
  struct ContentEntryRenderContext render_context = {
      .site_config = site_config,
      .source_paths = source_paths,
      .render_jobs = render_jobs_out->items,
      .is_verbose = is_verbose,
  };

  // Establish the sentinels each job and the aggregation below rely on. An empty `error_message`
  // means "no error", and `NULL` `entry`/`rendered_html` marks a skipped or unrendered slot.
  // Zeroing here (before any worker starts) means callers need not pre-initialize the slots.
  // Callers that already allocate with `calloc` pay a second pass, which keeps this module's
  // contract self-contained and lets tests hand over raw storage. The count guard is not an
  // optimization. `render_jobs_out->items` may be `NULL` when there are no sources (as its contract
  // states), and `memset` requires a non-`NULL` pointer even for a zero byte count, so an unguarded
  // call would be undefined behavior on an empty site. The pass runs one job per source path, so
  // the set's `count` must equal `source_paths->count`.
  if (render_jobs_out->count > 0) {
    memset(render_jobs_out->items, 0, render_jobs_out->count * sizeof(*render_jobs_out->items));
  }

  return render_job_run(render_jobs_out, worker_count, render_content_entry_job, &render_context,
                        is_verbose, error_out);
}

static int render_content_entry_job(size_t index, void* userdata) {
  struct ContentEntryRenderContext* render_context = userdata;
  struct RenderJob* result = &render_context->render_jobs[index];
  const char* source_path = render_context->source_paths->items[index];
  struct ContentEntryRenderSource source = {0};
  struct ContentEntry* entry = NULL;
  int rc = -1;

  entry = render_content_entry_create(source_path, result);
  if (entry == NULL) {
    goto cleanup;
  }
  if (render_content_entry_load_source(entry, source_path, &source, result) != 0) {
    goto cleanup;
  }
  if (entry->is_draft) {
    rc = 0;
    goto cleanup;
  }
  if (render_content_entry_body(entry, &source, source_path, result) != 0) {
    goto cleanup;
  }
  if (render_content_entry_finalize_paths(entry, render_context->site_config, source_path,
                                          result) != 0) {
    goto cleanup;
  }

  result->entry = entry;
  entry = NULL;
  rc = 0;

cleanup:
  render_content_entry_free_source(&source);
  if (entry != NULL) {
    content_entry_free(entry);
    free(entry);
  }
  render_job_progress_dot(render_context->is_verbose);
  return rc;
}

static struct ContentEntry* render_content_entry_create(const char* source_path,
                                                        struct RenderJob* result) {
  // Use `malloc`, not `calloc`. `content_entry_init` assigns a whole compound literal over the
  // struct, so it writes every field, and a prior zeroing pass would be dead. The `calloc` in
  // `cmd_build.c`'s `load_build_inputs` is the opposite case and stays, because there the zeroing
  // is the sentinel state the code reads its result slots against.
  struct ContentEntry* entry = malloc(sizeof(*entry));
  if (entry == NULL) {
    render_job_set_error(result, "out of memory allocating content entry for '%s'", source_path);
    return NULL;
  }
  content_entry_init(entry);
  return entry;
}

static int render_content_entry_load_source(struct ContentEntry* entry,
                                            const char* source_path,
                                            struct ContentEntryRenderSource* source_out,
                                            struct RenderJob* result) {
  size_t markdown_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(source_path, &source_out->markdown, &markdown_len, reason, sizeof(reason)) !=
      0) {
    render_job_set_error(result, "failed to read content: %s ('%s')", reason, source_path);
    return -1;
  }

  char error_message[ERROR_MESSAGE_SIZE];
  // This writes the callee's reason first, and keeps all of it. Leading with the source path would
  // put the reason at the tail of a second `ERROR_MESSAGE_SIZE` buffer, where a long path evicts it
  // entirely and leaves the user with a path twice and no cause. The path can still be cut for a
  // pathological one, which costs far less. The job can recover the path, but not the reason. This
  // parenthesizes the attribution, because a reason may itself end in a `: '<value>'` clause, which
  // a bare `in '%s'` suffix would read as part of.
  if (frontmatter_split(source_out->markdown, markdown_len, &source_out->split, error_message,
                        sizeof(error_message)) != 0) {
    render_job_set_error(result, "%s (in '%s')", error_message, source_path);
    return -1;
  }
  if (frontmatter_parse(entry, source_out->split.frontmatter, source_out->split.frontmatter_len,
                        source_path, error_message, sizeof(error_message)) != 0) {
    render_job_set_error(result, "%s (in '%s')", error_message, source_path);
    return -1;
  }
  return 0;
}

static int render_content_entry_body(struct ContentEntry* entry,
                                     const struct ContentEntryRenderSource* source,
                                     const char* source_path,
                                     struct RenderJob* result) {
  char error_message[ERROR_MESSAGE_SIZE] = "";
  char* html = markdown_to_html(source->split.body, source->split.body_len, error_message,
                                sizeof(error_message));
  int rc = -1;
  if (html != NULL) {
    // Copy into the entry arena so `content_entry_free` owns cleanup.
    entry->body_html = arena_strdup(&entry->arena, html);
    rc = entry->body_html != NULL ? 0 : -1;
  }
  if (rc != 0) {
    if (error_message[0] != '\0') {
      render_job_set_error(result, "%s (in '%s')", error_message, source_path);
    } else {
      render_job_set_error(result, "failed to render Markdown for '%s'", source_path);
    }
  }
  free(html);
  return rc;
}

static int render_content_entry_finalize_paths(struct ContentEntry* entry,
                                               const struct SiteConfig* site_config,
                                               const char* source_path,
                                               struct RenderJob* result) {
  const char* relative =
      render_content_entry_finalize_paths_relative(source_path, site_config->content_dir, result);
  if (relative == NULL) {
    return -1;
  }

  const char* section = render_content_entry_finalize_paths_section(relative, &entry->arena);
  if (section == NULL) {
    render_job_set_error(result, "out of memory deriving section for '%s'", source_path);
    return -1;
  }

  const char* url_path =
      permalink_expand(site_config->permalink, section, entry->slug, &entry->arena);
  if (url_path == NULL) {
    render_job_set_error(result, "out of memory building output path for '%s'", source_path);
    return -1;
  }
  return render_content_entry_finalize_paths_output(entry, site_config, url_path, source_path,
                                                    result);
}

static const char* render_content_entry_finalize_paths_relative(const char* source_path,
                                                                const char* content_dir,
                                                                struct RenderJob* result) {
  // The `<content_dir>/` prefix is a precondition this pass checks rather than assumes. Every
  // production caller satisfies it, because `fs_list_files_with_suffix` roots its walk at
  // `content_dir` and builds each path down from there, and `site_config_load` has already trimmed
  // any trailing `/`. The function refuses a source from outside that root. Otherwise, the whole
  // path would become the section, so the content directory's own name would land in the entry's
  // URL. The build would still report success. The `&&` short circuit keeps the byte read in
  // bounds, because a matching `strncmp` proves `content_dir_len` bytes are there.
  const size_t content_dir_len = strlen(content_dir);
  if (strncmp(source_path, content_dir, content_dir_len) != 0 ||
      source_path[content_dir_len] != '/') {
    // This deliberately does not name the configured root as well. It is unbounded, so a second
    // value could truncate away the reason, and `load_build_inputs` omits it from its own
    // diagnostics for the same reason. `sosig config` prints the resolved value.
    render_job_set_error(result, "content source must be under the configured 'content_dir': '%s'",
                         source_path);
    return NULL;
  }
  return source_path + content_dir_len + 1;
}

static char* render_content_entry_finalize_paths_section(const char* source_relative_path,
                                                         struct Arena* arena) {
  const char* last_slash = strrchr(source_relative_path, '/');
  if (last_slash == NULL) {
    return arena_strdup(arena, "");
  }

  struct SlugSegments segments;
  if (slugify_segments(source_relative_path, (size_t)(last_slash - source_relative_path), arena,
                       &segments) != 0) {
    return NULL;
  }
  return join_segments(&segments, arena);
}

static int slugify_segments(const char* text,
                            size_t text_len,
                            struct Arena* arena,
                            struct SlugSegments* segments_out) {
  size_t segment_count = 1;
  for (size_t i = 0; i < text_len; i++) {
    if (text[i] == '/') {
      segment_count++;
    }
  }
  const char** items = arena_calloc(arena, segment_count, sizeof(*items));
  size_t* item_lens = arena_calloc(arena, segment_count, sizeof(*item_lens));
  if (items == NULL || item_lens == NULL) {
    return -1;
  }

  size_t joined_len = 0;
  size_t index = 0;
  size_t segment_start = 0;
  // Run one index past `text_len` so the final segment, which has no separator after it within the
  // range, is flushed by the same branch as the others. The `i < text_len` half of the guard below
  // keeps the character test from reading at the sentinel index.
  for (size_t i = 0; i <= text_len; i++) {
    if (i < text_len && text[i] != '/') {
      continue;
    }
    // `text_slugify` writes the length it just computed, so `item_lens` and the running total below
    // are the same measurement rather than a second scan of the bytes.
    items[index] = text_slugify(text + segment_start, i - segment_start, arena, &item_lens[index]);
    if (items[index] == NULL) {
      return -1;
    }
    joined_len += item_lens[index];
    index++;
    segment_start = i + 1;
  }

  // This publishes only once every segment is slugified, so the struct never claims a count backed
  // by a partly filled array.
  *segments_out = (struct SlugSegments){
      .items = items, .item_lens = item_lens, .count = segment_count, .joined_len = joined_len};
  return 0;
}

static char* join_segments(const struct SlugSegments* segments, struct Arena* arena) {
  // One `/` between segments plus the terminator is exactly `count` bytes beyond the payload, so
  // the allocation is exact and has no slack. That is safe because `joined_len` was summed from the
  // same `item_lens` the loop below reads.
  char* joined = arena_alloc(arena, segments->joined_len + segments->count);
  if (joined == NULL) {
    return NULL;
  }
  size_t len = 0;
  for (size_t i = 0; i < segments->count; i++) {
    if (i > 0) {
      joined[len++] = '/';
    }
    memcpy(joined + len, segments->items[i], segments->item_lens[i]);
    len += segments->item_lens[i];
  }
  joined[len] = '\0';
  return joined;
}

static int render_content_entry_finalize_paths_output(struct ContentEntry* entry,
                                                      const struct SiteConfig* site_config,
                                                      const char* url_path,
                                                      const char* source_path,
                                                      struct RenderJob* result) {
  const char* relative_path = url_path + 1;
  if (!path_is_safe_relative(relative_path)) {
    // Same ordering rule as the length check below, for the same reason. `relative_path` is not yet
    // bounded on this branch, so naming it first would truncate away the permalink and the source
    // that identify which pattern and which file produced it.
    render_job_set_error(result, "permalink expanded to an unsafe output path for '%s': '%s'",
                         source_path, relative_path);
    return -1;
  }
  // Every message below leads with the limit and the measured length, then the entry, then the
  // generated value. A value longer than the diagnostic buffer must cost its own tail rather than
  // the reason that names the limit. The entry is the more actionable of the two values.
  //
  // This leaves `metrics` uninitialized deliberately. `path_check_output_limits` fills every field
  // whatever verdict it returns, so the fields read below are never an indeterminate value.
  struct PathOutputMetrics metrics;
  switch (path_check_output_limits(relative_path, &metrics)) {
    case PATH_OUTPUT_OK:
      break;
    case PATH_OUTPUT_TOO_LONG:
      render_job_set_error(
          result,
          "output path exceeds max output path length (%zu bytes) at %zu bytes for '%s': '%s'",
          (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, metrics.len, source_path, relative_path);
      return -1;
    case PATH_OUTPUT_SEGMENT_TOO_LONG:
      render_job_set_error(result,
                           "output path segment exceeds max filename length (%zu bytes) at %zu "
                           "bytes for '%s': '%.*s'",
                           (size_t)FILENAME_LEN_MAX, metrics.segment_len, source_path,
                           (int)metrics.segment_len, metrics.segment);
      return -1;
  }

  entry->url_path = url_path;
  entry->output_path = path_join(site_config->output_dir, relative_path, &entry->arena);
  if (entry->output_path == NULL) {
    // Same wording as the `permalink_expand` failure above. Both are the output path failing to be
    // built, and the user can act on neither differently.
    render_job_set_error(result, "out of memory building output path for '%s'", source_path);
    return -1;
  }
  return 0;
}

static void render_content_entry_free_source(struct ContentEntryRenderSource* source) {
  free(source->markdown);
  source->markdown = NULL;
}
