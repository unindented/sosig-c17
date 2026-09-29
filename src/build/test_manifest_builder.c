#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
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
#include "shared/arena.h"
#include "test_support.h"

/**
 * @brief Populates a manifest with one content entry and checks the input-root rejection.
 *
 * @param config      Configuration whose roots the output is checked against. Must not be `NULL`.
 * @param output_path Output path of the single entry, joined onto `config->output_dir`. Must not
 *                    be `NULL`.
 * @param root_key    Config key of the root the diagnostic must name. Must not be `NULL`.
 */
static void check_rejects_output_in_root(const struct SiteConfig* config,
                                         const char* output_path,
                                         const char* root_key) {
  struct ContentEntry entry = {.output_path = output_path, .source_path = "content/a.md"};
  const struct ContentEntry* entries[] = {&entry};
  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, config, "sosig.toml", &sources, entries, 1, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "output path would write inside '%s' for 'content/a.md': '%s'", root_key,
                         output_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);
  manifest_free(&manifest);
  path_list_free(&sources);
}

// Distinct output paths are all registered without error.
static void test_accepts_unique(void) {
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

// A `templates_dir` that does not exist holds no file an output could overwrite, so it claims
// nothing and the manifest still populates. The build reports the missing template later, from the
// render that needed it, as `test_reports_bad_template` in `src/app/test_cmd_build.c` pins.
static void test_accepts_missing_templates_dir(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-missing-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  struct Arena arena;
  arena_init(&arena);
  char* templates_dir = path_join(root_dir, "templates", &arena);
  TEST_ASSERT(templates_dir != NULL);

  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.templates_dir = templates_dir;
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct ContentEntry entry = {.output_path = "public/a.html", .source_path = "content/a.md"};
  const struct ContentEntry* entries[] = {&entry};

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 1, err,
                                       sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(manifest.count == 1);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output beside an input root, in a directory whose name only starts with the root's name, is
// accepted, because the root check compares whole directories rather than path prefixes.
static void test_accepts_output_beside_input_roots(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-beside-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "content/a.md", "") == 0);
  TEST_CHECK(write_fixture_file(root_dir, "templates/content.html", "") == 0);
  TEST_CHECK(write_fixture_file(root_dir, "contentx/old.html", "") == 0);
  struct Arena arena;
  arena_init(&arena);

  struct SiteConfig config;
  site_config_init(&config);
  config.content_dir = path_join(root_dir, "content", &arena);
  config.templates_dir = path_join(root_dir, "templates", &arena);
  config.output_dir = root_dir;
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;
  TEST_ASSERT(config.content_dir != NULL && config.templates_dir != NULL);

  struct ContentEntry a = {.output_path = path_join(root_dir, "contentx/a.html", &arena),
                           .source_path = "content/a.md"};
  struct ContentEntry b = {.output_path = path_join(root_dir, "templatesx/b.html", &arena),
                           .source_path = "content/b.md"};
  TEST_ASSERT(a.output_path != NULL && b.output_path != NULL);
  const struct ContentEntry* entries[] = {&a, &b};

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 2, err,
                                       sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_MSG("actual: '%s'", err);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two entries claiming the same output path are rejected with a diagnostic naming the path.
static void test_rejects_duplicate(void) {
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
                                       sizeof(err)) == -1);
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

// Two entries whose permalinks differ only in ASCII case claim one file on a case-insensitive
// filesystem such as the macOS default, where the second write would silently replace the first.
// They are rejected as a duplicate on every platform, so a site that builds on Linux does not
// break on macOS. The trailing path is the second claim's own spelling.
static void test_rejects_case_folded_duplicate(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct ContentEntry a = {.output_path = "public/About/index.html", .source_path = "content/a.md"};
  struct ContentEntry b = {.output_path = "public/about/index.html", .source_path = "content/b.md"};
  const struct ContentEntry* entries[] = {&a, &b};

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, entries, 2, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "content/a.md", "content/b.md", "public/about/index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
}

// Two entries whose output paths collide as a file and a directory, where one is a `/`-delimited
// prefix of the other, are rejected even though neither is a duplicate. The two producers lead the
// message ahead of the one unbounded path. The ancestor path is not repeated because it is a prefix
// of the path that is printed.
static void test_rejects_prefix_collision(void) {
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
                                       sizeof(err)) == -1);
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
static void test_rejects_duplicate_template(void) {
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
                                       sizeof(err)) == -1);
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
                                       shared_err, sizeof(shared_err)) == -1);
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
static void test_rejects_input_overwrite(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }

  char source_path[PATH_MAX];
  int n = snprintf(source_path, sizeof(source_path), "%s/input.md", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(source_path));
  TEST_CHECK(fs_write_file(source_path, "body", 4, NULL, 0) == 0);

  // A second spelling of the same file. `stat` collapses the `/./`, so the identities match while
  // the strings do not.
  char output_path[PATH_MAX];
  n = snprintf(output_path, sizeof(output_path), "%s/./input.md", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(output_path));

  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = root_dir;
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
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/input.md", output_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  unlink(source_path);
  rmdir(root_dir);
}

// The config file is an input like any other, so an output path naming it is rejected. Without this
// the build overwrote the file that configured it and still reported success: `output_dir = "."`
// plus a template named `sosig.toml` is all it takes. The loss is unrecoverable. Reached through
// the template list rather than a content entry because that is the shape a real project hits.
static void test_rejects_config_overwrite(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-config-XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }

  char config_path[PATH_MAX];
  int n = snprintf(config_path, sizeof(config_path), "%s/sosig.toml", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(config_path));
  TEST_CHECK(fs_write_file(config_path, "title = \"T\"\n", 12, NULL, 0) == 0);

  // `output_dir` is the fixture root and the template is named after the config file, so the
  // template's output path resolves to the config file itself.
  static const char* const aggregates[] = {"sosig.toml"};
  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = root_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  config.feed_template_count = 0;

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, NULL, 0, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "aggregate_templates[0]", config_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  unlink(config_path);
  rmdir(root_dir);
}

// Every file below `templates_dir` is a build input too, so an entry whose output path names one
// is rejected. The sibling above claims a content *source*. This claims a *template*. That is the
// other half of the protection: it guards the files the user writes by hand. An output inside
// `templates_dir` fails the root check first, as `test_rejects_output_in_templates_dir` pins, so
// each output here sits in `output_dir` as a hard link to a template, which only the identity claim
// can see. The claim comes from a walk of the whole template tree, so it covers a template named in
// a configured list, one named only by an entry's `template` override, and a partial that nothing
// configures at all, which `template.c` resolves only during the render. Each is a separate case,
// since a claim built from the configured names instead would pass the first two and miss the
// third.
static void test_rejects_template_overwrite(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-template-XXXXXX";
  char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }

  char templates_dir[PATH_MAX];
  char output_dir[PATH_MAX];
  char aggregate_path[PATH_MAX];
  char override_path[PATH_MAX];
  char partial_path[PATH_MAX];
  int n = snprintf(templates_dir, sizeof(templates_dir), "%s/templates", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(templates_dir));
  n = snprintf(output_dir, sizeof(output_dir), "%s/public", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(output_dir));
  n = snprintf(aggregate_path, sizeof(aggregate_path), "%s/templates/index.html", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(aggregate_path));
  n = snprintf(override_path, sizeof(override_path), "%s/templates/custom.html", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(override_path));
  n = snprintf(partial_path, sizeof(partial_path), "%s/templates/partials/card.html", root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(partial_path));
  TEST_CHECK(fs_write_file(aggregate_path, "{{title}}", 9, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(override_path, "{{title}}", 9, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(partial_path, "{{title}}", 9, NULL, 0) == 0);

  // A hard link in `output_dir` for each template file. The link shares the template's identity
  // while its path lies outside `templates_dir`.
  TEST_CHECK(fs_mkdir_p(output_dir, NULL, 0) == 0);
  char aggregate_output[PATH_MAX];
  char override_output[PATH_MAX];
  char partial_output[PATH_MAX];
  n = snprintf(aggregate_output, sizeof(aggregate_output), "%s/index.html", output_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(aggregate_output));
  n = snprintf(override_output, sizeof(override_output), "%s/custom.html", output_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(override_output));
  n = snprintf(partial_output, sizeof(partial_output), "%s/card.html", output_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(partial_output));
  TEST_CHECK(link(aggregate_path, aggregate_output) == 0);
  TEST_CHECK(link(override_path, override_output) == 0);
  TEST_CHECK(link(partial_path, partial_output) == 0);

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
                                       aggregate_entries, 1, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/a.md", aggregate_output);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  manifest_free(&manifest);

  // Second case: the collision is with an entry's own `template` override. No template list is
  // configured here, so the file is known only by that override and by the tree walk.
  config.aggregate_template_count = 0;
  struct ContentEntry override_entry = {
      .output_path = override_output, .source_path = "content/b.md", .template = "custom.html"};
  const struct ContentEntry* override_entries[] = {&override_entry};
  struct Manifest override_manifest;
  manifest_init(&override_manifest);
  char override_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&override_manifest, &config, "sosig.toml", &sources,
                                       override_entries, 1, override_err,
                                       sizeof(override_err)) == -1);
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/b.md", override_output);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(override_err, expected) == 0);
  manifest_free(&override_manifest);

  // Third case: the collision is with a partial that no configuration or entry names. Only the
  // walk of `templates_dir` can know the partial is there.
  struct ContentEntry partial_entry = {.output_path = partial_output,
                                       .source_path = "content/c.md"};
  const struct ContentEntry* partial_entries[] = {&partial_entry};
  struct Manifest partial_manifest;
  manifest_init(&partial_manifest);
  char partial_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&partial_manifest, &config, "sosig.toml", &sources,
                                       partial_entries, 1, partial_err, sizeof(partial_err)) == -1);
  n = snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/c.md", partial_output);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(partial_err, expected) == 0);
  manifest_free(&partial_manifest);

  path_list_free(&sources);
  site_config_free(&config);
  remove_fixture_tree(root_dir);
}

// An output that would land below `content_dir` is rejected even though no file sits at its path
// yet. Left through, `output_dir = "."` with a permalink aimed into the content tree wrote a
// Markdown file there, and the next build failed to parse it as a content source. The root is
// compared by identity, so an alternate spelling of it, a symlink to it, and an `output_dir` inside
// it are rejected the same way.
static void test_rejects_output_in_content_dir(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-content-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "content/a.md", "") == 0);
  TEST_CHECK(write_fixture_file(root_dir, "content/public/old.html", "") == 0);
  struct Arena arena;
  arena_init(&arena);
  const char* content_dir = path_join(root_dir, "content", &arena);
  const char* content_link = path_join(root_dir, "content-link", &arena);
  const char* output_dir = path_join(root_dir, "out", &arena);
  const char* output_link = path_join(root_dir, "out/link", &arena);
  TEST_ASSERT(content_dir != NULL && content_link != NULL && output_dir != NULL &&
              output_link != NULL);
  TEST_CHECK(symlink(content_dir, content_link) == 0);
  TEST_CHECK(fs_mkdir_p(output_dir, NULL, 0) == 0);
  TEST_CHECK(symlink(content_dir, output_link) == 0);

  struct SiteConfig config;
  site_config_init(&config);
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  // The output is a new file below the root, spelled the way `output_dir = "."` spells it.
  config.content_dir = content_dir;
  config.output_dir = root_dir;
  check_rejects_output_in_root(&config, path_join(root_dir, "content/b.md", &arena), "content_dir");

  // The root is configured through a symlink and spelled with a `./` component.
  config.content_dir = path_join(root_dir, "./content-link", &arena);
  check_rejects_output_in_root(&config, path_join(root_dir, "content/sub/b.md", &arena),
                               "content_dir");

  // `output_dir` itself lies inside the root, so every output does.
  config.content_dir = content_dir;
  config.output_dir = path_join(root_dir, "content/public", &arena);
  check_rejects_output_in_root(&config, path_join(config.output_dir, "a.html", &arena),
                               "content_dir");

  // A symlink inside `output_dir` leads into the root.
  config.output_dir = output_dir;
  check_rejects_output_in_root(&config, path_join(output_dir, "link/b.md", &arena), "content_dir");

  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output that would land below `templates_dir` is rejected even though no file sits at its path
// yet. Left through, a permalink aimed at `partials/` wrote a partial that the same build's
// aggregate render then read back. The check covers configured template outputs as well as content
// entries.
static void test_rejects_output_in_templates_dir(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-templates-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  TEST_CHECK(write_fixture_file(root_dir, "templates/partials/card.html", "") == 0);
  struct Arena arena;
  arena_init(&arena);

  struct SiteConfig config;
  site_config_init(&config);
  config.templates_dir = path_join(root_dir, "templates", &arena);
  config.output_dir = root_dir;
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;
  TEST_ASSERT(config.templates_dir != NULL);
  check_rejects_output_in_root(&config, path_join(root_dir, "templates/partials/b.html", &arena),
                               "templates_dir");

  // A configured template whose name leads into the template tree is rejected by its list entry.
  static const char* const aggregates[] = {"templates/index.html"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, NULL, 0, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "output path would write inside 'templates_dir' for "
                         "'aggregate_templates[0]': '%s/templates/index.html'",
                         root_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A `templates_dir` that exists but cannot be walked fails the manifest rather than claiming
// nothing, because a partial the walk could not see would stay overwritable. A regular file in
// place of the directory is the portable way to make the walk fail. The reason names the path the
// walk failed on, so the configured root is not repeated ahead of it.
static void test_rejects_unlistable_templates_dir(void) {
  char root_dir_template[] = "/tmp/sosig-manifest-builder-unlistable-XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  TEST_ASSERT(write_fixture_file(root_dir, "templates", "not a directory") == 0);
  struct Arena arena;
  arena_init(&arena);
  char* templates_dir = path_join(root_dir, "templates", &arena);
  TEST_ASSERT(templates_dir != NULL);

  struct SiteConfig config;
  site_config_init(&config);
  config.output_dir = "public";
  config.templates_dir = templates_dir;
  config.aggregate_template_count = 0;
  config.feed_template_count = 0;

  struct PathList sources;
  path_list_init(&sources);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, "sosig.toml", &sources, NULL, 0, err,
                                       sizeof(err)) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "failed to list template files: cannot open directory: %s ('%s')",
                         error_system_message(reason, sizeof(reason), ENOTDIR), templates_dir);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_CHECK(manifest.count == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A configured template whose output path exceeds the whole-path limit is rejected, and the
// diagnostic still carries the limit. This branch is reachable only when the template name is
// longer than `OUTPUT_PATH_RELATIVE_LEN_MAX`, which is larger than `ERROR_MESSAGE_SIZE`, so leading
// with the name would make the limit clause unreachable at every triggering input. The message
// would be 511 bytes of filename and nothing else. Asserting the head is what pins the ordering.
static void test_rejects_oversize_template_path(void) {
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
                                       sizeof(err)) == -1);
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
static void test_rejects_oversize_template_segment(void) {
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
                                       sizeof(err)) == -1);
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

// A configured template's output path is its name joined below `output_dir`, nested directories
// included. `site_writer` writes to this same path, so it is the one the manifest checked.
static void test_derive_template_output_joins_output_dir(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* top = manifest_builder_derive_template_output("public", "index.html", &arena);
  const char* nested = manifest_builder_derive_template_output("public", "feeds/atom.xml", &arena);
  TEST_ASSERT(top != NULL && nested != NULL);
  TEST_CHECK(strcmp(top, "public/index.html") == 0);
  TEST_CHECK(strcmp(nested, "public/feeds/atom.xml") == 0);
  arena_free(&arena);
}

// The writing this module guards lives in `site_writer`, whose `write_*` functions are exercised
// end-to-end by the golden test suite rather than in a unit test. Entry ordering and `site.updated`
// derivation live in `content_entry` and are tested in `src/domain/test_content_entry.c`.

TEST_LIST = {
    {"accepts unique", test_accepts_unique},
    {"accepts missing templates dir", test_accepts_missing_templates_dir},
    {"accepts output beside input roots", test_accepts_output_beside_input_roots},
    {"rejects duplicate", test_rejects_duplicate},
    {"rejects case folded duplicate", test_rejects_case_folded_duplicate},
    {"rejects prefix collision", test_rejects_prefix_collision},
    {"rejects duplicate template", test_rejects_duplicate_template},
    {"rejects input overwrite", test_rejects_input_overwrite},
    {"rejects config overwrite", test_rejects_config_overwrite},
    {"rejects template overwrite", test_rejects_template_overwrite},
    {"rejects output in content dir", test_rejects_output_in_content_dir},
    {"rejects output in templates dir", test_rejects_output_in_templates_dir},
    {"rejects unlistable templates dir", test_rejects_unlistable_templates_dir},
    {"rejects oversize template path", test_rejects_oversize_template_path},
    {"rejects oversize template segment", test_rejects_oversize_template_segment},
    {"derive template output joins output dir", test_derive_template_output_joins_output_dir},
    {NULL, NULL},
};
