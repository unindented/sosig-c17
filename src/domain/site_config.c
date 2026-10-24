#include "domain/site_config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tomlc17.h>

#include "core/ascii.h"
#include "core/path.h"
#include "domain/content_entry.h"
#include "domain/permalink.h"
#include "formats/toml.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** String-valued site config key policy. */
struct SiteConfigStringKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `SiteConfig` field this key fills. */
  struct Str* field;

  /** Whether absence is a load error rather than leaving the initialized default. */
  bool is_required;
};

/** Integer-valued site config key policy. */
struct SiteConfigIntKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `SiteConfig` field this key fills. */
  ptrdiff_t* field;

  /** Smallest accepted value, named in the `must be at least` rejection. */
  ptrdiff_t value_min;

  /** Largest accepted value. A larger one is rejected as exceeding `limit_name`. */
  ptrdiff_t value_max;

  /** Prose name of `value_max` in the overflow diagnostic: `exceeds max feed count`. */
  const char* limit_name;

  /** Whether absence is a load error rather than leaving the field's current value. */
  bool is_required;
};

const struct Str SITE_CONFIG_PATH_DEFAULT = STR_INIT("sosig.toml");

#define PERMALINK_DEFAULT S("/{section}/{slug}.html")
#define CONTENT_DIR_DEFAULT S("content")
#define OUTPUT_DIR_DEFAULT S("public")
#define TEMPLATES_DIR_DEFAULT S("templates")
#define STATIC_DIR_DEFAULT S("static")
#define CONTENT_TEMPLATE_DEFAULT S("content.html")
static const struct Str AGGREGATE_TEMPLATES_DEFAULT[] = {STR_INIT("index.html")};
static const struct Str FEED_TEMPLATES_DEFAULT[] = {STR_INIT("atom.xml")};

/**
 * Every key the top-level table may hold. The load rejects a key outside this list, so a misspelled
 * key cannot silently leave its default in place. A key `site_config_load_fields` reads must be
 * listed here.
 */
static const struct Str config_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("base_url"),
    STR_INIT("title"),
    STR_INIT("author"),
    STR_INIT("permalink"),
    STR_INIT("content_dir"),
    STR_INIT("output_dir"),
    STR_INIT("templates_dir"),
    STR_INIT("static_dir"),
    STR_INIT("content_template"),
    STR_INIT("aggregate_templates"),
    STR_INIT("feed_templates"),
    STR_INIT("feed_count"),
    // clang-format on
};

/** Default cap on content entries included in a feed when `feed_count` is unset. */
enum { FEED_COUNT_DEFAULT = 10 };

/**
 * Largest config file read, in bytes.
 *
 * A config is a few dozen keys, well under a kilobyte, so 1 MiB is three orders of magnitude of
 * headroom while keeping what tomlc17 copies and builds from it to a few MiB. The read checks it
 * before loading the file, so an oversize config is never resident.
 */
enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };

// `toml_parse_document` takes only a document whose length fits an `int`.
_Static_assert(CONFIG_FILE_LEN_MAX <= INT_MAX,
               "an accepted config must fit the int length tomlc17 takes");

/**
 * Sample values a permalink pattern is expanded with during validation. They stand in for real
 * ones, which are always slugified and so always safe on their own.
 *
 * The grid checks both a populated and an empty section. An empty one changes how redundant `/`
 * collapse, so a pattern like `{section}/..` must be rejected either way. The two slugs make a
 * pattern's per-entry variation observable, so both checks below draw from the same pair.
 */
static const struct Str SECTION_SAMPLES[] = {STR_INIT("section"), STR_INIT("")};
static const struct Str SLUG_SAMPLES[] = {STR_INIT("slug-a"), STR_INIT("slug-b")};

enum { SECTION_SAMPLE_COUNT = sizeof(SECTION_SAMPLES) / sizeof(SECTION_SAMPLES[0]) };
enum { SLUG_SAMPLE_COUNT = sizeof(SLUG_SAMPLES) / sizeof(SLUG_SAMPLES[0]) };

/**
 * Permalink-pattern check verdict, in the precedence order `check_permalink` applies. It catches an
 * unsafe expansion first, then two slugs colliding.
 */
enum PermalinkVerdict {
  /** Every expansion is a safe relative path and slug-dependent. */
  PERMALINK_VALID,

  /** Some expansion escapes the output directory or is otherwise unsafe. */
  PERMALINK_UNSAFE,

  /** Two different slugs expand to the same path, so every entry claims one output. */
  PERMALINK_NOT_DISTINCT,
};

/**
 * @brief Reads and parses a TOML config file into a `toml_result_t`.
 *
 * On success `parsed_out` owns the parse result until the caller runs `toml_free`.
 *
 * @param config_path Path to the config file.
 * @param os          Host the file is read from. Must not be `NULL`.
 * @param scratch     Arena for the file bytes, which tomlc17 copies.
 * @param parsed_out  Receives the parsed TOML result on success. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read, size, embedded `NUL`, or TOML parse error.
 */
static int site_config_load_toml(struct Str config_path,
                                 struct Os* os,
                                 struct Arena scratch,
                                 toml_result_t* parsed_out,
                                 struct Log* log) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Copies recognized configuration keys into site config storage.
 *
 * Rejects a key outside `config_keys`, then applies required and optional string keys,
 * template-name arrays, and `feed_count`. It copies every value into `perm` rather than borrowing
 * from `table`. `site_config_load` calls `toml_free` as soon as this returns, which releases the
 * pool every `toml_datum_t` string points into, while the config outlives the parse.
 *
 * @param site_config Config that receives the values. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param perm        Arena that owns the copied values. Must not be `NULL`.
 * @param scratch     Arena for validation temporaries.
 * @param log         Log that receives every error. Must not be `NULL`.
 */
static void site_config_load_fields(struct SiteConfig* site_config,
                                    toml_datum_t table,
                                    struct Arena* perm,
                                    struct Arena scratch,
                                    struct Log* log) __attribute__((nonnull(1, 3, 5)));

/**
 * @brief Copies one string key into the arena, enforcing required keys.
 *
 * @param table       Parsed TOML table.
 * @param key         Key to read. Must not be `NULL`.
 * @param is_required Whether absence is an error. When `false`, absence leaves `*value_out`
 *                    untouched.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the copied string when the key is present. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` if a required key is missing or mistyped.
 */
static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct Str* value_out,
                       struct Log* log) __attribute__((nonnull(2, 4, 5, 6)));

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
 * @param log                     Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when absent, or `-1` on a wrong type, a `NUL` byte, or an unsafe name.
 */
static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const struct Str** template_names_out,
                               ptrdiff_t* template_name_count_out,
                               struct Log* log) __attribute__((nonnull(2, 3, 4, 5, 6)));

/**
 * @brief Validates and copies one bounded non-negative integer key into `spec->field`.
 *
 * @param table      Parsed TOML table holding `spec->key`.
 * @param key_prefix Dotted path of `table` that diagnostics put before `spec->key`, or `""` for the
 *                   top-level table.
 * @param spec       Key policy and target field. Must not be `NULL`.
 * @param log        Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when an optional key is absent, or `-1` on a missing required key, a
 *         wrong type, a value below `spec->value_min`, or a value exceeding `spec->value_max`.
 */
static int copy_size(toml_datum_t table,
                     struct Str key_prefix,
                     const struct SiteConfigIntKey* spec,
                     struct Log* log) __attribute__((nonnull(3, 4)));

/**
 * @brief Validates `base_url` as an absolute HTTP(S) URL and trims any trailing `/`.
 *
 * `copy_string` accepts a relative or scheme-less value, because it checks only presence and type.
 * Such a value then silently produces broken links in every feed and canonical URL. Checking it
 * here keeps that a single config error.
 *
 * This must run after the required keys are copied. `base_url` is empty until then, and an empty
 * URL is rejected.
 *
 * @param site_config Config whose `base_url` is validated and normalized. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when the URL is not absolute, which leaves `base_url` empty.
 */
static int normalize_base_url(struct SiteConfig* site_config, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a URL is absolute: an HTTP(S) scheme followed by a non-empty host.
 *
 * @param url URL to check.
 * @return `true` when `url` has a recognized scheme and a host, `false` otherwise.
 */
static bool is_valid_base_url(struct Str url);

/**
 * @brief Reports whether `url` starts with `scheme`, matching ASCII letters case-insensitively.
 *
 * @param url    URL to inspect.
 * @param scheme Scheme prefix to match, such as `https://`.
 * @return `true` when `url` starts with `scheme`, `false` otherwise.
 */
static bool has_scheme(struct Str url, struct Str scheme);

/**
 * @brief Copies one optional directory key, trims its trailing `/` and rejects an empty result.
 *
 * A trailing slash would otherwise survive into path construction, where it defeats the
 * `<content_dir>/` prefix strip that derives an entry's section and so leaks the content directory
 * name into every URL. Normalizing once here keeps that rule in one place and makes
 * `site_config_print` round-trip the normalized form. The location is not otherwise constrained. An
 * absolute or parent-relative directory stays valid.
 *
 * @param table     Parsed TOML top-level table.
 * @param key       Config key name, used in diagnostics. Must not be `NULL`.
 * @param fallback  Default the field holds after any rejection, so later phases still have a
 *                  directory.
 * @param arena     Arena that owns the copied value. Must not be `NULL`.
 * @param value_out Config field holding the directory, which keeps its value when the key is
 *                  absent. Must not be `NULL`.
 * @param log       Log that receives the error. Must not be `NULL`.
 * @return `0` when the key is absent or accepted, or `-1` when it is not a string, holds a `NUL`
 *         byte, or is empty once trimmed.
 */
static int copy_dir(toml_datum_t table,
                    const char* key,
                    struct Str fallback,
                    struct Arena* arena,
                    struct Str* value_out,
                    struct Log* log) __attribute__((nonnull(2, 4, 5, 6)));

/**
 * @brief Returns `value` with any trailing `/` bytes excluded.
 *
 * @param value Value to trim.
 * @return View of `value` up to the first byte of its trailing `/` run, empty when `value` is
 *         entirely `/`.
 */
static struct Str trim_trailing_slashes(struct Str value);

/**
 * @brief Validates the `permalink` key, translating each verdict into its diagnostic.
 *
 * Splitting the translation from `check_permalink` keeps the grid check free of message wording and
 * leaves this function as the only place that decides what a verdict tells the user.
 *
 * @param pattern Permalink pattern to validate.
 * @param scratch Arena for the sample expansions.
 * @param log     Log that receives every error. Must not be `NULL`.
 * @return `0` when the pattern is valid, or `-1` with a diagnostic naming the verdict.
 */
static int require_valid_permalink(struct Str pattern, struct Arena scratch, struct Log* log)
    __attribute__((nonnull(3)));

/**
 * @brief Validates a permalink pattern by expanding it over the sample grid.
 *
 * Validating the pattern here keeps a bad `permalink` a single config error. If left to the render
 * phase, the render phase catches the same mistake per entry and reports it once for every content
 * file. The pattern is expanded once and every question reads that one grid. Reasoning about the
 * pattern's bytes directly would be a second implementation of `path_is_safe_relative`'s policy and
 * of `permalink_expand`'s meaning, each free to drift. Output path lengths are checked per entry,
 * in the render phase.
 *
 * @param pattern Permalink pattern to check.
 * @param scratch Arena for the sample expansions.
 * @return The first verdict that failed, or `PERMALINK_VALID` when every check passed.
 */
static enum PermalinkVerdict check_permalink(struct Str pattern, struct Arena scratch);

/**
 * @brief Judges the expanded sample grid, one question at a time over the whole grid.
 *
 * @param urls Expansion of the pattern for each section and slug sample, each with its leading `/`.
 *             Must not be `NULL`.
 * @return The first verdict that failed in `enum PermalinkVerdict` precedence, or `PERMALINK_VALID`
 *         when every check passed.
 */
static enum PermalinkVerdict check_permalink_expansions(
    struct Str urls[SECTION_SAMPLE_COUNT][SLUG_SAMPLE_COUNT]) __attribute__((nonnull(1)));

/**
 * @brief Writes one `key = "value"` line, escaping the value as a TOML basic string.
 *
 * Delegates to `print_string` so a scalar value quotes exactly like array items.
 *
 * @param buffer Buffer the line is appended to. Must not be `NULL`.
 * @param key    Terminated key name to write. Must not be `NULL`.
 * @param value  Value to quote and write.
 */
static void site_config_print_key_string(struct Buf* buffer, const char* key, struct Str value)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes one `key = [...]` line of TOML basic strings.
 *
 * Delegates to `print_string` so array items quote exactly like scalar values.
 *
 * @param buffer     Buffer the line is appended to. Must not be `NULL`.
 * @param key        Terminated key name to write. Must not be `NULL`.
 * @param items      Array of `item_count` strings. Must not be `NULL`.
 * @param item_count Number of items in `items`.
 */
static void site_config_print_string_array(struct Buf* buffer,
                                           const char* key,
                                           const struct Str* items,
                                           ptrdiff_t item_count) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes `text` as a quoted TOML basic string, escaping delimiters and control bytes.
 *
 * TOML basic strings forbid raw control bytes, so this escapes them alongside `"` and `\`.
 *
 * @param buffer Buffer the quoted string is appended to. Must not be `NULL`.
 * @param text   Text to quote and escape.
 */
static void print_string(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

void site_config_init(struct SiteConfig* site_config) {
  // This is one literal naming only the non-zero defaults, so a key added to `SiteConfig` cannot be
  // left out here. The three required keys stay empty until the load fills them.
  *site_config = (struct SiteConfig){
      .permalink = PERMALINK_DEFAULT,
      .content_dir = CONTENT_DIR_DEFAULT,
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .static_dir = STATIC_DIR_DEFAULT,
      .content_template = CONTENT_TEMPLATE_DEFAULT,
      .aggregate_templates = AGGREGATE_TEMPLATES_DEFAULT,
      .aggregate_template_count =
          sizeof(AGGREGATE_TEMPLATES_DEFAULT) / sizeof(AGGREGATE_TEMPLATES_DEFAULT[0]),
      .feed_templates = FEED_TEMPLATES_DEFAULT,
      .feed_template_count = sizeof(FEED_TEMPLATES_DEFAULT) / sizeof(FEED_TEMPLATES_DEFAULT[0]),
      .feed_count = FEED_COUNT_DEFAULT,
  };
}

int site_config_load(struct SiteConfig* site_config,
                     struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) {
  // This is zero-initialized, although every path reaching `toml_free` writes it. The struct passes
  // to `toml_free` by value, and without this clang-analyzer cannot prove across the call boundary
  // that its fields are set.
  toml_result_t parsed = {0};
  if (site_config_load_toml(config_path, os, scratch, &parsed, log) != 0) {
    return -1;
  }

  // A key that fails validation is in `log` already, and the load still succeeded in the sense this
  // result reports: the file was read and parsed.
  site_config_load_fields(site_config, parsed.toptab, perm, scratch, log);
  toml_free(parsed);
  return 0;
}

int site_config_print(const struct SiteConfig* site_config, struct Os* os, struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  site_config_print_key_string(&text, "base_url", site_config->base_url);
  site_config_print_key_string(&text, "title", site_config->title);
  site_config_print_key_string(&text, "author", site_config->author);
  site_config_print_key_string(&text, "permalink", site_config->permalink);
  site_config_print_key_string(&text, "content_dir", site_config->content_dir);
  site_config_print_key_string(&text, "output_dir", site_config->output_dir);
  site_config_print_key_string(&text, "templates_dir", site_config->templates_dir);
  site_config_print_key_string(&text, "static_dir", site_config->static_dir);
  site_config_print_key_string(&text, "content_template", site_config->content_template);
  site_config_print_string_array(&text, "aggregate_templates", site_config->aggregate_templates,
                                 site_config->aggregate_template_count);
  site_config_print_string_array(&text, "feed_templates", site_config->feed_templates,
                                 site_config->feed_template_count);
  buf_printf(&text, "feed_count = %td\n", site_config->feed_count);
  return os_print(os, 1, buf_str(&text));
}

static int site_config_load_toml(struct Str config_path,
                                 struct Os* os,
                                 struct Arena scratch,
                                 toml_result_t* parsed_out,
                                 struct Log* log) {
  struct Str config_data = {0};
  if (os_read(os, config_path, CONFIG_FILE_LEN_MAX, &scratch, &config_data,
              S("failed to read config"), log) != 0) {
    return -1;
  }

  // tomlc17 copies the input, so the bytes can stay in `scratch`. The read bounds the length at
  // `CONFIG_FILE_LEN_MAX`, which fits an `int`.
  return toml_parse_document(config_data, config_path, parsed_out, S("failed to parse config"),
                             log);
}

static void site_config_load_fields(struct SiteConfig* site_config,
                                    toml_datum_t table,
                                    struct Arena* perm,
                                    struct Arena scratch,
                                    struct Log* log) {
  // Every key is checked even after one fails, so a config with several mistakes reports all of
  // them in one run. A rejected value is never stored: the key keeps its default, so a build that
  // goes on to check its content does so against a usable config rather than reporting the same
  // mistake again once per content file.
  (void)toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                                S("config"), S(""), (struct Str){0}, log);

  const bool is_base_url_copied =
      copy_string(table, "base_url", true, perm, &site_config->base_url, log) == 0;
  const struct SiteConfigStringKey strings[] = {
      {"title", &site_config->title, true},
      {"author", &site_config->author, true},
      {"permalink", &site_config->permalink, false},
      {"content_template", &site_config->content_template, false},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(strings) / sizeof(strings[0])); i++) {
    (void)copy_string(table, strings[i].key, strings[i].is_required, perm, strings[i].field, log);
  }

  // A `base_url` that is missing or mistyped is already reported, so its empty value is not judged
  // a second time.
  if (is_base_url_copied) {
    (void)normalize_base_url(site_config, log);
  }
  site_config->is_content_dir_rejected = copy_dir(table, "content_dir", CONTENT_DIR_DEFAULT, perm,
                                                  &site_config->content_dir, log) != 0;
  site_config->is_output_dir_rejected =
      copy_dir(table, "output_dir", OUTPUT_DIR_DEFAULT, perm, &site_config->output_dir, log) != 0;
  site_config->is_templates_dir_rejected = copy_dir(table, "templates_dir", TEMPLATES_DIR_DEFAULT,
                                                    perm, &site_config->templates_dir, log) != 0;
  site_config->is_static_dir_rejected =
      copy_dir(table, "static_dir", STATIC_DIR_DEFAULT, perm, &site_config->static_dir, log) != 0;
  if (!path_is_safe_relative(site_config->content_template)) {
    (void)log_error(log,
                    "config key 'content_template' must be a safe relative template name: '%.*s'",
                    STR_FMT(site_config->content_template));
    site_config->content_template = CONTENT_TEMPLATE_DEFAULT;
  }
  if (require_valid_permalink(site_config->permalink, scratch, log) != 0) {
    site_config->permalink = PERMALINK_DEFAULT;
    site_config->is_output_key_rejected = true;
  }
  if (copy_template_names(table, "aggregate_templates", perm, &site_config->aggregate_templates,
                          &site_config->aggregate_template_count, log) != 0) {
    site_config->is_output_key_rejected = true;
  }
  if (copy_template_names(table, "feed_templates", perm, &site_config->feed_templates,
                          &site_config->feed_template_count, log) != 0) {
    site_config->is_output_key_rejected = true;
  }

  const struct SiteConfigIntKey feed_count = {
      .key = "feed_count",
      .field = &site_config->feed_count,
      // `PTRDIFF_MAX` binds only where `ptrdiff_t` is narrower than `int64_t`, as on a 32-bit
      // target.
      .value_max = PTRDIFF_MAX,
      .limit_name = "feed count",
  };
  (void)copy_size(table, S(""), &feed_count, log);
}

static int copy_string(toml_datum_t table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct Str* value_out,
                       struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    if (is_required) {
      return log_error(log, "missing required config key '%s'", key);
    }
    return 0;
  }
  if (value.type != TOML_STRING) {
    return log_error(log, "config key '%s' must be a string", key);
  }
  if (!toml_datum_is_text(value)) {
    return log_error(log, "config key '%s' must not contain a NUL byte", key);
  }
  *value_out = str_clone(toml_datum_str(value), arena);
  return 0;
}

static int copy_template_names(toml_datum_t table,
                               const char* key,
                               struct Arena* arena,
                               const struct Str** template_names_out,
                               ptrdiff_t* template_name_count_out,
                               struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_ARRAY) {
    return log_error(log, "config key '%s' must be an array", key);
  }

  const ptrdiff_t item_count = value.u.arr.size;
  // This allocates even for an empty array, rather than leaving it `NULL`. A zero-count allocation
  // still returns a pointer. Every consumer of the published array is declared `nonnull` on it. If
  // left `NULL`, `aggregate_templates = []` would reach `site_config_print_string_array` as a null
  // pointer and abort.
  struct Str* items = arena_new(arena, item_count, struct Str);

  // Every element is checked even after one fails, so each unsafe name is reported. A wrong type or
  // a `NUL` byte is reported once, since its message names no element and a repeat would add
  // nothing.
  int rc = 0;
  bool is_type_reported = false;
  bool is_nul_reported = false;
  for (ptrdiff_t i = 0; i < item_count; i++) {
    const toml_datum_t item = value.u.arr.elem[i];
    if (item.type != TOML_STRING) {
      if (!is_type_reported) {
        rc = log_error(log, "config key '%s' must contain only strings", key);
        is_type_reported = true;
      }
      continue;
    }
    if (!toml_datum_is_text(item)) {
      if (!is_nul_reported) {
        rc = log_error(log, "config key '%s' must not contain a NUL byte", key);
        is_nul_reported = true;
      }
      continue;
    }
    const struct Str name = toml_datum_str(item);
    if (!path_is_safe_relative(name)) {
      rc = log_error(log, "config key '%s' must contain only safe relative template names: '%.*s'",
                     key, STR_FMT(name));
      continue;
    }
    items[i] = str_clone(name, arena);
  }
  if (rc != 0) {
    return -1;
  }

  // The code fills these through a mutable local and publishes them as `const struct Str*`. The
  // elements have to be writable while it builds the arena copy, but must not be once the config
  // shares them with the render workers.
  *template_names_out = items;
  *template_name_count_out = item_count;
  return 0;
}

static int copy_size(toml_datum_t table,
                     struct Str key_prefix,
                     const struct SiteConfigIntKey* spec,
                     struct Log* log) {
  const toml_datum_t value = toml_get(table, spec->key);
  if (value.type == TOML_UNKNOWN) {
    if (spec->is_required) {
      return log_error(log, "missing required config key '%.*s%s'", STR_FMT(key_prefix), spec->key);
    }
    return 0;
  }
  if (value.type != TOML_INT64) {
    return log_error(log, "config key '%.*s%s' must be an integer", STR_FMT(key_prefix), spec->key);
  }
  if (value.u.int64 < spec->value_min) {
    return log_error(log, "config key '%.*s%s' must be at least %td: %" PRId64, STR_FMT(key_prefix),
                     spec->key, spec->value_min, value.u.int64);
  }
  // One comparison covers both the configured ceiling and the conversion below. `value_max` is a
  // `ptrdiff_t`, so a value this check accepts fits `ptrdiff_t` on every target.
  if (value.u.int64 > spec->value_max) {
    return log_error(log, "config key '%.*s%s' exceeds max %s (%td) at %" PRId64,
                     STR_FMT(key_prefix), spec->key, spec->limit_name, spec->value_max,
                     value.u.int64);
  }
  *spec->field = (ptrdiff_t)value.u.int64;
  return 0;
}

static int normalize_base_url(struct SiteConfig* site_config, struct Log* log) {
  if (!is_valid_base_url(site_config->base_url)) {
    (void)log_error(
        log,
        "config key 'base_url' must be an absolute 'http://' or 'https://' URL with a host: '%.*s'",
        STR_FMT(site_config->base_url));
    // A required key has no default, so a rejected one goes back to empty.
    site_config->base_url = (struct Str){0};
    return -1;
  }

  // Trim a trailing `/` so templates can join `base_url` with an entry's `url`, which always starts
  // with one, without producing `//`. Normalizing once here keeps the rule in one place and makes
  // `site_config_print` round-trip the normalized form, as `copy_dir` does for directories. The
  // trim cannot eat the scheme's slashes, because the check above proved a host byte follows them.
  // The value is a counted view, so trimming shortens it in place.
  site_config->base_url = trim_trailing_slashes(site_config->base_url);
  return 0;
}

static bool is_valid_base_url(struct Str url) {
  /**
   * It accepts a scheme plus a non-empty host, and nothing more. A port, subpath, or query string
   * must stay valid, and judging a host beyond "not empty" would mean ruling on what DNS and
   * punycode allow. Schemes are case-insensitive, so `HTTPS://` is the same URL.
   */
  static const struct Str URL_SCHEMES[] = {STR_INIT("http://"), STR_INIT("https://")};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(URL_SCHEMES) / sizeof(URL_SCHEMES[0])); i++) {
    if (has_scheme(url, URL_SCHEMES[i])) {
      // `https://` has no host at all. `https:///path`, `https://?q`, `https://#f` and
      // `https://:8080` have an empty host: `/`, `?`, `#`, or the port's `:` starts immediately.
      const struct Str host = str_tail(url, URL_SCHEMES[i].len);
      return host.len > 0 && host.data[0] != '/' && host.data[0] != '?' && host.data[0] != '#' &&
             host.data[0] != ':';
    }
  }
  return false;
}

static bool has_scheme(struct Str url, struct Str scheme) {
  if (url.len < scheme.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < scheme.len; i++) {
    if (ascii_to_lower((unsigned char)url.data[i]) !=
        ascii_to_lower((unsigned char)scheme.data[i])) {
      return false;
    }
  }
  return true;
}

static int copy_dir(toml_datum_t table,
                    const char* key,
                    struct Str fallback,
                    struct Arena* arena,
                    struct Str* value_out,
                    struct Log* log) {
  if (copy_string(table, key, false, arena, value_out, log) != 0) {
    return -1;
  }
  *value_out = trim_trailing_slashes(*value_out);
  if (value_out->len == 0) {
    *value_out = fallback;
    return log_error(log, "config key '%s' must not be empty", key);
  }
  return 0;
}

static struct Str trim_trailing_slashes(struct Str value) {
  while (str_endswith(value, S("/"))) {
    value = str_head(value, value.len - 1);
  }
  return value;
}

static int require_valid_permalink(struct Str pattern, struct Arena scratch, struct Log* log) {
  switch (check_permalink(pattern, scratch)) {
    case PERMALINK_VALID:
      break;
    case PERMALINK_UNSAFE:
      return log_error(log,
                       "config key 'permalink' must expand to a safe relative path using only "
                       "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                       "with no empty, '.' or '..' path segment: '%.*s'",
                       STR_FMT(pattern));
    case PERMALINK_NOT_DISTINCT:
      return log_error(log,
                       "config key 'permalink' must expand to a distinct path per content entry; "
                       "include the '{slug}' token: '%.*s'",
                       STR_FMT(pattern));
  }
  return 0;
}

static enum PermalinkVerdict check_permalink(struct Str pattern, struct Arena scratch) {
  struct Str urls[SECTION_SAMPLE_COUNT][SLUG_SAMPLE_COUNT];
  for (ptrdiff_t i = 0; i < SECTION_SAMPLE_COUNT; i++) {
    for (ptrdiff_t j = 0; j < SLUG_SAMPLE_COUNT; j++) {
      urls[i][j] = permalink_expand(pattern, SECTION_SAMPLES[i], SLUG_SAMPLES[j], &scratch);
    }
  }
  return check_permalink_expansions(urls);
}

static enum PermalinkVerdict check_permalink_expansions(
    struct Str urls[SECTION_SAMPLE_COUNT][SLUG_SAMPLE_COUNT]) {
  // Each question runs over the whole grid before the next one starts, so the verdict follows the
  // precedence `enum PermalinkVerdict` documents whichever expansion trips which check. Checking
  // one expansion at a time would let an early expansion fail a later question before a later
  // expansion failed an earlier one. A pattern that is unsafe only with an empty `{section}` would
  // then report a slug collision instead.
  for (ptrdiff_t i = 0; i < SECTION_SAMPLE_COUNT; i++) {
    for (ptrdiff_t j = 0; j < SLUG_SAMPLE_COUNT; j++) {
      if (!path_is_safe_relative(str_tail(urls[i][j], 1))) {
        return PERMALINK_UNSAFE;
      }
    }
  }
  for (ptrdiff_t i = 0; i < SECTION_SAMPLE_COUNT; i++) {
    for (ptrdiff_t j = 1; j < SLUG_SAMPLE_COUNT; j++) {
      if (str_eq(urls[i][0], urls[i][j])) {
        return PERMALINK_NOT_DISTINCT;
      }
    }
  }
  return PERMALINK_VALID;
}

static void site_config_print_key_string(struct Buf* buffer, const char* key, struct Str value) {
  buf_printf(buffer, "%s = ", key);
  print_string(buffer, value);
  buf_append_char(buffer, '\n');
}

static void site_config_print_string_array(struct Buf* buffer,
                                           const char* key,
                                           const struct Str* items,
                                           ptrdiff_t item_count) {
  buf_printf(buffer, "%s = [", key);
  for (ptrdiff_t i = 0; i < item_count; i++) {
    if (i > 0) {
      buf_append(buffer, S(", "));
    }
    print_string(buffer, items[i]);
  }
  buf_append(buffer, S("]\n"));
}

static void print_string(struct Buf* buffer, struct Str text) {
  buf_append_char(buffer, '"');
  // Walk the bytes as `unsigned char`. Where `char` is signed, every byte of a multi-byte UTF-8
  // sequence is negative, so the `< 0x20` test below would treat it as a control byte, and passing
  // a negative `int` to the `%04X` conversion is undefined.
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    switch (c) {
      case '"':
        buf_append(buffer, S("\\\""));
        break;
      case '\\':
        buf_append(buffer, S("\\\\"));
        break;
      case '\b':
        buf_append(buffer, S("\\b"));
        break;
      case '\t':
        buf_append(buffer, S("\\t"));
        break;
      case '\n':
        buf_append(buffer, S("\\n"));
        break;
      case '\f':
        buf_append(buffer, S("\\f"));
        break;
      case '\r':
        buf_append(buffer, S("\\r"));
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          buf_printf(buffer, "\\u%04X", c);
        } else {
          buf_append_char(buffer, (char)c);
        }
        break;
    }
  }
  buf_append_char(buffer, '"');
}
