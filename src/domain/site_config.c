#include "domain/site_config.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tomlc17.h>

#include "core/ascii.h"
#include "core/error.h"
#include "core/path.h"
#include "domain/content_entry.h"
#include "domain/permalink.h"
#include "formats/toml.h"
#include "runtime/fs.h"

/** String-valued site config key policy. */
struct SiteConfigStringKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `SiteConfig` field this key fills. */
  const char** field;

  /** Whether absence is a load error. */
  bool is_required;
};

const char* const SITE_CONFIG_PATH_DEFAULT = "sosig.toml";

static const char* const PERMALINK_DEFAULT = "/{section}/{slug}.html";
static const char* const CONTENT_DIR_DEFAULT = "content";
static const char* const OUTPUT_DIR_DEFAULT = "public";
static const char* const TEMPLATES_DIR_DEFAULT = "templates";
static const char* const CONTENT_TEMPLATE_DEFAULT = "content.html";
static const char* const AGGREGATE_TEMPLATES_DEFAULT[] = {"index.html"};
static const char* const FEED_TEMPLATES_DEFAULT[] = {"atom.xml"};

/** Default cap on content entries included in a feed when `feed_count` is unset. */
enum { FEED_COUNT_DEFAULT = 10 };

/**
 * Sample values a permalink pattern is expanded with during validation. They stand in for real
 * ones, which are always slugified and so always safe on their own.
 *
 * The grid checks both a populated and an empty section. An empty one changes how redundant `/`
 * collapse, so a pattern like `{section}/..` must be rejected either way. The two slugs make a
 * pattern's per-entry variation observable, so both checks below draw from the same pair.
 */
static const char* const SECTION_SAMPLES[] = {"section", ""};
static const char* const SLUG_SAMPLES[] = {"slug-a", "slug-b"};

enum { SECTION_SAMPLE_COUNT = sizeof(SECTION_SAMPLES) / sizeof(SECTION_SAMPLES[0]) };
enum { SLUG_SAMPLE_COUNT = sizeof(SLUG_SAMPLES) / sizeof(SLUG_SAMPLES[0]) };

/**
 * Permalink-pattern check verdict, in the precedence order `check_permalink` applies. It catches an
 * unsafe expansion first, then two slugs colliding, then either length limit.
 * `PERMALINK_OUT_OF_MEMORY` is the one member that says nothing about the pattern, so it stays
 * distinct rather than folded into a neighbor. A caller that reported it as a verdict would blame
 * the user's config for the allocator.
 */
enum PermalinkVerdict {
  /** Every expansion is a safe relative path, within both length limits, and slug-dependent. */
  PERMALINK_VALID,

  /** Some expansion escapes the output directory or is otherwise unsafe. */
  PERMALINK_UNSAFE,

  /** Some expansion exceeds `OUTPUT_PATH_RELATIVE_LEN_MAX` in total. */
  PERMALINK_TOO_LONG,

  /**
   * Some expansion has a path segment exceeding `FILENAME_LEN_MAX`. Only a literal segment of the
   * pattern can reach this from the sample grid, because the sample slugs are a few bytes each. A
   * literal's length does not vary per entry, which makes it knowable here rather than only per
   * content file.
   */
  PERMALINK_SEGMENT_TOO_LONG,

  /** Two different slugs expand to the same path, so every entry claims one output. */
  PERMALINK_NOT_DISTINCT,

  /** An expansion could not be allocated, so the pattern was never judged. */
  PERMALINK_OUT_OF_MEMORY,
};

/**
 * @brief Reads and parses a TOML config file into a `toml_result_t`.
 *
 * On success `parsed_out` owns the parse result until the caller runs `toml_free`.
 *
 * @param config_path Path to the config file. Must not be `NULL`.
 * @param parsed_out  Receives the parsed TOML result on success. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on a read, size, or TOML parse error.
 */
static int site_config_load_toml(const char* config_path,
                                 toml_result_t* parsed_out,
                                 char* err,
                                 size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Copies recognized configuration keys into site config storage.
 *
 * Applies required and optional string keys, template-name arrays, and `feed_count`. It copies
 * every value into the config's arena rather than borrowing from `table`. `site_config_load` calls
 * `toml_free` as soon as this returns, which releases the pool every `toml_datum_t` string points
 * into, while the config outlives the parse.
 *
 * @param site_config Config that receives the values. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on the first key that fails validation.
 */
static int site_config_load_fields(struct SiteConfig* site_config,
                                   toml_datum_t table,
                                   char* err,
                                   size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Copies one string key into the arena, enforcing required keys.
 *
 * @param table       Parsed TOML table.
 * @param key         Key to read. Must not be `NULL`.
 * @param is_required Whether absence is an error. When `false`, absence leaves `*value_out`
 *                    untouched.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the copied string when the key is present. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` if a required key is missing, mistyped, or cannot be copied.
 */
static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       const char** value_out,
                       char* err,
                       size_t err_len) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Copies one optional template-name array key and validates each name.
 *
 * When the key is absent, this leaves the outputs untouched and keeps the defaults from
 * `site_config_init`.
 *
 * @param table                   Parsed TOML table.
 * @param key                     Array key to read. Must not be `NULL`.
 * @param arena                   Arena that owns the copied names. Must not be `NULL`.
 * @param template_names_out      Receives the copied name array when the key is present. Must not
 *                                be `NULL`.
 * @param template_name_count_out Receives the number of names when the key is present. Must not be
 *                                `NULL`.
 * @param err                     Buffer for a diagnostic message on failure.
 * @param err_len                 Size of `err` in bytes.
 * @return `0` on success or when absent, or `-1` on a wrong type, unsafe name, or allocation
 *         failure.
 */
static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const char* const** template_names_out,
                               size_t* template_name_count_out,
                               char* err,
                               size_t err_len) __attribute__((nonnull(2, 3, 4, 5)));

/**
 * @brief Validates `base_url` as an absolute http(s) URL and trims any trailing `/`.
 *
 * `copy_string` accepts a relative or scheme-less value, because it checks only presence and type.
 * Such a value then silently produces broken links in every feed and canonical URL. Checking it
 * here keeps that a single config error.
 *
 * This must run after the required keys are copied. `base_url` is `NULL` until then, and
 * `is_valid_base_url` is declared `nonnull` and dereferences it, so calling this before the key
 * loop makes `skip_scheme` read through null. The `nonnull` also licenses the compiler to assume
 * the argument is never null and delete the null case, so the resulting failure need not resemble
 * the one the source suggests.
 *
 * @param site_config Config whose `base_url` is validated and normalized. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` when the URL is not absolute or the copy fails.
 */
static int normalize_base_url(struct SiteConfig* site_config, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether a URL is absolute: an http(s) scheme followed by a non-empty host.
 *
 * @param url URL to check. Must not be `NULL`.
 * @return `true` when `url` has a recognized scheme and a host, `false` otherwise.
 */
static bool is_valid_base_url(const char* url) __attribute__((nonnull(1)));

/**
 * @brief Returns the bytes of `url` after `scheme`, matching ASCII letters case-insensitively.
 *
 * @param url    URL to inspect. Must not be `NULL`.
 * @param scheme Terminated scheme prefix to match, such as `https://`. Must not be `NULL`.
 * @return Pointer just past the scheme, or `NULL` when `url` does not start with it.
 */
static const char* skip_scheme(const char* url, const char* scheme) __attribute__((nonnull(1, 2)));

/**
 * @brief Trims trailing `/` from every configured directory and rejects an empty result.
 *
 * A trailing slash would otherwise survive into path construction, where it defeats the
 * `<content_dir>/` prefix strip that derives an entry's section and so leaks the content directory
 * name into every URL. Normalizing once here keeps that rule in one place and makes
 * `site_config_print` round-trip the normalized form. The location is not otherwise constrained. An
 * absolute or parent-relative directory stays valid.
 *
 * @param site_config Config whose `content_dir`, `output_dir`, and `templates_dir` are normalized.
 *                    Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` when a directory is empty or trims to empty.
 */
static int normalize_dirs(struct SiteConfig* site_config, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Trims trailing `/` from one configured directory in place, rejecting an empty result.
 *
 * This has a plain readable name rather than a `normalize_dirs_` prefix, because it is the whole
 * operation applied to one directory rather than one step of a decomposition. The caller is three
 * calls to it.
 *
 * @param value   Address of the config field holding the directory. Rewritten only when a trailing
 *                `/` was trimmed, so an already-normalized value costs no allocation. Must not be
 * `NULL`.
 * @param key     Config key name used in diagnostics. Must not be `NULL`.
 * @param arena   Arena that owns the trimmed copy. Must not be `NULL`.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` when the directory is empty, trims to empty, or cannot be copied.
 */
static int normalize_dir(const char** value,
                         const char* key,
                         struct Arena* arena,
                         char* err,
                         size_t err_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns the length of `value` with any trailing `/` bytes excluded.
 *
 * @param value Terminated value to measure. Must not be `NULL`.
 * @return Length in bytes up to the first byte of the trailing `/` run, or `0` when `value` is
 *         entirely `/`.
 */
static size_t trimmed_slash_len(const char* value) __attribute__((nonnull(1)));

/**
 * @brief Validates the `permalink` key, translating each verdict into its diagnostic.
 *
 * Splitting the translation from `check_permalink` keeps the grid pass free of message wording and
 * leaves this function as the only place that decides what a verdict tells the user.
 *
 * @param pattern Permalink pattern to validate. Must not be `NULL`.
 * @param err     Buffer for a diagnostic message on failure.
 * @param err_len Size of `err` in bytes.
 * @return `0` when the pattern is valid, or `-1` with a diagnostic naming the verdict.
 */
static int require_valid_permalink(const char* pattern, char* err, size_t err_len)
    __attribute__((nonnull(1)));

/**
 * @brief Validates a permalink pattern by expanding it over the sample grid.
 *
 * Validating the pattern here keeps a bad `permalink` a single config error. If left to the render
 * phase, the render phase catches the same mistake per entry and reports it once for every content
 * file. Every question shares one grid pass because they share the expansions. Reasoning about the
 * pattern's bytes directly would be a second implementation of `path_is_safe_relative`'s policy and
 * of `permalink_expand`'s meaning, each free to drift. Both length limits come from
 * `path_check_output_limits` for the same reason.
 *
 * The two length verdicts reject what the pattern itself makes knowable. Each is knowable from a
 * different part of it. The check applies the whole-path limit to every expansion rather than only
 * the shortest, so it rejects a pattern that fits with an empty `{section}` and overflows with a
 * populated one. A real slug is longer than the samples, so a pattern that only overflows for a
 * particular entry is still the render's to catch. The per-segment limit can only fire on a literal
 * segment of the pattern, whose length does not vary per entry. The per-entry check in the render
 * phase stays as the backstop for a long slug or a deep section.
 *
 * @param pattern          Permalink pattern to check. Must not be `NULL`.
 * @param expanded_len_out Receives the offending expansion's relative length on
 *                         `PERMALINK_TOO_LONG`, and `0` on every other path. Must not be `NULL`.
 * @param segment_len_out  Receives the offending segment's length on `PERMALINK_SEGMENT_TOO_LONG`,
 *                         and `0` on every other path. Must not be `NULL`.
 * @return The first verdict that failed, `PERMALINK_OUT_OF_MEMORY` when an expansion could not be
 *         allocated, or `PERMALINK_VALID` when every check passed.
 */
static enum PermalinkVerdict check_permalink(const char* pattern,
                                             size_t* expanded_len_out,
                                             size_t* segment_len_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Validates and applies the optional `feed_count` key.
 *
 * @param site_config Config that receives `feed_count`. Must not be `NULL`.
 * @param table       Parsed TOML table.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type or negative value.
 */
static int populate_feed_count(struct SiteConfig* site_config,
                               toml_datum_t table,
                               char* err,
                               size_t err_len) __attribute__((nonnull(1)));

/**
 * @brief Writes one `key = "value"` line, escaping the value as a TOML basic string.
 *
 * Delegates to `print_string` so a scalar value quotes exactly like array items.
 *
 * @param stream Destination stream. Must not be `NULL`.
 * @param key    Key name to write. Must not be `NULL`.
 * @param value  Value to quote and write. Must not be `NULL`.
 */
static void site_config_print_key_string(FILE* stream, const char* key, const char* value)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes one `key = [...]` line of TOML basic strings.
 *
 * Delegates to `print_string` so array items quote exactly like scalar values.
 *
 * @param stream     Destination stream. Must not be `NULL`.
 * @param key        Key name to write. Must not be `NULL`.
 * @param items      Array of `item_count` terminated strings. Must not be `NULL`.
 * @param item_count Number of items in `items`.
 */
static void site_config_print_string_array(FILE* stream,
                                           const char* key,
                                           const char* const* items,
                                           size_t item_count) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes `text` as a quoted TOML basic string, escaping delimiters and control bytes.
 *
 * TOML basic strings forbid raw control bytes, so this escapes them alongside `"` and `\`.
 *
 * @param stream Destination stream. Must not be `NULL`.
 * @param text   Terminated text to quote and escape. Must not be `NULL`.
 */
static void print_string(FILE* stream, const char* text) __attribute__((nonnull(1, 2)));

void site_config_init(struct SiteConfig* site_config) {
  // This is one literal naming only the non-zero defaults, so a key added to `SiteConfig` cannot be
  // left out here. A zeroed arena is a valid initialized arena, which is what `arena_init` writes.
  // The three required keys stay `NULL` until the load fills them.
  *site_config = (struct SiteConfig){
      .permalink = PERMALINK_DEFAULT,
      .content_dir = CONTENT_DIR_DEFAULT,
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .content_template = CONTENT_TEMPLATE_DEFAULT,
      .aggregate_templates = AGGREGATE_TEMPLATES_DEFAULT,
      .aggregate_template_count =
          sizeof(AGGREGATE_TEMPLATES_DEFAULT) / sizeof(AGGREGATE_TEMPLATES_DEFAULT[0]),
      .feed_templates = FEED_TEMPLATES_DEFAULT,
      .feed_template_count = sizeof(FEED_TEMPLATES_DEFAULT) / sizeof(FEED_TEMPLATES_DEFAULT[0]),
      .feed_count = FEED_COUNT_DEFAULT,
  };
}

void site_config_free(struct SiteConfig* site_config) {
  arena_free(&site_config->arena);
  site_config_init(site_config);
}

int site_config_load(struct SiteConfig* site_config,
                     const char* config_path,
                     char* err,
                     size_t err_len) {
  // This is zero-initialized, although every path reaching `toml_free` writes it. The struct passes
  // to `toml_free` by value, and without this clang-analyzer cannot prove across the call boundary
  // that its fields are set.
  toml_result_t parsed = {0};
  if (site_config_load_toml(config_path, &parsed, err, err_len) != 0) {
    return -1;
  }

  const int rc = site_config_load_fields(site_config, parsed.toptab, err, err_len);
  toml_free(parsed);
  return rc;
}

int site_config_print(FILE* stream, const struct SiteConfig* site_config) {
  site_config_print_key_string(stream, "base_url", site_config->base_url);
  site_config_print_key_string(stream, "title", site_config->title);
  site_config_print_key_string(stream, "author", site_config->author);
  site_config_print_key_string(stream, "permalink", site_config->permalink);
  site_config_print_key_string(stream, "content_dir", site_config->content_dir);
  site_config_print_key_string(stream, "output_dir", site_config->output_dir);
  site_config_print_key_string(stream, "templates_dir", site_config->templates_dir);
  site_config_print_key_string(stream, "content_template", site_config->content_template);
  site_config_print_string_array(stream, "aggregate_templates", site_config->aggregate_templates,
                                 site_config->aggregate_template_count);
  site_config_print_string_array(stream, "feed_templates", site_config->feed_templates,
                                 site_config->feed_template_count);
  fprintf(stream, "feed_count = %zu\n", site_config->feed_count);
  // A stream buffered to a pipe or file only surfaces a failed write at flush time, not at the
  // individual `fprintf`/`fputc` calls above.
  if (fflush(stream) != 0) {
    return -1;
  }
  if (ferror(stream) != 0) {
    // The stream latched an error from an earlier call, whose `errno` may have been overwritten
    // since. Name a generic I/O failure rather than let the caller relay a stale value.
    errno = EIO;
    return -1;
  }
  return 0;
}

static int site_config_load_toml(const char* config_path,
                                 toml_result_t* parsed_out,
                                 char* err,
                                 size_t err_len) {
  char* config_data = NULL;
  size_t config_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(config_path, &config_data, &config_len, reason, sizeof(reason)) != 0) {
    return error_report(err, err_len, "failed to read config: %s ('%s')", reason, config_path);
  }

  int rc = -1;
  // Reject an oversize config before the cast below. `toml_parse_named` takes the length as an
  // `int`, and converting a `size_t` greater than `INT_MAX` to `int` is implementation-defined and
  // may raise a signal. `fs_read_file` bounds a read only at `SIZE_MAX - 1`, so this is reachable
  // for a config over 2 GiB rather than dead code.
  if (config_len > (size_t)INT_MAX) {
    (void)error_report(err, err_len,
                       "failed to read config: exceeds max config size (%d bytes) at %zu bytes "
                       "('%s')",
                       INT_MAX, config_len, config_path);
    goto cleanup;
  }
  // tomlc17 copies the input, so cleanup frees the source buffer once.
  *parsed_out = toml_parse_named(config_data, (int)config_len, config_path);
  if (!parsed_out->ok) {
    (void)error_report(err, err_len, "failed to parse config: %s ('%s')", parsed_out->errmsg,
                       config_path);
    toml_free(*parsed_out);
    goto cleanup;
  }
  rc = 0;

cleanup:
  free(config_data);
  return rc;
}

static int site_config_load_fields(struct SiteConfig* site_config,
                                   toml_datum_t table,
                                   char* err,
                                   size_t err_len) {
  struct SiteConfigStringKey strings[] = {
      {"base_url", &site_config->base_url, true},
      {"title", &site_config->title, true},
      {"author", &site_config->author, true},
      {"permalink", &site_config->permalink, false},
      {"content_dir", &site_config->content_dir, false},
      {"output_dir", &site_config->output_dir, false},
      {"templates_dir", &site_config->templates_dir, false},
      {"content_template", &site_config->content_template, false},
  };

  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
    if (copy_string(table, strings[i].key, strings[i].is_required, &site_config->arena,
                    strings[i].field, err, err_len) != 0) {
      return -1;
    }
  }

  if (normalize_base_url(site_config, err, err_len) != 0) {
    return -1;
  }
  if (normalize_dirs(site_config, err, err_len) != 0) {
    return -1;
  }
  if (!path_is_safe_relative(site_config->content_template)) {
    return error_report(err, err_len,
                        "config key 'content_template' must be a safe relative template name: '%s'",
                        site_config->content_template);
  }
  if (require_valid_permalink(site_config->permalink, err, err_len) != 0) {
    return -1;
  }
  if (copy_template_names(table, "aggregate_templates", &site_config->arena,
                          &site_config->aggregate_templates, &site_config->aggregate_template_count,
                          err, err_len) != 0) {
    return -1;
  }
  if (copy_template_names(table, "feed_templates", &site_config->arena,
                          &site_config->feed_templates, &site_config->feed_template_count, err,
                          err_len) != 0) {
    return -1;
  }

  return populate_feed_count(site_config, table, err, err_len);
}

static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       const char** value_out,
                       char* err,
                       size_t err_len) {
  toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    if (is_required) {
      return error_report(err, err_len, "missing required config key '%s'", key);
    }
    return 0;
  }
  if (value.type != TOML_STRING) {
    return error_report(err, err_len, "config key '%s' must be a string", key);
  }
  if (!toml_datum_is_text(value)) {
    return error_report(err, err_len, "config key '%s' must not contain a NUL byte", key);
  }
  *value_out = arena_strndup(arena, value.u.str.ptr, (size_t)value.u.str.len);
  if (*value_out == NULL) {
    return error_report(err, err_len, "out of memory reading config key '%s'", key);
  }
  return 0;
}

static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const char* const** template_names_out,
                               size_t* template_name_count_out,
                               char* err,
                               size_t err_len) {
  toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_ARRAY) {
    return error_report(err, err_len, "config key '%s' must be an array", key);
  }

  const size_t item_count = (size_t)value.u.arr.size;
  // This allocates even for an empty array, rather than leaving it `NULL`. `arena_alloc` normalizes
  // a zero-byte request to a distinct one-byte allocation, so this never yields `NULL`. Every
  // consumer of the published array is declared `nonnull` on it. If left `NULL`,
  // `aggregate_templates = []` would reach `site_config_print_string_array` as a null pointer and
  // abort.
  const char** items = arena_calloc(arena, item_count, sizeof(*items));
  if (items == NULL) {
    return error_report(err, err_len, "out of memory reading config key '%s'", key);
  }

  for (size_t i = 0; i < item_count; i++) {
    if (value.u.arr.elem[i].type != TOML_STRING) {
      return error_report(err, err_len, "config key '%s' must contain only strings", key);
    }
    if (!toml_datum_is_text(value.u.arr.elem[i])) {
      return error_report(err, err_len, "config key '%s' must not contain a NUL byte", key);
    }
    items[i] =
        arena_strndup(arena, value.u.arr.elem[i].u.str.ptr, (size_t)value.u.arr.elem[i].u.str.len);
    if (items[i] == NULL) {
      return error_report(err, err_len, "out of memory reading config key '%s'", key);
    }
    if (!path_is_safe_relative(items[i])) {
      return error_report(err, err_len,
                          "config key '%s' must contain only safe relative template names: '%s'",
                          key, items[i]);
    }
  }

  // The code fills these through a mutable local and publishes them as `const char* const*`. The
  // elements have to be writable while it builds the arena copy, but must not be once the config
  // shares them with the render workers.
  *template_names_out = items;
  *template_name_count_out = item_count;
  return 0;
}

static int normalize_base_url(struct SiteConfig* site_config, char* err, size_t err_len) {
  if (!is_valid_base_url(site_config->base_url)) {
    return error_report(err, err_len,
                        "config key 'base_url' must be an absolute 'http://' or 'https://' URL "
                        "with a host: '%s'",
                        site_config->base_url);
  }

  // Trim a trailing `/` so templates can join `base_url` with an entry's `url`, which always starts
  // with one, without producing `//`. Normalizing once here keeps the rule in one place and makes
  // `site_config_print` round-trip the normalized form, as `normalize_dirs` does for directories.
  // The trim cannot eat the scheme's slashes, because the check above proved a host byte follows
  // them.
  const size_t value_len = strlen(site_config->base_url);
  const size_t trimmed_len = trimmed_slash_len(site_config->base_url);
  if (trimmed_len < value_len) {
    site_config->base_url = arena_strndup(&site_config->arena, site_config->base_url, trimmed_len);
    if (site_config->base_url == NULL) {
      return error_report(err, err_len, "out of memory reading config key 'base_url'");
    }
  }
  return 0;
}

static bool is_valid_base_url(const char* url) {
  /**
   * It accepts a scheme plus a non-empty host, and nothing more. A port, subpath, or query string
   * must stay valid, and judging a host beyond "not empty" would mean ruling on what DNS and
   * punycode allow. Schemes are case-insensitive, so `HTTPS://` is the same URL.
   */
  static const char* const URL_SCHEMES[] = {"http://", "https://"};

  for (size_t i = 0; i < sizeof(URL_SCHEMES) / sizeof(URL_SCHEMES[0]); i++) {
    const char* host = skip_scheme(url, URL_SCHEMES[i]);
    if (host != NULL) {
      // `https://` has no host at all. `https:///path`, `https://?q` and `https://#f` have an empty
      // host: `/`, `?`, or `#` starts immediately.
      return *host != '\0' && *host != '/' && *host != '?' && *host != '#';
    }
  }
  return false;
}

static const char* skip_scheme(const char* url, const char* scheme) {
  size_t i = 0;
  // A `url` shorter than `scheme` stops at its terminator, which no scheme byte matches.
  for (; scheme[i] != '\0'; i++) {
    if (ascii_to_lower((unsigned char)url[i]) != ascii_to_lower((unsigned char)scheme[i])) {
      return NULL;
    }
  }
  return url + i;
}

static int normalize_dirs(struct SiteConfig* site_config, char* err, size_t err_len) {
  if (normalize_dir(&site_config->content_dir, "content_dir", &site_config->arena, err, err_len) !=
      0) {
    return -1;
  }
  if (normalize_dir(&site_config->output_dir, "output_dir", &site_config->arena, err, err_len) !=
      0) {
    return -1;
  }
  return normalize_dir(&site_config->templates_dir, "templates_dir", &site_config->arena, err,
                       err_len);
}

static int normalize_dir(const char** value,
                         const char* key,
                         struct Arena* arena,
                         char* err,
                         size_t err_len) {
  const size_t value_len = strlen(*value);
  const size_t trimmed_len = trimmed_slash_len(*value);
  if (trimmed_len == 0) {
    return error_report(err, err_len, "config key '%s' must not be empty", key);
  }
  if (trimmed_len < value_len) {
    *value = arena_strndup(arena, *value, trimmed_len);
    if (*value == NULL) {
      return error_report(err, err_len, "out of memory reading config key '%s'", key);
    }
  }
  return 0;
}

static size_t trimmed_slash_len(const char* value) {
  size_t len = strlen(value);
  while (len > 0 && value[len - 1] == '/') {
    len--;
  }
  return len;
}

static int require_valid_permalink(const char* pattern, char* err, size_t err_len) {
  size_t expanded_len = 0;
  size_t segment_len = 0;
  switch (check_permalink(pattern, &expanded_len, &segment_len)) {
    case PERMALINK_VALID:
      break;
    case PERMALINK_UNSAFE:
      return error_report(
          err, err_len,
          "config key 'permalink' must expand to a safe relative path using only "
          "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
          "with no empty, '.' or '..' path segment: '%s'",
          pattern);
    case PERMALINK_TOO_LONG:
      return error_report(err, err_len,
                          "config key 'permalink' exceeds max output path length (%zu bytes) at "
                          "%zu bytes when expanded",
                          (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, expanded_len);
    case PERMALINK_SEGMENT_TOO_LONG:
      // The limit and the measurement lead, and the pattern trails, matching its siblings. The
      // qualifier trails the measurement the same way the `PERMALINK_TOO_LONG` case above puts
      // "when expanded" last, so both read as `<subject> exceeds max <limit> at <measurement>`.
      // This deliberately does not name the segment itself. It borrows into the expansion arena
      // this function's callee already released, and a segment long enough to fail would crowd out
      // the reason anyway.
      return error_report(err, err_len,
                          "config key 'permalink' exceeds max filename length (%zu bytes) at %zu "
                          "bytes in an expanded path segment: '%s'",
                          (size_t)FILENAME_LEN_MAX, segment_len, pattern);
    case PERMALINK_NOT_DISTINCT:
      return error_report(
          err, err_len,
          "config key 'permalink' must expand to a distinct path per content entry; "
          "include the '{slug}' token: '%s'",
          pattern);
    case PERMALINK_OUT_OF_MEMORY:
      return error_report(err, err_len, "out of memory checking config key 'permalink'");
  }
  return 0;
}

static enum PermalinkVerdict check_permalink(const char* pattern,
                                             size_t* expanded_len_out,
                                             size_t* segment_len_out) {
  struct Arena scratch;
  arena_init(&scratch);
  enum PermalinkVerdict verdict = PERMALINK_VALID;
  size_t failing_len = 0;
  size_t failing_segment_len = 0;

  for (size_t i = 0; verdict == PERMALINK_VALID && i < SECTION_SAMPLE_COUNT; i++) {
    const char* first = NULL;
    for (size_t j = 0; verdict == PERMALINK_VALID && j < SLUG_SAMPLE_COUNT; j++) {
      const char* url = permalink_expand(pattern, SECTION_SAMPLES[i], SLUG_SAMPLES[j], &scratch);
      if (url == NULL) {
        verdict = PERMALINK_OUT_OF_MEMORY;
        break;
      }
      if (!path_is_safe_relative(url + 1)) {
        verdict = PERMALINK_UNSAFE;
        break;
      }
      if (j == 0) {
        first = url;
      } else if (strcmp(first, url) == 0) {
        verdict = PERMALINK_NOT_DISTINCT;
        break;
      }
      // Both limits come from `path_check_output_limits`, the single place that applies them,
      // rather than being respelled here. The check runs on every expansion instead of only the
      // shortest, which makes the whole-path limit a property of the pattern. One that fits with an
      // empty `{section}` and overflows with a populated one is a config error, and leaving it to
      // the render reports it once per content file. It is ordered after the distinctness check, so
      // the verdict precedence this enum documents stays unchanged.
      struct PathOutputMetrics metrics;
      const enum PathOutputVerdict limits = path_check_output_limits(url + 1, &metrics);
      if (limits == PATH_OUTPUT_TOO_LONG) {
        failing_len = metrics.len;
        verdict = PERMALINK_TOO_LONG;
        break;
      }
      if (limits == PATH_OUTPUT_SEGMENT_TOO_LONG) {
        failing_segment_len = metrics.segment_len;
        verdict = PERMALINK_SEGMENT_TOO_LONG;
        break;
      }
    }
  }

  *expanded_len_out = failing_len;
  *segment_len_out = failing_segment_len;
  arena_free(&scratch);
  return verdict;
}

static int populate_feed_count(struct SiteConfig* site_config,
                               toml_datum_t table,
                               char* err,
                               size_t err_len) {
  toml_datum_t feed_count = toml_get(table, "feed_count");
  if (feed_count.type == TOML_UNKNOWN) {
    return 0;
  }
  if (feed_count.type != TOML_INT64) {
    return error_report(err, err_len, "config key 'feed_count' must be an integer");
  }
  if (feed_count.u.int64 < 0) {
    return error_report(err, err_len, "config key 'feed_count' must not be negative");
  }
#if SIZE_MAX < INT64_MAX
  // Reachable only where `size_t` is narrower than `int64_t`, as on a 32-bit target.
  if ((uint64_t)feed_count.u.int64 > (uint64_t)SIZE_MAX) {
    return error_report(err, err_len, "config key 'feed_count' exceeds max feed count (%zu) at %ju",
                        (size_t)SIZE_MAX, (uintmax_t)feed_count.u.int64);
  }
#endif
  site_config->feed_count = (size_t)feed_count.u.int64;
  return 0;
}

static void site_config_print_key_string(FILE* stream, const char* key, const char* value) {
  fprintf(stream, "%s = ", key);
  print_string(stream, value);
  fputc('\n', stream);
}

static void site_config_print_string_array(FILE* stream,
                                           const char* key,
                                           const char* const* items,
                                           size_t item_count) {
  fprintf(stream, "%s = [", key);
  for (size_t i = 0; i < item_count; i++) {
    if (i > 0) {
      fputs(", ", stream);
    }
    print_string(stream, items[i]);
  }
  fputs("]\n", stream);
}

static void print_string(FILE* stream, const char* text) {
  fputc('"', stream);
  // Walk the bytes as `unsigned char`. Where `char` is signed, every byte of a multi-byte UTF-8
  // sequence is negative, so the `< 0x20` test below would treat it as a control byte, and passing
  // a negative `int` to the `%04X` conversion is undefined.
  for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
    switch (*p) {
      case '"':
        fputs("\\\"", stream);
        break;
      case '\\':
        fputs("\\\\", stream);
        break;
      case '\b':
        fputs("\\b", stream);
        break;
      case '\t':
        fputs("\\t", stream);
        break;
      case '\n':
        fputs("\\n", stream);
        break;
      case '\f':
        fputs("\\f", stream);
        break;
      case '\r':
        fputs("\\r", stream);
        break;
      default:
        if (*p < 0x20 || *p == 0x7F) {
          fprintf(stream, "\\u%04X", *p);
        } else {
          fputc(*p, stream);
        }
        break;
    }
  }
  fputc('"', stream);
}
