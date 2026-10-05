#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
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
 * @brief Creates a fixture holding `sosig.toml` and `templates/content.html`, and points a
 *        configuration at it.
 *
 * `content_dir` is `content`, `output_dir` is `public`, and `templates_dir` is `templates`, each
 * below `root_dir`. No aggregate or feed template is configured. Only the two files exist, so no
 * source or output is present.
 *
 * @param root_dir    Writable `mkdtemp` template. Receives the created directory path.
 * @param arena       Arena that owns the joined paths. Must not be `NULL`.
 * @param site_config Configuration to initialize. The caller releases it with `site_config_free`.
 *                    Must not be `NULL`.
 * @return The path of the fixture's `sosig.toml`, owned by `arena`.
 */
static char* init_manifest_fixture(char* root_dir,
                                   struct Arena* arena,
                                   struct SiteConfig* site_config) {
  TEST_ASSERT(init_fixture_dir(root_dir) != NULL);
  TEST_ASSERT(write_fixture_file(root_dir, "sosig.toml", "title = 'x'\n") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/content.html", "content") == 0);
  char* config_path = path_join(root_dir, "sosig.toml", arena);
  site_config_init(site_config);
  site_config->content_dir = path_join(root_dir, "content", arena);
  site_config->output_dir = path_join(root_dir, "public", arena);
  site_config->templates_dir = path_join(root_dir, "templates", arena);
  site_config->aggregate_template_count = 0;
  site_config->feed_template_count = 0;
  TEST_ASSERT(config_path != NULL && site_config->content_dir != NULL &&
              site_config->output_dir != NULL && site_config->templates_dir != NULL);
  return config_path;
}

/**
 * @brief Populates a manifest with one content entry and checks the input-root rejection.
 *
 * @param config      Configuration whose roots the output is checked against. Must not be `NULL`.
 * @param config_path Path of the fixture's `sosig.toml`. Must not be `NULL`.
 * @param output_path Output path of the single entry, joined onto `config->output_dir`. Must not be
 *                    `NULL`.
 * @param root_key    Config key of the root the diagnostic must name. Must not be `NULL`.
 */
static void check_rejects_output_in_root(const struct SiteConfig* config,
                                         const char* config_path,
                                         const char* output_path,
                                         const char* root_key) {
  struct ContentEntry entry = {.output_path = output_path, .source_path = "content/a.md"};
  const struct ContentEntry* entries[] = {&entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, config, config_path, &empty, entries, 1, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "output path would write inside '%s' for '%s': '%s'",
               root_key, "content/a.md", output_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);
  manifest_free(&manifest);
  path_list_free(&empty);
}

/**
 * @brief Checks that `manifest_builder_check_output_dir` rejects `config->output_dir` and creates
 *        nothing.
 *
 * @param config   Configuration whose `output_dir` lies at or below an input root. Must not be
 *                 `NULL`.
 * @param root_key Config key of the root the diagnostic must name. Must not be `NULL`.
 */
static void check_rejects_output_dir_in_root(const struct SiteConfig* config,
                                             const char* root_key) {
  const bool had_output_dir = access(config->output_dir, F_OK) == 0;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_check_output_dir(config, err, sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "output directory would write inside '%s': '%s'",
               root_key, config->output_dir);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);
  TEST_CHECK((access(config->output_dir, F_OK) == 0) == had_output_dir);
}

// Every content entry, aggregate, and feed output is registered exactly once.
static void test_registers_complete_output_set(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", "source") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "content/b.md", "source") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/index.html", "index") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/feed.xml", "feed") == 0);
  char* first_source = path_join(config.content_dir, "a.md", &arena);
  char* second_source = path_join(config.content_dir, "b.md", &arena);
  char* first_output = path_join(config.output_dir, "a/index.html", &arena);
  char* second_output = path_join(config.output_dir, "b/index.html", &arena);
  TEST_ASSERT(first_source != NULL && second_source != NULL && first_output != NULL &&
              second_output != NULL);

  static const char* const aggregates[] = {"index.html"};
  static const char* const feeds[] = {"feed.xml"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  config.feed_templates = feeds;
  config.feed_template_count = 1;
  struct ContentEntry first_entry = {.output_path = first_output, .source_path = first_source};
  struct ContentEntry second_entry = {.output_path = second_output, .source_path = second_source};
  const struct ContentEntry* entries[] = {&first_entry, &second_entry};
  struct PathList sources;
  path_list_init(&sources);
  TEST_ASSERT(path_list_push(&sources, first_source) == 0);
  TEST_ASSERT(path_list_push(&sources, second_source) == 0);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, entries, 2, err,
                                       sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_MSG("actual: '%s'", err);
  TEST_CHECK(manifest.count == 4);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A `templates_dir` that does not exist holds no file an output could overwrite, so it claims
// nothing and the manifest still populates. The build refuses a missing `templates_dir` before the
// manifest runs, as `test_reports_absent_templates_dir` in `src/app/test_cmd_build.c` pins, so this
// is the module's own contract rather than a path the build reaches.
static void test_accepts_missing_templates_dir(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  char* entry_output = path_join(config.output_dir, "a.html", &arena);
  char* templates_dir = path_join(root_dir, "missing", &arena);
  TEST_ASSERT(entry_output != NULL && templates_dir != NULL);
  config.templates_dir = templates_dir;
  struct ContentEntry entry = {.output_path = entry_output, .source_path = "content/a.md"};
  const struct ContentEntry* entries[] = {&entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, entries, 1, err,
                                       sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(manifest.count == 1);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output beside an input root, in a directory whose name only starts with the root's name, is
// accepted, because the root check compares whole directories rather than path prefixes.
static void test_accepts_output_beside_input_roots(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", "source") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "contentx/old.html", "old") == 0);
  config.output_dir = root_dir;
  char* content_output = path_join(root_dir, "contentx/index.html", &arena);
  char* templates_output = path_join(root_dir, "templatesx/index.html", &arena);
  TEST_ASSERT(content_output != NULL && templates_output != NULL);
  struct ContentEntry content_entry = {.output_path = content_output,
                                       .source_path = "content/a.md"};
  struct ContentEntry templates_entry = {.output_path = templates_output,
                                         .source_path = "content/b.md"};
  const struct ContentEntry* entries[] = {&content_entry, &templates_entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, entries, 2, err,
                                       sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_MSG("actual: '%s'", err);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An `output_dir` outside every input root passes the early check whether or not it exists yet.
// That includes a directory whose name only starts with a root's name, and one that holds the roots
// rather than lying inside them, which is the `output_dir = "."` layout.
static void test_check_output_dir_accepts_dir_outside_input_roots(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  (void)init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", "source") == 0);
  const char* const output_dirs[] = {
      root_dir,
      path_join(root_dir, "public", &arena),
      path_join(root_dir, "contentx/new", &arena),
      path_join(root_dir, "content/../public/new", &arena),
  };
  for (size_t i = 0; i < sizeof(output_dirs) / sizeof(output_dirs[0]); i++) {
    TEST_ASSERT(output_dirs[i] != NULL);
    config.output_dir = output_dirs[i];
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(manifest_builder_check_output_dir(&config, err, sizeof(err)) == 0);
    TEST_MSG("output_dir: '%s', actual: '%s'", output_dirs[i], err);
  }

  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two entries claiming the same output path are rejected with a diagnostic naming the path.
static void test_rejects_duplicate(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  char* output_path = path_join(config.output_dir, "post/index.html", &arena);
  char* first_label = path_join(config.content_dir, "a.md", &arena);
  char* second_label = path_join(config.content_dir, "b.md", &arena);
  TEST_ASSERT(output_path != NULL && first_label != NULL && second_label != NULL);
  struct ContentEntry first_entry = {.output_path = output_path, .source_path = first_label};
  struct ContentEntry second_entry = {.output_path = output_path, .source_path = second_label};
  const struct ContentEntry* entries[] = {&first_entry, &second_entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, entries, 2, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               first_label, second_label, output_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two entries whose output paths differ only in ASCII case claim one file on a case-insensitive
// filesystem such as the macOS default, where the second write would silently replace the first.
// They are rejected as a duplicate on every platform, so a site that builds on Linux does not break
// on macOS. The trailing path is the second claim's own spelling.
static void test_rejects_case_folded_duplicate(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  char* first_output = path_join(config.output_dir, "About/index.html", &arena);
  char* second_output = path_join(config.output_dir, "about/index.html", &arena);
  char* first_label = path_join(config.content_dir, "a.md", &arena);
  char* second_label = path_join(config.content_dir, "b.md", &arena);
  TEST_ASSERT(first_output != NULL && second_output != NULL && first_label != NULL &&
              second_label != NULL);
  struct ContentEntry first_entry = {.output_path = first_output, .source_path = first_label};
  struct ContentEntry second_entry = {.output_path = second_output, .source_path = second_label};
  const struct ContentEntry* entries[] = {&first_entry, &second_entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, entries, 2, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               first_label, second_label, second_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Two entries whose output paths collide as a file and a directory, where one is a `/`-delimited
// prefix of the other, are rejected even though neither is a duplicate. The two producers lead the
// message ahead of the one unbounded path. The ancestor path is not repeated because it is a prefix
// of the path that is printed.
static void test_rejects_prefix_collision(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  char* file_output = path_join(config.output_dir, "post", &arena);
  char* nested_output = path_join(config.output_dir, "post/index.html", &arena);
  char* file_label = path_join(config.content_dir, "a.md", &arena);
  char* nested_label = path_join(config.content_dir, "b.md", &arena);
  TEST_ASSERT(file_output != NULL && nested_output != NULL && file_label != NULL &&
              nested_label != NULL);
  struct ContentEntry file_entry = {.output_path = file_output, .source_path = file_label};
  struct ContentEntry nested_entry = {.output_path = nested_output, .source_path = nested_label};
  const struct ContentEntry* entries[] = {&file_entry, &nested_entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, entries, 2, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "output path for '%s' nests under output path for '%s': '%s'",
                                    nested_label, file_label, nested_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `manifest_builder_populate` rejects two configured templates that claim one output path. It names
// each by its list entry rather than by its own name. These two configurations are different
// mistakes with different fixes. A bare template name reports both as
// `for 'dup.html' and 'dup.html'`, which reads as a value colliding with itself and says nothing
// about which key to edit. Both cases are asserted because a label carrying only the config key
// would distinguish them in the second case but not the first.
static void test_rejects_duplicate_template(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  char* template_output = path_join(config.output_dir, "dup.html", &arena);
  TEST_ASSERT(template_output != NULL);
  struct PathList empty;
  path_list_init(&empty);

  // First case: the same name twice inside `aggregate_templates`, so the two indexes differ.
  static const char* const aggregates[] = {"dup.html", "dup.html"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  struct Manifest one_list_manifest;
  manifest_init(&one_list_manifest);
  char one_list_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&one_list_manifest, &config, config_path, &empty, NULL, 0,
                                       one_list_err, sizeof(one_list_err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "aggregate_templates[0]", "aggregate_templates[1]", template_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(one_list_err, expected) == 0);
  manifest_free(&one_list_manifest);

  // Second case: one name in each list, so the two keys differ.
  static const char* const shared[] = {"dup.html"};
  config.aggregate_templates = shared;
  config.aggregate_template_count = 1;
  config.feed_templates = shared;
  config.feed_template_count = 1;
  struct Manifest shared_manifest;
  manifest_init(&shared_manifest);
  char shared_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&shared_manifest, &config, config_path, &empty, NULL, 0,
                                       shared_err, sizeof(shared_err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "duplicate output path for '%s' and '%s': '%s'",
               "aggregate_templates[0]", "feed_templates[0]", template_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(shared_err, expected) == 0);
  manifest_free(&shared_manifest);

  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// `manifest_builder_populate` rejects an output path that names one of the build's own input files,
// so a build cannot overwrite its own source. Here an entry's output is a hard link to its own
// content source, so the two paths differ and only filesystem identity sees the collision. An
// output inside `content_dir` fails the root check first, as `test_rejects_output_in_content_dir`
// pins.
static void test_rejects_input_overwrite(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", "source") == 0);
  char* source_path = path_join(config.content_dir, "a.md", &arena);
  char* entry_output = path_join(config.output_dir, "a.md", &arena);
  TEST_ASSERT(source_path != NULL && entry_output != NULL);
  TEST_ASSERT(fs_mkdir_p(config.output_dir, NULL, 0) == 0);
  TEST_ASSERT(link(source_path, entry_output) == 0);
  struct ContentEntry entry = {.output_path = entry_output, .source_path = source_path};
  const struct ContentEntry* entries[] = {&entry};
  struct PathList sources;
  path_list_init(&sources);
  TEST_ASSERT(path_list_push(&sources, source_path) == 0);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, entries, 1, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               source_path, entry_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The input check finds an aimed-at source wherever it falls among many claimed inputs, and still
// accepts an existing output that is not an input. The sources are created in reverse name order,
// so the order they are claimed in differs from the order their identities sort in. Each aimed-at
// output is a hard link to its source, for the reason `test_rejects_input_overwrite` gives.
static void test_rejects_input_overwrite_among_many_inputs(void) {
  enum { SOURCE_COUNT = 32 };
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "public/existing.html", "output") == 0);
  char* existing_output = path_join(config.output_dir, "existing.html", &arena);
  TEST_ASSERT(existing_output != NULL);
  for (size_t i = SOURCE_COUNT; i > 0; i--) {
    char relative[32];
    (void)snprintf(relative, sizeof(relative), "content/%02zu.md", i - 1);
    TEST_ASSERT(write_fixture_file(root_dir, relative, "source") == 0);
  }
  struct PathList sources;
  path_list_init(&sources);
  for (size_t i = 0; i < SOURCE_COUNT; i++) {
    char name[16];
    (void)snprintf(name, sizeof(name), "%02zu.md", i);
    char* source_path = path_join(config.content_dir, name, &arena);
    TEST_ASSERT(source_path != NULL && path_list_push(&sources, source_path) == 0);
  }

  static const size_t targets[] = {0, SOURCE_COUNT / 2, SOURCE_COUNT - 1};
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    const char* target = sources.items[targets[i]];
    char name[16];
    (void)snprintf(name, sizeof(name), "%02zu.html", targets[i]);
    char* output = path_join(config.output_dir, name, &arena);
    TEST_ASSERT(output != NULL && link(target, output) == 0);
    // The existing output comes first, so a lookup that wrongly matched it would name it instead.
    struct ContentEntry existing = {.output_path = existing_output, .source_path = "content/x.md"};
    struct ContentEntry aimed = {.output_path = output, .source_path = target};
    const struct ContentEntry* entries[] = {&existing, &aimed};
    struct Manifest manifest;
    manifest_init(&manifest);
    char err[ERROR_MESSAGE_SIZE] = "";

    TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &sources, entries, 2, err,
                                         sizeof(err)) == -1);
    char expected[ERROR_MESSAGE_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "output path would overwrite build input for '%s': '%s'", target, output);
    TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);
    TEST_MSG("target %zu: '%s'", targets[i], err);
    manifest_free(&manifest);
  }

  path_list_free(&sources);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// The config file is an input like any other, so an output path naming it is rejected. Without this
// the build would overwrite the file that configured it and still report success: `output_dir` at
// the project root plus an aggregate template named `sosig.toml` is all it takes, and the loss is
// unrecoverable.
static void test_rejects_config_overwrite(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  static const char* const aggregates[] = {"sosig.toml"};
  config.output_dir = root_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, NULL, 0, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "aggregate_templates[0]", config_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// Every file below `templates_dir` is a build input, so an output naming a configured template, an
// entry's `template` override, or a partial is rejected. An output inside `templates_dir` fails the
// root check first, as `test_rejects_output_in_templates_dir` pins, so each output here sits in
// `output_dir` as a hard link to a template, which only the identity claim can see. The override
// and the partial are named in no configured list, and the partial is named nowhere at all, so only
// the walk of the template tree can claim it, which is the case a claim of just the configured
// templates would miss.
static void test_rejects_template_overwrite(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/index.html", "index") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/custom.html", "custom") == 0);
  TEST_ASSERT(write_fixture_file(root_dir, "templates/partials/card.html", "card") == 0);
  char* aggregate_path = path_join(config.templates_dir, "index.html", &arena);
  char* override_path = path_join(config.templates_dir, "custom.html", &arena);
  char* partial_path = path_join(config.templates_dir, "partials/card.html", &arena);
  char* aggregate_output = path_join(config.output_dir, "index.html", &arena);
  char* override_output = path_join(config.output_dir, "custom.html", &arena);
  char* partial_output = path_join(config.output_dir, "card.html", &arena);
  TEST_ASSERT(aggregate_path != NULL && override_path != NULL && partial_path != NULL &&
              aggregate_output != NULL && override_output != NULL && partial_output != NULL);
  TEST_ASSERT(fs_mkdir_p(config.output_dir, NULL, 0) == 0);
  TEST_ASSERT(link(aggregate_path, aggregate_output) == 0);
  TEST_ASSERT(link(override_path, override_output) == 0);
  TEST_ASSERT(link(partial_path, partial_output) == 0);
  struct PathList empty;
  path_list_init(&empty);

  // First case: an entry lands on a hard link to a template named in `aggregate_templates`.
  static const char* const aggregates[] = {"index.html"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct ContentEntry aggregate_entry = {.output_path = aggregate_output,
                                         .source_path = "content/a.md"};
  const struct ContentEntry* aggregate_entries[] = {&aggregate_entry};
  struct Manifest aggregate_manifest;
  manifest_init(&aggregate_manifest);
  char aggregate_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&aggregate_manifest, &config, config_path, &empty,
                                       aggregate_entries, 1, aggregate_err,
                                       sizeof(aggregate_err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/a.md", aggregate_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(aggregate_err, expected) == 0);
  manifest_free(&aggregate_manifest);

  // Second case: an entry lands on a hard link to its own `template` override, which no configured
  // list names.
  config.aggregate_template_count = 0;
  struct ContentEntry override_entry = {
      .output_path = override_output, .source_path = "content/b.md", .template = "custom.html"};
  const struct ContentEntry* override_entries[] = {&override_entry};
  struct Manifest override_manifest;
  manifest_init(&override_manifest);
  char override_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&override_manifest, &config, config_path, &empty,
                                       override_entries, 1, override_err,
                                       sizeof(override_err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/b.md", override_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(override_err, expected) == 0);
  manifest_free(&override_manifest);

  // Third case: an entry lands on a hard link to an unconfigured partial.
  struct ContentEntry partial_entry = {.output_path = partial_output,
                                       .source_path = "content/c.md"};
  const struct ContentEntry* partial_entries[] = {&partial_entry};
  struct Manifest partial_manifest;
  manifest_init(&partial_manifest);
  char partial_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&partial_manifest, &config, config_path, &empty,
                                       partial_entries, 1, partial_err, sizeof(partial_err)) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "output path would overwrite build input for '%s': '%s'",
               "content/c.md", partial_output);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(partial_err, expected) == 0);
  manifest_free(&partial_manifest);

  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An `output_dir` at or below an input root is rejected before anything walks or writes, whether or
// not it exists yet, and the check creates none of it. A missing `output_dir` is judged by the
// directory it would be created in, so a nested path, a `..` past a missing component, and a path
// through a symlinked root all land where `fs_mkdir_p` would put them.
static void test_check_output_dir_rejects_dir_in_input_root(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  (void)init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "content/public/old.html", "old") == 0);
  const char* content_link = path_join(root_dir, "content-link", &arena);
  TEST_ASSERT(content_link != NULL);
  TEST_CHECK(symlink(config.content_dir, content_link) == 0);
  const char* const content_output_dirs[] = {
      config.content_dir,
      path_join(root_dir, "content/public", &arena),
      path_join(root_dir, "content/new/deeper", &arena),
      path_join(root_dir, "missing/../content/new", &arena),
      path_join(root_dir, "content-link/new", &arena),
  };
  for (size_t i = 0; i < sizeof(content_output_dirs) / sizeof(content_output_dirs[0]); i++) {
    TEST_ASSERT(content_output_dirs[i] != NULL);
    config.output_dir = content_output_dirs[i];
    check_rejects_output_dir_in_root(&config, "content_dir");
  }

  config.output_dir = path_join(root_dir, "templates/partials", &arena);
  TEST_ASSERT(config.output_dir != NULL);
  check_rejects_output_dir_in_root(&config, "templates_dir");

  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output that would land below `content_dir` is rejected even though no file sits at its path
// yet, because the next build could parse it as a content source. The root is compared by identity,
// so an alternate spelling of it, a symlink to it, a symlink inside `output_dir` that leads into
// it, and an `output_dir` inside it are rejected the same way.
static void test_rejects_output_in_content_dir(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "content/a.md", "source") == 0);
  const char* content_dir = config.content_dir;
  const char* content_link = path_join(root_dir, "content-link", &arena);
  const char* output_dir = path_join(root_dir, "out", &arena);
  const char* output_link = path_join(root_dir, "out/link", &arena);
  TEST_ASSERT(content_link != NULL && output_dir != NULL && output_link != NULL);
  TEST_CHECK(symlink(content_dir, content_link) == 0);
  TEST_CHECK(fs_mkdir_p(output_dir, NULL, 0) == 0);
  TEST_CHECK(symlink(content_dir, output_link) == 0);

  // The output is a new file below the root, reached from an `output_dir` that holds the root.
  config.output_dir = root_dir;
  check_rejects_output_in_root(&config, config_path, path_join(root_dir, "content/b.md", &arena),
                               "content_dir");

  // The root is configured through a symlink and spelled with a `./` component.
  config.content_dir = path_join(root_dir, "./content-link", &arena);
  check_rejects_output_in_root(&config, config_path,
                               path_join(root_dir, "content/sub/b.md", &arena), "content_dir");

  // A symlink inside `output_dir` leads into the root.
  config.content_dir = content_dir;
  config.output_dir = output_dir;
  check_rejects_output_in_root(&config, config_path, path_join(output_dir, "link/b.md", &arena),
                               "content_dir");

  // `output_dir` itself lies inside the root, and does not exist yet. Populate runs the early check
  // itself, so a caller that skipped it is still refused, with the early check's diagnostic.
  config.output_dir = path_join(root_dir, "content/public", &arena);
  TEST_ASSERT(config.output_dir != NULL);
  struct ContentEntry entry = {.output_path = path_join(config.output_dir, "a.html", &arena),
                               .source_path = "content/a.md"};
  const struct ContentEntry* entries[] = {&entry};
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, entries, 1, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "output directory would write inside 'content_dir': '%s'", config.output_dir);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);
  TEST_CHECK(manifest.count == 0);
  manifest_free(&manifest);
  path_list_free(&empty);

  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// An output that would land below `templates_dir` is rejected even though no file sits at its path
// yet, because the same build's render could read it back as a partial. The check covers configured
// template outputs as well as content entries.
static void test_rejects_output_in_templates_dir(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  config.output_dir = root_dir;
  check_rejects_output_in_root(&config, config_path,
                               path_join(root_dir, "templates/partials/b.html", &arena),
                               "templates_dir");

  // A configured template whose name leads into the template tree is rejected by its list entry.
  static const char* const aggregates[] = {"templates/index.html"};
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, NULL, 0, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "output path would write inside 'templates_dir' for "
                                    "'aggregate_templates[0]': '%s/templates/index.html'",
                                    root_dir);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("actual: '%s'", err);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A `templates_dir` that exists but cannot be walked fails the manifest rather than claiming
// nothing, because a partial the walk could not see would stay overwritable. A regular file in
// place of the directory is the portable way to make the walk fail. The reason names the path the
// walk failed on, so the configured root is not repeated ahead of it.
static void test_rejects_unlistable_templates_dir(void) {
  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  TEST_ASSERT(write_fixture_file(root_dir, "templates-file", "not a directory") == 0);
  char* templates_dir = path_join(root_dir, "templates-file", &arena);
  TEST_ASSERT(templates_dir != NULL);
  config.templates_dir = templates_dir;
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, NULL, 0, err,
                                       sizeof(err)) == -1);
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected), "failed to list template files: cannot open directory: %s ('%s')",
      error_system_message(reason, sizeof(reason), ENOTDIR), templates_dir);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_CHECK(manifest.count == 0);

  manifest_free(&manifest);
  path_list_free(&empty);
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

  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, NULL, 0, err,
                                       sizeof(err)) == -1);
  char expected_head[128];
  const int expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "output path exceeds max output path length (%zu bytes) at %zu bytes (for "
               "'aggregate_templates[0]'): '",
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
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
}

// A configured template with one overlong path segment is rejected against the per-filename limit
// rather than the whole-path one, so the two limits stay distinguishable. The whole message fits,
// so it is asserted in full. A feed template exercises the second configured list.
static void test_rejects_oversize_template_segment(void) {
  char name[FILENAME_LEN_MAX + 32];
  memset(name, 'b', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const char* feeds[] = {name};

  char root_dir[] = "/tmp/sosig-manifest-XXXXXX";
  struct Arena arena;
  arena_init(&arena);
  struct SiteConfig config;
  const char* config_path = init_manifest_fixture(root_dir, &arena, &config);
  config.feed_templates = feeds;
  config.feed_template_count = 1;
  struct PathList empty;
  path_list_init(&empty);
  struct Manifest manifest;
  manifest_init(&manifest);
  char err[ERROR_MESSAGE_SIZE] = "";

  TEST_CHECK(manifest_builder_populate(&manifest, &config, config_path, &empty, NULL, 0, err,
                                       sizeof(err)) == -1);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "output path segment exceeds max filename length (%zu bytes) at %zu "
               "bytes (for 'feed_templates[0]'): '%s'",
               (size_t)FILENAME_LEN_MAX, sizeof(name) - 1, name);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  manifest_free(&manifest);
  path_list_free(&empty);
  site_config_free(&config);
  arena_free(&arena);
  remove_fixture_tree(root_dir);
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

// The writing this module guards lives in `site_writer` and is tested in
// `src/build/test_site_writer.c`. Entry ordering and `site.updated` derivation live in
// `content_entry` and are tested in `src/domain/test_content_entry.c`.

TEST_LIST = {
    {"registers complete output set", test_registers_complete_output_set},
    {"accepts missing templates dir", test_accepts_missing_templates_dir},
    {"accepts output beside input roots", test_accepts_output_beside_input_roots},
    {"check output dir accepts dir outside input roots",
     test_check_output_dir_accepts_dir_outside_input_roots},
    {"rejects duplicate", test_rejects_duplicate},
    {"rejects case folded duplicate", test_rejects_case_folded_duplicate},
    {"rejects prefix collision", test_rejects_prefix_collision},
    {"rejects duplicate template", test_rejects_duplicate_template},
    {"rejects input overwrite", test_rejects_input_overwrite},
    {"rejects input overwrite among many inputs", test_rejects_input_overwrite_among_many_inputs},
    {"rejects config overwrite", test_rejects_config_overwrite},
    {"rejects template overwrite", test_rejects_template_overwrite},
    {"check output dir rejects dir in input root", test_check_output_dir_rejects_dir_in_input_root},
    {"rejects output in content dir", test_rejects_output_in_content_dir},
    {"rejects output in templates dir", test_rejects_output_in_templates_dir},
    {"rejects unlistable templates dir", test_rejects_unlistable_templates_dir},
    {"rejects oversize template path", test_rejects_oversize_template_path},
    {"rejects oversize template segment", test_rejects_oversize_template_segment},
    {"derive template output joins output dir", test_derive_template_output_joins_output_dir},
    {NULL, NULL},
};
