#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/error.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "test_support.h"

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
 * @brief Renders a site configuration into a terminated text buffer.
 *
 * @param site_config  Configuration to render.
 * @param text_out     Buffer that receives the rendered text.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero.
 */
static void render(const struct SiteConfig* site_config, char* text_out, size_t text_out_len) {
  FILE* stream = tmpfile();
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  TEST_CHECK(site_config_print(stream, site_config) == 0);
  (void)read_capture(stream, text_out, text_out_len);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);
}

/**
 * @brief Writes TOML to `sosig.toml` in a fresh fixture root.
 *
 * The caller removes the root with `remove_fixture_tree(temp_config_out->root_dir)`.
 *
 * @param temp_config_out Receives the fixture root and the config path.
 * @param toml            Terminated TOML text to write.
 * @return The config path, which aliases `temp_config_out->path`.
 */
static const char* write_temp_config(struct TempConfig* temp_config_out, const char* toml) {
  memcpy(temp_config_out->root_dir, TEMP_CONFIG_ROOT_TEMPLATE, sizeof(TEMP_CONFIG_ROOT_TEMPLATE));
  TEST_ASSERT(init_fixture_dir(temp_config_out->root_dir) != NULL);
  TEST_ASSERT(write_fixture_file(temp_config_out->root_dir, "sosig.toml", toml) == 0);
  const int n = snprintf(temp_config_out->path, sizeof(temp_config_out->path), "%s/sosig.toml",
                         temp_config_out->root_dir);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(temp_config_out->path));
  return temp_config_out->path;
}

/**
 * @brief Loads a config with the given permalink and checks it fails with one diagnostic.
 *
 * @param permalink Permalink pattern to load, embedded in an otherwise valid config.
 * @param expected  Exact diagnostic `site_config_load` must report.
 */
static void check_load_rejects_permalink(const char* permalink, const char* expected) {
  const char toml_format[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "permalink = \"%s\"\n";
  char toml[OUTPUT_PATH_RELATIVE_LEN_MAX + 512];
  const int n = snprintf(toml, sizeof(toml), toml_format, permalink);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// Loading a file with only the required keys fills the rest from `site_config_init`'s defaults.
static void test_load_applies_required_and_defaults(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);

  TEST_CHECK(strcmp(config.base_url, "https://example.com") == 0);
  TEST_CHECK(strcmp(config.title, "Example Site") == 0);
  TEST_CHECK(strcmp(config.author, "Example Author") == 0);
  TEST_CHECK(strcmp(config.permalink, "/{section}/{slug}.html") == 0);
  TEST_CHECK(strcmp(config.content_dir, "content") == 0);
  TEST_CHECK(strcmp(config.output_dir, "public") == 0);
  TEST_CHECK(strcmp(config.templates_dir, "templates") == 0);
  TEST_CHECK(strcmp(config.content_template, "content.html") == 0);
  TEST_ASSERT(config.aggregate_template_count == 1);
  TEST_CHECK(strcmp(config.aggregate_templates[0], "index.html") == 0);
  TEST_ASSERT(config.feed_template_count == 1);
  TEST_CHECK(strcmp(config.feed_templates[0], "atom.xml") == 0);
  TEST_CHECK(config.feed_count == 10);

  site_config_free(&config);
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
      "content_template = \"post.html\"\n"
      "aggregate_templates = [\"index.html\", \"archive.html\"]\n"
      "feed_templates = []\n"
      "feed_count = 5\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);

  TEST_CHECK(strcmp(config.permalink, "/{slug}/index.html") == 0);
  TEST_CHECK(strcmp(config.content_dir, "posts") == 0);
  TEST_CHECK(strcmp(config.output_dir, "dist") == 0);
  TEST_CHECK(strcmp(config.templates_dir, "layouts") == 0);
  TEST_CHECK(strcmp(config.content_template, "post.html") == 0);
  TEST_ASSERT(config.aggregate_template_count == 2);
  TEST_CHECK(strcmp(config.aggregate_templates[0], "index.html") == 0);
  TEST_CHECK(strcmp(config.aggregate_templates[1], "archive.html") == 0);
  TEST_CHECK(config.feed_template_count == 0);
  TEST_CHECK(config.feed_count == 5);

  site_config_free(&config);
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
      "templates_dir = \"../shared/templates/\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(config.content_dir, "content") == 0);
  TEST_CHECK(strcmp(config.output_dir, "/srv/www") == 0);
  TEST_CHECK(strcmp(config.templates_dir, "../shared/templates") == 0);

  site_config_free(&config);
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"%s\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);
    TEST_CHECK(err[0] == '\0');
    TEST_CHECK(strcmp(config.base_url, cases[i][1]) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// Permalink patterns that expand to a safe relative path are accepted, including a directory-style
// pattern and one that omits the section.
static void test_load_accepts_valid_permalinks(void) {
  static const char* const permalinks_valid[] = {
      "/{section}/{slug}.html", "/{slug}.html", "/{section}/{slug}/",
      "/posts/{slug}.html",     "{slug}",       "/a.b-c_d/{slug}.html",
  };

  for (size_t i = 0; i < sizeof(permalinks_valid) / sizeof(permalinks_valid[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_valid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);
    TEST_CHECK(err[0] == '\0');
    TEST_CHECK(strcmp(config.permalink, permalinks_valid[i]) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// Zero is accepted, which is the other side of the negative `feed_count` rejection and the boundary
// between them. Every other accepting case in this file uses a positive count, so a guard of `<= 0`
// instead of `< 0` rejects a legal config while still passing all of them. Zero means a feed
// template sees no entries, which is a configuration a user can ask for.
static void test_load_accepts_zero_feed_count(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "feed_count = 0\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(config.feed_count == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A directory key that is empty, or that trims to empty, is rejected by name.
static void test_load_rejects_empty_directory_keys(void) {
  static const char* const cases[][2] = {
      {"content_dir = \"\"\n", "config key 'content_dir' must not be empty"},
      {"output_dir = \"/\"\n", "config key 'output_dir' must not be empty"},
      {"templates_dir = \"///\"\n", "config key 'templates_dir' must not be empty"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "%s",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// `site_config_load` rejects a `base_url` that is not an absolute http(s) URL. Without that
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
  };

  for (size_t i = 0; i < sizeof(base_urls_invalid) / sizeof(base_urls_invalid[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"%s\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n",
                           base_urls_invalid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'base_url' must be an absolute 'http://' or 'https://' URL with "
                 "a host: '%s'",
                 base_urls_invalid[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// Loading a config path that does not exist reports a read failure naming the path and cause.
static void test_load_rejects_missing_file(void) {
  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  const char* missing_path = "/tmp/sosig-site-config-test-missing.toml";
  TEST_CHECK(site_config_load(&config, missing_path, err, sizeof(err)) == -1);
  // The cause distinguishes a missing file from an unreadable or malformed one. Derived from the
  // running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('%s')",
               error_system_message(reason, sizeof(reason), ENOENT), missing_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  site_config_free(&config);
}

// A config larger than `CONFIG_FILE_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size. The constant is file-local to `site_config.c`, so the 1 MiB below is spelled
// out and must change with it. The file is sparse, so the fixture costs no disk.
static void test_load_rejects_oversize_file(void) {
  enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, "title = \"Site\"\n");
  const off_t config_len = (off_t)CONFIG_FILE_LEN_MAX + 1;
  TEST_ASSERT(truncate(config_path, config_len) == 0);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: exceeds max file size (%d bytes) at %jd bytes ('%s')",
               CONFIG_FILE_LEN_MAX, (intmax_t)config_len, config_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// `site_config_load` reports a syntactically malformed config as a parse failure naming the config
// path, rather than reaching the field pass with an empty table and reporting a false missing-key
// error.
static void test_load_rejects_malformed_toml(void) {
  const char toml[] = "base_url = \n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  // The prefix and the trailing path are this module's own contribution. The text between them is
  // the vendored parser's `errmsg`, left unpinned so rewording it upstream does not fail this test.
  // The two together are still the whole first-party claim, which neither the missing-key message
  // nor the read-failure message could satisfy.
  const char prefix[] = "failed to parse config: ";
  TEST_CHECK(strncmp(err, prefix, sizeof(prefix) - 1) == 0);
  char suffix[ERROR_MESSAGE_SIZE];
  const int n = snprintf(suffix, sizeof(suffix), " ('%s')", config_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(suffix));
  const size_t err_len = strlen(err);
  // A cause of at least one byte has to sit between the two, or the parser reported nothing.
  TEST_CHECK(err_len > sizeof(prefix) - 1 + (size_t)n);
  TEST_CHECK(strcmp(err + err_len - (size_t)n, suffix) == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// Omitting a required key is reported as missing rather than as a type error.
static void test_load_rejects_missing_required_key(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "missing required config key 'base_url'") == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A required key of the wrong TOML type is reported as a type error, not a missing key.
static void test_load_rejects_wrong_key_type(void) {
  const char toml[] =
      "base_url = 123\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "config key 'base_url' must be a string") == 0);

  site_config_free(&config);
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, cases[i][0]);
    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);
    TEST_MSG("case %zu: got '%s'", i, err);
    site_config_free(&config);
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, cases[i][0]);
    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);
    TEST_MSG("case %zu: got '%s'", i, err);
    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// An unsafe `content_template` is rejected at load, in the singular wording that names the key,
// rather than flowing through to the render and failing once per content entry that has no
// frontmatter `template` of its own. Parent-relative, absolute and empty names are all unsafe.
static void test_load_rejects_unsafe_content_template(void) {
  const char base[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  const char* const unsafe_names[] = {"../evil.html", "/etc/passwd", ""};

  for (size_t i = 0; i < sizeof(unsafe_names) / sizeof(unsafe_names[0]); i++) {
    char toml[512];
    const int n =
        snprintf(toml, sizeof(toml), "%scontent_template = \"%s\"\n", base, unsafe_names[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'content_template' must be a safe relative template name: '%s'",
                 unsafe_names[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// A non-array value, a non-string element, and an unsafe name for a template array key are each
// rejected.
static void test_load_rejects_invalid_template_arrays(void) {
  const char base[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n";
  char err[ERROR_MESSAGE_SIZE];

  struct TempConfig not_array_temp;
  char not_array_toml[512];
  int n = snprintf(not_array_toml, sizeof(not_array_toml),
                   "%saggregate_templates = \"index.html\"\n", base);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(not_array_toml));
  const char* not_array_path = write_temp_config(&not_array_temp, not_array_toml);
  struct SiteConfig not_array_config;
  site_config_init(&not_array_config);
  TEST_CHECK(site_config_load(&not_array_config, not_array_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "config key 'aggregate_templates' must be an array") == 0);
  site_config_free(&not_array_config);
  remove_fixture_tree(not_array_temp.root_dir);

  struct TempConfig non_string_temp;
  char non_string_toml[512];
  n = snprintf(non_string_toml, sizeof(non_string_toml), "%saggregate_templates = [1]\n", base);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(non_string_toml));
  const char* non_string_path = write_temp_config(&non_string_temp, non_string_toml);
  struct SiteConfig non_string_config;
  site_config_init(&non_string_config);
  TEST_CHECK(site_config_load(&non_string_config, non_string_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "config key 'aggregate_templates' must contain only strings") == 0);
  site_config_free(&non_string_config);
  remove_fixture_tree(non_string_temp.root_dir);

  struct TempConfig unsafe_name_temp;
  char unsafe_name_toml[512];
  n = snprintf(unsafe_name_toml, sizeof(unsafe_name_toml),
               "%saggregate_templates = [\"../evil.html\"]\n", base);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(unsafe_name_toml));
  const char* unsafe_name_path = write_temp_config(&unsafe_name_temp, unsafe_name_toml);
  struct SiteConfig unsafe_name_config;
  site_config_init(&unsafe_name_config);
  TEST_CHECK(site_config_load(&unsafe_name_config, unsafe_name_path, err, sizeof(err)) == -1);
  char expected_unsafe_name[ERROR_MESSAGE_SIZE];
  n = snprintf(expected_unsafe_name, sizeof(expected_unsafe_name),
               "config key '%s' must contain only safe relative template names: '%s'",
               "aggregate_templates", "../evil.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_unsafe_name));
  TEST_CHECK(strcmp(err, expected_unsafe_name) == 0);
  site_config_free(&unsafe_name_config);
  remove_fixture_tree(unsafe_name_temp.root_dir);
}

// A negative `feed_count` is rejected, and says the value is out of range rather than mistyped.
static void test_load_rejects_negative_feed_count(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "feed_count = -1\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "config key 'feed_count' must not be negative") == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A non-integer `feed_count` is rejected, and fails a different assertion from a negative one: the
// user has to change the type, not the number.
static void test_load_rejects_non_integer_feed_count(void) {
  const char toml[] =
      "base_url = \"https://example.com\"\n"
      "title = \"Example Site\"\n"
      "author = \"Example Author\"\n"
      "feed_count = \"ten\"\n";
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "config key 'feed_count' must be an integer") == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A TOML escape decoding to `U+0000` is rejected per key. tomlc17 accepts the escape and returns a
// string whose bytes carry an embedded `NUL`, so this is a separate boundary from `fs_read_file`:
// the file itself holds no `NUL`. Left through, `strlen` would truncate the value silently.
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, cases[i][0]);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
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

  for (size_t i = 0; i < sizeof(permalinks_unsafe) / sizeof(permalinks_unsafe[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_unsafe[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a safe relative path using only "
                 "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                 "with no empty, '.' or '..' path segment: '%s'",
                 permalinks_unsafe[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);

    site_config_free(&config);
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

  for (size_t i = 0; i < sizeof(permalinks_indistinct) / sizeof(permalinks_indistinct[0]); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "base_url = \"https://example.com\"\n"
                           "title = \"Example Site\"\n"
                           "author = \"Example Author\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_indistinct[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct TempConfig temp_config;
    const char* config_path = write_temp_config(&temp_config, toml);

    struct SiteConfig config;
    site_config_init(&config);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a distinct path per content entry; "
                 "include the '{slug}' token: '%s'",
                 permalinks_indistinct[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);

    site_config_free(&config);
    remove_fixture_tree(temp_config.root_dir);
  }
}

// A permalink whose shortest expansion exceeds the output-path limit is rejected at load.
static void test_load_rejects_oversize_permalink(void) {
  // Rejected here rather than once per content file, which is what the render phase's own backstop
  // would do for the same pattern.
  char pattern[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(pattern, 'a', sizeof(pattern) - 1);
  pattern[sizeof(pattern) - 1] = '\0';
  pattern[0] = '/';
  memcpy(pattern + sizeof(pattern) - sizeof("/{slug}.html"), "/{slug}.html",
         sizeof("/{slug}.html"));

  char toml[sizeof(pattern) + 256];
  int n = snprintf(toml, sizeof(toml),
                   "base_url = \"https://example.com\"\n"
                   "title = \"Example Site\"\n"
                   "author = \"Example Author\"\n"
                   "permalink = \"%s\"\n",
                   pattern);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  // `{slug}` and the `slug-a` sample are both six bytes and the pattern has no `{section}`, so the
  // shortest expansion is the pattern's own length, less the leading `/` the limit excludes.
  n = snprintf(expected, sizeof(expected),
               "config key 'permalink' exceeds max output path length (%zu bytes) at %zu bytes "
               "when expanded",
               (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, sizeof(pattern) - 2);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A permalink that fits with an empty `{section}` but overflows with a populated one is rejected at
// load, which is what makes the whole-path limit a property of the pattern rather than of its
// shortest expansion. Leaving this to the render would report it once per content file that happens
// to sit in a populated section.
static void test_load_rejects_permalink_oversize_in_populated_section(void) {
  // `check_permalink`'s sample grid is file-local, so the two expansion tails are spelled out here:
  // with the samples in `src/domain/site_config.c` the empty-section tail is `/slug-a.html` and the
  // populated one is `/section/slug-a.html`. Changing `SECTION_SAMPLES` or `SLUG_SAMPLES` must
  // update these. The filler is sized so the shorter expansion lands exactly on the limit and stays
  // within it, and is split into segments below the per-segment limit so that limit cannot fire
  // first.
  enum { FITTING_TAIL_LEN = sizeof("/slug-a.html") - 1 };
  enum { OVERFLOWING_TAIL_LEN = sizeof("/section/slug-a.html") - 1 };
  enum { FILLER_LEN = OUTPUT_PATH_RELATIVE_LEN_MAX - FITTING_TAIL_LEN };
  enum { FILLER_SEGMENT_LEN = 100 };
  _Static_assert((size_t)FILLER_SEGMENT_LEN < (size_t)FILENAME_LEN_MAX,
                 "filler segments must clear the per-segment name limit");
  _Static_assert(FILLER_LEN + OVERFLOWING_TAIL_LEN > OUTPUT_PATH_RELATIVE_LEN_MAX,
                 "the populated-section expansion must overflow the whole-path limit");

  char filler[FILLER_LEN + 1];
  for (size_t i = 0; i < (size_t)FILLER_LEN; i++) {
    filler[i] = (i + 1) % (FILLER_SEGMENT_LEN + 1) == 0 ? '/' : 'a';
  }
  filler[FILLER_LEN] = '\0';
  // A trailing separator would make the joined pattern contain `//`, which is not a safe relative
  // path, so the failure under test would be pre-empted by the safety check.
  TEST_CHECK(filler[FILLER_LEN - 1] != '/');

  char toml[FILLER_LEN + 256];
  int n = snprintf(toml, sizeof(toml),
                   "base_url = \"https://example.com\"\n"
                   "title = \"Example Site\"\n"
                   "author = \"Example Author\"\n"
                   "permalink = \"/%s/{section}/{slug}.html\"\n",
                   filler);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  // The reported length is the populated expansion's, which is reachable only by checking every
  // expansion: the empty-section one sits exactly on the limit and passes.
  n = snprintf(expected, sizeof(expected),
               "config key 'permalink' exceeds max output path length (%zu bytes) at %zu bytes "
               "when expanded",
               (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, (size_t)(FILLER_LEN + OVERFLOWING_TAIL_LEN));
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A permalink with a literal segment longer than the filename limit is rejected at load, against
// the per-segment limit rather than the whole-path one, so the two stay distinguishable. Only a
// literal can fail this from the sample grid, since the sample slugs are six bytes. A literal's
// length is the same for every entry, which is what makes it a config error rather than a per-file
// one.
static void test_load_rejects_permalink_with_oversize_segment(void) {
  char pattern[FILENAME_LEN_MAX + 46];
  memset(pattern, 's', sizeof(pattern) - 1);
  pattern[sizeof(pattern) - 1] = '\0';
  pattern[0] = '/';
  memcpy(pattern + sizeof(pattern) - sizeof("/{slug}.html"), "/{slug}.html",
         sizeof("/{slug}.html"));
  // The offending segment is everything between the two `/`, and the whole path stays well inside
  // `OUTPUT_PATH_RELATIVE_LEN_MAX` so the whole-path limit cannot fire first.
  const size_t segment_len = sizeof(pattern) - sizeof("/{slug}.html") - 1;

  char toml[sizeof(pattern) + 256];
  int n = snprintf(toml, sizeof(toml),
                   "base_url = \"https://example.com\"\n"
                   "title = \"Example Site\"\n"
                   "author = \"Example Author\"\n"
                   "permalink = \"%s\"\n",
                   pattern);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected),
               "config key 'permalink' exceeds max filename length (%zu bytes) at %zu bytes in an "
               "expanded path segment: '%s'",
               (size_t)FILENAME_LEN_MAX, segment_len, pattern);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A permalink without `{slug}` whose literal also breaks a length limit reports the missing token,
// which is the fix it needs whatever its length. The first sample expansion is judged on length
// before the second is compared with it, so a check that ran per expansion reported the length.
static void test_load_prefers_distinctness_over_length(void) {
  // One pattern breaks the whole-path limit and one the per-segment limit, each with a single
  // literal segment and no token.
  char pattern_too_long[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  char pattern_segment_too_long[FILENAME_LEN_MAX + 46];
  char* const patterns[] = {pattern_too_long, pattern_segment_too_long};
  const size_t pattern_sizes[] = {sizeof(pattern_too_long), sizeof(pattern_segment_too_long)};
  for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
    memset(patterns[i], 'a', pattern_sizes[i] - 1);
    patterns[i][0] = '/';
    patterns[i][pattern_sizes[i] - 1] = '\0';

    // The trailing pattern outgrows the buffer, so the expected text goes through `error_report` to
    // get the same truncation marker.
    char expected[ERROR_MESSAGE_SIZE];
    (void)error_report(expected, sizeof(expected),
                       "config key 'permalink' must expand to a distinct path per content entry; "
                       "include the '{slug}' token: '%s'",
                       patterns[i]);
    check_load_rejects_permalink(patterns[i], expected);
  }
}

// A permalink that is unsafe only with an empty `{section}` reports the unsafe expansion ahead of a
// length limit or a slug collision that the populated section already shows. The populated section
// is judged first, so a check that ran per expansion reported the later verdict.
static void test_load_prefers_safety_over_length_and_distinctness(void) {
  // With an empty section, `{section}.` expands to a `.` segment, which is unsafe. With the
  // populated sample it expands to `section.`, which is safe.
  char pattern_too_long[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  char pattern_segment_too_long[FILENAME_LEN_MAX + 64];
  char* const patterns_long[] = {pattern_too_long, pattern_segment_too_long};
  const size_t pattern_sizes[] = {sizeof(pattern_too_long), sizeof(pattern_segment_too_long)};
  static const char tail[] = "/{section}./{slug}";
  for (size_t i = 0; i < sizeof(patterns_long) / sizeof(patterns_long[0]); i++) {
    memset(patterns_long[i], 'a', pattern_sizes[i] - 1);
    patterns_long[i][0] = '/';
    memcpy(patterns_long[i] + pattern_sizes[i] - sizeof(tail), tail, sizeof(tail));
  }
  const char* const patterns[] = {
      pattern_too_long,          // overflows the whole-path limit with a populated section
      pattern_segment_too_long,  // holds an oversize literal segment
      "/{section}./index.html",  // collides across slugs with a populated section
  };

  for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
    // A long pattern outgrows the buffer, so the expected text goes through `error_report` to get
    // the same truncation marker.
    char expected[ERROR_MESSAGE_SIZE];
    (void)error_report(expected, sizeof(expected),
                       "config key 'permalink' must expand to a safe relative path using only "
                       "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                       "with no empty, '.' or '..' path segment: '%s'",
                       patterns[i]);
    check_load_rejects_permalink(patterns[i], expected);
  }
}

// Printing emits every key, with the defaults `site_config_init` supplies for the optional ones.
// `base_url`, `title` and `author` have no default and are set here only because
// `site_config_print` requires them non-`NULL`.
static void test_print_defaults(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = "https://www.example.com";
  config.title = "C17 Notes";
  config.author = "Author Name";

  char config_out[1024];
  render(&config, config_out, sizeof(config_out));

  // The whole buffer, not a set of per-line searches: only an exact comparison can catch a key that
  // is missing, duplicated, extra, or emitted in the wrong order.
  TEST_CHECK(strcmp(config_out,
                    "base_url = \"https://www.example.com\"\n"
                    "title = \"C17 Notes\"\n"
                    "author = \"Author Name\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = [\"index.html\"]\n"
                    "feed_templates = [\"atom.xml\"]\n"
                    "feed_count = 10\n") == 0);

  site_config_free(&config);
}

// Printing escapes quotes, backslashes, and every named control escape (\t \b \n \f \r). It uses
// the `\uXXXX` fallback for a sub-`0x20` byte and `U+007F` (`DEL`).
static void test_print_escapes_strings(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = "https://example.com";
  config.title = "Quote \" and \\ backslash";
  // Each hex escape is followed by a non-hex-digit char ('Z', 'G') so it ends after one byte.
  config.author = "a\tA\bB\nC\fD\rE\x01Z\x7FG";

  char config_out[1024];
  render(&config, config_out, sizeof(config_out));

  // Compared whole, so an escape leaking into a neighboring field cannot hide.
  TEST_CHECK(strcmp(config_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Quote \\\" and \\\\ backslash\"\n"
                    "author = \"a\\tA\\bB\\nC\\fD\\rE\\u0001Z\\u007FG\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = [\"index.html\"]\n"
                    "feed_templates = [\"atom.xml\"]\n"
                    "feed_count = 10\n") == 0);

  site_config_free(&config);
}

// Printing a config and loading the printed output back yields the same field values, exercising
// the round-trip invariant documented on `site_config_print` across escaped strings (including a
// `U+007F` byte), a non-default permalink, and multi-element template arrays.
static void test_print_load_round_trips(void) {
  static const char* aggregates[] = {"index.html", "archive.html"};
  static const char* feeds[] = {"atom.xml"};

  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = "https://example.com/base";
  // '!' after `\x7F` is not a hex digit, so the escape ends after the single `DEL` byte.
  config.title = "Quote \" back\\slash\nnewline\ttab\x7F!";
  config.author = "Example Author";
  config.permalink = "/{slug}/index.html";
  config.content_dir = "posts";
  config.output_dir = "dist";
  config.templates_dir = "layouts";
  config.content_template = "post.html";
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  config.feed_templates = feeds;
  config.feed_template_count = 1;
  config.feed_count = 7;

  char config_out[2048];
  render(&config, config_out, sizeof(config_out));

  struct TempConfig temp_config;
  const char* config_path = write_temp_config(&temp_config, config_out);

  struct SiteConfig reloaded;
  site_config_init(&reloaded);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(site_config_load(&reloaded, config_path, err, sizeof(err)) == 0);

  TEST_CHECK(strcmp(reloaded.base_url, config.base_url) == 0);
  TEST_CHECK(strcmp(reloaded.title, config.title) == 0);
  TEST_CHECK(strcmp(reloaded.author, config.author) == 0);
  TEST_CHECK(strcmp(reloaded.permalink, config.permalink) == 0);
  TEST_CHECK(strcmp(reloaded.content_dir, config.content_dir) == 0);
  TEST_CHECK(strcmp(reloaded.output_dir, config.output_dir) == 0);
  TEST_CHECK(strcmp(reloaded.templates_dir, config.templates_dir) == 0);
  TEST_CHECK(strcmp(reloaded.content_template, config.content_template) == 0);
  TEST_ASSERT(reloaded.aggregate_template_count == 2);
  TEST_CHECK(strcmp(reloaded.aggregate_templates[0], "index.html") == 0);
  TEST_CHECK(strcmp(reloaded.aggregate_templates[1], "archive.html") == 0);
  TEST_ASSERT(reloaded.feed_template_count == 1);
  TEST_CHECK(strcmp(reloaded.feed_templates[0], "atom.xml") == 0);
  TEST_CHECK(reloaded.feed_count == config.feed_count);

  site_config_free(&reloaded);
  site_config_free(&config);
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
  const char* config_path = write_temp_config(&temp_config, toml);

  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(site_config_load(&config, config_path, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_ASSERT(config.aggregate_template_count == 0);
  TEST_ASSERT(config.feed_template_count == 0);
  // The empty array is a distinct non-`NULL` pointer, which the `nonnull` consumers need.
  TEST_CHECK(config.aggregate_templates != NULL);
  TEST_CHECK(config.feed_templates != NULL);

  char config_out[1024];
  render(&config, config_out, sizeof(config_out));
  // Compared whole: an empty array must print as `[]` and must not disturb its neighbors.
  TEST_CHECK(strcmp(config_out,
                    "base_url = \"https://example.com\"\n"
                    "title = \"Example Site\"\n"
                    "author = \"Example Author\"\n"
                    "permalink = \"/{section}/{slug}.html\"\n"
                    "content_dir = \"content\"\n"
                    "output_dir = \"public\"\n"
                    "templates_dir = \"templates\"\n"
                    "content_template = \"content.html\"\n"
                    "aggregate_templates = []\n"
                    "feed_templates = []\n"
                    "feed_count = 10\n") == 0);

  site_config_free(&config);
  remove_fixture_tree(temp_config.root_dir);
}

// A stream that latched a write error makes `site_config_print` report `-1` with `errno` set, which
// is what lets `cmd_config_run` name a reason instead of reporting success over truncated output.
// The reason is `EIO` rather than the underlying `EBADF`: a latched error's own `errno` may have
// been overwritten by the time it is noticed, so the contract substitutes a generic I/O failure
// rather than relaying a stale value. A read-mode stream is the deterministic way to reach that
// branch, since the writes fail while `fflush` itself succeeds. The same shape is asserted for
// `cli_print_version` by `test_print_version_reports_write_failure`.
static void test_print_reports_write_failure(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.base_url = "https://www.example.com";
  config.title = "C17 Notes";
  config.author = "Author Name";

  FILE* stream = fopen("/dev/null", "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    site_config_free(&config);
    return;
  }
  errno = 0;
  TEST_CHECK(site_config_print(stream, &config) == -1);
  TEST_CHECK(errno == EIO);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);

  site_config_free(&config);
}

TEST_LIST = {
    {"load applies required and defaults", test_load_applies_required_and_defaults},
    {"load overrides optional keys", test_load_overrides_optional_keys},
    {"load normalizes directory keys", test_load_normalizes_directory_keys},
    {"load normalizes base url", test_load_normalizes_base_url},
    {"load accepts valid permalinks", test_load_accepts_valid_permalinks},
    {"load accepts zero feed count", test_load_accepts_zero_feed_count},
    {"load rejects empty directory keys", test_load_rejects_empty_directory_keys},
    {"load rejects relative base url", test_load_rejects_relative_base_url},
    {"load rejects missing file", test_load_rejects_missing_file},
    {"load rejects oversize file", test_load_rejects_oversize_file},
    {"load rejects malformed toml", test_load_rejects_malformed_toml},
    {"load rejects missing required key", test_load_rejects_missing_required_key},
    {"load rejects wrong key type", test_load_rejects_wrong_key_type},
    {"load rejects unknown keys", test_load_rejects_unknown_keys},
    {"load rejects table values", test_load_rejects_table_values},
    {"load rejects unsafe content template", test_load_rejects_unsafe_content_template},
    {"load rejects invalid template arrays", test_load_rejects_invalid_template_arrays},
    {"load rejects negative feed count", test_load_rejects_negative_feed_count},
    {"load rejects non-integer feed count", test_load_rejects_non_integer_feed_count},
    {"load rejects nul in string values", test_load_rejects_nul_in_string_values},
    {"load rejects unsafe permalink", test_load_rejects_unsafe_permalink},
    {"load rejects permalink without slug", test_load_rejects_permalink_without_slug},
    {"load rejects oversize permalink", test_load_rejects_oversize_permalink},
    {"load rejects permalink oversize in populated section",
     test_load_rejects_permalink_oversize_in_populated_section},
    {"load rejects permalink with oversize segment",
     test_load_rejects_permalink_with_oversize_segment},
    {"load prefers distinctness over length", test_load_prefers_distinctness_over_length},
    {"load prefers safety over length and distinctness",
     test_load_prefers_safety_over_length_and_distinctness},
    {"print defaults", test_print_defaults},
    {"print escapes strings", test_print_escapes_strings},
    {"print load round trips", test_print_load_round_trips},
    {"print empty template arrays", test_print_empty_template_arrays},
    {"print reports write failure", test_print_reports_write_failure},
    {NULL, NULL},
};
