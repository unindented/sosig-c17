#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/error.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 8192 };

/** Fixture root template for a config written by `write_temp_config`. */
#define TEMP_CONFIG_ROOT_TEMPLATE "/tmp/sosig-site-config-test.XXXXXX"

/** A `sosig.toml` written alone into a fresh fixture root. */
struct TempConfig {
  /** Fixture root, which `remove_fixture_tree` removes together with the config. */
  char root_dir[sizeof(TEMP_CONFIG_ROOT_TEMPLATE)];

  /** Path of the written config. */
  char path[sizeof(TEMP_CONFIG_ROOT_TEMPLATE) + sizeof("/sosig.toml") - 1];
};

/**
 * @brief Loads a config with a fresh long-lived arena and a fresh scratch arena.
 *
 * Each call reserves its own two arenas, so the loaded values stay valid for the rest of the test
 * and never overlap the scratch the load worked in.
 *
 * @param site_config Initialized config that receives the values. Must not be `NULL`.
 * @param config_path Terminated path of the config to load. Must not be `NULL`.
 * @param log         Log that receives every error the load records. Must not be `NULL`.
 * @return `0` when the config was loaded with no error, or `-1` when it could not be read or
 *         parsed, or any key was rejected. `site_config_load` itself returns `0` in the last case,
 *         which `test_load_returns_zero_with_key_errors` pins.
 */
static int load_site_config(struct SiteConfig* site_config,
                            const char* config_path,
                            struct Log* log) {
  struct Arena perm = test_arena();
  const int rc = site_config_load(site_config, str_from_cstr(config_path), os_posix(), &perm,
                                  test_arena(), log);
  return rc != 0 || log->errors > 0 ? -1 : 0;
}

/**
 * @brief Renders a site configuration into a terminated text buffer.
 *
 * `site_config_print` writes to standard output, so its descriptor is captured while it runs.
 *
 * @param site_config  Configuration to render.
 * @param text_out     Buffer that receives the rendered text.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test failure.
 */
static int render(const struct SiteConfig* site_config, char* text_out, ptrdiff_t text_out_len) {
  text_out[0] = '\0';
  struct StreamCapture capture;
  if (capture_begin(stdout, &capture) != 0) {
    return -1;
  }
  const bool is_printed = TEST_CHECK(site_config_print(site_config, os_posix(), test_arena()) == 0);
  const bool is_captured = capture_end(&capture, text_out, text_out_len) == 0;
  return is_printed && is_captured ? 0 : -1;
}

/**
 * @brief Writes TOML to `sosig.toml` in a fresh fixture root.
 *
 * On success the caller removes the root with `remove_fixture_tree(temp_config_out->root_dir)`. On
 * failure no fixture is left behind.
 *
 * @param temp_config_out Receives the fixture root and the config path.
 * @param toml            Terminated TOML text to write.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int write_temp_config(struct TempConfig* temp_config_out, const char* toml) {
  memcpy(temp_config_out->root_dir, TEMP_CONFIG_ROOT_TEMPLATE, sizeof(TEMP_CONFIG_ROOT_TEMPLATE));
  if (init_fixture_dir(temp_config_out->root_dir) != 0) {
    return -1;
  }
  const int n = snprintf(temp_config_out->path, sizeof(temp_config_out->path), "%s/sosig.toml",
                         temp_config_out->root_dir);
  if (!TEST_CHECK(n > 0 && (size_t)n < sizeof(temp_config_out->path))) {
    remove_fixture_tree(temp_config_out->root_dir);
    return -1;
  }
  if (!TEST_CHECK(write_fixture_file(temp_config_out->root_dir, "sosig.toml", toml) == 0)) {
    remove_fixture_tree(temp_config_out->root_dir);
    return -1;
  }
  return 0;
}

/**
 * @brief Loads a config with the given extra TOML and checks it fails with one diagnostic.
 *
 * @param toml_extra Terminated TOML appended to the required keys of an otherwise valid config.
 * @param expected   Exact diagnostic `site_config_load` must report.
 */
static void check_load_rejects(const char* toml_extra, const char* expected) {
  char toml[4096];
  const int n = snprintf(toml, sizeof(toml),
                         "base_url = \"https://example.com\"\n"
                         "title = \"Example Site\"\n"
                         "author = \"Example Author\"\n"
                         "%s",
                         toml_extra);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));

  remove_fixture_tree(temp_config.root_dir);
}

/**
 * @brief Loads a config with the given permalink and checks it fails with one diagnostic.
 *
 * @param permalink Permalink pattern to load, embedded in an otherwise valid config.
 * @param expected  Exact diagnostic `site_config_load` must report.
 */
static void check_load_rejects_permalink(const char* permalink, const char* expected) {
  char toml_extra[512];
  const int n = snprintf(toml_extra, sizeof(toml_extra), "permalink = \"%s\"\n", permalink);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
  check_load_rejects(toml_extra, expected);
}

// Loading a file with only the required keys fills the rest from `site_config_init`'s defaults.
static void test_load_applies_required_and_defaults(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);

  TEST_CHECK(str_eq(config.base_url, S("https://example.com")));
  TEST_CHECK(str_eq(config.title, S("Example Site")));
  TEST_CHECK(str_eq(config.author, S("Example Author")));
  TEST_CHECK(str_eq(config.permalink, S("/{section}/{slug}.html")));
  TEST_CHECK(str_eq(config.content_dir, S("content")));
  TEST_CHECK(str_eq(config.output_dir, S("public")));
  TEST_CHECK(str_eq(config.templates_dir, S("templates")));
  TEST_CHECK(str_eq(config.static_dir, S("static")));
  TEST_CHECK(str_eq(config.content_template, S("content.html")));
  if (!TEST_CHECK(config.aggregate_template_count == 1)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(config.aggregate_templates[0], S("index.html")));
  if (!TEST_CHECK(config.feed_template_count == 1)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(config.feed_templates[0], S("atom.xml")));
  TEST_CHECK(config.feed_count == 10);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// Loading a file that sets every optional key overrides each corresponding default.
static void test_load_overrides_optional_keys(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "permalink = \"/{slug}/index.html\"\n"
      "content_dir = \"posts\"\n"
      "output_dir = \"dist\"\n"
      "templates_dir = \"layouts\"\n"
      "static_dir = \"files\"\n"
      "content_template = \"post.html\"\n"
      "aggregate_templates = [\"index.html\", \"archive.html\"]\n"
      "feed_templates = []\n"
      "feed_count = 5\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);

  TEST_CHECK(str_eq(config.permalink, S("/{slug}/index.html")));
  TEST_CHECK(str_eq(config.content_dir, S("posts")));
  TEST_CHECK(str_eq(config.output_dir, S("dist")));
  TEST_CHECK(str_eq(config.templates_dir, S("layouts")));
  TEST_CHECK(str_eq(config.static_dir, S("files")));
  TEST_CHECK(str_eq(config.content_template, S("post.html")));
  if (!TEST_CHECK(config.aggregate_template_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(config.aggregate_templates[0], S("index.html")));
  TEST_CHECK(str_eq(config.aggregate_templates[1], S("archive.html")));
  TEST_CHECK(config.feed_template_count == 0);
  TEST_CHECK(config.feed_count == 5);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// Directory keys have trailing separators trimmed at load. Left in place, a trailing slash defeats
// the `<content_dir>/` prefix strip during section derivation and leaks the content directory name
// into every URL. The end-to-end consequence is asserted in the `cmd_build` tests. An absolute or
// parent-relative directory survives normalization unchanged.
static void test_load_normalizes_directory_keys(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "content_dir = \"content/\"\n"
      "output_dir = \"/srv/www///\"\n"
      "templates_dir = \"../shared/templates/\"\n"
      "static_dir = \"files/\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(str_eq(config.content_dir, S("content")));
  TEST_CHECK(str_eq(config.output_dir, S("/srv/www")));
  TEST_CHECK(str_eq(config.templates_dir, S("../shared/templates")));
  TEST_CHECK(str_eq(config.static_dir, S("files")));

  remove_fixture_tree(temp_config.root_dir);
}

// An absolute `base_url` keeps its scheme, host, port, path and query, and loses only a trailing
// `/`, which templates would otherwise double when joining it with an entry's `url`.
static void test_load_normalizes_base_url(void) {
  static const char* const cases[][2] = {
      {"https://example.com", "https://example.com"},
      {"https://example.com/", "https://example.com"},
      {"https://example.com///", "https://example.com"},
      {"http://example.com/blog/", "http://example.com/blog"},
      {"https://example.com:8443/blog?draft=1", "https://example.com:8443/blog?draft=1"},
      {"HTTPS://Example.COM", "HTTPS://Example.COM"},  // the scheme matches case-insensitively
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"%s\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);
    TEST_CHECK(log.errors == 0);
    TEST_CHECK(str_eq(config.base_url, str_from_cstr(cases[i][1])));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// Overlapping directories load, because comparing path text cannot tell whether two directories
// overlap. The build compares them by identity before it walks or writes anything.
static void test_load_leaves_directory_overlap_to_the_build(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "content_dir = \"posts\"\n"
      "output_dir = \"posts/generated\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(str_eq(config.content_dir, S("posts")));
  TEST_CHECK(str_eq(config.output_dir, S("posts/generated")));

  remove_fixture_tree(temp_config.root_dir);
}

// Permalink patterns that expand to a safe relative path are accepted, including a directory-style
// pattern and one that omits the section.
static void test_load_accepts_valid_permalinks(void) {
  static const char* const permalinks_valid[] = {
      "/{section}/{slug}.html", "/{slug}.html", "/{section}/{slug}/",
      "/posts/{slug}.html",     "{slug}",       "/a.b-c_d/{slug}.html",
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(permalinks_valid) / sizeof(permalinks_valid[0]));
       i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_valid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);
    TEST_CHECK(log.errors == 0);
    TEST_CHECK(str_eq(config.permalink, str_from_cstr(permalinks_valid[i])));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// Zero is accepted, which is the boundary below every other accepting case in this file. A guard of
// `< 1` on `feed_count` would reject a legal config while still passing all of them. Zero means a
// feed template sees no entries, which is a configuration a user can ask for.
static void test_load_accepts_zero_feed_count(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "feed_count = 0\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(config.feed_count == 0);

  remove_fixture_tree(temp_config.root_dir);
}

// Loading a config path that does not exist reports a read failure naming the path and cause.
static void test_load_rejects_missing_file(void) {
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  const char* missing_path = "/tmp/sosig-site-config-test-missing.toml";
  TEST_CHECK(load_site_config(&config, missing_path, &log) == -1);
  // The cause distinguishes a missing file from an unreadable or malformed one. Derived from the
  // running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('%s')",
               error_system_message(reason, sizeof(reason), ENOENT), missing_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(test_log_is_one(&log, expected));
}

// A config larger than `CONFIG_FILE_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size. The constant is file-local to `site_config.c`, so the 1 MiB below is spelled
// out and must change with it. The file is sparse, so the fixture costs no disk.
static void test_load_rejects_oversize_file(void) {
  enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, "title = \"Site\"\n") != 0) {
    return;
  }
  const off_t config_len = (off_t)CONFIG_FILE_LEN_MAX + 1;
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: exceeds max file size (%d bytes) at %jd bytes ('%s')",
               CONFIG_FILE_LEN_MAX, (intmax_t)config_len, temp_config.path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(truncate(temp_config.path, config_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// A config file holding an embedded `NUL` is rejected at the read, before the parse, naming the
// config path. `os_read` rejects it for every caller, because the byte has no meaning in any format
// `sosig` reads or writes.
static void test_load_rejects_nul_in_file(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, "") != 0) {
    return;
  }
  static const char config_data[] = "title = \"Site\"\0author = \"Example Author\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: contains an embedded NUL byte ('%s')", temp_config.path);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  if (!TEST_CHECK(os_write(os_posix(), str_from_cstr(temp_config.path),
                           (struct OsSource){.bytes = {config_data, sizeof(config_data) - 1}},
                           S("failed to write fixture"), &log) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// `site_config_load` reports a syntactically malformed config as a parse failure naming the config
// path, rather than reaching the field pass with an empty table and reporting a false missing-key
// error.
static void test_load_rejects_malformed_toml(void) {
  const char toml[] = "base_url = \n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  // The prefix and the trailing attribution are this module's own contribution. The text between
  // them is the vendored parser's `errmsg`, left unpinned so rewording it upstream does not fail
  // this test. The two together are still the whole first-party claim, which neither the
  // missing-key message nor the read-failure message could satisfy.
  const struct Str prefix = S("failed to parse config: ");
  const struct Str message = buf_str(&log.buf);
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(str_startswith(message, prefix));
  char suffix[EXPECTED_SIZE];
  const int n = snprintf(suffix, sizeof(suffix), " (in '%s')\n", temp_config.path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(suffix));
  // A cause of at least one byte has to sit between the two, or the parser reported nothing.
  TEST_CHECK(message.len > prefix.len + n);
  TEST_CHECK(str_endswith(message, (struct Str){suffix, n}));

  remove_fixture_tree(temp_config.root_dir);
}

/**
 * A config that parses but holds six independent mistakes, one each for the unknown-key check, a
 * string key's type, `base_url`, a directory, the permalink, and `feed_count`.
 */
static const char TOML_KEYS_INVALID[] =
    "base_url = \"ftp://example.com\"\n"
    "title = 1\n"
    "author = \"Example Author\"\n"
    "colour = \"blue\"\n"
    "content_dir = \"/\"\n"
    "permalink = \"/fixed.html\"\n"
    "feed_count = -1\n";

// Every invalid key in one config is reported in one load, in the order the load checks them, not
// only the first.
static void test_load_reports_every_invalid_key(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, TOML_KEYS_INVALID) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown config key 'colour'\n"
                         "config key 'title' must be a string\n"
                         "config key 'base_url' must be an absolute 'http://' or 'https://' URL "
                         "with a host: 'ftp://example.com'\n"
                         "config key 'content_dir' must not be empty\n"
                         "config key 'permalink' must expand to a distinct path per content entry; "
                         "include the '{slug}' token: '/fixed.html'\n"
                         "config key 'feed_count' must be at least 0: -1\n"));
  TEST_CHECK(log.errors == 6);

  remove_fixture_tree(temp_config.root_dir);
}

// A config that was read and parsed loads successfully even when keys are rejected: the errors are
// in the log, and the result only says whether there was a file to check.
static void test_load_returns_zero_with_key_errors(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, TOML_KEYS_INVALID) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  struct Arena perm = test_arena();
  TEST_CHECK(site_config_load(&config, str_from_cstr(temp_config.path), os_posix(), &perm,
                              test_arena(), &log) == 0);
  TEST_CHECK(log.errors == 6);

  remove_fixture_tree(temp_config.root_dir);
}

// A rejected value is never stored. Each rejected key keeps the default `site_config_init` gave it,
// so later checks run against a usable config, while the valid keys around them still load. A
// rejected directory key is marked as such.
static void test_load_keeps_defaults_for_rejected_keys(void) {
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, TOML_KEYS_INVALID) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(str_eq(config.content_dir, S("content")));
  TEST_CHECK(config.is_content_dir_rejected);
  TEST_CHECK(!config.is_output_dir_rejected);
  TEST_CHECK(!config.is_templates_dir_rejected);
  TEST_CHECK(!config.is_static_dir_rejected);
  TEST_CHECK(str_eq(config.permalink, S("/{section}/{slug}.html")));
  TEST_CHECK(config.feed_count == 10);
  TEST_CHECK(config.base_url.len == 0);
  TEST_CHECK(config.title.len == 0);
  TEST_CHECK(str_eq(config.author, S("Example Author")));

  remove_fixture_tree(temp_config.root_dir);
}

// Omitting a required key is reported as missing rather than as a type error.
static void test_load_rejects_missing_required_key(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "missing required config key 'base_url'"));

  remove_fixture_tree(temp_config.root_dir);
}

// A required key of the wrong TOML type is reported as a type error, not a missing key.
static void test_load_rejects_wrong_key_type(void) {
  const char toml[] =
      "base_url = 123\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "config key 'base_url' must be a string"));

  remove_fixture_tree(temp_config.root_dir);
}

// A key the schema does not define is rejected, naming the key, so a misspelling cannot silently
// keep a default. A key that is a prefix of a known key, or extends one, is as unknown as any
// other, and so is a table the schema has no place for.
static void test_load_rejects_unknown_keys(void) {
  static const char* const cases[][2] = {
      {"base_url = \"https://example.com\"\ntitle = \"Example Site\"\n"
       "author = \"Example Author\"\nouput_dir = \"site\"\n",
       "unknown config key 'ouput_dir'"},
      {"base_url = \"https://example.com\"\ntitl = \"Example Site\"\n"
       "title = \"Example Site\"\nauthor = \"Example Author\"\n",
       "unknown config key 'titl'"},
      {"base_url = \"https://example.com\"\ntitle = \"Example Site\"\n"
       "author = \"Example Author\"\ntitles = \"Example Site\"\n",
       "unknown config key 'titles'"},
      {"base_url = \"https://example.com\"\ntitle = \"Example Site\"\n"
       "author = \"Example Author\"\n[extra]\nkey = 1\n",
       "unknown config key 'extra'"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, cases[i][0]) != 0) {
      continue;
    }
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// No key in the schema holds a table, so a table under a known key fails that key's type check.
static void test_load_rejects_table_values(void) {
  static const char* const cases[][2] = {
      {"base_url = \"https://example.com\"\nauthor = \"Example Author\"\n"
       "[title]\nname = \"Example Site\"\n",
       "config key 'title' must be a string"},
      {"base_url = \"https://example.com\"\ntitle = \"Example Site\"\n"
       "author = \"Example Author\"\n[aggregate_templates]\nname = \"index.html\"\n",
       "config key 'aggregate_templates' must be an array"},
      {"base_url = \"https://example.com\"\ntitle = \"Example Site\"\n"
       "author = \"Example Author\"\n[feed_count]\ncount = 10\n",
       "config key 'feed_count' must be an integer"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, cases[i][0]) != 0) {
      continue;
    }
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// An integer key rejects a TOML type other than integer and enforces its exact lower bound, which
// is 0 for `feed_count`. A non-integer fails a different assertion from an out-of-range one: the
// user has to change the type, not the number.
static void test_load_rejects_invalid_integer_type_and_minimum(void) {
  check_load_rejects("feed_count = \"ten\"\n", "config key 'feed_count' must be an integer");
  check_load_rejects("feed_count = -1\n", "config key 'feed_count' must be at least 0: -1");
}

// A TOML escape decoding to `U+0000` is rejected per key. tomlc17 accepts the escape and returns a
// string whose bytes carry an embedded `NUL`, so this is a separate boundary from `os_read`: the
// file itself holds no `NUL`. The byte has no meaning in any format `sosig` reads or writes, so the
// load refuses it.
static void test_load_rejects_nul_in_string_values(void) {
  static const char* const cases[][2] = {
      {"base_url = \"a\\u0000b\"\ntitle = \"T\"\nauthor = \"A\"\n",
       "config key 'base_url' must not contain a NUL byte"},
      {"base_url = \"https://example.com\"\ntitle = \"a\\u0000b\"\nauthor = \"A\"\n",
       "config key 'title' must not contain a NUL byte"},
      {"base_url = \"https://example.com\"\ntitle = \"T\"\nauthor = \"A\"\n"
       "content_dir = \"a\\u0000b\"\n",
       "config key 'content_dir' must not contain a NUL byte"},
      {"base_url = \"https://example.com\"\ntitle = \"T\"\nauthor = \"A\"\n"
       "aggregate_templates = [\"a\\u0000b\"]\n",
       "config key 'aggregate_templates' must not contain a NUL byte"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, cases[i][0]) != 0) {
      continue;
    }

    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// A directory key that is empty, or that trims to empty, is rejected by name.
static void test_load_rejects_empty_directory_keys(void) {
  static const char* const cases[][2] = {
      {"content_dir = \"\"\n", "config key 'content_dir' must not be empty"},
      {"output_dir = \"/\"\n", "config key 'output_dir' must not be empty"},
      {"templates_dir = \"///\"\n", "config key 'templates_dir' must not be empty"},
      {"static_dir = \"\"\n", "config key 'static_dir' must not be empty"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    check_load_rejects(cases[i][0], cases[i][1]);
  }
}

// `site_config_load` rejects a `base_url` that is not an absolute HTTP(S) URL. Without that
// rejection it is not a load error at all: it silently produces a broken link in every feed entry
// and canonical URL.
static void test_load_rejects_relative_base_url(void) {
  static const char* const base_urls_invalid[] = {
      "example.com",         // no scheme
      "/relative",           // a path, not a URL
      "",                    // present and a string, but empty
      "ftp://example.com",   // a scheme, but not one a browser follows from a feed
      "https:/example.com",  // one slash short of a scheme
      "https://",            // scheme with no host
      "https:///path",       // the host ends before it starts
      "https://?draft=1",    // likewise
      "https://:8080",       // a port with no host before it
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(base_urls_invalid) / sizeof(base_urls_invalid[0]));
       i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"%s\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n",
                           base_urls_invalid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'base_url' must be an absolute 'http://' or 'https://' URL with "
                 "a host: '%s'",
                 base_urls_invalid[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(test_log_is_one(&log, expected));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// An unsafe `content_template` is rejected at load, in the singular wording that names the key,
// rather than flowing through to the render and failing once per content entry that has no
// frontmatter `template` of its own. Parent-relative, absolute and empty names are all unsafe.
static void test_load_rejects_unsafe_content_template(void) {
  const char* const unsafe_names[] = {"../evil.html", "/etc/passwd", ""};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(unsafe_names) / sizeof(unsafe_names[0])); i++) {
    char toml_extra[256];
    const int n =
        snprintf(toml_extra, sizeof(toml_extra), "content_template = \"%s\"\n", unsafe_names[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'content_template' must be a safe relative template name: '%s'",
                 unsafe_names[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    check_load_rejects(toml_extra, expected);
  }
}

// A non-array value, a non-string element, and an unsafe name for a template array key are each
// rejected.
static void test_load_rejects_invalid_template_arrays(void) {
  check_load_rejects("aggregate_templates = \"index.html\"\n",
                     "config key 'aggregate_templates' must be an array");
  check_load_rejects("aggregate_templates = [1]\n",
                     "config key 'aggregate_templates' must contain only strings");
  check_load_rejects("aggregate_templates = [\"../evil.html\"]\n",
                     "config key 'aggregate_templates' must contain only safe relative template "
                     "names: '../evil.html'");
}

// Every element of a template array is checked even after one fails, so each unsafe name is
// reported in element order. A wrong type or a `NUL` byte is reported once however many elements
// have it, since its message names no element. The rejected key keeps its default.
static void test_load_reports_every_invalid_template_name(void) {
  struct TempConfig temp_config;
  if (write_temp_config(
          &temp_config,
          "base_url = \"https://example.com\"\n"
          "title = \"Example Site\"\n"
          "author = \"Example Author\"\n"
          "feed_templates = [\"../a.xml\", 1, \"a\\u0000b\", \"ok.xml\", \"/b.xml\", 2, "
          "\"c\\u0000d\"]\n") != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  const struct Str* const feed_templates_default = config.feed_templates;
  const ptrdiff_t feed_template_count_default = config.feed_template_count;
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "config key 'feed_templates' must contain only safe relative template "
                         "names: '../a.xml'\n"
                         "config key 'feed_templates' must contain only strings\n"
                         "config key 'feed_templates' must not contain a NUL byte\n"
                         "config key 'feed_templates' must contain only safe relative template "
                         "names: '/b.xml'\n"));
  TEST_CHECK(log.errors == 4);
  TEST_CHECK(config.feed_templates == feed_templates_default);
  TEST_CHECK(config.feed_template_count == feed_template_count_default);

  remove_fixture_tree(temp_config.root_dir);
}

// A permalink that could escape the output directory, or that uses bytes the expanded path cannot
// contain, is rejected once at load rather than once per content entry during the render.
static void test_load_rejects_unsafe_permalink(void) {
  static const char* const permalinks_unsafe[] = {
      "/../{slug}.html",     // a `..` segment escapes `output_dir`
      "/./{slug}.html",      // a `.` segment
      "{section}/..",        // a trailing `..` segment
      "/{slug}?draft.html",  // a byte a safe relative path cannot contain
      "/{unknown}/{slug}",   // an unrecognized token stays literal and cannot be safe
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(permalinks_unsafe) / sizeof(permalinks_unsafe[0]));
       i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_unsafe[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a safe relative path using only "
                 "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                 "with no empty, '.' or '..' path segment: '%s'",
                 permalinks_unsafe[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(test_log_is_one(&log, expected));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// A permalink that expands to the same path for every content entry is rejected at load, naming the
// token that would make it vary. Left through, two or more entries collide at the manifest with a
// message describing the symptom. A single entry publishes a wrong site with no diagnostic.
static void test_load_rejects_permalink_without_slug(void) {
  static const char* const permalinks_indistinct[] = {
      "/about.html",            // a fixed path, the same for every entry
      "",                       // expands to `/index.html`
      "/{section}/",            // varies by section, but not within one
      "/{section}/index.html",  // the same, spelled out
  };

  for (ptrdiff_t i = 0;
       i < (ptrdiff_t)(sizeof(permalinks_indistinct) / sizeof(permalinks_indistinct[0])); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_indistinct[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    if (write_temp_config(&temp_config, toml) != 0) {
      continue;
    }

    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_site_config(&config, temp_config.path, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a distinct path per content entry; "
                 "include the '{slug}' token: '%s'",
                 permalinks_indistinct[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(test_log_is_one(&log, expected));

    remove_fixture_tree(temp_config.root_dir);
  }
}

// A permalink that is unsafe only with an empty `{section}` reports the unsafe expansion ahead of
// the slug collision the populated section shows. The populated section is judged first, so a check
// that ran per expansion reported the collision.
static void test_load_prefers_safety_over_distinctness(void) {
  // With an empty section, `{section}.` expands to a `.` segment, which is unsafe. With the
  // populated sample it expands to `section.`, which is safe, and the same path for every slug.
  const char* pattern = "/{section}./index.html";
  char expected[EXPECTED_SIZE];
  (void)snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a safe relative path using only "
                 "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                 "with no empty, '.' or '..' path segment: '%s'",
                 pattern);
  check_load_rejects_permalink(pattern, expected);
}

// Printing emits every key, with the defaults `site_config_init` supplies for the optional ones.
// `base_url`, `title` and `author` have no default and are set here so the output shows each.
static void test_print_defaults(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = S("https://example.com");
  config.title = S("Example Site");
  config.author = S("Example Author");

  char config_out[1024];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  // The whole buffer, not a set of per-line searches: only an exact comparison can catch a key that
  // is missing, duplicated, extra, or emitted in the wrong order.
  TEST_CHECK(strcmp(config_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Example Site\"\n"
                    "author = \"Example Author\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = [\"index.html\"]\n"
                    "feed_templates = [\"atom.xml\"]\n"
                    "feed_count = 10\n") == 0);
}

// Printing escapes quotes, backslashes, and every named control escape (\t \b \n \f \r). It uses
// the `\uXXXX` fallback for a sub-`0x20` byte and `U+007F` (`DEL`).
static void test_print_escapes_strings(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = S("https://example.com");
  config.title = S("Quote \" and \\ backslash");
  // Each hex escape is followed by a non-hex-digit char ('Z', 'G') so it ends after one byte.
  config.author = S("a\tA\bB\nC\fD\rE\x01Z\x7FG");

  char config_out[1024];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  // Compared whole, so an escape leaking into a neighboring field cannot hide.
  TEST_CHECK(strcmp(config_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Quote \\\" and \\\\ backslash\"\n"
                    "author = \"a\\tA\\bB\\nC\\fD\\rE\\u0001Z\\u007FG\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = [\"index.html\"]\n"
                    "feed_templates = [\"atom.xml\"]\n"
                    "feed_count = 10\n") == 0);
}

// Printing a config and loading the printed output back yields the same field values, exercising
// the round-trip invariant documented on `site_config_print` across escaped strings (including a
// `U+007F` byte), a non-default permalink, and multi-element template arrays.
static void test_print_load_round_trips(void) {
  static const struct Str aggregates[] = {STR_INIT("index.html"), STR_INIT("archive.html")};
  static const struct Str feeds[] = {STR_INIT("atom.xml")};

  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = S("https://example.com/base");
  // '!' after `\x7F` is not a hex digit, so the escape ends after the single `DEL` byte.
  config.title = S("Quote \" back\\slash\nnewline\ttab\x7F!");
  config.author = S("Example Author");
  config.permalink = S("/{slug}/index.html");
  config.content_dir = S("posts");
  config.output_dir = S("dist");
  config.templates_dir = S("layouts");
  config.static_dir = S("files");
  config.content_template = S("post.html");
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  config.feed_templates = feeds;
  config.feed_template_count = 1;
  config.feed_count = 7;

  char config_out[2048];
  TEST_ASSERT(render(&config, config_out, sizeof(config_out)) == 0);

  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, config_out) != 0) {
    return;
  }

  struct SiteConfig reloaded;
  site_config_init(&reloaded);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&reloaded, temp_config.path, &log) == 0);

  TEST_CHECK(str_eq(reloaded.base_url, config.base_url));
  TEST_CHECK(str_eq(reloaded.title, config.title));
  TEST_CHECK(str_eq(reloaded.author, config.author));
  TEST_CHECK(str_eq(reloaded.permalink, config.permalink));
  TEST_CHECK(str_eq(reloaded.content_dir, config.content_dir));
  TEST_CHECK(str_eq(reloaded.output_dir, config.output_dir));
  TEST_CHECK(str_eq(reloaded.templates_dir, config.templates_dir));
  TEST_CHECK(str_eq(reloaded.static_dir, config.static_dir));
  TEST_CHECK(str_eq(reloaded.content_template, config.content_template));
  if (!TEST_CHECK(reloaded.aggregate_template_count == 2)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(reloaded.aggregate_templates[0], S("index.html")));
  TEST_CHECK(str_eq(reloaded.aggregate_templates[1], S("archive.html")));
  if (!TEST_CHECK(reloaded.feed_template_count == 1)) {
    goto cleanup;
  }
  TEST_CHECK(str_eq(reloaded.feed_templates[0], S("atom.xml")));
  TEST_CHECK(reloaded.feed_count == config.feed_count);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// An empty template array survives a load followed by a print. Neither operation alone tests this
// sequence. If `copy_template_names` stored `NULL` for `[]`, it would pass that value to the
// `nonnull` function `site_config_print_string_array`. Then `sosig config` would abort for a config
// that the loader accepted.
static void test_print_empty_template_arrays(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  struct TempConfig temp_config;
  if (write_temp_config(&temp_config, toml) != 0) {
    return;
  }

  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, temp_config.path, &log) == 0);
  TEST_CHECK(log.errors == 0);
  if (!TEST_CHECK(config.aggregate_template_count == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(config.feed_template_count == 0)) {
    goto cleanup;
  }
  // The empty array is a distinct non-`NULL` pointer, which the `nonnull` consumers need.
  TEST_CHECK(config.aggregate_templates != NULL);
  TEST_CHECK(config.feed_templates != NULL);

  char config_out[1024];
  if (render(&config, config_out, sizeof(config_out)) != 0) {
    goto cleanup;
  }
  // Compared whole: an empty array must print as `[]` and must not disturb its neighbors.
  TEST_CHECK(strcmp(config_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Example Site\"\n"
                    "author = \"Example Author\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "static_dir = \"static\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = []\n"
                    "feed_templates = []\n"
                    "feed_count = 10\n") == 0);

cleanup:
  remove_fixture_tree(temp_config.root_dir);
}

// A standard output that refuses the write makes `site_config_print` report `-1` with the write's
// own `errno`, which is what lets `cmd_config_run` name a reason instead of reporting success over
// lost output. A read-only descriptor in place of standard output is the deterministic way to get
// one: every write fails with `EBADF`. The same shape is asserted for `cli_print_version` by
// `test_print_version_reports_write_failure`.
static void test_print_reports_write_failure(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = S("https://example.com");
  config.title = S("Example Site");
  config.author = S("Example Author");

  struct StreamCapture capture;
  if (capture_begin_unwritable(stdout, &capture) != 0) {
    return;
  }
  errno = 0;
  const int rc = site_config_print(&config, os_posix(), test_arena());
  const int print_errno = errno;
  TEST_CHECK(capture_end(&capture, NULL, 0) == 0);
  TEST_CHECK(rc == -1);
  TEST_CHECK(print_errno == EBADF);
}

TEST_LIST = {
    {"load applies required and defaults", test_load_applies_required_and_defaults},
    {"load overrides optional keys", test_load_overrides_optional_keys},
    {"load normalizes directory keys", test_load_normalizes_directory_keys},
    {"load normalizes base url", test_load_normalizes_base_url},
    {"load leaves directory overlap to the build", test_load_leaves_directory_overlap_to_the_build},
    {"load accepts valid permalinks", test_load_accepts_valid_permalinks},
    {"load accepts zero feed count", test_load_accepts_zero_feed_count},
    {"load rejects missing file", test_load_rejects_missing_file},
    {"load rejects oversize file", test_load_rejects_oversize_file},
    {"load rejects nul in file", test_load_rejects_nul_in_file},
    {"load rejects malformed toml", test_load_rejects_malformed_toml},
    {"load reports every invalid key", test_load_reports_every_invalid_key},
    {"load returns zero with key errors", test_load_returns_zero_with_key_errors},
    {"load keeps defaults for rejected keys", test_load_keeps_defaults_for_rejected_keys},
    {"load rejects missing required key", test_load_rejects_missing_required_key},
    {"load rejects wrong key type", test_load_rejects_wrong_key_type},
    {"load rejects unknown keys", test_load_rejects_unknown_keys},
    {"load rejects table values", test_load_rejects_table_values},
    {"load rejects invalid integer type and minimum",
     test_load_rejects_invalid_integer_type_and_minimum},
    {"load rejects nul in string values", test_load_rejects_nul_in_string_values},
    {"load rejects empty directory keys", test_load_rejects_empty_directory_keys},
    {"load rejects relative base url", test_load_rejects_relative_base_url},
    {"load rejects unsafe content template", test_load_rejects_unsafe_content_template},
    {"load rejects invalid template arrays", test_load_rejects_invalid_template_arrays},
    {"load reports every invalid template name", test_load_reports_every_invalid_template_name},
    {"load rejects unsafe permalink", test_load_rejects_unsafe_permalink},
    {"load rejects permalink without slug", test_load_rejects_permalink_without_slug},
    {"load prefers safety over distinctness", test_load_prefers_safety_over_distinctness},
    {"print defaults", test_print_defaults},
    {"print escapes strings", test_print_escapes_strings},
    {"print load round trips", test_print_load_round_trips},
    {"print empty template arrays", test_print_empty_template_arrays},
    {"print reports write failure", test_print_reports_write_failure},
    {NULL, NULL},
};
