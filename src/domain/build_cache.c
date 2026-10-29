#include "domain/build_cache.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/parse.h"
#include "core/path.h"
#include "core/sosig_version.h"
#include "domain/site_config.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** Nanoseconds in a second, the bound of a row's `mtime_ns`. */
enum { BUILD_CACHE_NS_PER_S = 1000000000 };

/** First field of each row, by `enum BuildCacheKind`. */
static const struct Str BUILD_CACHE_KIND_NAMES[] = {
    [BUILD_CACHE_KIND_CONTENT] = STR_INIT("content"),
    [BUILD_CACHE_KIND_TEMPLATE] = STR_INIT("template"),
};

/**
 * @brief Appends the page recipe, the first line of a build cache, with its newline.
 *
 * @param buffer      Buffer that receives the line. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 */
static void format_recipe(struct Buf* buffer, const struct SiteConfig* site_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends a list of names to the page recipe, joined by commas.
 *
 * @param buffer Buffer that receives the list. Must not be `NULL`.
 * @param names  Array of `count` names. May be `NULL` only when `count` is 0.
 * @param count  Number of names.
 */
static void format_recipe_list(struct Buf* buffer, const struct Str* names, ptrdiff_t count)
    __attribute__((nonnull(1)));

/**
 * @brief Appends one row of a build cache, or nothing for a name that holds a tab or a newline.
 *
 * @param buffer Buffer that receives the row. Must not be `NULL`.
 * @param entry  Row to append. Must not be `NULL`.
 */
static void build_cache_format_entry(struct Buf* buffer, const struct BuildCacheEntry* entry)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Parses one row of a build cache, without its newline.
 *
 * @param line      Row to parse.
 * @param entry_out Receives the row, whose name points into `line`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the row has another kind, the wrong number of fields for its
 *         kind, or a field out of range.
 */
static int parse_entry(struct Str line, struct BuildCacheEntry* entry_out)
    __attribute__((nonnull(2)));

/**
 * @brief Splits the next tab-separated field off `rest`.
 *
 * @param rest      Text the field is taken from, which receives what follows its tab. Must not be
 *                  `NULL`.
 * @param field_out Receives the field. Must not be `NULL`.
 * @return `true` when a field was taken, or `false` when `rest` was already used up.
 */
static bool take_field(struct StrCut* rest, struct Str* field_out) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two rows by kind and then bytewise by name, for `bsearch`.
 *
 * @param a First `struct BuildCacheEntry`. Must not be `NULL`.
 * @param b Second `struct BuildCacheEntry`. Must not be `NULL`.
 * @return A negative, zero, or positive value as `a` orders before, equal to, or after `b`.
 */
static int compare_entries(const void* a, const void* b) __attribute__((nonnull(1, 2)));

struct Str build_cache_path(struct Str output_dir, struct Arena* arena) {
  struct Buf name = {.arena = arena};
  buf_printf(&name, "_sosig/v%d-build-cache.tsv", BUILD_CACHE_VERSION);
  return path_join(output_dir, buf_str(&name), arena);
}

int build_cache_parse(struct Str text,
                      const struct SiteConfig* site_config,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) {
  if (text.len == 0) {
    return log_error(log, "build cache is empty, as a build that did not finish leaves it");
  }
  struct Arena arena = *perm;
  struct Arena recipe_scratch = arena;
  struct Buf recipe = {.arena = &recipe_scratch};
  format_recipe(&recipe, site_config);
  if (!str_startswith(text, buf_str(&recipe))) {
    return log_error(log, "build cache was written for another page recipe");
  }
  const struct Str rows = str_tail(text, buf_str(&recipe).len);
  ptrdiff_t row_count = 0;
  for (ptrdiff_t i = 0; i < rows.len; i++) {
    row_count += rows.data[i] == '\n';
  }
  struct BuildCache cache = {.entries = arena_new(&arena, row_count, struct BuildCacheEntry)};
  // A file cut short by an interrupted write ends without its last newline, which is the reason
  // every row must end in one.
  struct StrCut cut = str_cut(rows, '\n');
  for (; cut.is_found; cut = str_cut(cut.tail, '\n')) {
    struct BuildCacheEntry* entry = &cache.entries[cache.count];
    if (parse_entry(cut.head, entry) != 0 ||
        (cache.count > 0 && compare_entries(&cache.entries[cache.count - 1], entry) >= 0)) {
      return log_error(log, "build cache does not parse at line %td", cache.count + 2);
    }
    entry->name = str_clone(entry->name, &arena);
    cache.count++;
  }
  if (cut.head.len > 0) {
    return log_error(log, "build cache does not parse at line %td", cache.count + 2);
  }
  *perm = arena;
  *cache_out = cache;
  return 0;
}

const struct BuildCacheEntry* build_cache_find(const struct BuildCache* cache,
                                               enum BuildCacheKind kind,
                                               struct Str name,
                                               const struct OsStat* stat) {
  if (cache->count == 0) {
    return NULL;
  }
  const struct BuildCacheEntry key = {.kind = kind, .name = name};
  const struct BuildCacheEntry* entry =
      bsearch(&key, cache->entries, (size_t)cache->count, sizeof(*cache->entries), compare_entries);
  if (entry == NULL || entry->len != stat->len || entry->mtime_s != stat->mtime_s ||
      entry->mtime_ns != stat->mtime_ns) {
    return NULL;
  }
  return entry;
}

void build_cache_format(struct Buf* buffer,
                        const struct SiteConfig* site_config,
                        struct BuildCache* cache) {
  format_recipe(buffer, site_config);
  if (cache->count > 0) {
    qsort(cache->entries, (size_t)cache->count, sizeof(*cache->entries), compare_entries);
  }
  for (ptrdiff_t i = 0; i < cache->count; i++) {
    build_cache_format_entry(buffer, &cache->entries[i]);
  }
}

static void format_recipe(struct Buf* buffer, const struct SiteConfig* site_config) {
  const struct Str generator = sosig_generator_string();
  buf_printf(buffer,
             "base_url=%.*s\ttitle=%.*s\tauthor=%.*s\tpermalink=%.*s\tcontent_template=%.*s\t"
             "aggregate_templates=",
             STR_FMT(site_config->base_url), STR_FMT(site_config->title),
             STR_FMT(site_config->author), STR_FMT(site_config->permalink),
             STR_FMT(site_config->content_template));
  format_recipe_list(buffer, site_config->aggregate_templates,
                     site_config->aggregate_template_count);
  buf_append(buffer, S("\tfeed_templates="));
  format_recipe_list(buffer, site_config->feed_templates, site_config->feed_template_count);
  buf_printf(buffer, "\tfeed_count=%td\tgenerator=%.*s\tpage_recipe_version=%d\n",
             site_config->feed_count, STR_FMT(generator), PAGE_RECIPE_VERSION);
}

static void format_recipe_list(struct Buf* buffer, const struct Str* names, ptrdiff_t count) {
  for (ptrdiff_t i = 0; i < count; i++) {
    if (i > 0) {
      buf_append_char(buffer, ',');
    }
    buf_append(buffer, names[i]);
  }
}

static void build_cache_format_entry(struct Buf* buffer, const struct BuildCacheEntry* entry) {
  for (ptrdiff_t i = 0; i < entry->name.len; i++) {
    if (entry->name.data[i] == '\t' || entry->name.data[i] == '\n') {
      return;
    }
  }
  const struct Str kind = BUILD_CACHE_KIND_NAMES[entry->kind];
  buf_printf(buffer, "%.*s\t%.*s\t%td\t%" PRId64 "\t%" PRId64, STR_FMT(kind), STR_FMT(entry->name),
             entry->len, entry->mtime_s, entry->mtime_ns);
  if (entry->kind == BUILD_CACHE_KIND_CONTENT) {
    buf_printf(buffer, "\t%d", entry->is_entry_list_read ? 1 : 0);
  }
  buf_append_char(buffer, '\n');
}

static int parse_entry(struct Str line, struct BuildCacheEntry* entry_out) {
  struct StrCut rest = {.tail = line, .is_found = true};
  struct Str kind_name = {0};
  struct BuildCacheEntry entry = {0};
  if (!take_field(&rest, &kind_name) || !take_field(&rest, &entry.name) || entry.name.len == 0) {
    return -1;
  }
  if (str_eq(kind_name, BUILD_CACHE_KIND_NAMES[BUILD_CACHE_KIND_CONTENT])) {
    entry.kind = BUILD_CACHE_KIND_CONTENT;
  } else if (str_eq(kind_name, BUILD_CACHE_KIND_NAMES[BUILD_CACHE_KIND_TEMPLATE])) {
    entry.kind = BUILD_CACHE_KIND_TEMPLATE;
  } else {
    return -1;
  }
  ptrdiff_t values[3] = {0};
  for (ptrdiff_t i = 0; i < 3; i++) {
    struct Str field = {0};
    if (!take_field(&rest, &field) || parse_size(field, &values[i]) != 0) {
      return -1;
    }
  }
  if (values[2] >= BUILD_CACHE_NS_PER_S) {
    return -1;
  }
  if (entry.kind == BUILD_CACHE_KIND_CONTENT) {
    struct Str flag = {0};
    if (!take_field(&rest, &flag) || !(str_eq(flag, S("0")) || str_eq(flag, S("1")))) {
      return -1;
    }
    entry.is_entry_list_read = str_eq(flag, S("1"));
  }
  if (rest.is_found) {
    return -1;
  }
  entry.len = values[0];
  entry.mtime_s = values[1];
  entry.mtime_ns = values[2];
  *entry_out = entry;
  return 0;
}

static bool take_field(struct StrCut* rest, struct Str* field_out) {
  if (!rest->is_found) {
    return false;
  }
  *rest = str_cut(rest->tail, '\t');
  *field_out = rest->head;
  return true;
}

static int compare_entries(const void* a, const void* b) {
  const struct BuildCacheEntry* left = a;
  const struct BuildCacheEntry* right = b;
  if (left->kind != right->kind) {
    return left->kind < right->kind ? -1 : 1;
  }
  return str_compare(left->name, right->name);
}
