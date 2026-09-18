#include "domain/frontmatter.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc17.h>

#include "core/error.h"
#include "core/path.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "formats/toml.h"
#include "shared/arena.h"

/** UTF-8 byte order mark, tolerated as an optional prefix before the opening frontmatter fence. */
static const char UTF8_BOM[] = "\xEF\xBB\xBF";

/** Fence that delimits the TOML frontmatter block at the start and end. */
static const char FRONTMATTER_FENCE[] = "+++";

/**
 * @brief Returns the length of the line starting at `text`, including its trailing newline.
 *
 * It clamps the measure to `text_len`, so an unterminated final line measures to the end of the
 * available bytes. That lets a scan advance by the returned length and land exactly on the end
 * without passing it, keeping any remaining-byte subtraction from wrapping.
 *
 * @param text     Bytes to measure, starting at the line's first byte. Must not be `NULL`.
 * @param text_len Number of bytes available from `text`.
 * @return Line length in bytes including any trailing newline, never more than `text_len`, and at
 *         least 1 unless `text_len` is 0.
 */
static size_t line_length(const char* text, size_t text_len) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a line is exactly a fence, ignoring the line ending.
 *
 * Accepts either a Unix or CRLF line ending after the fence.
 *
 * @param line     Line bytes, including any trailing newline. Must not be `NULL`.
 * @param line_len Length of the line in bytes.
 * @return `true` when the line is a fence, `false` otherwise.
 */
static bool is_frontmatter_fence_line(const char* line, size_t line_len)
    __attribute__((nonnull(1)));

/**
 * @brief Parses frontmatter bytes as a standalone, terminated TOML document.
 *
 * On success `parsed_out` owns the parse result and the caller must run `toml_free` exactly once.
 * On failure it has nothing to release. An oversize or allocation failure never writes it. A parse
 * failure leaves a result that has already released everything it owned, so a caller that frees on
 * that path neither leaks nor double-frees.
 *
 * @param frontmatter     Frontmatter TOML bytes. Must not be `NULL`.
 * @param frontmatter_len Number of frontmatter bytes.
 * @param source_path     Source file path, recorded as tomlc17's document source name. The parser's
 *                        error text carries only a line number, so the path is not in `err`. That
 *                        number counts from the start of the file, not from the start of the
 *                        frontmatter, so a caller may present it to the user alongside
 * the path.
 * @param parsed_out      Receives the parsed TOML result on success. Must not be `NULL`.
 * @param err             Buffer for a diagnostic message on failure.
 * @param err_len         Size of `err` in bytes.
 * @return `0` on success, or `-1` on an oversize input, allocation, or TOML parse error.
 */
static int frontmatter_parse_toml(const char* frontmatter,
                                  size_t frontmatter_len,
                                  const char* source_path,
                                  toml_result_t* parsed_out,
                                  char* err,
                                  size_t err_len) __attribute__((nonnull(1, 4)));

/**
 * @brief Populates every entry metadata field from the parsed frontmatter table.
 *
 * @param entry       Entry that receives the metadata. Must not be `NULL`.
 * @param table       Parsed frontmatter top-level table.
 * @param source_path Source file path, used for slug fallback and diagnostics. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first field that fails validation.
 */
static int frontmatter_parse_metadata(struct ContentEntry* entry,
                                      toml_datum_t table,
                                      const char* source_path,
                                      char* err,
                                      size_t err_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Records the source path on the entry, for diagnostics and slug derivation.
 *
 * This step takes no table because the path comes from the caller, not from frontmatter.
 *
 * @param entry       Entry that receives `source_path`. Must not be `NULL`.
 * @param source_path Source file path stored on the entry. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` when the copy cannot be allocated.
 */
static int frontmatter_parse_metadata_source_path(struct ContentEntry* entry,
                                                  const char* source_path,
                                                  char* err,
                                                  size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Copies the required title.
 *
 * @param entry   Entry that receives `title`. Must not be `NULL`.
 * @param table   Parsed frontmatter table.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` if the title is missing, mistyped, or cannot be copied.
 */
static int frontmatter_parse_metadata_title(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            char* err,
                                            size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Reads the required date and records both its display and sortable forms.
 *
 * @param entry   Entry that receives `date` and `date_epoch`. Must not be `NULL`.
 * @param table   Parsed frontmatter table.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` if the date is missing, mistyped, or cannot be formatted.
 */
static int frontmatter_parse_metadata_date(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           char* err,
                                           size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Copies the optional description string when present.
 *
 * @param entry   Entry that receives the description. Must not be `NULL`.
 * @param table   Parsed frontmatter table.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type or allocation failure.
 */
static int frontmatter_parse_metadata_description(struct ContentEntry* entry,
                                                  toml_datum_t table,
                                                  char* err,
                                                  size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Uses an explicit slug or derives one from the source filename.
 *
 * This slugifies and length-checks both explicit and derived slugs against `SLUG_LEN_MAX`. A slug
 * becomes a single path component, so this normalizes an explicit one rather than trusting it as a
 * path. `/` and `..` must not survive into the output path.
 *
 * @param entry       Entry that receives the slug. Must not be `NULL`.
 * @param table       Parsed frontmatter table.
 * @param source_path Source file path used to derive a fallback slug. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on a wrong type, oversize slug, or allocation failure.
 */
static int frontmatter_parse_metadata_slug(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           const char* source_path,
                                           char* err,
                                           size_t err_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Copies the optional tags array into the entry's arena-owned storage.
 *
 * @param entry   Entry that receives `tags` and `tag_count`. Must not be `NULL`.
 * @param table   Parsed frontmatter table.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type or allocation failure.
 */
static int frontmatter_parse_metadata_tags(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           char* err,
                                           size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Applies the optional draft flag after type-checking it.
 *
 * @param entry   Entry that receives `is_draft`. Must not be `NULL`.
 * @param table   Parsed frontmatter table.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type.
 */
static int frontmatter_parse_metadata_draft(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            char* err,
                                            size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Applies an optional template override after validating it is a safe relative name.
 *
 * @param entry   Entry that receives `template`. Must not be `NULL`.
 * @param table   Parsed frontmatter table.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type, unsafe name, or
 *         allocation failure.
 */
static int frontmatter_parse_metadata_template(struct ContentEntry* entry,
                                               toml_datum_t table,
                                               char* err,
                                               size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Requires a TOML string key and copies it into arena-owned storage.
 *
 * @param table   Parsed TOML table.
 * @param key     Key that must be present and string-typed. Must not be `NULL`.
 * @param arena   Arena that owns the copied value. Must not be `NULL`.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return The copied, terminated string owned by `arena`, or `NULL` if the key is missing,
 *         mistyped, or cannot be copied.
 */
static const char* require_string(toml_datum_t table,
                                  const char* key,
                                  struct Arena* arena,
                                  char* err,
                                  size_t err_len) __attribute__((nonnull(2, 3)));

/**
 * @brief Validates a present TOML string datum and copies it into arena-owned storage.
 *
 * The single boundary where a frontmatter string becomes an owned string: it rejects a non-string
 * datum and one carrying an embedded `NUL` (see `toml_datum_is_text`) before copying, so callers
 * only decide whether the key was required.
 *
 * @param value   Datum to validate and copy. Must be present, not `TOML_UNKNOWN`.
 * @param key     Key name used in diagnostics. Must not be `NULL`.
 * @param arena   Arena that owns the copy. Must not be `NULL`.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return The copied, terminated string owned by `arena`, or `NULL` if the datum is mistyped, holds
 *         a `NUL`, or cannot be copied.
 */
static const char* copy_string_value(toml_datum_t value,
                                     const char* key,
                                     struct Arena* arena,
                                     char* err,
                                     size_t err_len) __attribute__((nonnull(2, 3)));

int frontmatter_split(const char* markdown,
                      size_t markdown_len,
                      struct FrontmatterSplit* split_out,
                      char* err,
                      size_t err_len) {
  size_t pos = 0;
  const size_t bom_len = sizeof(UTF8_BOM) - 1;
  // The `&&` short circuit keeps the read in bounds. `memcmp` reads all `bom_len` bytes however
  // short the buffer, so on a file shorter than the BOM the compare would read past the end of
  // `markdown`. That is an out-of-bounds read, not merely a wrong answer.
  if (markdown_len >= bom_len && memcmp(markdown, UTF8_BOM, bom_len) == 0) {
    pos = bom_len;
  }
  const size_t line_first_len = line_length(markdown + pos, markdown_len - pos);
  if (!is_frontmatter_fence_line(markdown + pos, line_first_len)) {
    return error_report(err, err_len, "missing opening '%s' frontmatter fence", FRONTMATTER_FENCE);
  }
  pos += line_first_len;
  const size_t frontmatter_start = pos;
  // Under the loop's guard `line_length` returns at least one byte, so the scan always advances. It
  // never returns more than the bytes that remain, so `pos` reaches `markdown_len` without passing
  // it.
  while (pos < markdown_len) {
    const char* line = markdown + pos;
    const size_t line_len = line_length(line, markdown_len - pos);
    if (is_frontmatter_fence_line(line, line_len)) {
      split_out->frontmatter = markdown + frontmatter_start;
      split_out->frontmatter_len = pos - frontmatter_start;
      split_out->body = markdown + pos + line_len;
      split_out->body_len = markdown_len - (pos + line_len);
      return 0;
    }
    pos += line_len;
  }
  return error_report(err, err_len, "missing closing '%s' frontmatter fence", FRONTMATTER_FENCE);
}

int frontmatter_parse(struct ContentEntry* entry,
                      const char* frontmatter,
                      size_t frontmatter_len,
                      const char* source_path,
                      char* err,
                      size_t err_len) {
  // This is zero-initialized although every path reaching `toml_free` writes it. `toml_free` takes
  // the struct by value, and without this clang-analyzer cannot prove across the call boundary that
  // its fields are set.
  toml_result_t parsed = {0};
  if (frontmatter_parse_toml(frontmatter, frontmatter_len, source_path, &parsed, err, err_len) !=
      0) {
    return -1;
  }

  const int rc = frontmatter_parse_metadata(entry, parsed.toptab, source_path, err, err_len);
  // The populate step copies every value it keeps into the entry's arena, so `frontmatter_parse`
  // can release the parse result here and the entry holds no pointer into it.
  toml_free(parsed);
  return rc;
}

static size_t line_length(const char* text, size_t text_len) {
  const char* line_end = memchr(text, '\n', text_len);
  return line_end == NULL ? text_len : (size_t)(line_end - text) + 1;
}

static bool is_frontmatter_fence_line(const char* line, size_t line_len) {
  if (line_len > 0 && line[line_len - 1] == '\n') {
    line_len--;
  }
  if (line_len > 0 && line[line_len - 1] == '\r') {
    line_len--;
  }
  const size_t fence_len = sizeof(FRONTMATTER_FENCE) - 1;
  // The length equality gates the compare. The `&&` short circuit keeps its read in bounds:
  // `memcmp` reads all `fence_len` bytes, so comparing a shorter line would read past its end.
  return line_len == fence_len && memcmp(line, FRONTMATTER_FENCE, fence_len) == 0;
}

// Copies bytes for tomlc17 because the parser expects a terminated document.
static int frontmatter_parse_toml(const char* frontmatter,
                                  size_t frontmatter_len,
                                  const char* source_path,
                                  toml_result_t* parsed_out,
                                  char* err,
                                  size_t err_len) {
  // tomlc17 takes the document length as an `int`, so the guard rejects an oversize document rather
  // than letting the cast below truncate it silently. The copy reserves one extra byte for the
  // synthesized newline below, so the limit is `INT_MAX - 1` rather than `INT_MAX`. That bound also
  // keeps `frontmatter_len + 2` from wrapping when the terminator is stored.
  if (frontmatter_len > (size_t)INT_MAX - 1) {
    // The wrapping caller appends the file, so naming it here too would print the path twice.
    return error_report(err, err_len,
                        "failed to parse frontmatter: exceeds max frontmatter size (%d bytes) at "
                        "%zu bytes",
                        INT_MAX - 1, frontmatter_len);
  }

  char* copy = malloc(frontmatter_len + 2);
  if (copy == NULL) {
    return error_report(err, err_len, "out of memory parsing frontmatter");
  }
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
  copy[0] = '\n';
  memcpy(copy + 1, frontmatter, frontmatter_len);
  copy[frontmatter_len + 1] = '\0';
  *parsed_out = toml_parse_named(copy, (int)(frontmatter_len + 1), source_path);
  free(copy);

  if (!parsed_out->ok) {
    (void)error_report(err, err_len, "failed to parse frontmatter: %s", parsed_out->errmsg);
    // The call currently releases nothing. tomlc17 frees its datum tree and pool on failure, then
    // returns a zeroed result. The vendor requires callers to pass every result to `toml_free`.
    // Following that contract keeps this code correct if a later version retains data after
    // failure.
    toml_free(*parsed_out);
    return -1;
  }
  return 0;
}

// The key order matches the frontmatter documented in `README.md`.
static int frontmatter_parse_metadata(struct ContentEntry* entry,
                                      toml_datum_t table,
                                      const char* source_path,
                                      char* err,
                                      size_t err_len) {
  if (frontmatter_parse_metadata_source_path(entry, source_path, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_title(entry, table, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_date(entry, table, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_description(entry, table, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_slug(entry, table, source_path, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_tags(entry, table, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_draft(entry, table, err, err_len) != 0) {
    return -1;
  }
  if (frontmatter_parse_metadata_template(entry, table, err, err_len) != 0) {
    return -1;
  }
  return 0;
}

static int frontmatter_parse_metadata_source_path(struct ContentEntry* entry,
                                                  const char* source_path,
                                                  char* err,
                                                  size_t err_len) {
  entry->source_path = arena_strdup(&entry->arena, source_path);
  if (entry->source_path == NULL) {
    return error_report(err, err_len, "out of memory copying source path");
  }
  return 0;
}

static int frontmatter_parse_metadata_title(struct ContentEntry* entry,
                                            toml_datum_t table,
                                            char* err,
                                            size_t err_len) {
  entry->title = require_string(table, "title", &entry->arena, err, err_len);
  if (entry->title == NULL) {
    return -1;
  }
  return 0;
}

static int frontmatter_parse_metadata_date(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           char* err,
                                           size_t err_len) {
  toml_datum_t date = toml_get(table, "date");
  if (date.type == TOML_UNKNOWN) {
    return error_report(err, err_len, "missing required frontmatter key 'date'");
  }
  if (toml_datum_to_epoch(date, &entry->date_epoch) != 0) {
    return error_report(err, err_len, "frontmatter key 'date' must be a TOML date/datetime");
  }
  entry->date = toml_datum_format_rfc3339(date, &entry->arena);
  if (entry->date == NULL) {
    return error_report(err, err_len, "out of memory formatting frontmatter key 'date'");
  }
  return 0;
}

static int frontmatter_parse_metadata_description(struct ContentEntry* entry,
                                                  toml_datum_t table,
                                                  char* err,
                                                  size_t err_len) {
  toml_datum_t description = toml_get(table, "description");
  if (description.type == TOML_UNKNOWN) {
    return 0;
  }
  entry->description = copy_string_value(description, "description", &entry->arena, err, err_len);
  return entry->description != NULL ? 0 : -1;
}

static int frontmatter_parse_metadata_slug(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           const char* source_path,
                                           char* err,
                                           size_t err_len) {
  toml_datum_t slug = toml_get(table, "slug");
  const char* slug_source = NULL;
  if (slug.type != TOML_UNKNOWN) {
    // Copy through the shared boundary so it type- and `NUL`-checks an explicit slug like every
    // other frontmatter string before the slug reaches `text_slugify`, which reads it as a C
    // string.
    slug_source = copy_string_value(slug, "slug", &entry->arena, err, err_len);
    if (slug_source == NULL) {
      return -1;
    }
  } else {
    slug_source = path_basename_without_extension(source_path, &entry->arena);
  }

  // `path_basename_without_extension` returns `NULL` on allocation failure. The code must not call
  // `strlen` on `NULL`, so the guard folds that failure into the same diagnostic below.
  size_t slug_len = 0;
  entry->slug = slug_source == NULL
                    ? NULL
                    : text_slugify(slug_source, strlen(slug_source), &entry->arena, &slug_len);
  if (entry->slug == NULL) {
    return error_report(err, err_len, "out of memory deriving slug for '%s'", source_path);
  }
  if (slug_len > SLUG_LEN_MAX) {
    // The wrapping caller appends the file, so naming it here too would print the path twice.
    // Leading with it would also push this limit clause out of the buffer for any source path over
    // roughly 465 bytes. The measured length precedes the slug for the same reason. It is the one
    // part that cannot itself truncate.
    return error_report(err, err_len, "slug exceeds max slug length (%zu bytes) at %zu bytes: '%s'",
                        (size_t)SLUG_LEN_MAX, slug_len, entry->slug);
  }
  return 0;
}

static int frontmatter_parse_metadata_tags(struct ContentEntry* entry,
                                           toml_datum_t table,
                                           char* err,
                                           size_t err_len) {
  toml_datum_t tags = toml_get(table, "tags");
  if (tags.type == TOML_UNKNOWN) {
    return 0;
  }
  if (tags.type != TOML_ARRAY) {
    return error_report(err, err_len, "frontmatter key 'tags' must be an array");
  }

  const size_t tag_count = (size_t)tags.u.arr.size;
  // There is no count check beside this one. `arena_alloc` normalizes a zero-byte request to a
  // distinct one-byte allocation, so `arena_calloc` never returns `NULL` for an empty tag list.
  const char** items = arena_calloc(&entry->arena, tag_count, sizeof(*items));
  if (items == NULL) {
    return error_report(err, err_len, "out of memory copying frontmatter key 'tags'");
  }
  for (size_t i = 0; i < tag_count; i++) {
    if (tags.u.arr.elem[i].type != TOML_STRING) {
      return error_report(err, err_len, "frontmatter key 'tags' must contain only strings");
    }
    items[i] = copy_string_value(tags.u.arr.elem[i], "tags", &entry->arena, err, err_len);
    if (items[i] == NULL) {
      return -1;
    }
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
                                            char* err,
                                            size_t err_len) {
  toml_datum_t draft = toml_get(table, "draft");
  if (draft.type == TOML_UNKNOWN) {
    return 0;
  }
  if (draft.type != TOML_BOOLEAN) {
    return error_report(err, err_len, "frontmatter key 'draft' must be a boolean");
  }
  entry->is_draft = draft.u.boolean;
  return 0;
}

static int frontmatter_parse_metadata_template(struct ContentEntry* entry,
                                               toml_datum_t table,
                                               char* err,
                                               size_t err_len) {
  toml_datum_t template = toml_get(table, "template");
  if (template.type == TOML_UNKNOWN) {
    return 0;
  }
  entry->template = copy_string_value(template, "template", &entry->arena, err, err_len);
  if (entry->template == NULL) {
    return -1;
  }
  if (!path_is_safe_relative(entry->template)) {
    return error_report(err, err_len,
                        "frontmatter key 'template' must be a safe relative template name: '%s'",
                        entry->template);
  }
  return 0;
}

static const char* require_string(toml_datum_t table,
                                  const char* key,
                                  struct Arena* arena,
                                  char* err,
                                  size_t err_len) {
  toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    (void)error_report(err, err_len, "missing required frontmatter key '%s'", key);
    return NULL;
  }
  return copy_string_value(value, key, arena, err, err_len);
}

static const char* copy_string_value(toml_datum_t value,
                                     const char* key,
                                     struct Arena* arena,
                                     char* err,
                                     size_t err_len) {
  // This is reachable for every caller but `tags`, whose loop pre-checks the element type so it can
  // name the array in the diagnostic.
  if (value.type != TOML_STRING) {
    (void)error_report(err, err_len, "frontmatter key '%s' must be a string", key);
    return NULL;
  }
  if (!toml_datum_is_text(value)) {
    (void)error_report(err, err_len, "frontmatter key '%s' must not contain a NUL byte", key);
    return NULL;
  }
  const char* copy = arena_strndup(arena, value.u.str.ptr, (size_t)value.u.str.len);
  if (copy == NULL) {
    (void)error_report(err, err_len, "out of memory copying frontmatter key '%s'", key);
    return NULL;
  }
  return copy;
}
