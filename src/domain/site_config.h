#ifndef SOSIG_SITE_CONFIG_H
#define SOSIG_SITE_CONFIG_H

#include <stddef.h>
#include <stdio.h>

#include "core/arena.h"

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
extern const char* const SITE_CONFIG_PATH_DEFAULT;

/** Site configuration loaded from `sosig.toml`. */
struct SiteConfig {
  /** Owns strings copied from configuration. */
  struct Arena arena;

  /** Required absolute base URL used by templates. */
  const char* base_url;

  /** Required site title used by templates. */
  const char* title;

  /** Required author name used by templates. */
  const char* author;

  /**
   * Permalink pattern expanded into each entry's URL and output path. Defaults to
   * `/{section}/{slug}.html`. Supports the `{slug}` and `{section}` tokens, where `{section}` is
   * the source directory relative to `content_dir`.
   */
  const char* permalink;

  /** Directory containing Markdown content. Defaults to `content`. */
  const char* content_dir;

  /** Directory where generated files are written. Defaults to `public`. */
  const char* output_dir;

  /** Directory containing templates. Defaults to `templates`. */
  const char* templates_dir;

  /** Default safe relative template name for content entries. Defaults to `content.html`. */
  const char* content_template;

  /**
   * Safe relative template names rendered as aggregate outputs. Defaults to `[index.html]`. It is
   * deeply `const` as a thread-safety invariant. Render workers reach this config through
   * `TemplateContext`, and the default arrays only land in read-only data while their elements are
   * `const` too. Do not relax to `const char**`.
   */
  const char* const* aggregate_templates;

  /** Number of entries in `aggregate_templates`. */
  size_t aggregate_template_count;

  /** Safe relative template names rendered with feed-limited context. Defaults to `[atom.xml]`. */
  const char* const* feed_templates;

  /** Number of entries in `feed_templates`. */
  size_t feed_template_count;

  /** Maximum number of content entries included in the feed. Defaults to `10`. */
  size_t feed_count;
};

/**
 * @brief Initializes a site config with defaults for optional configuration keys.
 *
 * Prepares the config's arena and sets defaults for optional keys, leaving required keys unset for
 * `site_config_load` to fill.
 *
 * @param site_config Config handle to prepare. Must not be `NULL`.
 */
void site_config_init(struct SiteConfig* site_config) __attribute__((nonnull(1)));

/**
 * @brief Releases arena-owned config data and resets it for reuse.
 *
 * Invalidates every arena-owned pointer on the config. The config stays initialized, so it may be
 * reused without calling `site_config_init`.
 *
 * @param site_config Config to release. Must not be `NULL`.
 */
void site_config_free(struct SiteConfig* site_config) __attribute__((nonnull(1)));

/**
 * @brief Loads `sosig.toml` configuration into `site_config`.
 *
 * Reads and parses the file, validates required keys and template names, and copies values into the
 * config's arena over the defaults from `site_config_init`.
 *
 * @param site_config Initialized config that receives the loaded values. Must not be `NULL`.
 * @param config_path Path to the TOML configuration file. Must not be `NULL`.
 * @param err         Buffer for a diagnostic message on failure.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on a read, parse, or validation error (with a diagnostic in
 *         `err`).
 */
int site_config_load(struct SiteConfig* site_config,
                     const char* config_path,
                     char* err,
                     size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the effective configuration to `stream` as valid TOML.
 *
 * The output round-trips. Reloading it through `site_config_load` yields the same values.
 *
 * @param stream      Destination stream. Must not be `NULL`.
 * @param site_config Config to serialize. Must be fully populated (as after a successful
 *                    `site_config_load`), with `base_url`, `title`, and `author` non-`NULL`. Must
 * not be `NULL`.
 * @return `0` on success, or `-1` if writing to `stream` failed, with `errno` set by the failing
 *         write, or to `EIO` when the stream had latched an error earlier and the original `errno`
 *         is no longer available, so the caller always has a reason to report.
 */
int site_config_print(FILE* stream, const struct SiteConfig* site_config)
    __attribute__((nonnull(1, 2)));

#endif
