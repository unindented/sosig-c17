#include "build/entry_renderer.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/job.h"
#include "build/output_path.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "domain/frontmatter.h"
#include "domain/permalink.h"
#include "domain/site_config.h"
#include "formats/markdown.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** Source bytes and frontmatter slices for one content entry parse job. */
struct ContentEntryRenderSource {
  /** Complete Markdown source file contents, in the job's scratch. */
  struct Str markdown;

  /** Borrowed slices into `markdown` after frontmatter splitting. */
  struct FrontmatterSplit split;
};

/**
 * Worker context shared by all content entry parse jobs. Every field is read-only to a job, except
 * `source_entries`, where each job writes its own slot.
 */
struct EntryRenderContext {
  /** Site configuration shared by every parse job. */
  const struct SiteConfig* site_config;

  /** Content entry source paths shared by every parse job. */
  const struct PathList* source_paths;

  /** Largest source a job reads, in bytes. */
  ptrdiff_t source_len_max;

  /** Host every job reads its source from. */
  struct Os* os;

  /** One entry slot per source path. A job writes only `source_entries[index]`. */
  struct ContentEntry** source_entries;
};

/**
 * @brief Parses one content entry source path into its entry slot.
 *
 * This is the `JobFn` of the parse phase, so it runs concurrently with other indexes and writes
 * only `source_entries[index]`, its own log, and its worker's arenas. It builds the entry in a copy
 * of the worker's `perm` and commits that copy only when it publishes the entry, so a draft or a
 * failed parse leaves nothing behind. Its temporaries go into a copy of the worker's scratch.
 *
 * @param index    Source path index this job handles. Must be below the source path count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives every failure. Must not be `NULL`.
 * @param userdata `struct EntryRenderContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or a skipped draft, or `-1` after recording the failure.
 */
static int render_content_entry_job(ptrdiff_t index,
                                    struct JobArenas* arenas,
                                    struct Log* log,
                                    void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Reads a content entry source file and parses its frontmatter into the entry.
 *
 * @param entry          Entry that receives the parsed metadata. Must not be `NULL`.
 * @param source_path    Path of the source file to read.
 * @param source_len_max Largest source file accepted, in bytes.
 * @param os             Host the file is read from. Must not be `NULL`.
 * @param perm           Arena that owns the parsed metadata. Must not be `NULL`.
 * @param source_arena   Arena that owns the file bytes, which the caller keeps until the body is
 *                       converted. The parse takes its temporaries from the free space after them.
 *                       Must not be `NULL`.
 * @param source_out     Receives the file bytes and frontmatter/body slices. Must not be `NULL`.
 * @param log            Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read, split, or parse failure.
 */
static int render_content_entry_load_source(struct ContentEntry* entry,
                                            struct Str source_path,
                                            ptrdiff_t source_len_max,
                                            struct Os* os,
                                            struct Arena* perm,
                                            struct Arena* source_arena,
                                            struct ContentEntryRenderSource* source_out,
                                            struct Log* log)
    __attribute__((nonnull(1, 4, 5, 6, 7, 8)));

/**
 * @brief Computes the public URL path and filesystem output path by expanding the permalink.
 *
 * Expands `site_config->permalink` with the entry slug and the source's slugified section, then
 * length-checks the resulting relative path before rooting it in `output_dir`.
 *
 * @param entry       Entry that receives `url_path` and `output_path`. Must not be `NULL`.
 * @param site_config Configuration supplying `permalink`, `content_dir`, and `output_dir`. Must not
 *                    be `NULL`.
 * @param source_path Source path used to derive the section and for failure diagnostics. Must be
 *                    spelled with `site_config->content_dir` as its literal prefix.
 * @param perm        Arena that owns the entry's paths. Must not be `NULL`.
 * @param scratch     Arena for the section and its slugified segments.
 * @param log         Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on an oversize path.
 */
static int render_content_entry_finalize_paths(struct ContentEntry* entry,
                                               const struct SiteConfig* site_config,
                                               struct Str source_path,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Log* log)
    __attribute__((nonnull(1, 2, 4, 6)));

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
 * @param source_relative_path Source Markdown path with the `<content_dir>/` prefix removed.
 * @param arena                Arena that owns the returned section string. Must not be `NULL`.
 * @return Section owned by `arena`.
 */
static struct Str render_content_entry_finalize_paths_section(struct Str source_relative_path,
                                                              struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Roots an expanded permalink URL under the output directory as the entry's output path.
 *
 * Strips the leading `/` from the URL, checks the remainder against the output-path length limits,
 * then joins it under `output_dir`. The path is safe by construction: `site_config_load` accepts
 * only a permalink whose every expansion is safe, the slug is a safe identifier, and each section
 * segment is a slug.
 *
 * @param entry       Entry that receives `url_path` and `output_path`. Must not be `NULL`.
 * @param site_config Configuration supplying `output_dir`, and `permalink` for diagnostics. Must
 *                    not be `NULL`.
 * @param url_path    Expanded permalink URL with a leading `/`, owned by `perm`. The entry stores
 *                    it without copying.
 * @param source_path Source path used in failure diagnostics.
 * @param perm        Arena that owns the output path. Must not be `NULL`.
 * @param log         Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on an oversize path.
 */
static int render_content_entry_finalize_paths_output(struct ContentEntry* entry,
                                                      const struct SiteConfig* site_config,
                                                      struct Str url_path,
                                                      struct Str source_path,
                                                      struct Arena* perm,
                                                      struct Log* log)
    __attribute__((nonnull(1, 2, 5, 6)));

int entry_renderer_render_entries(const struct SiteConfig* site_config,
                                  const struct PathList* source_paths,
                                  ptrdiff_t source_len_max,
                                  ptrdiff_t worker_count,
                                  struct JobArenas* worker_arenas,
                                  bool is_verbose,
                                  struct Os* os,
                                  struct Arena scratch,
                                  struct ContentEntry** source_entries,
                                  struct Log* log) {
  // A source within this limit is a body `markdown_to_html` accepts.
  assert(source_len_max <= CONTENT_FILE_LEN_MAX);
  struct EntryRenderContext context = {
      .site_config = site_config,
      .source_paths = source_paths,
      .source_len_max = source_len_max,
      .os = os,
      .source_entries = source_entries,
  };
  return job_run(source_paths->count, worker_count, worker_arenas, render_content_entry_job,
                 &context, S("parsing content"), (struct Str){0}, is_verbose, os, scratch, log);
}

static int render_content_entry_job(ptrdiff_t index,
                                    struct JobArenas* arenas,
                                    struct Log* log,
                                    void* userdata) {
  const struct EntryRenderContext* context = userdata;
  const struct Str source_path = context->source_paths->items[index];
  memory_oom_set_job(source_path, log);
  struct Arena perm = arenas->perm;
  struct Arena scratch = arenas->scratch;

  struct ContentEntry* entry = arena_new(&perm, 1, struct ContentEntry);
  struct ContentEntryRenderSource source = {0};
  if (render_content_entry_load_source(entry, source_path, context->source_len_max, context->os,
                                       &perm, &scratch, &source, log) != 0) {
    return -1;
  }
  if (entry->is_draft) {
    return 0;
  }
  // The HTML grows in place at the end of `perm`, which nothing else allocates from while the
  // converter runs, so a large body is built without a copy. The read capped the file at
  // `CONTENT_FILE_LEN_MAX`, the converter's input limit.
  entry->body_html = markdown_to_html(source.split.body, &perm);
  // The entry keeps copies of what it needs. The worker's scratch outlives the job, so a large
  // source's pages are given back.
  if (source.markdown.len >= JOB_SOURCE_DISCARD_LEN_MIN) {
    arena_discard(&scratch, source.markdown.data, source.markdown.len);
  }
  if (render_content_entry_finalize_paths(entry, context->site_config, source_path, &perm, scratch,
                                          log) != 0) {
    return -1;
  }

  context->source_entries[index] = entry;
  arenas->perm = perm;
  return 0;
}

static int render_content_entry_load_source(struct ContentEntry* entry,
                                            struct Str source_path,
                                            ptrdiff_t source_len_max,
                                            struct Os* os,
                                            struct Arena* perm,
                                            struct Arena* source_arena,
                                            struct ContentEntryRenderSource* source_out,
                                            struct Log* log) {
  if (os_read(os, source_path, source_len_max, source_arena, &source_out->markdown,
              S("failed to read content"), log) != 0) {
    return -1;
  }
  // Both name the source in each error they record, so nothing is added here.
  if (frontmatter_split(source_out->markdown, source_path, &source_out->split, log) != 0) {
    return -1;
  }
  return frontmatter_parse(entry, source_out->split.frontmatter, source_path, perm, *source_arena,
                           log);
}

static int render_content_entry_finalize_paths(struct ContentEntry* entry,
                                               const struct SiteConfig* site_config,
                                               struct Str source_path,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Log* log) {
  // The walk roots every source at `content_dir`, whose trailing `/` config load trims.
  struct Str relative = {0};
  const bool is_below = path_is_below(source_path, site_config->content_dir, &relative);
  assert(is_below);
  (void)is_below;
  // The section only feeds the expansion, so it stays in scratch, and the URL the entry keeps goes
  // into `perm`.
  const struct Str section = render_content_entry_finalize_paths_section(relative, &scratch);
  const struct Str url_path = permalink_expand(site_config->permalink, section, entry->slug, perm);
  return render_content_entry_finalize_paths_output(entry, site_config, url_path, source_path, perm,
                                                    log);
}

static struct Str render_content_entry_finalize_paths_section(struct Str source_relative_path,
                                                              struct Arena* arena) {
  // Every `/`-terminated segment is a directory, and the final, unterminated one is the filename.
  struct Buf section = {.arena = arena};
  for (struct StrCut cut = str_cut(source_relative_path, '/'); cut.is_found;
       cut = str_cut(cut.tail, '/')) {
    if (section.len > 0) {
      buf_append_char(&section, '/');
    }
    buf_append(&section, text_slugify(cut.head, arena));
  }
  return buf_str(&section);
}

static int render_content_entry_finalize_paths_output(struct ContentEntry* entry,
                                                      const struct SiteConfig* site_config,
                                                      struct Str url_path,
                                                      struct Str source_path,
                                                      struct Arena* perm,
                                                      struct Log* log) {
  const struct Str relative_path = str_tail(url_path, 1);
  assert(path_is_safe_relative(relative_path));
  // Every output path producer reports a limit failure through `output_path_check_limits`, so a
  // content entry and a configured template over a limit read alike.
  if (output_path_check_limits(relative_path, source_path, log) != 0) {
    return -1;
  }

  entry->url_path = url_path;
  entry->output_path = path_join(site_config->output_dir, relative_path, perm);
  return 0;
}
