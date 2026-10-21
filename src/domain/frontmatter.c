#include "domain/frontmatter.h"

#include <stdbool.h>
#include <stddef.h>
#include <tomlc17.h>

#include "core/path.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "formats/toml.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/** UTF-8 byte order mark, tolerated as an optional prefix before the opening frontmatter fence. */
#define UTF8_BOM S("\xEF\xBB\xBF")

/** Fence that delimits the TOML frontmatter block at the start and end. */
#define FRONTMATTER_FENCE S("+++")

/**
 * Every key the frontmatter table may hold. The parse rejects a key outside this list, so a
 * misspelled key cannot silently leave its default in place. A key a `frontmatter_parse_metadata_`
 * step reads must be listed here.
 */
static const struct Str frontmatter_keys[] = {
    STR_INIT("title"), STR_INIT("date"),  STR_INIT("description"), STR_INIT("slug"),
    STR_INIT("tags"),  STR_INIT("draft"), STR_INIT("template"),
};

/**
 * @brief Reports whether a line is exactly a fence, ignoring a CRLF line ending's `\r`.
 *
 * @param line Line bytes, without the trailing `\n`.
 * @return `true` when the line is a fence, `false` otherwise.
 */
static bool is_frontmatter_fence_line(struct Str line);

/**
 * @brief Parses frontmatter bytes as a standalone TOML document, behind a synthesized blank first
 *        line.
 *
 * On success `parsed_out` owns the parse result and the caller must run `toml_free` exactly once.
 * On failure it has nothing to release.
 *
 * @param frontmatter Frontmatter TOML bytes, shorter than `INT_MAX` bytes so the copy tomlc17
 *                    parses, one byte longer, fits the `int` length it takes.
 * @param source_path Source file path, named in every error. The parser's error text carries only a
 *                    line number, which counts from the start of the file, not from the start of
 *                    the frontmatter, so it reads correctly next to the path.
 * @param scratch     Arena for the copy tomlc17 parses.
 * @param parsed_out  Receives the parsed TOML result on success. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a TOML parse error.
 */
static int frontmatter_parse_toml(struct Str frontmatter,
                                  struct Str source_path,
                                  struct Arena scratch,
                                  toml_result_t* parsed_out,
                                  struct Log* log) __attribute__((nonnull(4, 5)));

/**
 * @brief Populates every entry metadata field from the parsed frontmatter table.
 *
 * Checks every field even after one fails, so a file with several mistakes reports all of them.
 *
 * @param entry       Entry that receives the metadata. Must not be `NULL`.
 * @param table       Parsed frontmatter top-level table.
 * @param source_path Source file path, used for slug fallback and named in every error.
 * @param perm        Arena that owns every value copied onto the entry. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when any key is unknown or any field fails validation.
 */
static int frontmatter_parse_metadata(struct ContentEntry* entry,
                                      toml_datum_t table,
                                      struct Str source_path,
                                      struct Arena* perm,
                                      struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Copies the required title.
 *
 * @param entry       Entry that receives `title`. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` if the title is missing or mistyped.
 */
static int frontmatter_parse_metadata_title(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            struct Str source_path,
                                            struct Arena* perm,
                                            struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Reads the required date and records both its display and sortable forms.
 *
 * @param entry       Entry that receives `date` and `date_epoch`. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` if the date is missing or mistyped.
 */
static int frontmatter_parse_metadata_date(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           struct Str source_path,
                                           struct Arena* perm,
                                           struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Copies the optional description string when present.
 *
 * @param entry       Entry that receives the description. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type.
 */
static int frontmatter_parse_metadata_description(struct ContentEntry* entry,
                                                  toml_datum_t table,
                                                  struct Str source_path,
                                                  struct Arena* perm,
                                                  struct Log* log)
    __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Uses an explicit slug or derives one from the source filename.
 *
 * This slugifies both explicit and derived slugs. A slug becomes a single path component, so this
 * normalizes an explicit one rather than trusting it as a path. `/` and `..` must not survive into
 * the output path.
 *
 * @param entry       Entry that receives the slug. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file path used to derive a fallback slug, and named in the error.
 * @param perm        Arena that owns the slug. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a wrong-typed slug.
 */
static int frontmatter_parse_metadata_slug(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           struct Str source_path,
                                           struct Arena* perm,
                                           struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Copies the optional tags array into `perm`.
 *
 * @param entry       Entry that receives `tags` and `tag_count`. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type.
 */
static int frontmatter_parse_metadata_tags(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           struct Str source_path,
                                           struct Arena* perm,
                                           struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Applies the optional draft flag after type-checking it.
 *
 * @param entry       Entry that receives `is_draft`. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type.
 */
static int frontmatter_parse_metadata_draft(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            struct Str source_path,
                                            struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Applies an optional template override after validating it is a safe relative name.
 *
 * @param entry       Entry that receives `template`. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type or unsafe name.
 */
static int frontmatter_parse_metadata_template(struct ContentEntry* entry,
                                               toml_datum_t table,
                                               struct Str source_path,
                                               struct Arena* perm,
                                               struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Requires a TOML string key and copies it into arena-owned storage.
 *
 * @param table       Parsed TOML table.
 * @param key         Terminated key that must be present and string-typed. Must not be `NULL`.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the copy on success. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` if the key is missing or mistyped.
 */
static int require_string(toml_datum_t table,
                          const char* key,
                          struct Str source_path,
                          struct Arena* arena,
                          struct Str* value_out,
                          struct Log* log) __attribute__((nonnull(2, 4, 5, 6)));

/**
 * @brief Validates a present TOML string datum and returns a view of its bytes.
 *
 * The single boundary where a frontmatter string becomes a value: it rejects a non-string datum and
 * one carrying an embedded `NUL` (see `toml_datum_is_text`), so callers only decide whether the key
 * was required. The view borrows the parse result, so a caller that keeps it clones it.
 *
 * @param value       Datum to validate. Must be present, not `TOML_UNKNOWN`.
 * @param key         Terminated key name used in diagnostics. Must not be `NULL`.
 * @param source_path Source file the frontmatter came from, named in the error.
 * @param value_out   Receives the borrowed view on success. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` if the datum is mistyped or holds a `NUL`.
 */
static int view_string_value(toml_datum_t value,
                             const char* key,
                             struct Str source_path,
                             struct Str* value_out,
                             struct Log* log) __attribute__((nonnull(2, 4, 5)));

int frontmatter_split(struct Str markdown,
                      struct Str source_path,
                      struct FrontmatterSplit* split_out,
                      struct Log* log) {
  struct Str rest = markdown;
  if (str_startswith(rest, UTF8_BOM)) {
    rest = str_tail(rest, UTF8_BOM.len);
  }
  struct StrCut line = str_cut(rest, '\n');
  if (!is_frontmatter_fence_line(line.head)) {
    return log_error_in(log, source_path, "missing opening '%.*s' frontmatter fence",
                        STR_FMT(FRONTMATTER_FENCE));
  }
  const struct Str frontmatter = line.tail;
  rest = line.tail;
  while (rest.len > 0) {
    line = str_cut(rest, '\n');
    if (is_frontmatter_fence_line(line.head)) {
      split_out->frontmatter = str_head(frontmatter, rest.data - frontmatter.data);
      split_out->body = line.tail;
      return 0;
    }
    rest = line.tail;
  }
  return log_error_in(log, source_path, "missing closing '%.*s' frontmatter fence",
                      STR_FMT(FRONTMATTER_FENCE));
}

int frontmatter_parse(struct ContentEntry* entry,
                      struct Str frontmatter,
                      struct Str source_path,
                      struct Arena* perm,
                      struct Arena scratch,
                      struct Log* log) {
  // This is zero-initialized although every path reaching `toml_free` writes it. `toml_free` takes
  // the struct by value, and without this clang-analyzer cannot prove across the call boundary that
  // its fields are set.
  toml_result_t parsed = {0};
  if (frontmatter_parse_toml(frontmatter, source_path, scratch, &parsed, log) != 0) {
    return -1;
  }

  const int rc = frontmatter_parse_metadata(entry, parsed.toptab, source_path, perm, log);
  // The populate step copies every value it keeps into `perm`, so `frontmatter_parse` can release
  // the parse result here and the entry holds no pointer into it.
  toml_free(parsed);
  return rc;
}

static bool is_frontmatter_fence_line(struct Str line) {
  if (str_endswith(line, S("\r"))) {
    line = str_head(line, line.len - 1);
  }
  return str_eq(line, FRONTMATTER_FENCE);
}

static int frontmatter_parse_toml(struct Str frontmatter,
                                  struct Str source_path,
                                  struct Arena scratch,
                                  toml_result_t* parsed_out,
                                  struct Log* log) {
  // A synthesized blank first line stands in for the opening fence that `frontmatter_split` already
  // consumed, so every line number tomlc17 reports counts from the start of the file rather than
  // from the start of the slice. Without it each one is exactly one too low. The wrapping caller
  // names the file, so the user reads the number as a file line and lands on the wrong one. A blank
  // line is valid TOML and parses to nothing.
  //
  // This shifts what the parser counts rather than rewriting its message. tomlc17 reports a line
  // number in two shapes, the `(line N)` prefix and an `on line N` clause inside some messages.
  // Both come from the one counter this newline advances. Patching the prefix would leave the
  // others wrong.
  const struct Str document = str_concat(S("\n"), frontmatter, &scratch);
  return toml_parse_document(document, source_path, parsed_out, S("failed to parse frontmatter"),
                             log);
}

// The key order matches the frontmatter documented in `README.md`.
static int frontmatter_parse_metadata(struct ContentEntry* entry,
                                      toml_datum_t table,
                                      struct Str source_path,
                                      struct Arena* perm,
                                      struct Log* log) {
  int rc = toml_require_known_keys(table, frontmatter_keys,
                                   sizeof(frontmatter_keys) / sizeof(frontmatter_keys[0]),
                                   S("frontmatter"), S(""), source_path, log);
  entry->source_path = str_clone(source_path, perm);
  if (frontmatter_parse_metadata_title(entry, table, source_path, perm, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_metadata_date(entry, table, source_path, perm, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_metadata_description(entry, table, source_path, perm, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_metadata_slug(entry, table, source_path, perm, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_metadata_tags(entry, table, source_path, perm, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_metadata_draft(entry, table, source_path, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_metadata_template(entry, table, source_path, perm, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int frontmatter_parse_metadata_title(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            struct Str source_path,
                                            struct Arena* perm,
                                            struct Log* log) {
  return require_string(table, "title", source_path, perm, &entry->title, log);
}

static int frontmatter_parse_metadata_date(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           struct Str source_path,
                                           struct Arena* perm,
                                           struct Log* log) {
  toml_datum_t date = toml_get(table, "date");
  if (date.type == TOML_UNKNOWN) {
    return log_error_in(log, source_path, "missing required frontmatter key 'date'");
  }
  if (toml_datum_to_epoch(date, &entry->date_epoch) != 0) {
    return log_error_in(log, source_path, "frontmatter key 'date' must be a TOML date/datetime");
  }
  // The epoch conversion has just accepted the datum's type, so the format cannot refuse it.
  entry->date = toml_datum_format_rfc3339(date, perm);
  return 0;
}

static int frontmatter_parse_metadata_description(struct ContentEntry* entry,
                                                  toml_datum_t table,
                                                  struct Str source_path,
                                                  struct Arena* perm,
                                                  struct Log* log) {
  toml_datum_t description = toml_get(table, "description");
  if (description.type == TOML_UNKNOWN) {
    return 0;
  }
  struct Str value = {0};
  if (view_string_value(description, "description", source_path, &value, log) != 0) {
    return -1;
  }
  entry->description = str_clone(value, perm);
  return 0;
}

static int frontmatter_parse_metadata_slug(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           struct Str source_path,
                                           struct Arena* perm,
                                           struct Log* log) {
  toml_datum_t slug = toml_get(table, "slug");
  struct Str slug_source = {0};
  if (slug.type != TOML_UNKNOWN) {
    // The shared boundary type- and `NUL`-checks an explicit slug like every other frontmatter
    // string. `text_slugify` copies it, so the view need not outlive the parse result.
    if (view_string_value(slug, "slug", source_path, &slug_source, log) != 0) {
      return -1;
    }
  } else {
    slug_source = path_basename_without_extension(source_path);
  }

  entry->slug = text_slugify(slug_source, perm);
  return 0;
}

static int frontmatter_parse_metadata_tags(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           struct Str source_path,
                                           struct Arena* perm,
                                           struct Log* log) {
  toml_datum_t tags = toml_get(table, "tags");
  if (tags.type == TOML_UNKNOWN) {
    return 0;
  }
  if (tags.type != TOML_ARRAY) {
    return log_error_in(log, source_path, "frontmatter key 'tags' must be an array");
  }

  const ptrdiff_t tag_count = tags.u.arr.size;
  // An empty tag list still gets a non-`NULL` array, because a zero-count allocation returns a
  // pointer.
  struct Str* items = arena_new(perm, tag_count, struct Str);
  for (ptrdiff_t i = 0; i < tag_count; i++) {
    if (tags.u.arr.elem[i].type != TOML_STRING) {
      return log_error_in(log, source_path, "frontmatter key 'tags' must contain only strings");
    }
    if (view_string_value(tags.u.arr.elem[i], "tags", source_path, &items[i], log) != 0) {
      return -1;
    }
    items[i] = str_clone(items[i], perm);
  }

  // This publishes both fields only once it validates every element, so the entry never claims a
  // count backed by a partially filled array. `copy_template_names` publishes on the same
  // discipline.
  entry->tags = items;
  entry->tag_count = tag_count;
  return 0;
}

static int frontmatter_parse_metadata_draft(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            struct Str source_path,
                                            struct Log* log) {
  toml_datum_t draft = toml_get(table, "draft");
  if (draft.type == TOML_UNKNOWN) {
    return 0;
  }
  if (draft.type != TOML_BOOLEAN) {
    return log_error_in(log, source_path, "frontmatter key 'draft' must be a boolean");
  }
  entry->is_draft = draft.u.boolean;
  return 0;
}

static int frontmatter_parse_metadata_template(struct ContentEntry* entry,
                                               toml_datum_t table,
                                               struct Str source_path,
                                               struct Arena* perm,
                                               struct Log* log) {
  toml_datum_t template = toml_get(table, "template");
  if (template.type == TOML_UNKNOWN) {
    return 0;
  }
  struct Str value = {0};
  if (view_string_value(template, "template", source_path, &value, log) != 0) {
    return -1;
  }
  if (!path_is_safe_relative(value)) {
    return log_error_in(log, source_path,
                        "frontmatter key 'template' must be a safe relative template name: '%.*s'",
                        STR_FMT(value));
  }
  entry->template = str_clone(value, perm);
  return 0;
}

static int require_string(toml_datum_t table,
                          const char* key,
                          struct Str source_path,
                          struct Arena* arena,
                          struct Str* value_out,
                          struct Log* log) {
  toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return log_error_in(log, source_path, "missing required frontmatter key '%s'", key);
  }
  struct Str text = {0};
  if (view_string_value(value, key, source_path, &text, log) != 0) {
    return -1;
  }
  *value_out = str_clone(text, arena);
  return 0;
}

static int view_string_value(toml_datum_t value,
                             const char* key,
                             struct Str source_path,
                             struct Str* value_out,
                             struct Log* log) {
  // This is reachable for every caller but `tags`, whose loop pre-checks the element type so it can
  // name the array in the diagnostic.
  if (value.type != TOML_STRING) {
    return log_error_in(log, source_path, "frontmatter key '%s' must be a string", key);
  }
  if (!toml_datum_is_text(value)) {
    return log_error_in(log, source_path, "frontmatter key '%s' must not contain a NUL byte", key);
  }
  *value_out = toml_datum_str(value);
  return 0;
}
