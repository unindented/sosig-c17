#ifndef SOSIG_BUILD_CACHE_H
#define SOSIG_BUILD_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/str.h"

struct Arena;
struct Buf;
struct Log;
struct OsStat;
struct SiteConfig;

/**
 * Format of the build cache's rows, in its file name. Bump it whenever the row format or the
 * meaning of a field changes, so a build never opens a file written in another format, and the
 * stale-output prune removes the old one.
 */
#define BUILD_CACHE_VERSION 1

/**
 * Version of how a page is made from its inputs, in the page recipe. Bump it whenever a change to
 * the code, or to a vendored renderer (md4c, mustache4c, tomlc17), means that an output its inputs
 * still match must be made again. A release changes the generator string, which the page recipe
 * also holds, so a missed bump only reuses outputs between releases, but that is the one way this
 * cache produces wrong output silently.
 */
#define PAGE_RECIPE_VERSION 1

/** Largest build cache a build reads, in bytes. A larger file is an empty build cache. */
#define BUILD_CACHE_FILE_LEN_MAX ((ptrdiff_t)64 * 1024 * 1024)

/** What a build cache row records the stat of. Rows sort by kind first, in this order. */
enum BuildCacheKind {
  /** A content entry's source, named by its page's output path relative to `output_dir`. */
  BUILD_CACHE_KIND_CONTENT,

  /** A file below `templates_dir`, named by its path relative to it. */
  BUILD_CACHE_KIND_TEMPLATE,
};

/** The stat of one input the pages depend on, as the build that wrote the row saw it. */
struct BuildCacheEntry {
  /** What the row names. */
  enum BuildCacheKind kind;

  /** Output path of the page, or path of the template, as `kind` says. */
  struct Str name;

  /** Size of the input in bytes. */
  ptrdiff_t len;

  /** Modification time of the input, in whole seconds. */
  int64_t mtime_s;

  /** Nanoseconds within `mtime_s`. */
  int64_t mtime_ns;

  /**
   * For a content row, whether the page read the entry list, `content_entries` or `site.updated`,
   * so that it changes when any other entry does. `false` for a template row.
   */
  bool is_entry_list_read;
};

/** The rows of one build cache, sorted by kind and then bytewise by name. A zeroed one is empty. */
struct BuildCache {
  /** Rows, sorted by `kind` and then bytewise by `name`, with no row twice. */
  struct BuildCacheEntry* entries;

  /** Number of rows in `entries`. */
  ptrdiff_t count;
};

/**
 * @brief Returns the path of the build cache below `output_dir`, `_sosig/v<n>-build-cache.tsv`,
 *        where `<n>` is `BUILD_CACHE_VERSION`.
 *
 * @param output_dir Output directory the build cache lives in.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path, owned by `arena`.
 */
struct Str build_cache_path(struct Str output_dir, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Parses a build cache, rejecting a first line other than the page recipe of `site_config`.
 *
 * The page recipe is what an output depends on besides its input files: the configuration a render
 * reads, the generator string, and `PAGE_RECIPE_VERSION`, each as `name=value`, joined by tabs.
 *
 * Every row, the last included, ends in a newline, and the rows are sorted by kind and then by
 * name, with none twice. An empty file, which a build leaves until it has written every page, is
 * reported as such.
 *
 * @param text        Bytes of the build cache.
 * @param site_config Configuration of this build, for its page recipe. Must not be `NULL`.
 * @param perm        Arena that owns the rows. Must not be `NULL`.
 * @param cache_out   Receives the rows on success. Written only on success. Must not be `NULL`.
 * @param log         Log that receives the reason the build cache cannot be used. Must not be
 *                    `NULL`.
 * @return `0` on success, or `-1` when the file is empty, the first line differs, or a row does not
 *         parse.
 */
int build_cache_parse(struct Str text,
                      const struct SiteConfig* site_config,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) __attribute__((nonnull(2, 3, 4, 5)));

/**
 * @brief Finds the row of `kind` named `name` whose size and modification time match `stat`.
 *
 * @param cache Build cache to search. Must not be `NULL`.
 * @param kind  Kind of the row.
 * @param name  Name of the row.
 * @param stat  What `os_stat` reported for the input now. Must not be `NULL`.
 * @return The matching row, or `NULL` when there is none or the input changed since.
 */
const struct BuildCacheEntry* build_cache_find(const struct BuildCache* cache,
                                               enum BuildCacheKind kind,
                                               struct Str name,
                                               const struct OsStat* stat)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Appends a whole build cache: the page recipe, then the rows, sorted in place first.
 *
 * A row whose name holds a tab or a newline is left out, so what depends on its input is made on
 * every build, and no name needs escaping.
 *
 * @param buffer      Buffer that receives the text. Must not be `NULL`.
 * @param site_config Configuration of the build, for the page recipe. Must not be `NULL`.
 * @param cache       Rows to append, each name once, which this sorts. Must not be `NULL`.
 */
void build_cache_format(struct Buf* buffer,
                        const struct SiteConfig* site_config,
                        struct BuildCache* cache) __attribute__((nonnull(1, 2, 3)));

#endif
