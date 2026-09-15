#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "build/manifest_builder.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/manifest.h"
#include "domain/site_config.h"
#include "runtime/fs.h"

// Distinct output paths are all registered without error.
static void test_populate_manifest_accepts_unique(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct ContentEntry a = {.output_path = "public/a.html", .source_path = "content/a.md"};
  struct ContentEntry b = {.output_path = "public/b.html", .source_path = "content/b.md"};
  const struct ContentEntry* entries[] = {&a, &b};

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 2, err,
                                       sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
}

// Two entries claiming the same output path are rejected with a diagnostic naming the path.
static void test_populate_manifest_rejects_duplicate(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  // Isolate content-entry paths from template registration.
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct ContentEntry a = {.output_path = "public/post.html", .source_path = "content/a.md"};
  struct ContentEntry b = {.output_path = "public/post.html", .source_path = "content/b.md"};
  const struct ContentEntry* entries[] = {&a, &b};

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 2, err,
                                       sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "content/a.md", "content/b.md", "public/post.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
}

// Two entries whose output paths collide as a file and a directory, where one is a `/`-delimited
// prefix of the other, are rejected even though neither is a byte-equal duplicate. The two
// producers lead the message ahead of the one unbounded path. The ancestor path is not repeated
// because it is a prefix of the path that is printed.
static void test_populate_manifest_rejects_prefix_collision(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct ContentEntry a = {.output_path = "public/post", .source_path = "content/a.md"};
  struct ContentEntry b = {.output_path = "public/post/index.html", .source_path = "content/b.md"};
  const struct ContentEntry* entries[] = {&a, &b};

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 2, err,
                                       sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "output path for '%s' nests under output path for '%s': '%s'",
                         "content/b.md", "content/a.md", "public/post/index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
}

// `manifest_builder_populate` rejects two configured templates that claim one output path. It names
// each by its list entry rather than by its own name. These two configurations are different
// mistakes with different fixes. A bare template name reports both as
// `for 'dup.html' and 'dup.html'`, which reads as a value colliding with itself and says nothing
// about which key to edit. Both cases are asserted because a label carrying only the config key
// would distinguish them in the second case but not the first.
static void test_populate_manifest_rejects_duplicate_template(void) {
  static const char* const one_list[] = {"dup.html", "dup.html"};
  static const char* const shared[] = {"dup.html"};

  // First case: the same name twice inside `aggregate_templates`, so the two indexes differ.
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.aggregate_templates = one_list;
  config.aggregate_template_count = 2;
  config.feed_template_count = 0;

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, NULL, 0, err,
                                       sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  int n = snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
                   "aggregate_templates[0]", "aggregate_templates[1]", "public/dup.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  manifest_free(&manifest);

  // Second case: one name in each list, so the two keys differ.
  config.aggregate_templates = shared;
  config.aggregate_template_count = 1;
  config.feed_templates = shared;
  config.feed_template_count = 1;

  struct Manifest shared_manifest;
  manifest_init(&shared_manifest);
  char shared_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&shared_manifest, &config, "sosig.toml", &sources, NULL, 0,
                                       shared_err, sizeof(shared_err)) != 0);
  n = snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "aggregate_templates[0]", "feed_templates[0]", "public/dup.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(shared_err, expected) == 0);
  manifest_free(&shared_manifest);

  path_list_free(&sources);
  site_config_free(&config);
}

// `manifest_builder_populate` rejects an output path that names one of the build's own input files,
// so a build cannot overwrite its own source. The two paths here spell one file differently, which
// is what makes the check meaningful. The function compares identity rather than path text, because
// a real collision arrives as an output rooted at `output_dir` against a source rooted at
// `content_dir`.
static void test_populate_manifest_rejects_input_overwrite(void) {
  char dir[] = "/tmp/sosig-manifest-builder-XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);

  char source_path[PATH_MAX];
  int n = snprintf(source_path, sizeof(source_path), "%s/input.md", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(source_path));
  TEST_CHECK(fs_write_file(source_path, "body", 4, NULL, 0) == 0);

  // A second spelling of the same file. `stat` collapses the `/./`, so the identities match while
  // the strings do not.
  char output_path[PATH_MAX];
  n = snprintf(output_path, sizeof(output_path), "%s/./input.md", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(output_path));

  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = dir;
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct ContentEntry entry = {.output_path = output_path, .source_path = "content/input.md"};
  const struct ContentEntry* entries[] = {&entry};

  struct PathList sources;
  path_list_init(&sources);
  TEST_CHECK(path_list_push(&sources, source_path) == 0);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 1, err,
                                       sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/input.md", output_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  unlink(source_path);
  rmdir(dir);
}

// The config file is an input like any other, so an output path naming it is rejected. Without this
// the build overwrote the file that configured it and still reported success: `output_dir = "."`
// plus a template named `sosig.toml` is all it takes. The loss is unrecoverable. Reached through
// the template list rather than a content entry because that is the shape a real project hits.
static void test_populate_manifest_rejects_config_overwrite(void) {
  char dir[] = "/tmp/sosig-manifest-builder-config-XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);

  char config_path[PATH_MAX];
  int n = snprintf(config_path, sizeof(config_path), "%s/sosig.toml", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(config_path));
  TEST_CHECK(fs_write_file(config_path, "title = \"T\"\n", 12, NULL, 0) == 0);

  // `output_dir` is the fixture root and the template is named after the config file, so the
  // template's output path resolves to the config file itself.
  static const char* const aggregates[] = {"sosig.toml"};
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  config.feed_template_count = 0;

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, NULL, 0, err,
                                       sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "aggregate_templates[0]", config_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  unlink(config_path);
  rmdir(dir);
}

// A configured template file is a build input too, so an entry whose output path names one is
// rejected. The sibling above claims a content *source*. This claims a *template*. That is the
// other half of the protection: it guards the files the user writes by hand. Two loops do it, one
// over the configured template lists and one over each entry's `template` override. Both are
// covered, since deleting either leaves every other test, including the golden tests, passing.
static void test_populate_manifest_rejects_template_overwrite(void) {
  char dir[] = "/tmp/sosig-manifest-builder-template-XXXXXX";
  TEST_ASSERT(mkdtemp(dir) != NULL);

  char templates_dir[PATH_MAX];
  char output_dir[PATH_MAX];
  char aggregate_path[PATH_MAX];
  char override_path[PATH_MAX];
  int n = snprintf(templates_dir, sizeof(templates_dir), "%s/templates", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(templates_dir));
  n = snprintf(output_dir, sizeof(output_dir), "%s/public", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(output_dir));
  n = snprintf(aggregate_path, sizeof(aggregate_path), "%s/templates/index.html", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(aggregate_path));
  n = snprintf(override_path, sizeof(override_path), "%s/templates/custom.html", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(override_path));
  TEST_CHECK(fs_write_file(aggregate_path, "{{title}}", 9, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(override_path, "{{title}}", 9, NULL, 0) == 0);

  // Second spellings of the two template files. `stat` collapses the `/./`, so the identities match
  // while the path strings do not, the same trick the source-overwrite test uses.
  char aggregate_output[PATH_MAX];
  char override_output[PATH_MAX];
  n = snprintf(aggregate_output, sizeof(aggregate_output), "%s/templates/./index.html", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(aggregate_output));
  n = snprintf(override_output, sizeof(override_output), "%s/templates/./custom.html", dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(override_output));

  static const char* const aggregates[] = {"index.html"};

  // First case: the collision is with a template named in `aggregate_templates`.
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = output_dir;
  config.templates_dir = templates_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  config.feed_template_count = 0;

  struct ContentEntry aggregate_entry = {.output_path = aggregate_output,
                                         .source_path = "content/a.md"};
  const struct ContentEntry* aggregate_entries[] = {&aggregate_entry};
  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources,
                                       aggregate_entries, 1, err, sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/a.md", aggregate_output);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  manifest_free(&manifest);

  // Second case: the collision is with an entry's own `template` override, claimed by the other
  // loop. No template list is configured here, so only that loop can claim the file.
  config.aggregate_template_count = 0;
  struct ContentEntry override_entry = {
      .output_path = override_output, .source_path = "content/b.md", .template = "custom.html"};
  const struct ContentEntry* override_entries[] = {&override_entry};
  struct Manifest override_manifest;
  manifest_init(&override_manifest);
  char override_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&override_manifest, &config, "sosig.toml", &sources,
                                       override_entries, 1, override_err,
                                       sizeof(override_err)) != 0);
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/b.md", override_output);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(override_err, expected) == 0);
  manifest_free(&override_manifest);

  path_list_free(&sources);
  site_config_free(&config);
  unlink(aggregate_path);
  unlink(override_path);
  rmdir(templates_dir);
  rmdir(dir);
}

// A configured template whose output path exceeds the whole-path limit is rejected, and the
// diagnostic still carries the limit. This branch is reachable only when the template name is
// longer than `OUTPUT_PATH_RELATIVE_LEN_MAX`, which is larger than `ERROR_MESSAGE_SIZE`, so leading
// with the name would make the limit clause unreachable at every triggering input. The message
// would be 511 bytes of filename and nothing else. Asserting the head is what pins the ordering.
static void test_populate_manifest_rejects_oversize_template_path(void) {
  char name[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(name, 'a', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const char* aggregates[] = {name};

  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  config.feed_template_count = 0;

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, NULL, 0, err,
                                       sizeof(err)) != 0);
  char expected_head[128];
  const int expected_head_len = snprintf(
      expected_head, sizeof(expected_head),
      "output path for a configured template exceeds max output path length (%zu bytes) at %zu "
      "bytes: '",
      (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, sizeof(name) - 1);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, (size_t)expected_head_len) == 0);
  // The head is a prefix, so on its own it says nothing about the rest of the buffer. The name is
  // `OUTPUT_PATH_RELATIVE_LEN_MAX + 63` bytes, so the composed message cannot fit `err` and must
  // come back truncated: asserting the exact length proves that is still true. The marker proves
  // the cut is visible to the user. The `...` is spelled out because `TRUNCATION_MARKER` is
  // file-local to `core/error.c`. Changing it there must update this line.
  const size_t err_actual_len = strlen(err);
  TEST_CHECK(err_actual_len == ERROR_MESSAGE_SIZE - 1);
  TEST_CHECK(err_actual_len >= 3 && strcmp(err + err_actual_len - 3, "...") == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
}

// A configured template with one overlong path segment is rejected against the per-filename limit
// rather than the whole-path one, so the two limits stay distinguishable. The whole message fits,
// so it is asserted in full.
static void test_populate_manifest_rejects_oversize_template_segment(void) {
  char name[FILENAME_LEN_MAX + 32];
  memset(name, 'b', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const char* feeds[] = {name};

  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.aggregate_template_count = 0;
  config.feed_templates = feeds;
  config.feed_template_count = 1;

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, NULL, 0, err,
                                       sizeof(err)) != 0);
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected),
               "output path segment for a configured template exceeds max filename length "
               "(%zu bytes) at %zu bytes: '%s'",
               (size_t)FILENAME_LEN_MAX, sizeof(name) - 1, name);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
}

// `manifest_builder_populate` is this module's one exported function, tested above. The writing it
// guards lives in `site_writer`, whose `write_*` functions are exercised end-to-end by the golden
// test suite rather than in a unit test. Entry ordering and `site.updated` derivation live in
// `content_entry` and are tested in `src/domain/test_content_entry.c`.

TEST_LIST = {
    {"populate manifest accepts unique", test_populate_manifest_accepts_unique},
    {"populate manifest rejects duplicate", test_populate_manifest_rejects_duplicate},
    {"populate manifest rejects prefix collision", test_populate_manifest_rejects_prefix_collision},
    {"populate manifest rejects duplicate template",
     test_populate_manifest_rejects_duplicate_template},
    {"populate manifest rejects input overwrite", test_populate_manifest_rejects_input_overwrite},
    {"populate manifest rejects config overwrite", test_populate_manifest_rejects_config_overwrite},
    {"populate manifest rejects template overwrite",
     test_populate_manifest_rejects_template_overwrite},
    {"populate manifest rejects oversize template path",
     test_populate_manifest_rejects_oversize_template_path},
    {"populate manifest rejects oversize template segment",
     test_populate_manifest_rejects_oversize_template_segment},
    {NULL, NULL}};
