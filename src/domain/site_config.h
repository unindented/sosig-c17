#ifndef SOSIG_SITE_CONFIG_H
#define SOSIG_SITE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;
struct Os;

/**
 * Path every command loads its site configuration from, relative to the working directory.
 *
 * It is declared here rather than inside a command, because two commands need it and must agree on
 * it. `cmd_config_run` reads it, and `cmd_build_run` both reads it and claims it as a build input,
 * so no output path can overwrite it. `site_config_load` still takes the path as an argument,
 * because tests load fixtures from elsewhere. This is the default the commands pass, not a bound on
 * the loader.
 *
 * It is `extern`, with the definition in `site_config.c`, rather than a `static` initializer in
 * this header. Every translation unit then shares one object instead of getting a copy of its own
 * that most of them never read.
 */
extern const struct Str SITE_CONFIG_PATH_DEFAULT;

/**
 * Site configuration loaded from `sosig.toml`. Every string and array it points at is either a
 * default literal or a copy in the arena `site_config_load` was given.
 */
struct SiteConfig {
  /** Required absolute base URL used by templates. */
  struct Str base_url;

  /** Required site title used by templates. */
  struct Str title;

  /** Required author name used by templates. */
  struct Str author;

  /**
   * Permalink pattern expanded into each entry's URL and output path. Defaults to
   * `/{section}/{slug}.html`. Supports the `{slug}` and `{section}` tokens, where `{section}` is
   * the source directory relative to `content_dir`.
   */
  struct Str permalink;

  /** Directory containing the content source files. Defaults to `content`. */
  struct Str content_dir;

  /** Directory where generated files are written. Defaults to `public`. */
  struct Str output_dir;

  /** Directory containing templates. Defaults to `templates`. */
  struct Str templates_dir;

  /** Directory containing static files copied as-is into `output_dir`. Defaults to `static`. */
  struct Str static_dir;

  /** Default safe relative template name for content entries. Defaults to `content.html`. */
  struct Str content_template;

  /**
   * Safe relative template names rendered as aggregate outputs. Defaults to `[index.html]`. It is
   * `const` as a thread-safety invariant. Render workers reach this config through
   * `TemplateContext`, and the default arrays only land in read-only data while they are `const`.
   */
  const struct Str* aggregate_templates;

  /** Number of names in `aggregate_templates`. */
  ptrdiff_t aggregate_template_count;

  /** Safe relative template names rendered with feed-limited context. Defaults to `[atom.xml]`. */
  const struct Str* feed_templates;

  /** Number of names in `feed_templates`. */
  ptrdiff_t feed_template_count;

  /** Maximum number of content entries included in the feed. Defaults to `10`. */
  ptrdiff_t feed_count;

  /**
   * Whether the load rejected `content_dir`, which then holds its default rather than a directory
   * the user chose.
   */
  bool is_content_dir_rejected;

  /** Whether the load rejected `output_dir`, which then holds its default. */
  bool is_output_dir_rejected;

  /** Whether the load rejected `templates_dir`, which then holds its default. */
  bool is_templates_dir_rejected;

  /** Whether the load rejected `static_dir`, which then holds its default. */
  bool is_static_dir_rejected;

  /**
   * Whether the load rejected `permalink`, `aggregate_templates` or `feed_templates`, which then
   * hold defaults that name outputs the user never asked for.
   */
  bool is_output_key_rejected;
};

/**
 * @brief Initializes a site config with defaults for optional configuration keys.
 *
 * Sets defaults for optional keys, leaving required keys unset for `site_config_load` to fill.
 *
 * @param site_config Config to prepare. Must not be `NULL`.
 */
void site_config_init(struct SiteConfig* site_config) __attribute__((nonnull(1)));

/**
 * @brief Loads `sosig.toml` configuration into `site_config`.
 *
 * Reads and parses the file, rejects a key outside the schema, validates required keys and template
 * names, and copies values into `perm` over the defaults from `site_config_init`.
 *
 * @param site_config Initialized config that receives the loaded values. Must not be `NULL`.
 * @param config_path Path to the TOML configuration file.
 * @param os          Host the file is read from. Must not be `NULL`.
 * @param perm        Arena that owns the loaded values for as long as the config is used. Must not
 *                    be `NULL`.
 * @param scratch     Arena for the file bytes and validation temporaries.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` when the file was read and parsed, or `-1` when it could not be. A key that fails
 *         validation is logged and keeps its default, and the load goes on to check every other
 *         key, so `0` does not mean the config is valid: a caller checks `log->errors` before it
 *         relies on the values, and checks no directory whose key was rejected. That lets a build
 *         report a bad key and the errors in its content in one run, while a file that is missing
 *         or not TOML stops it, because there is nothing to check the content against.
 */
int site_config_load(struct SiteConfig* site_config,
                     struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Writes the effective configuration to standard output as valid TOML.
 *
 * The output round-trips. Reloading it through `site_config_load` yields the same values. The text
 * is formatted whole in `scratch` and printed in one `os_print` call.
 *
 * @param site_config Config to serialize. Must be fully populated (as after a successful
 *                    `site_config_load`). Must not be `NULL`.
 * @param os          Host to print on. Must not be `NULL`.
 * @param scratch     Arena the text is formatted in.
 * @return `0` on success, or `-1` when the write fails.
 */
int site_config_print(const struct SiteConfig* site_config, struct Os* os, struct Arena scratch)
    __attribute__((nonnull(1, 2)));

#endif
