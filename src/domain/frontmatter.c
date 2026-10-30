#include "domain/frontmatter.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <tomlc17.h>

#include "core/ascii.h"
#include "core/parse.h"
#include "core/path.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "formats/toml.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** UTF-8 byte order mark, tolerated as an optional prefix of a Markdown or Org source. */
#define UTF8_BOM S("\xEF\xBB\xBF")

/** Fence that delimits the TOML frontmatter block at the start and end. */
#define FRONTMATTER_FENCE S("+++")

/** Bytes Org reads as blanks inside a line, with the `\r` of a CRLF line ending. */
#define ORG_BLANKS S(" \t\r")

/** Bytes that separate the tags of `#+filetags:`, which Org writes as `:a:b:`. */
#define ORG_TAG_SEPARATORS S(": \t")

/** Result of an Org keyword lookup: a keyword's trimmed value, and whether the file sets it. */
struct OrgKeyword {
  /**
   * Value bytes, borrowed from the file, or from the lookup's arena when it joins several lines.
   * Empty when the keyword is absent.
   */
  struct Str value;

  /**
   * Bytes after the keyword's line, where `find_org_keyword` looks for its next occurrence. Empty
   * in a joined value.
   */
  struct Str rest;

  /** Whether the keyword occurs in the leading run. */
  bool is_found;
};

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
static int frontmatter_parse_document(struct Str frontmatter,
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

/**
 * @brief Copies the required `#+title:`.
 *
 * @param entry       Entry that receives `title`. Must not be `NULL`.
 * @param org         Full Org file bytes.
 * @param source_path Source file the keywords came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param scratch     Arena for the lines a repeated keyword joins.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` if the keyword is missing.
 */
static int frontmatter_parse_org_title(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Str source_path,
                                       struct Arena* perm,
                                       struct Arena scratch,
                                       struct Log* log) __attribute__((nonnull(1, 4, 6)));

/**
 * @brief Reads the required `#+date:` timestamp and records both its display and sortable forms.
 *
 * The timestamp becomes a TOML date or datetime datum, so it takes the same conversion and RFC 3339
 * spelling as a TOML `date`. Repeated lines join, as Emacs joins them, so two dates are not one
 * timestamp, while an empty line beside one date leaves it one.
 *
 * @param entry       Entry that receives `date` and `date_epoch`. Must not be `NULL`.
 * @param org         Full Org file bytes.
 * @param source_path Source file the keywords came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param scratch     Arena for the lines a repeated keyword joins.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` if the keyword is missing or not an Org timestamp.
 */
static int frontmatter_parse_org_date(struct ContentEntry* entry,
                                      struct Str org,
                                      struct Str source_path,
                                      struct Arena* perm,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(1, 4, 6)));

/**
 * @brief Copies the optional `#+description:` when present.
 *
 * @param entry   Entry that receives the description. Must not be `NULL`.
 * @param org     Full Org file bytes.
 * @param perm    Arena that owns the copied value. Must not be `NULL`.
 * @param scratch Arena for the lines a repeated keyword joins.
 */
static void frontmatter_parse_org_description(struct ContentEntry* entry,
                                              struct Str org,
                                              struct Arena* perm,
                                              struct Arena scratch) __attribute__((nonnull(1, 3)));

/**
 * @brief Slugifies an explicit `#+slug:` or derives one from the source filename.
 *
 * Both are slugified, as in `frontmatter_parse_metadata_slug`, so `/` and `..` cannot survive into
 * the output path.
 *
 * @param entry       Entry that receives the slug. Must not be `NULL`.
 * @param org         Full Org file bytes.
 * @param source_path Source file path used to derive a fallback slug.
 * @param perm        Arena that owns the slug. Must not be `NULL`.
 */
static void frontmatter_parse_org_slug(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Str source_path,
                                       struct Arena* perm) __attribute__((nonnull(1, 4)));

/**
 * @brief Copies the tags of the optional `#+filetags:` lines into `perm`.
 *
 * The tags are separated by `:` or blanks, so `:Emacs:Hugo:` gives `Emacs` and `Hugo`. Each line
 * adds its tags after the ones before it, and a repeated tag stays, as in Emacs.
 *
 * @param entry   Entry that receives `tags` and `tag_count`. Must not be `NULL`.
 * @param org     Full Org file bytes.
 * @param perm    Arena that owns the copied tags. Must not be `NULL`.
 * @param scratch Arena for the lines the keyword joins.
 */
static void frontmatter_parse_org_tags(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Arena* perm,
                                       struct Arena scratch) __attribute__((nonnull(1, 3)));

/**
 * @brief Applies the optional `#+draft:` flag, spelled as a TOML boolean.
 *
 * @param entry       Entry that receives `is_draft`. Must not be `NULL`.
 * @param org         Full Org file bytes.
 * @param source_path Source file the keywords came from, named in the error.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the keyword is absent, or `-1` when it is neither `true` nor
 *         `false`.
 */
static int frontmatter_parse_org_draft(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Str source_path,
                                       struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Applies an optional `#+template:` override after validating it is a safe relative name.
 *
 * @param entry       Entry that receives `template`. Must not be `NULL`.
 * @param org         Full Org file bytes.
 * @param source_path Source file the keywords came from, named in the error.
 * @param perm        Arena that owns the copied value. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the keyword is absent, or `-1` on an unsafe name.
 */
static int frontmatter_parse_org_template(struct ContentEntry* entry,
                                          struct Str org,
                                          struct Str source_path,
                                          struct Arena* perm,
                                          struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Joins the values of every occurrence of a keyword in the leading run, as Emacs joins
 *        `#+title:` lines.
 *
 * @param org       Full Org file bytes.
 * @param key       Lowercase keyword name, without `#+` and `:`.
 * @param separator Bytes put between two values.
 * @param arena     Arena that owns the joined value when the keyword occurs more than once. Must
 *                  not be `NULL`.
 * @return The joined value and `true`, or an empty value and `false` when the run does not set the
 *         keyword. A single occurrence's value is borrowed from `org`.
 */
static struct OrgKeyword join_org_keyword(struct Str org,
                                          struct Str key,
                                          struct Str separator,
                                          struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Looks up the first occurrence of a keyword in the leading run of an Org file.
 *
 * The run is the blank lines, comment lines, property drawers and keyword lines at the top of the
 * file, after a UTF-8 BOM. The first other line ends it. A lookup from the result's `rest` finds
 * the next occurrence.
 *
 * @param org Full Org file bytes, or the `rest` of an earlier lookup.
 * @param key Lowercase keyword name, without `#+` and `:`.
 * @return The keyword's trimmed value, the bytes after its line, and `true`, or an empty value and
 *         `false` when the run does not set it.
 */
static struct OrgKeyword find_org_keyword(struct Str org, struct Str key);

/**
 * @brief Splits the lines after a `:PROPERTIES:` line at the drawer's `:END:` line.
 *
 * @param rest File bytes after the `:PROPERTIES:` line.
 * @return The bytes after its `:END:` line as `tail`, and whether the drawer ends.
 */
static struct StrCut cut_org_drawer(struct Str rest);

/**
 * @brief Splits a keyword line `#+key: value` into its key and its trimmed value.
 *
 * @param line Line bytes, without leading blanks or a line ending.
 * @return The key, the trimmed value, and whether the line is a keyword line. The key is non-empty
 *         and holds no blank.
 */
static struct StrCut cut_org_keyword(struct Str line);

/**
 * @brief Splits off the first token of a list, skipping the separators before it.
 *
 * Calling it again on `tail` walks the tokens.
 *
 * @param text       List to split.
 * @param separators Bytes that separate the tokens.
 * @return The first token, the bytes after it, and whether there is a token.
 */
static struct StrCut cut_org_token(struct Str text, struct Str separators);

/**
 * @brief Parses an Org timestamp into a TOML date or datetime datum.
 *
 * Accepts `YYYY-MM-DD`, optionally followed by a day name in any language and by a time `H:MM` or
 * `HH:MM`, inside `[...]`, inside `<...>`, or bare. The month, the day for its month, the hour and
 * the minute must be in range, because the TOML conversions trust them.
 *
 * @param value     Trimmed keyword value.
 * @param datum_out Receives a `TOML_DATE` or `TOML_DATETIME` datum on success. Must not be `NULL`.
 * @return `0` on success, or `-1` when `value` is not such a timestamp.
 */
static int parse_org_timestamp(struct Str value, toml_datum_t* datum_out)
    __attribute__((nonnull(2)));

/**
 * @brief Parses the date `YYYY-MM-DD` of an Org timestamp into a datum's date fields.
 *
 * @param datum Datum that receives the year, month and day on success. Must not be `NULL`.
 * @param word  Date bytes.
 * @return `0` on success, or `-1` on a malformed or out-of-range date.
 */
static int parse_org_timestamp_date(toml_datum_t* datum, struct Str word)
    __attribute__((nonnull(1)));

/**
 * @brief Parses the time `H:MM` or `HH:MM` of an Org timestamp and makes the datum a datetime.
 *
 * @param datum Datum that receives the type, hour and minute on success. Must not be `NULL`.
 * @param word  Time bytes.
 * @return `0` on success, or `-1` on a malformed or out-of-range time.
 */
static int parse_org_timestamp_time(toml_datum_t* datum, struct Str word)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether a word of an Org timestamp is a day name.
 *
 * Org accepts a day name in any language, made of any bytes but a digit, `+`, `-`, `]` and `>`.
 *
 * @param word Word bytes, non-empty.
 * @return `true` when `word` is a day name, `false` otherwise.
 */
static bool is_org_day_name(struct Str word);

/**
 * @brief Reports whether text equals a lowercase name, folding the case of ASCII letters.
 *
 * @param text Text to compare.
 * @param name Lowercase name.
 * @return `true` when they match, `false` otherwise.
 */
static bool is_equal_folded(struct Str text, struct Str name);

/**
 * @brief Drops the blanks at both ends of a string.
 *
 * @param text Text to trim.
 * @return A view of `text` without leading or trailing blanks.
 */
static struct Str trim_org_blanks(struct Str text);

/**
 * @brief Drops the bytes of a set from the start of a string.
 *
 * @param text  Text to skip into.
 * @param bytes Bytes to skip.
 * @return A view of `text` from its first byte outside `bytes`.
 */
static struct Str skip_bytes(struct Str text, struct Str bytes);

/**
 * @brief Reports whether a byte is one of a set.
 *
 * @param c     Byte to look for.
 * @param bytes Bytes of the set.
 * @return `true` when `c` is in `bytes`, `false` otherwise.
 */
static bool is_byte_in(char c, struct Str bytes);

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
  if (frontmatter_parse_document(frontmatter, source_path, scratch, &parsed, log) != 0) {
    return -1;
  }

  const int rc = frontmatter_parse_metadata(entry, parsed.toptab, source_path, perm, log);
  // The populate step copies every value it keeps into `perm`, so `frontmatter_parse` can release
  // the parse result here and the entry holds no pointer into it.
  toml_free(parsed);
  return rc;
}

// The keyword order is the key order of `frontmatter_parse_metadata`.
int frontmatter_parse_org(struct ContentEntry* entry,
                          struct Str org,
                          struct Str source_path,
                          struct Arena* perm,
                          struct Arena scratch,
                          struct Log* log) {
  int rc = 0;
  entry->source_path = str_clone(source_path, perm);
  if (frontmatter_parse_org_title(entry, org, source_path, perm, scratch, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_org_date(entry, org, source_path, perm, scratch, log) != 0) {
    rc = -1;
  }
  frontmatter_parse_org_description(entry, org, perm, scratch);
  frontmatter_parse_org_slug(entry, org, source_path, perm);
  frontmatter_parse_org_tags(entry, org, perm, scratch);
  if (frontmatter_parse_org_draft(entry, org, source_path, log) != 0) {
    rc = -1;
  }
  if (frontmatter_parse_org_template(entry, org, source_path, perm, log) != 0) {
    rc = -1;
  }
  return rc;
}

static bool is_frontmatter_fence_line(struct Str line) {
  if (str_endswith(line, S("\r"))) {
    line = str_head(line, line.len - 1);
  }
  return str_eq(line, FRONTMATTER_FENCE);
}

static int frontmatter_parse_document(struct Str frontmatter,
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

static int frontmatter_parse_org_title(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Str source_path,
                                       struct Arena* perm,
                                       struct Arena scratch,
                                       struct Log* log) {
  const struct OrgKeyword title = join_org_keyword(org, S("title"), S(" "), &scratch);
  if (!title.is_found) {
    return log_error_in(log, source_path, "missing required Org keyword 'title'");
  }
  entry->title = str_clone(title.value, perm);
  return 0;
}

static int frontmatter_parse_org_date(struct ContentEntry* entry,
                                      struct Str org,
                                      struct Str source_path,
                                      struct Arena* perm,
                                      struct Arena scratch,
                                      struct Log* log) {
  const struct OrgKeyword date = join_org_keyword(org, S("date"), S(" "), &scratch);
  if (!date.is_found) {
    return log_error_in(log, source_path, "missing required Org keyword 'date'");
  }
  toml_datum_t datum = {0};
  if (parse_org_timestamp(trim_org_blanks(date.value), &datum) != 0) {
    return log_error_in(log, source_path,
                        "Org keyword 'date' must be one Org timestamp, such as '[2024-07-12 Fri]'");
  }
  // The conversion accepts every date and datetime datum, which is all the timestamp parse makes.
  const int epoch_rc = toml_datum_to_epoch(datum, &entry->date_epoch);
  assert(epoch_rc == 0);
  (void)epoch_rc;
  entry->date = toml_datum_format_rfc3339(datum, perm);
  return 0;
}

static void frontmatter_parse_org_description(struct ContentEntry* entry,
                                              struct Str org,
                                              struct Arena* perm,
                                              struct Arena scratch) {
  const struct OrgKeyword description = join_org_keyword(org, S("description"), S("\n"), &scratch);
  if (description.is_found) {
    entry->description = str_clone(description.value, perm);
  }
}

static void frontmatter_parse_org_slug(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Str source_path,
                                       struct Arena* perm) {
  const struct OrgKeyword slug = find_org_keyword(org, S("slug"));
  const struct Str slug_source =
      slug.is_found ? slug.value : path_basename_without_extension(source_path);
  entry->slug = text_slugify(slug_source, perm);
}

static void frontmatter_parse_org_tags(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Arena* perm,
                                       struct Arena scratch) {
  const struct OrgKeyword filetags = join_org_keyword(org, S("filetags"), S(" "), &scratch);
  if (!filetags.is_found) {
    return;
  }
  ptrdiff_t tag_count = 0;
  for (struct StrCut tag = cut_org_token(filetags.value, ORG_TAG_SEPARATORS); tag.is_found;
       tag = cut_org_token(tag.tail, ORG_TAG_SEPARATORS)) {
    tag_count++;
  }
  struct Str* tags = arena_new(perm, tag_count, struct Str);
  ptrdiff_t index = 0;
  for (struct StrCut tag = cut_org_token(filetags.value, ORG_TAG_SEPARATORS); tag.is_found;
       tag = cut_org_token(tag.tail, ORG_TAG_SEPARATORS)) {
    tags[index] = str_clone(tag.head, perm);
    index++;
  }
  entry->tags = tags;
  entry->tag_count = tag_count;
}

static int frontmatter_parse_org_draft(struct ContentEntry* entry,
                                       struct Str org,
                                       struct Str source_path,
                                       struct Log* log) {
  const struct OrgKeyword draft = find_org_keyword(org, S("draft"));
  if (!draft.is_found) {
    return 0;
  }
  if (!str_eq(draft.value, S("true")) && !str_eq(draft.value, S("false"))) {
    return log_error_in(log, source_path, "Org keyword 'draft' must be 'true' or 'false'");
  }
  entry->is_draft = str_eq(draft.value, S("true"));
  return 0;
}

static int frontmatter_parse_org_template(struct ContentEntry* entry,
                                          struct Str org,
                                          struct Str source_path,
                                          struct Arena* perm,
                                          struct Log* log) {
  const struct OrgKeyword template = find_org_keyword(org, S("template"));
  if (!template.is_found) {
    return 0;
  }
  if (!path_is_safe_relative(template.value)) {
    return log_error_in(log, source_path,
                        "Org keyword 'template' must be a safe relative template name: '%.*s'",
                        STR_FMT(template.value));
  }
  entry->template = str_clone(template.value, perm);
  return 0;
}

static struct OrgKeyword join_org_keyword(struct Str org,
                                          struct Str key,
                                          struct Str separator,
                                          struct Arena* arena) {
  const struct OrgKeyword first = find_org_keyword(org, key);
  struct OrgKeyword next = find_org_keyword(first.rest, key);
  if (!next.is_found) {
    return first;
  }
  struct Buf joined = {.arena = arena};
  buf_append(&joined, first.value);
  for (; next.is_found; next = find_org_keyword(next.rest, key)) {
    buf_append(&joined, separator);
    buf_append(&joined, next.value);
  }
  return (struct OrgKeyword){.value = buf_str(&joined), .is_found = true};
}

static struct OrgKeyword find_org_keyword(struct Str org, struct Str key) {
  struct Str rest = org;
  if (str_startswith(rest, UTF8_BOM)) {
    rest = str_tail(rest, UTF8_BOM.len);
  }
  while (rest.len > 0) {
    const struct StrCut line_cut = str_cut(rest, '\n');
    rest = line_cut.tail;
    struct Str line = skip_bytes(line_cut.head, ORG_BLANKS);
    if (str_endswith(line, S("\r"))) {
      line = str_head(line, line.len - 1);
    }
    const struct Str trimmed = trim_org_blanks(line);
    // Org reads `#` followed by a tab as text, not as a comment, so only a space is checked.
    if (trimmed.len == 0 || str_eq(line, S("#")) || str_startswith(line, S("# "))) {
      continue;
    }
    if (is_equal_folded(trimmed, S(":properties:"))) {
      const struct StrCut drawer = cut_org_drawer(rest);
      if (!drawer.is_found) {
        break;
      }
      rest = drawer.tail;
      continue;
    }
    const struct StrCut keyword = cut_org_keyword(line);
    if (!keyword.is_found) {
      break;
    }
    if (is_equal_folded(keyword.head, key)) {
      return (struct OrgKeyword){.value = keyword.tail, .rest = rest, .is_found = true};
    }
  }
  return (struct OrgKeyword){0};
}

static struct StrCut cut_org_drawer(struct Str rest) {
  struct Str lines = rest;
  while (lines.len > 0) {
    const struct StrCut line = str_cut(lines, '\n');
    if (is_equal_folded(trim_org_blanks(line.head), S(":end:"))) {
      return (struct StrCut){.tail = line.tail, .is_found = true};
    }
    lines = line.tail;
  }
  return (struct StrCut){0};
}

static struct StrCut cut_org_keyword(struct Str line) {
  if (!str_startswith(line, S("#+"))) {
    return (struct StrCut){.head = line};
  }
  const struct Str key_and_value = str_tail(line, S("#+").len);
  ptrdiff_t key_len = 0;
  while (key_len < key_and_value.len && key_and_value.data[key_len] != ':' &&
         !is_byte_in(key_and_value.data[key_len], ORG_BLANKS)) {
    key_len++;
  }
  if (key_len == 0 || key_len == key_and_value.len || key_and_value.data[key_len] != ':') {
    return (struct StrCut){.head = line};
  }
  return (struct StrCut){
      .head = str_head(key_and_value, key_len),
      .tail = trim_org_blanks(str_tail(key_and_value, key_len + 1)),
      .is_found = true,
  };
}

static struct StrCut cut_org_token(struct Str text, struct Str separators) {
  const struct Str rest = skip_bytes(text, separators);
  ptrdiff_t token_len = 0;
  while (token_len < rest.len && !is_byte_in(rest.data[token_len], separators)) {
    token_len++;
  }
  return (struct StrCut){
      .head = str_head(rest, token_len),
      .tail = str_tail(rest, token_len),
      .is_found = token_len > 0,
  };
}

static int parse_org_timestamp(struct Str value, toml_datum_t* datum_out) {
  struct Str inner = value;
  if (inner.len >= 2 && ((inner.data[0] == '[' && inner.data[inner.len - 1] == ']') ||
                         (inner.data[0] == '<' && inner.data[inner.len - 1] == '>'))) {
    inner = str_head(str_tail(inner, 1), inner.len - 2);
  }
  toml_datum_t datum = {.type = TOML_DATE};
  struct StrCut word = cut_org_token(inner, ORG_BLANKS);
  if (parse_org_timestamp_date(&datum, word.head) != 0) {
    return -1;
  }
  word = cut_org_token(word.tail, ORG_BLANKS);
  if (word.is_found && is_org_day_name(word.head)) {
    word = cut_org_token(word.tail, ORG_BLANKS);
  }
  if (word.is_found) {
    if (parse_org_timestamp_time(&datum, word.head) != 0) {
      return -1;
    }
    word = cut_org_token(word.tail, ORG_BLANKS);
  }
  // A repeater, a warning delay or a time range is more than an entry's date holds.
  if (word.is_found) {
    return -1;
  }
  *datum_out = datum;
  return 0;
}

static int parse_org_timestamp_date(toml_datum_t* datum, struct Str word) {
  static const ptrdiff_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  ptrdiff_t year = 0;
  ptrdiff_t month = 0;
  ptrdiff_t day = 0;
  if (word.len != S("YYYY-MM-DD").len || word.data[4] != '-' || word.data[7] != '-' ||
      parse_size(str_head(word, 4), &year) != 0 ||
      parse_size(str_head(str_tail(word, 5), 2), &month) != 0 ||
      parse_size(str_tail(word, 8), &day) != 0 || month < 1 || month > 12 || day < 1) {
    return -1;
  }
  const bool is_leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  const ptrdiff_t day_max = month == 2 && is_leap_year ? 29 : month_days[month - 1];
  if (day > day_max) {
    return -1;
  }
  datum->u.ts.year = (int16_t)year;
  datum->u.ts.month = (int16_t)month;
  datum->u.ts.day = (int16_t)day;
  return 0;
}

static int parse_org_timestamp_time(toml_datum_t* datum, struct Str word) {
  const struct StrCut hour_and_minute = str_cut(word, ':');
  ptrdiff_t hour = 0;
  ptrdiff_t minute = 0;
  if (hour_and_minute.head.len < 1 || hour_and_minute.head.len > 2 ||
      hour_and_minute.tail.len != 2 || parse_size(hour_and_minute.head, &hour) != 0 ||
      parse_size(hour_and_minute.tail, &minute) != 0 || hour > 23 || minute > 59) {
    return -1;
  }
  datum->type = TOML_DATETIME;
  datum->u.ts.hour = (int16_t)hour;
  datum->u.ts.minute = (int16_t)minute;
  return 0;
}

static bool is_org_day_name(struct Str word) {
  for (ptrdiff_t i = 0; i < word.len; i++) {
    if (ascii_is_digit((unsigned char)word.data[i]) || is_byte_in(word.data[i], S("+-]>"))) {
      return false;
    }
  }
  return true;
}

static bool is_equal_folded(struct Str text, struct Str name) {
  if (text.len != name.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < text.len; i++) {
    if (ascii_to_lower((unsigned char)text.data[i]) != (unsigned char)name.data[i]) {
      return false;
    }
  }
  return true;
}

static struct Str trim_org_blanks(struct Str text) {
  struct Str trimmed = skip_bytes(text, ORG_BLANKS);
  while (trimmed.len > 0 && is_byte_in(trimmed.data[trimmed.len - 1], ORG_BLANKS)) {
    trimmed = str_head(trimmed, trimmed.len - 1);
  }
  return trimmed;
}

static struct Str skip_bytes(struct Str text, struct Str bytes) {
  ptrdiff_t skipped = 0;
  while (skipped < text.len && is_byte_in(text.data[skipped], bytes)) {
    skipped++;
  }
  return str_tail(text, skipped);
}

static bool is_byte_in(char c, struct Str bytes) {
  return memchr(bytes.data, c, (size_t)bytes.len) != NULL;
}
