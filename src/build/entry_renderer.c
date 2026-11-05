#include "build/entry_renderer.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "build/job.h"
#include "build/output_path.h"
#include "core/ascii.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "domain/frontmatter.h"
#include "domain/permalink.h"
#include "domain/site_config.h"
#include "formats/body_html.h"
#include "formats/markdown.h"
#include "formats/org.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * Worker context shared by all content entry parse jobs. Every field is read-only to a job, except
 * `source_entries` and `draft_flags`, where each job writes its own slot.
 */
struct EntryParseContext {
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

  /** One draft flag per source path. A job writes only `draft_flags[index]`. */
  bool* draft_flags;
};

/** One file that a denote identifier in its name can name: a source or an asset. */
struct DenoteFile {
  /** Identifier from the file's name, which borrows its path. */
  struct Str identifier;

  /** Index of the file in `source_paths`, or in `asset_paths` when `is_asset`. */
  ptrdiff_t index;

  /** Whether the file is an asset rather than a content source. */
  bool is_asset;
};

/**
 * Worker context shared by all body conversion jobs. Every field is read-only to a job, except the
 * entry in its own `source_entries` slot.
 */
struct BodyConvertContext {
  /** Site configuration, whose `content_dir` every path is below. */
  const struct SiteConfig* site_config;

  /** Content entry source paths, sorted. */
  const struct PathList* source_paths;

  /** One entry slot per source path, `NULL` for a draft and a failed parse. */
  struct ContentEntry* const* source_entries;

  /** One draft flag per source path. */
  const bool* draft_flags;

  /** Every other file below `content_dir`. */
  const struct PathList* asset_paths;

  /**
   * Every file whose name holds a denote identifier, sorted by identifier, then with sources before
   * assets, then by path.
   */
  const struct DenoteFile* denote_files;
  ptrdiff_t denote_file_count;
};

/** Links of the body one job converts, which `write_content_link` receives as its userdata. */
struct BodyLinks {
  /** Context shared by every conversion job. */
  const struct BodyConvertContext* context;

  /** Entry whose body is converted, whose URL each link is relative to. */
  const struct ContentEntry* entry;

  /** Whether the body is Org rather than Markdown, which decides how a link names its file. */
  bool is_org;

  /** Log of the job, which receives each link error. */
  struct Log* log;

  /** Whether a link named another content source. */
  bool has_entry_link;

  /** Whether a link named an asset. */
  bool has_asset_link;
};

/** A link target that names a content file, by its path or by its denote identifier. */
struct ContentLink {
  /** Path as written, or the denote identifier. */
  struct Str name;

  /** Fragment the URL ends with, `#` included, or empty. */
  struct Str fragment;

  /** Whether `name` is a denote identifier rather than a path. */
  bool is_denote;

  /** Whether the target names a content file at all. */
  bool is_found;
};

/**
 * @brief Parses one content entry source path into its entry slot.
 *
 * This is the `JobFn` of the parse phase, so it runs concurrently with other indexes and writes
 * only `source_entries[index]`, `draft_flags[index]`, its own log, and its worker's arenas. It
 * builds the entry and keeps its source in a copy of the worker's `perm`, and commits that copy
 * only when it publishes the entry, so a draft or a failed parse leaves nothing behind. Its
 * temporaries go into a copy of the worker's scratch.
 *
 * @param index    Source path index this job handles. Must be below the source path count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives every failure. Must not be `NULL`.
 * @param userdata `struct EntryParseContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or a skipped draft, or `-1` after recording the failure.
 */
static int parse_content_entry_job(ptrdiff_t index,
                                   struct JobArenas* arenas,
                                   struct Log* log,
                                   void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Stats and reads a content entry source file and parses its frontmatter or Org keywords
 *        into the entry.
 *
 * @param entry          Entry that receives the source's stat and the parsed metadata. Must not be
 *                       `NULL`.
 * @param site_config    Configuration supplying `frontmatter_ignored_keys`. Must not be `NULL`.
 * @param source_path    Path of the source file to read.
 * @param source_len_max Largest source file accepted, in bytes.
 * @param os             Host the file is read from. Must not be `NULL`.
 * @param perm           Arena that owns the file bytes and the parsed metadata. Must not be `NULL`.
 * @param scratch        Arena for the parse's temporaries.
 * @param body_out       Receives the body, the part of the file bytes that the converter reads: the
 *                       file after its frontmatter, or the whole Org file. Must not be `NULL`.
 * @param log            Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read, split, or parse failure.
 */
static int parse_content_entry_load_source(struct ContentEntry* entry,
                                           const struct SiteConfig* site_config,
                                           struct Str source_path,
                                           ptrdiff_t source_len_max,
                                           struct Os* os,
                                           struct Arena* perm,
                                           struct Arena scratch,
                                           struct Str* body_out,
                                           struct Log* log)
    __attribute__((nonnull(1, 2, 5, 6, 8, 9)));

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
static int parse_content_entry_finalize_paths(struct ContentEntry* entry,
                                              const struct SiteConfig* site_config,
                                              struct Str source_path,
                                              struct Arena* perm,
                                              struct Arena scratch,
                                              struct Log* log) __attribute__((nonnull(1, 2, 4, 6)));

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
 * @param source_relative_path Content source path with the `<content_dir>/` prefix removed.
 * @param arena                Arena that owns the returned section string. Must not be `NULL`.
 * @return Section owned by `arena`.
 */
static struct Str parse_content_entry_finalize_paths_section(struct Str source_relative_path,
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
static int parse_content_entry_finalize_paths_output(struct ContentEntry* entry,
                                                     const struct SiteConfig* site_config,
                                                     struct Str url_path,
                                                     struct Str source_path,
                                                     struct Arena* perm,
                                                     struct Log* log)
    __attribute__((nonnull(1, 2, 5, 6)));

/**
 * @brief Lists every source and asset whose name holds a denote identifier, sorted for a binary
 *        search.
 *
 * @param source_paths Content entry source paths. Must not be `NULL`.
 * @param asset_paths  Every other file below `content_dir`. Must not be `NULL`.
 * @param arena        Arena that owns the list. Must not be `NULL`.
 * @param count_out    Receives the number of files in the list. Must not be `NULL`.
 * @return The list, sorted by identifier, then with sources before assets, then by path.
 */
static struct DenoteFile* entry_renderer_convert_bodies_denote_files(
    const struct PathList* source_paths,
    const struct PathList* asset_paths,
    struct Arena* arena,
    ptrdiff_t* count_out) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Returns the denote identifier in a file's name, as denote reads it.
 *
 * @param path File path.
 * @return A leading `YYYYMMDDTHHMMSS`, or else the text after the first `@@` up to the next `==`,
 *         `--`, `__`, `@@` or `.`, borrowed from `path`, or empty when the name holds neither.
 */
static struct Str denote_identifier(struct Str path);

/**
 * @brief Compares two `struct DenoteFile` items for `qsort`.
 *
 * @param a Pointer to the first item. Must not be `NULL`.
 * @param b Pointer to the second item. Must not be `NULL`.
 * @return A negative, zero, or positive value: by identifier, then sources before assets, then by
 *         index.
 */
static int compare_denote_files(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Converts the body of the entry in one slot.
 *
 * This is the `JobFn` of the conversion phase, so it runs concurrently with other indexes and
 * writes only the body and link flags of `source_entries[index]`, its own log, and its worker's
 * arenas. The HTML grows at the end of its worker's `perm`, which keeps it.
 *
 * @param index    Source path index this job handles. Must be below the source path count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives every link error. Must not be `NULL`.
 * @param userdata `struct BodyConvertContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or for an empty slot, or `-1` when a link failed.
 */
static int convert_content_body_job(ptrdiff_t index,
                                    struct JobArenas* arenas,
                                    struct Log* log,
                                    void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Appends the URL of the content file that a link names, relative to the linking page, and
 *        declines every other link and every image. This is the `LinkTargetWriteFn` the converters
 *        call.
 *
 * @param buffer         Buffer the HTML grows in. Must not be `NULL`.
 * @param type           Org link type, or empty for a Markdown link.
 * @param target         Target as written.
 * @param is_image       Whether the target is an image's `src`.
 * @param is_written_out Receives whether a URL was appended. Must not be `NULL`.
 * @param userdata       `struct BodyLinks*` of the job. Must not be `NULL`.
 * @return `0` after appending the URL or declining, or `-1` with nothing appended after reporting a
 *         link that names no file or a draft, or when the link names a source whose parse failed.
 */
static int write_content_link(struct Buf* buffer,
                              struct Str type,
                              struct Str target,
                              bool is_image,
                              bool* is_written_out,
                              void* userdata) __attribute__((nonnull(1, 5, 6)));

/**
 * @brief Finds the content file a link target names, as the rules of its format read it.
 *
 * In Org, a `file` link names a path, and a `fuzzy` link to `denote:` names an identifier, each up
 * to a `::` search option, whose `::#custom-id` form gives the fragment. In Markdown, a destination
 * to `denote:` names an identifier in the same way, and a destination without a scheme names a path
 * up to its `#fragment`. A path names a content file only when it ends in `.md` or `.org`, in any
 * letter case, and does not start with `/`.
 *
 * @param is_org Whether the link is Org rather than Markdown.
 * @param type   Org link type, or empty for a Markdown link.
 * @param target Target as written.
 * @return The file's name and the fragment, with `is_found` unset when the target names none.
 */
static struct ContentLink write_content_link_parse(bool is_org, struct Str type, struct Str target);

/**
 * @brief Splits a link at its `::` search option, keeping a `::#custom-id` as a fragment.
 *
 * @param link Path or denote identifier, then any search option.
 * @return The part before the first `::` as `name`, and `#custom-id` or empty as `fragment`.
 */
static struct ContentLink split_search_option(struct Str link);

/**
 * @brief Reports whether a link destination starts with a URL scheme, such as `https:`.
 *
 * @param target Link destination as written.
 * @return `true` when a letter, then letters, digits, `+`, `-` or `.`, then `:` start it, `false`
 *         otherwise.
 */
static bool has_url_scheme(struct Str target);

/**
 * @brief Reports whether an org4c link type is `name`, in any letter case, as org4c compares it.
 *
 * @param type Link type.
 * @param name Lowercase type to compare with.
 * @return `true` when the two match, `false` otherwise.
 */
static bool is_type_named(struct Str type, struct Str name);

/**
 * @brief Appends the URL of the source that a path link names.
 *
 * @param links  Links of the job's body. Must not be `NULL`.
 * @param buffer Buffer the HTML grows in, past whose end the source's path is looked for. Must not
 *               be `NULL`.
 * @param target Path as written, relative to the directory of the linking source.
 * @return `0` after appending the URL, or `-1` with nothing appended, as for `write_content_link`.
 */
static int write_content_link_path(struct BodyLinks* links, struct Buf* buffer, struct Str target)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends the path that a path link names, below `content_dir`, with its `.` and `..`
 *        segments resolved.
 *
 * @param buffer      Buffer the path is appended to. Must not be `NULL`.
 * @param content_dir Directory the sources are below.
 * @param source_path Path of the linking source, below `content_dir`.
 * @param target      Path as written, relative to the directory of `source_path`.
 * @return `0` after appending the path, or `-1`, with part of it appended, when a `..` segment
 *         leaves `content_dir`.
 */
static int write_content_link_path_resolved(struct Buf* buffer,
                                            struct Str content_dir,
                                            struct Str source_path,
                                            struct Str target) __attribute__((nonnull(1)));

/**
 * @brief Appends the URL of the file that a denote link names.
 *
 * @param links  Links of the job's body. Must not be `NULL`.
 * @param buffer Buffer the HTML grows in. Must not be `NULL`.
 * @param target Denote identifier.
 * @return `0` after appending the URL, or `-1` with nothing appended, as for `write_content_link`.
 */
static int write_content_link_denote(struct BodyLinks* links, struct Buf* buffer, struct Str target)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends the URL of a source's page, or reports why the page cannot be linked.
 *
 * @param links     Links of the job's body. Must not be `NULL`.
 * @param buffer    Buffer the HTML grows in. Must not be `NULL`.
 * @param index     Index of the source in `source_paths`.
 * @param link_kind Kind of link named in a diagnostic, `link` or `denote link`. Must not be `NULL`.
 * @param target    Target of the link, named in a diagnostic.
 * @return `0` after appending the URL, or `-1` with nothing appended when the source is a draft,
 *         which is reported, or its parse failed.
 */
static int write_source_page_url(struct BodyLinks* links,
                                 struct Buf* buffer,
                                 ptrdiff_t index,
                                 const char* link_kind,
                                 struct Str target) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Finds a path in a sorted path list.
 *
 * @param paths Path list, sorted by `str_compare`. Must not be `NULL`.
 * @param path  Path to find.
 * @return The index of `path`, or `-1` when the list does not hold it.
 */
static ptrdiff_t find_path(const struct PathList* paths, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Appends the relative URL from one page to another output, from the root of the site.
 *
 * @param buffer    Buffer the URL is appended to, escaped. Must not be `NULL`.
 * @param from_path Path of the linking page, without its leading `/`.
 * @param to_path   Path of the output the link points to, without a leading `/`.
 */
static void write_relative_url(struct Buf* buffer, struct Str from_path, struct Str to_path)
    __attribute__((nonnull(1)));

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
                                 struct Log* log) {
  // A source within this limit is a body either converter accepts.
  assert(source_len_max <= CONTENT_FILE_LEN_MAX);
  struct EntryParseContext context = {
      .site_config = site_config,
      .source_paths = source_paths,
      .source_len_max = source_len_max,
      .os = os,
      .source_entries = source_entries,
      .draft_flags = draft_flags,
  };
  return job_run(source_paths->count, worker_count, worker_arenas, parse_content_entry_job,
                 &context, S("parsing content"), (struct Str){0}, is_verbose, os, scratch, log);
}

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
                                  struct Log* log) {
  struct BodyConvertContext context = {
      .site_config = site_config,
      .source_paths = source_paths,
      .source_entries = source_entries,
      .draft_flags = draft_flags,
      .asset_paths = asset_paths,
  };
  // The list lives in the phase's scratch, and the jobs' logs go after it.
  context.denote_files = entry_renderer_convert_bodies_denote_files(
      source_paths, asset_paths, &scratch, &context.denote_file_count);
  return job_run(source_paths->count, worker_count, worker_arenas, convert_content_body_job,
                 &context, S("converting bodies"), (struct Str){0}, is_verbose, os, scratch, log);
}

static int parse_content_entry_job(ptrdiff_t index,
                                   struct JobArenas* arenas,
                                   struct Log* log,
                                   void* userdata) {
  const struct EntryParseContext* context = userdata;
  const struct Str source_path = context->source_paths->items[index];
  memory_oom_set_job(source_path, log);
  struct Arena perm = arenas->perm;

  struct ContentEntry* entry = arena_new(&perm, 1, struct ContentEntry);
  // The body stays in `perm` until the conversion phase reads it, once every entry has its URL.
  if (parse_content_entry_load_source(entry, context->site_config, source_path,
                                      context->source_len_max, context->os, &perm, arenas->scratch,
                                      &entry->body_source, log) != 0) {
    return -1;
  }
  if (entry->is_draft) {
    context->draft_flags[index] = true;
    return 0;
  }
  if (parse_content_entry_finalize_paths(entry, context->site_config, source_path, &perm,
                                         arenas->scratch, log) != 0) {
    return -1;
  }

  context->source_entries[index] = entry;
  arenas->perm = perm;
  return 0;
}

static int parse_content_entry_load_source(struct ContentEntry* entry,
                                           const struct SiteConfig* site_config,
                                           struct Str source_path,
                                           ptrdiff_t source_len_max,
                                           struct Os* os,
                                           struct Arena* perm,
                                           struct Arena scratch,
                                           struct Str* body_out,
                                           struct Log* log) {
  // The stat comes first, so an edit made during the read changes the stat the build cache records
  // and the next build sees it. A source that cannot be inspected fails the read below.
  (void)os_stat(os, source_path, &entry->source_stat);
  struct Str text = {0};
  if (os_read(os, source_path, source_len_max, perm, &text, S("failed to read content"), log) !=
      0) {
    return -1;
  }
  // `cmd_build.c`'s `load_build_inputs_split_sources` keeps only `.md` and `.org`, in any letter
  // case, so a source that is not Org is Markdown. Every parse below names the source in each error
  // it records, so nothing is added here.
  if (text_has_suffix_folded(source_path, S(".org"))) {
    // Org keywords and a top property drawer render nothing, so the whole file is the body and
    // nothing is cut out of it.
    *body_out = text;
    return frontmatter_parse_org(entry, text, source_path, perm, scratch, log);
  }
  struct FrontmatterSplit split = {0};
  if (frontmatter_split(text, source_path, &split, log) != 0) {
    return -1;
  }
  *body_out = split.body;
  return frontmatter_parse(entry, split.frontmatter, source_path,
                           site_config->frontmatter_ignored_keys,
                           site_config->frontmatter_ignored_key_count, perm, scratch, log);
}

static int parse_content_entry_finalize_paths(struct ContentEntry* entry,
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
  const struct Str section = parse_content_entry_finalize_paths_section(relative, &scratch);
  const struct Str url_path = permalink_expand(site_config->permalink, section, entry->slug, perm);
  return parse_content_entry_finalize_paths_output(entry, site_config, url_path, source_path, perm,
                                                   log);
}

static struct Str parse_content_entry_finalize_paths_section(struct Str source_relative_path,
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

static int parse_content_entry_finalize_paths_output(struct ContentEntry* entry,
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

static struct DenoteFile* entry_renderer_convert_bodies_denote_files(
    const struct PathList* source_paths,
    const struct PathList* asset_paths,
    struct Arena* arena,
    ptrdiff_t* count_out) {
  struct DenoteFile* files =
      arena_new(arena, source_paths->count + asset_paths->count, struct DenoteFile);
  ptrdiff_t count = 0;
  const struct PathList* lists[] = {source_paths, asset_paths};
  for (ptrdiff_t list = 0; list < 2; list++) {
    for (ptrdiff_t i = 0; i < lists[list]->count; i++) {
      const struct Str identifier = denote_identifier(lists[list]->items[i]);
      if (identifier.len > 0) {
        files[count++] =
            (struct DenoteFile){.identifier = identifier, .index = i, .is_asset = list > 0};
      }
    }
  }
  // An empty list's pointer must not reach `qsort`, which is undefined for a null pointer even with
  // a count of 0.
  if (count > 0) {
    qsort(files, (size_t)count, sizeof(*files), compare_denote_files);
  }
  *count_out = count;
  return files;
}

static struct Str denote_identifier(struct Str path) {
  enum { DATE_IDENTIFIER_LEN = 15 };
  const struct Str name = path_basename(path);
  bool is_date = name.len >= DATE_IDENTIFIER_LEN;
  for (ptrdiff_t i = 0; is_date && i < DATE_IDENTIFIER_LEN; i++) {
    is_date = i == 8 ? name.data[i] == 'T' : ascii_is_digit((unsigned char)name.data[i]);
  }
  if (is_date) {
    return str_head(name, DATE_IDENTIFIER_LEN);
  }
  // The `@@` field ends at a delimiter after at least one byte, and its first byte is not a `.`.
  static const struct Str delimiters[] = {STR_INIT("=="), STR_INIT("--"), STR_INIT("__"),
                                          STR_INIT("@@"), STR_INIT(".")};
  for (ptrdiff_t start = 0; start + 2 < name.len; start++) {
    if (!str_startswith(str_tail(name, start), S("@@")) || name.data[start + 2] == '.') {
      continue;
    }
    const struct Str field = str_tail(name, start + 2);
    ptrdiff_t len = 1;
    bool is_end = false;
    while (len < field.len && !is_end) {
      for (ptrdiff_t i = 0; !is_end && i < (ptrdiff_t)(sizeof(delimiters) / sizeof(delimiters[0]));
           i++) {
        is_end = str_startswith(str_tail(field, len), delimiters[i]);
      }
      len += is_end ? 0 : 1;
    }
    return str_head(field, len);
  }
  return (struct Str){0};
}

static int compare_denote_files(const void* a, const void* b) {
  const struct DenoteFile* left = a;
  const struct DenoteFile* right = b;
  const int order = str_compare(left->identifier, right->identifier);
  if (order != 0) {
    return order;
  }
  if (left->is_asset != right->is_asset) {
    return left->is_asset ? 1 : -1;
  }
  return (left->index > right->index) - (left->index < right->index);
}

static int convert_content_body_job(ptrdiff_t index,
                                    struct JobArenas* arenas,
                                    struct Log* log,
                                    void* userdata) {
  const struct BodyConvertContext* context = userdata;
  struct ContentEntry* entry = context->source_entries[index];
  if (entry == NULL) {
    return 0;
  }
  memory_oom_set_job(entry->source_path, log);
  // The parse phase picked the converter's format from the same suffix.
  struct BodyLinks links = {
      .context = context,
      .entry = entry,
      .is_org = text_has_suffix_folded(entry->source_path, S(".org")),
      .log = log,
  };
  const struct LinkTargets link_targets = {.write = write_content_link, .userdata = &links};
  // The HTML grows in place at the end of `perm`, which nothing else allocates from while the
  // converter runs, so a large body is built without a copy. The read capped the file at
  // `CONTENT_FILE_LEN_MAX`, the input limit of both converters.
  entry->body_html =
      links.is_org
          ? org_to_html(entry->body_source, &link_targets, &arenas->perm, arenas->scratch)
          : markdown_to_html(entry->body_source, &link_targets, &arenas->perm, arenas->scratch);
  entry->has_entry_link = links.has_entry_link;
  entry->has_asset_link = links.has_asset_link;
  return log->errors > 0 ? -1 : 0;
}

static int write_content_link(struct Buf* buffer,
                              struct Str type,
                              struct Str target,
                              bool is_image,
                              bool* is_written_out,
                              void* userdata) {
  struct BodyLinks* links = userdata;
  *is_written_out = false;
  // An image keeps the path as written, which the copy of an asset keeps too.
  struct ContentLink link = {0};
  if (!is_image) {
    link = write_content_link_parse(links->is_org, type, target);
  }
  if (!link.is_found) {
    return 0;
  }
  if ((link.is_denote ? write_content_link_denote(links, buffer, link.name)
                      : write_content_link_path(links, buffer, link.name)) != 0) {
    return -1;
  }
  body_html_url_escape_append(buffer, link.fragment);
  *is_written_out = true;
  return 0;
}

static struct ContentLink write_content_link_parse(bool is_org,
                                                   struct Str type,
                                                   struct Str target) {
  if ((!is_org || is_type_named(type, S("fuzzy"))) && str_startswith(target, S("denote:"))) {
    struct ContentLink link = split_search_option(str_tail(target, S("denote:").len));
    link.is_denote = true;
    link.is_found = true;
    return link;
  }
  struct ContentLink link = {0};
  if (is_org && is_type_named(type, S("file"))) {
    link = split_search_option(target);
  } else if (!is_org && !has_url_scheme(target)) {
    const struct StrCut fragment = str_cut(target, '#');
    link.name = fragment.head;
    link.fragment = str_tail(target, fragment.head.len);
  } else {
    return link;
  }
  // A path to any other file, or from the root, keeps the link as written, which the copy of an
  // asset matches.
  link.is_found =
      !str_startswith(link.name, S("/")) &&
      (text_has_suffix_folded(link.name, S(".md")) || text_has_suffix_folded(link.name, S(".org")));
  return link;
}

static struct ContentLink split_search_option(struct Str link) {
  ptrdiff_t name_len = 0;
  while (name_len < link.len && !str_startswith(str_tail(link, name_len), S("::"))) {
    name_len++;
  }
  // The render does not see the other file's headlines, so only a custom id survives.
  const struct Str option = name_len < link.len ? str_tail(link, name_len + 2) : (struct Str){0};
  return (struct ContentLink){
      .name = str_head(link, name_len),
      .fragment = str_startswith(option, S("#")) ? option : (struct Str){0},
  };
}

static bool has_url_scheme(struct Str target) {
  if (target.len == 0 || ascii_is_digit((unsigned char)target.data[0]) ||
      !ascii_is_alphanumeric((unsigned char)target.data[0])) {
    return false;
  }
  for (ptrdiff_t i = 1; i < target.len; i++) {
    const char c = target.data[i];
    if (c == ':') {
      return true;
    }
    if (!ascii_is_alphanumeric((unsigned char)c) && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return false;
}

static bool is_type_named(struct Str type, struct Str name) {
  return type.len == name.len && text_has_suffix_folded(type, name);
}

static int write_content_link_path(struct BodyLinks* links, struct Buf* buffer, struct Str target) {
  const struct BodyConvertContext* context = links->context;
  const struct Str source_path = links->entry->source_path;
  // The converter's arena belongs to the HTML while it runs, so the path is built past the HTML's
  // end, and dropped before the URL is written there.
  const ptrdiff_t start = buffer->len;
  ptrdiff_t index = -1;
  if (write_content_link_path_resolved(buffer, context->site_config->content_dir, source_path,
                                       target) == 0) {
    index =
        find_path(context->source_paths, (struct Str){buffer->data + start, buffer->len - start});
  }
  buffer->len = start;
  if (index < 0) {
    return log_error_in(links->log, source_path, "link '%.*s' names no content file",
                        STR_FMT(target));
  }
  return write_source_page_url(links, buffer, index, "link", target);
}

static int write_content_link_path_resolved(struct Buf* buffer,
                                            struct Str content_dir,
                                            struct Str source_path,
                                            struct Str target) {
  struct Str source_relative = {0};
  const bool is_below = path_is_below(source_path, content_dir, &source_relative);
  assert(is_below);
  (void)is_below;
  buf_append(buffer, content_dir);
  const ptrdiff_t root_len = buffer->len;
  // The segments are the linking source's directory, then the link's.
  const struct Str parts[] = {path_parent(source_relative), target};
  for (ptrdiff_t part = 0; part < 2; part++) {
    struct StrCut cut = {.tail = parts[part], .is_found = parts[part].len > 0};
    while (cut.is_found) {
      cut = str_cut(cut.tail, '/');
      if (cut.head.len == 0 || str_eq(cut.head, S("."))) {
        continue;
      }
      if (!str_eq(cut.head, S(".."))) {
        buf_append_char(buffer, '/');
        buf_append(buffer, cut.head);
        continue;
      }
      if (buffer->len == root_len) {
        return -1;
      }
      while (buffer->data[buffer->len - 1] != '/') {
        buffer->len--;
      }
      buffer->len--;
    }
  }
  return 0;
}

static int write_content_link_denote(struct BodyLinks* links,
                                     struct Buf* buffer,
                                     struct Str target) {
  const struct BodyConvertContext* context = links->context;
  // The first file with the identifier is the one the link names.
  ptrdiff_t low = 0;
  ptrdiff_t high = context->denote_file_count;
  while (low < high) {
    const ptrdiff_t middle = low + (high - low) / 2;
    if (str_compare(context->denote_files[middle].identifier, target) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low == context->denote_file_count || !str_eq(context->denote_files[low].identifier, target)) {
    return log_error_in(links->log, links->entry->source_path, "denote link '%.*s' names no file",
                        STR_FMT(target));
  }
  const struct DenoteFile* file = &context->denote_files[low];
  if (!file->is_asset) {
    return write_source_page_url(links, buffer, file->index, "denote link", target);
  }
  // An asset's copy is at its path below `content_dir`.
  struct Str asset_relative = {0};
  (void)path_is_below(context->asset_paths->items[file->index], context->site_config->content_dir,
                      &asset_relative);
  write_relative_url(buffer, str_tail(links->entry->url_path, 1), asset_relative);
  links->has_asset_link = true;
  return 0;
}

static int write_source_page_url(struct BodyLinks* links,
                                 struct Buf* buffer,
                                 ptrdiff_t index,
                                 const char* link_kind,
                                 struct Str target) {
  const struct BodyConvertContext* context = links->context;
  if (context->draft_flags[index]) {
    const struct Str draft_path = context->source_paths->items[index];
    return log_error_in(links->log, links->entry->source_path, "%s '%.*s' names draft '%.*s'",
                        link_kind, STR_FMT(target), STR_FMT(draft_path));
  }
  // A source whose parse failed has reported its own error, which fails the build.
  const struct ContentEntry* target_entry = context->source_entries[index];
  if (target_entry == NULL) {
    return -1;
  }
  write_relative_url(buffer, str_tail(links->entry->url_path, 1),
                     str_tail(target_entry->url_path, 1));
  links->has_entry_link = true;
  return 0;
}

static ptrdiff_t find_path(const struct PathList* paths, struct Str path) {
  ptrdiff_t low = 0;
  ptrdiff_t high = paths->count;
  while (low < high) {
    const ptrdiff_t middle = low + (high - low) / 2;
    const int order = str_compare(paths->items[middle], path);
    if (order == 0) {
      return middle;
    }
    if (order < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return -1;
}

static void write_relative_url(struct Buf* buffer, struct Str from_path, struct Str to_path) {
  // The two paths share the directories up to the last `/` of their common prefix.
  ptrdiff_t common_len = 0;
  for (ptrdiff_t i = 0;
       i < from_path.len && i < to_path.len && from_path.data[i] == to_path.data[i]; i++) {
    if (from_path.data[i] == '/') {
      common_len = i + 1;
    }
  }
  for (ptrdiff_t i = common_len; i < from_path.len; i++) {
    if (from_path.data[i] == '/') {
      buf_append(buffer, S("../"));
    }
  }
  body_html_url_escape_append(buffer, str_tail(to_path, common_len));
}
