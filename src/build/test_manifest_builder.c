#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build/manifest_builder.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/manifest.h"
#include "domain/site_config.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host. The identity checks see what they would see on
// disk: two spellings of one directory, a symlink and its target, and two hard links to one file
// share an identity, and `..` resolves against the directory actually reached.

/** Path of the fixture's configuration file. */
#define CONFIG_PATH S("site/sosig.toml")

/**
 * @brief Creates a host holding `site/sosig.toml` and `site/templates/content.html`, and points a
 *        configuration at it.
 *
 * `content_dir` is `site/content`, `output_dir` is `site/public`, `templates_dir` is
 * `site/templates`, and `static_dir` is `site/static`. No aggregate or feed template is configured.
 * Only the two files exist, so no source, static file, or output is present.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 * @return The host.
 */
static struct Os* init_manifest_fixture(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->content_dir = S("site/content");
  site_config->output_dir = S("site/public");
  site_config->templates_dir = S("site/templates");
  site_config->static_dir = S("site/static");
  site_config->aggregate_template_count = 0;
  site_config->feed_template_count = 0;
  struct Os* os = os_memory();
  os_memory_write(os, "site/sosig.toml", "title = 'x'\n");
  os_memory_write(os, "site/templates/content.html", "content");
  return os;
}

/**
 * @brief Populates a manifest from content entries, as the build does, with no sources claimed.
 *
 * The manifest's data goes in a reservation of its own, never in the log's arena, so a failure the
 * log records cannot grow over the paths the populate is still using.
 *
 * @param manifest            Manifest that receives the outputs. Must not be `NULL`.
 * @param site_config         Configuration to populate from. Must not be `NULL`.
 * @param source_paths        Claimed content sources, or `NULL` for none.
 * @param static_paths        Enumerated static files, or `NULL` for none.
 * @param content_entries     Array of `content_entry_count` entries. May be `NULL` only when
 *                            `content_entry_count` is 0.
 * @param content_entry_count Number of entries.
 * @param os                  Host the fixture is on. Must not be `NULL`.
 * @param log                 Log that receives every error. Must not be `NULL`.
 * @return The result of `manifest_builder_populate`.
 */
static int populate(struct Manifest* manifest,
                    const struct SiteConfig* site_config,
                    const struct PathList* source_paths,
                    const struct PathList* static_paths,
                    const struct ContentEntry* const* content_entries,
                    ptrdiff_t content_entry_count,
                    struct Os* os,
                    struct Log* log) {
  static const struct PathList empty = {0};
  struct Arena perm = test_arena();
  return manifest_builder_populate(manifest, site_config, CONFIG_PATH,
                                   source_paths != NULL ? source_paths : &empty,
                                   static_paths != NULL ? static_paths : &empty, content_entries,
                                   content_entry_count, os, &perm, test_arena(), log);
}

/**
 * @brief Populates a manifest with one content entry and checks the input-root rejection.
 *
 * @param site_config Configuration whose roots the output is checked against. Must not be `NULL`.
 * @param output_path Terminated output path of the single entry. Must not be `NULL`.
 * @param root_key    Config key of the root the diagnostic must name. Must not be `NULL`.
 * @param os          Host the fixture is on. Must not be `NULL`.
 */
static void check_rejects_output_in_root(const struct SiteConfig* site_config,
                                         const char* output_path,
                                         const char* root_key,
                                         struct Os* os) {
  struct ContentEntry entry = {.output_path = str_from_cstr(output_path),
                               .source_path = S("content/a.md")};
  const struct ContentEntry* entries[] = {&entry};
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, site_config, NULL, NULL, entries, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "output path would write inside '%s' for 'content/a.md': '%s'\n",
                         root_key, output_path));
}

/**
 * @brief Checks that `manifest_builder_check_output_dir` rejects `site_config->output_dir` and
 *        creates nothing.
 *
 * @param site_config Configuration whose `output_dir` lies at or below an input root. Must not be
 *                    `NULL`.
 * @param root_key    Config key of the root the diagnostic must name. Must not be `NULL`.
 * @param os          Host the fixture is on. Must not be `NULL`.
 */
static void check_rejects_output_dir_in_root(const struct SiteConfig* site_config,
                                             const char* root_key,
                                             struct Os* os) {
  struct Arena arena = test_arena();
  const char* output_dir = str_cstr(site_config->output_dir, &arena);
  const bool had_output_dir = os_memory_has_path(os, output_dir);
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_check_output_dir(site_config, os, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "output directory would write inside '%s': '%.*s'\n", root_key,
                         STR_FMT(site_config->output_dir)));
  TEST_CHECK(os_memory_has_path(os, output_dir) == had_output_dir);
  TEST_MSG("output_dir: '%s'", output_dir);
}

/**
 * @brief Builds a content entry with the given output and source paths.
 *
 * @param output_path Terminated output path. Must not be `NULL`.
 * @param source_path Terminated source path. Must not be `NULL`.
 * @return The entry, borrowing both strings.
 */
static struct ContentEntry entry_at(const char* output_path, const char* source_path) {
  return (struct ContentEntry){.output_path = str_from_cstr(output_path),
                               .source_path = str_from_cstr(source_path)};
}

// Every content entry, aggregate, feed, and static file output is registered exactly once.
static void test_registers_complete_output_set(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/content/a.md", "source");
  os_memory_write(os, "site/content/b.md", "source");
  os_memory_write(os, "site/templates/index.html", "index");
  os_memory_write(os, "site/templates/feed.xml", "feed");
  os_memory_write(os, "site/static/a.txt", "static");
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  static const struct Str feeds[] = {STR_INIT("feed.xml")};
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  site_config.feed_templates = feeds;
  site_config.feed_template_count = 1;
  const struct ContentEntry first = entry_at("site/public/a/index.html", "site/content/a.md");
  const struct ContentEntry second = entry_at("site/public/b/index.html", "site/content/b.md");
  const struct ContentEntry* entries[] = {&first, &second};
  struct Arena arena = test_arena();
  struct PathList sources = {0};
  path_list_push(&sources, first.source_path, &arena);
  path_list_push(&sources, second.source_path, &arena);
  struct PathList static_paths = {0};
  path_list_push(&static_paths, S("site/static/a.txt"), &arena);
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, &sources, &static_paths, entries, 2, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest.count == 5);
}

// A `templates_dir` that does not exist holds no file an output could overwrite, so it claims
// nothing and the manifest still populates. The build refuses a missing `templates_dir` before the
// manifest runs, as `test_reports_absent_templates_dir` in `src/app/test_cmd_build.c` pins, so this
// is the module's own contract rather than a path the build reaches.
static void test_accepts_missing_templates_dir(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  site_config.templates_dir = S("site/missing");
  const struct ContentEntry entry = entry_at("site/public/a.html", "content/a.md");
  const struct ContentEntry* entries[] = {&entry};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 1, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest.count == 1);
}

// An output beside an input root, in a directory whose name only starts with the root's name, is
// accepted, because the root check compares whole directories rather than path prefixes.
static void test_accepts_output_beside_input_roots(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/content/a.md", "source");
  os_memory_write(os, "site/static/a.css", "static");
  os_memory_write(os, "site/contentx/old.html", "old");
  site_config.output_dir = S("site");
  const struct ContentEntry content_entry = entry_at("site/contentx/index.html", "content/a.md");
  const struct ContentEntry templates_entry =
      entry_at("site/templatesx/index.html", "content/b.md");
  const struct ContentEntry static_entry = entry_at("site/staticx/index.html", "content/c.md");
  const struct ContentEntry* entries[] = {&content_entry, &templates_entry, &static_entry};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 3, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// An `output_dir` outside every input root passes the early check whether or not it exists yet.
// That includes a directory whose name only starts with a root's name, and one that holds the roots
// rather than lying inside them, which is the `output_dir = "."` layout.
static void test_check_output_dir_accepts_dir_outside_input_roots(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/content/a.md", "source");
  os_memory_write(os, "site/static/a.css", "static");
  static const struct Str output_dirs[] = {
      STR_INIT("site"),
      STR_INIT("site/public"),
      STR_INIT("site/contentx/new"),
      STR_INIT("site/content/../public/new"),
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(output_dirs) / sizeof(output_dirs[0])); i++) {
    site_config.output_dir = output_dirs[i];
    struct Log log = test_log();
    TEST_CHECK(manifest_builder_check_output_dir(&site_config, os, test_arena(), &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_MSG("output_dir: '%.*s'", STR_FMT(output_dirs[i]));
  }
}

// Two entries claiming the same output path are rejected with a diagnostic naming the path.
static void test_rejects_duplicate(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  const struct ContentEntry first = entry_at("site/public/post/index.html", "site/content/a.md");
  const struct ContentEntry second = entry_at("site/public/post/index.html", "site/content/b.md");
  const struct ContentEntry* entries[] = {&first, &second};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 2, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'site/content/a.md' and 'site/content/b.md': "
                         "'site/public/post/index.html'\n"));
}

// Every collision is reported in one run, in registration order, rather than only the first. A
// rejected claim is not recorded, so the manifest keeps one path from each pair.
static void test_reports_every_collision(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  const struct ContentEntry entry_a = entry_at("site/public/one/index.html", "site/content/a.md");
  const struct ContentEntry entry_b = entry_at("site/public/one/index.html", "site/content/b.md");
  const struct ContentEntry entry_c = entry_at("site/public/two/index.html", "site/content/c.md");
  const struct ContentEntry entry_d = entry_at("site/public/two/index.html", "site/content/d.md");
  const struct ContentEntry* entries[] = {&entry_a, &entry_b, &entry_c, &entry_d};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 4, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'site/content/a.md' and 'site/content/b.md': "
                         "'site/public/one/index.html'\n"
                         "duplicate output path for 'site/content/c.md' and 'site/content/d.md': "
                         "'site/public/two/index.html'\n"));
  TEST_CHECK(log.errors == 2);
  TEST_CHECK(manifest.count == 2);
}

// Two entries whose output paths differ only in ASCII case claim one file on a case-insensitive
// filesystem such as the macOS default, where the second write would silently replace the first.
// They are rejected as a duplicate on every platform, so a site that builds on Linux does not break
// on macOS. The trailing path is the second claim's own spelling.
static void test_rejects_case_folded_duplicate(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  const struct ContentEntry first = entry_at("site/public/About/index.html", "site/content/a.md");
  const struct ContentEntry second = entry_at("site/public/about/index.html", "site/content/b.md");
  const struct ContentEntry* entries[] = {&first, &second};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 2, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'site/content/a.md' and 'site/content/b.md': "
                         "'site/public/about/index.html'\n"));
}

// Two entries whose output paths collide as a file and a directory, where one is a `/`-delimited
// prefix of the other, are rejected even though neither is a duplicate. The two producers lead the
// message and the path trails. The ancestor path is not repeated because it is a prefix of the path
// that is printed.
static void test_rejects_prefix_collision(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  const struct ContentEntry file = entry_at("site/public/post", "site/content/a.md");
  const struct ContentEntry nested = entry_at("site/public/post/index.html", "site/content/b.md");
  const struct ContentEntry* entries[] = {&file, &nested};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 2, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'site/content/b.md' nests under output path for "
                         "'site/content/a.md': 'site/public/post/index.html'\n"));
}

// Every output that nests under another is reported in one run, not only the first.
static void test_rejects_every_prefix_collision(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  const struct ContentEntry post = entry_at("site/public/post", "site/content/a.md");
  const struct ContentEntry post_index =
      entry_at("site/public/post/index.html", "site/content/b.md");
  const struct ContentEntry page = entry_at("site/public/page", "site/content/c.md");
  const struct ContentEntry page_index =
      entry_at("site/public/page/index.html", "site/content/d.md");
  const struct ContentEntry* entries[] = {&post, &post_index, &page, &page_index};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 4, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'site/content/b.md' nests under output path for "
                         "'site/content/a.md': 'site/public/post/index.html'\n"
                         "output path for 'site/content/d.md' nests under output path for "
                         "'site/content/c.md': 'site/public/page/index.html'\n"));
  TEST_CHECK(log.errors == 2);
}

// A content entry and a configured `index.html` aggregate collide before any writer runs. The entry
// is labeled by its source path, a real path, rather than a placeholder another entry could share.
static void test_rejects_entry_aggregate_collision(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/templates/index.html", "aggregate");
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  const struct ContentEntry entry = entry_at("site/public/index.html", "site/content/index.md");
  const struct ContentEntry* entries[] = {&entry};
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, entries, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'site/content/index.md' and "
                         "'aggregate_templates[0]': 'site/public/index.html'\n"));
}

// `manifest_builder_populate` rejects two configured templates that claim one output path. It names
// each by its list entry rather than by its own name. These two configurations are different
// mistakes with different fixes. A bare template name reports both as
// `for 'dup.html' and 'dup.html'`, which reads as a value colliding with itself and says nothing
// about which key to edit. Both cases are asserted because a label carrying only the config key
// would distinguish them in the second case but not the first.
static void test_rejects_duplicate_template(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);

  // First case: the same name twice inside `aggregate_templates`, so the two indexes differ.
  static const struct Str aggregates[] = {STR_INIT("dup.html"), STR_INIT("dup.html")};
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 2;
  struct Manifest one_list_manifest = {0};
  struct Log one_list_log = test_log();
  TEST_CHECK(populate(&one_list_manifest, &site_config, NULL, NULL, NULL, 0, os, &one_list_log) ==
             -1);
  TEST_CHECK(test_log_is(&one_list_log,
                         "duplicate output path for 'aggregate_templates[0]' and "
                         "'aggregate_templates[1]': 'site/public/dup.html'\n"));

  // Second case: one name in each list, so the two keys differ.
  static const struct Str shared[] = {STR_INIT("dup.html")};
  site_config.aggregate_templates = shared;
  site_config.aggregate_template_count = 1;
  site_config.feed_templates = shared;
  site_config.feed_template_count = 1;
  struct Manifest shared_manifest = {0};
  struct Log shared_log = test_log();
  TEST_CHECK(populate(&shared_manifest, &site_config, NULL, NULL, NULL, 0, os, &shared_log) == -1);
  TEST_CHECK(test_log_is(&shared_log,
                         "duplicate output path for 'aggregate_templates[0]' and "
                         "'feed_templates[0]': 'site/public/dup.html'\n"));
}

// `manifest_builder_populate` rejects an output path that names one of the build's own input files,
// so a build cannot overwrite its own source. Here an entry's output is a hard link to its own
// content source, so the two paths differ and only file identity sees the collision. An output
// inside `content_dir` fails the root check first, as `test_rejects_output_in_content_dir` pins.
static void test_rejects_input_overwrite(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/content/a.md", "source");
  os_memory_link(os, "site/content/a.md", "site/public/a.md");
  const struct ContentEntry entry = entry_at("site/public/a.md", "site/content/a.md");
  const struct ContentEntry* entries[] = {&entry};
  struct Arena arena = test_arena();
  struct PathList sources = {0};
  path_list_push(&sources, entry.source_path, &arena);
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, &sources, NULL, entries, 1, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would overwrite build input for 'site/content/a.md': "
                         "'site/public/a.md'\n"));
}

// The input check finds an aimed-at source wherever it falls among many claimed inputs, and still
// accepts an existing output that is not an input. The sources are created in reverse name order,
// so the order they are claimed in differs from the order their identities sort in. Each aimed-at
// output is a hard link to its source, for the reason `test_rejects_input_overwrite` gives.
static void test_rejects_input_overwrite_among_many_inputs(void) {
  enum { SOURCE_COUNT = 32 };
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/public/existing.html", "output");
  struct Arena arena = test_arena();
  struct PathList sources = {0};
  for (ptrdiff_t i = SOURCE_COUNT; i > 0; i--) {
    char path[48];
    (void)snprintf(path, sizeof(path), "site/content/%02td.md", i - 1);
    os_memory_write(os, path, "source");
  }
  for (ptrdiff_t i = 0; i < SOURCE_COUNT; i++) {
    char path[48];
    (void)snprintf(path, sizeof(path), "site/content/%02td.md", i);
    path_list_push(&sources, str_from_cstr(path), &arena);
  }

  static const ptrdiff_t targets[] = {0, SOURCE_COUNT / 2, SOURCE_COUNT - 1};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(targets) / sizeof(targets[0])); i++) {
    const char* target = str_cstr(sources.items[targets[i]], &arena);
    char output[32];
    (void)snprintf(output, sizeof(output), "site/public/%02td.html", targets[i]);
    os_memory_link(os, target, output);
    // The existing output comes first, so a lookup that wrongly matched it would name it instead.
    const struct ContentEntry existing = entry_at("site/public/existing.html", "content/x.md");
    const struct ContentEntry aimed = entry_at(output, target);
    const struct ContentEntry* entries[] = {&existing, &aimed};
    struct Manifest manifest = {0};
    struct Log log = test_log();

    TEST_CHECK(populate(&manifest, &site_config, &sources, NULL, entries, 2, os, &log) == -1);
    TEST_CHECK(test_log_is(&log, "output path would overwrite build input for '%s': '%s'\n", target,
                           output));
  }
}

// The config file is an input like any other, so an output path naming it is rejected. Without this
// the build would overwrite the file that configured it and still report success: `output_dir` at
// the project root plus an aggregate template named `sosig.toml` is all it takes, and the loss is
// unrecoverable.
static void test_rejects_config_overwrite(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  static const struct Str aggregates[] = {STR_INIT("sosig.toml")};
  site_config.output_dir = S("site");
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would overwrite build input for 'aggregate_templates[0]': "
                         "'site/sosig.toml'\n"));
}

// Every file below `templates_dir` is a build input, so an output naming a configured template, an
// entry's `template` override, or a partial is rejected. An output inside `templates_dir` fails the
// root check first, as `test_rejects_output_in_templates_dir` pins, so each output here sits in
// `output_dir` as a hard link to a template, which only the identity claim can see. The override
// and the partial are named in no configured list, and the partial is named nowhere at all, so only
// the walk of the template tree can claim it, which is the case a claim of just the configured
// templates would miss.
static void test_rejects_template_overwrite(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/templates/index.html", "index");
  os_memory_write(os, "site/templates/custom.html", "custom");
  os_memory_write(os, "site/templates/partials/card.html", "card");
  os_memory_link(os, "site/templates/index.html", "site/public/index.html");
  os_memory_link(os, "site/templates/custom.html", "site/public/custom.html");
  os_memory_link(os, "site/templates/partials/card.html", "site/public/card.html");

  // First case: an entry lands on a hard link to a template named in `aggregate_templates`. The
  // aggregate's own output is that same link, so every output is checked and both are reported.
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  const struct ContentEntry aggregate_entry = entry_at("site/public/index.html", "content/a.md");
  const struct ContentEntry* aggregate_entries[] = {&aggregate_entry};
  struct Manifest aggregate_manifest = {0};
  struct Log aggregate_log = test_log();
  TEST_CHECK(populate(&aggregate_manifest, &site_config, NULL, NULL, aggregate_entries, 1, os,
                      &aggregate_log) == -1);
  TEST_CHECK(test_log_is(&aggregate_log,
                         "output path would overwrite build input for 'content/a.md': "
                         "'site/public/index.html'\n"
                         "output path would overwrite build input for 'aggregate_templates[0]': "
                         "'site/public/index.html'\n"));

  // Second case: an entry lands on a hard link to its own `template` override, which no configured
  // list names.
  site_config.aggregate_template_count = 0;
  struct ContentEntry override_entry = entry_at("site/public/custom.html", "content/b.md");
  override_entry.template = S("custom.html");
  const struct ContentEntry* override_entries[] = {&override_entry};
  struct Manifest override_manifest = {0};
  struct Log override_log = test_log();
  TEST_CHECK(populate(&override_manifest, &site_config, NULL, NULL, override_entries, 1, os,
                      &override_log) == -1);
  TEST_CHECK(test_log_is(&override_log,
                         "output path would overwrite build input for 'content/b.md': "
                         "'site/public/custom.html'\n"));

  // Third case: an entry lands on a hard link to an unconfigured partial.
  const struct ContentEntry partial_entry = entry_at("site/public/card.html", "content/c.md");
  const struct ContentEntry* partial_entries[] = {&partial_entry};
  struct Manifest partial_manifest = {0};
  struct Log partial_log = test_log();
  TEST_CHECK(populate(&partial_manifest, &site_config, NULL, NULL, partial_entries, 1, os,
                      &partial_log) == -1);
  TEST_CHECK(test_log_is(&partial_log,
                         "output path would overwrite build input for 'content/c.md': "
                         "'site/public/card.html'\n"));
}

// An `output_dir` at or below an input root is rejected before anything walks or writes, whether or
// not it exists yet, and the check creates none of it. A missing `output_dir` is judged by the
// directory it would be created in, so a nested path, a `..` past a missing component, and a path
// through a symlinked root all land where `os_mkdirs` would put them.
static void test_check_output_dir_rejects_dir_in_input_root(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/content/public/old.html", "old");
  os_memory_write(os, "site/static/a.css", "static");
  os_memory_symlink(os, "content", "site/content-link");
  static const struct Str content_output_dirs[] = {
      STR_INIT("site/content"),
      STR_INIT("site/content/public"),
      STR_INIT("site/content/new/deeper"),
      STR_INIT("site/missing/../content/new"),
      STR_INIT("site/content-link/new"),
  };
  for (ptrdiff_t i = 0;
       i < (ptrdiff_t)(sizeof(content_output_dirs) / sizeof(content_output_dirs[0])); i++) {
    site_config.output_dir = content_output_dirs[i];
    check_rejects_output_dir_in_root(&site_config, "content_dir", os);
  }

  site_config.output_dir = S("site/templates/partials");
  check_rejects_output_dir_in_root(&site_config, "templates_dir", os);
  site_config.output_dir = S("site/static/generated");
  check_rejects_output_dir_in_root(&site_config, "static_dir", os);
}

// An output that would land below `content_dir` is rejected even though no file sits at its path
// yet, because the next build could parse it as a content source. The root is compared by identity,
// so an alternate spelling of it, a symlink to it, and a symlink inside `output_dir` that leads
// into it are rejected the same way.
static void test_rejects_output_in_content_dir(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/content/a.md", "source");
  os_memory_symlink(os, "content", "site/content-link");
  os_memory_symlink(os, "../content", "site/out/link");

  // The output is a new file below the root, reached from an `output_dir` that holds the root.
  site_config.output_dir = S("site");
  check_rejects_output_in_root(&site_config, "site/content/b.md", "content_dir", os);

  // The root is configured through a symlink and spelled with a `./` component.
  site_config.content_dir = S("site/./content-link");
  check_rejects_output_in_root(&site_config, "site/content/sub/b.md", "content_dir", os);

  // A symlink inside `output_dir` leads into the root.
  site_config.content_dir = S("site/content");
  site_config.output_dir = S("site/out");
  check_rejects_output_in_root(&site_config, "site/out/link/b.md", "content_dir", os);
}

// An output that would land below `templates_dir` is rejected even though no file sits at its path
// yet, because the same build's render could read it back as a partial. The check covers configured
// template outputs as well as content entries.
static void test_rejects_output_in_templates_dir(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  site_config.output_dir = S("site");
  check_rejects_output_in_root(&site_config, "site/templates/partials/b.html", "templates_dir", os);

  // A configured template whose name leads into the template tree is rejected by its list entry.
  static const struct Str aggregates[] = {STR_INIT("templates/index.html")};
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would write inside 'templates_dir' for "
                         "'aggregate_templates[0]': 'site/templates/index.html'\n"));
}

// An output that would land below `static_dir` is rejected, because the next build would copy it as
// a static file.
static void test_rejects_output_in_static_dir(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/static/a.css", "static");
  site_config.output_dir = S("site");
  check_rejects_output_in_root(&site_config, "site/static/b.html", "static_dir", os);
}

// A static file whose copy would land on a generated output collides with it, rather than one
// silently replacing the other. The static file is labeled by its source path.
static void test_rejects_static_aggregate_collision(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/templates/index.html", "aggregate");
  os_memory_write(os, "site/static/index.html", "static");
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  struct Arena arena = test_arena();
  struct PathList static_paths = {0};
  path_list_push(&static_paths, S("site/static/index.html"), &arena);
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, &static_paths, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'aggregate_templates[0]' and "
                         "'site/static/index.html': 'site/public/index.html'\n"));
}

// A `templates_dir` that exists but cannot be walked fails the manifest rather than claiming
// nothing, because a partial the walk could not see would stay overwritable. The reason names the
// path the walk failed on, so the configured root is not repeated ahead of it. A regular file in
// place of the directory and a subdirectory that refuses to be listed fail the same way.
static void test_rejects_unlistable_templates_dir(void) {
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  os_memory_write(os, "site/templates-file", "not a directory");
  site_config.templates_dir = S("site/templates-file");
  struct Manifest manifest = {0};
  struct Log log = test_log();
  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to list template files: cannot open directory: %s ('site/templates-file')\n",
      strerror(ENOTDIR)));
  TEST_CHECK(manifest.count == 0);

  os_memory_write(os, "site/templates/partials/card.html", "card");
  os_memory_fail(os, OS_MEMORY_OP_LIST, "site/templates/partials", EACCES);
  site_config.templates_dir = S("site/templates");
  log = test_log();
  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log,
      "failed to list template files: cannot open directory: %s ('site/templates/partials')\n",
      strerror(EACCES)));
}

// A configured template whose output path exceeds the whole-path limit is rejected with a message
// naming the limit, the measured length, the list entry, and the whole name.
static void test_rejects_oversize_template_path(void) {
  char name[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(name, 'a', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const struct Str aggregates[] = {{name, (ptrdiff_t)sizeof(name) - 1}};
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 1;
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'aggregate_templates[0]' exceeds max output path length "
                         "(%d bytes) at %td bytes: '%.*s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, aggregates[0].len, STR_FMT(aggregates[0])));
}

// A configured template with one overlong path segment is rejected against the per-filename limit
// rather than the whole-path one, so the two limits stay distinguishable. A feed template exercises
// the second configured list.
static void test_rejects_oversize_template_segment(void) {
  char name[FILENAME_LEN_MAX + 32];
  memset(name, 'b', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  const struct Str feeds[] = {{name, (ptrdiff_t)sizeof(name) - 1}};
  struct SiteConfig site_config;
  struct Os* os = init_manifest_fixture(&site_config);
  site_config.feed_templates = feeds;
  site_config.feed_template_count = 1;
  struct Manifest manifest = {0};
  struct Log log = test_log();

  TEST_CHECK(populate(&manifest, &site_config, NULL, NULL, NULL, 0, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'feed_templates[0]' exceeds max filename length "
                         "(%d bytes) at %zu bytes: '%s'\n",
                         FILENAME_LEN_MAX, sizeof(name) - 1, name));
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
    {"reports every collision", test_reports_every_collision},
    {"rejects case folded duplicate", test_rejects_case_folded_duplicate},
    {"rejects prefix collision", test_rejects_prefix_collision},
    {"rejects every prefix collision", test_rejects_every_prefix_collision},
    {"rejects entry aggregate collision", test_rejects_entry_aggregate_collision},
    {"rejects duplicate template", test_rejects_duplicate_template},
    {"rejects input overwrite", test_rejects_input_overwrite},
    {"rejects input overwrite among many inputs", test_rejects_input_overwrite_among_many_inputs},
    {"rejects config overwrite", test_rejects_config_overwrite},
    {"rejects template overwrite", test_rejects_template_overwrite},
    {"check output dir rejects dir in input root", test_check_output_dir_rejects_dir_in_input_root},
    {"rejects output in content dir", test_rejects_output_in_content_dir},
    {"rejects output in templates dir", test_rejects_output_in_templates_dir},
    {"rejects output in static dir", test_rejects_output_in_static_dir},
    {"rejects static aggregate collision", test_rejects_static_aggregate_collision},
    {"rejects unlistable templates dir", test_rejects_unlistable_templates_dir},
    {"rejects oversize template path", test_rejects_oversize_template_path},
    {"rejects oversize template segment", test_rejects_oversize_template_segment},
    {NULL, NULL},
};
