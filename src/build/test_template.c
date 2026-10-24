#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "build/template.h"
#include "core/error.h"
#include "core/path.h"
#include "core/sosig_version.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 1024 };

/** Timestamp every render below passes as `site.updated`, which `TemplateContext` requires. */
static const struct Str SITE_UPDATED = STR_INIT("2026-07-01T00:00:00Z");

/**
 * @brief Initializes shared site metadata for template tests.
 *
 * The assigned fields are string literals.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 */
static void init_test_site_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->title = S("Site");
  site_config->author = S("Author");
  site_config->base_url = S("https://example.com");
}

/**
 * @brief Initializes shared content-entry metadata for template tests.
 *
 * The assigned fields are string literals.
 *
 * @param entry Content entry to initialize. Must not be `NULL`.
 */
static void init_test_content_entry(struct ContentEntry* entry) {
  *entry = (struct ContentEntry){0};
  entry->title = S("Content Entry");
  entry->date = S("2026-07-01T00:00:00Z");
  entry->description = S("Desc");
  entry->slug = S("content-entry");
  entry->url_path = S("/content-entry.html");
  entry->body_html = S("<p>raw</p>");
}

/**
 * @brief Creates a fixture root holding one `index.html` template.
 *
 * @param root_dir Writable `mkdtemp` template. Receives the created directory path. Must not be
 *                 `NULL`.
 * @param text     Terminated template text to write.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with no fixture left
 *         behind.
 */
static int write_template_fixture(char root_dir[static 1], const char* text) {
  if (init_fixture_dir(root_dir) != 0) {
    return -1;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "index.html", text) == 0)) {
    remove_fixture_tree(root_dir);
    return -1;
  }
  return 0;
}

/**
 * @brief Writes one fixture file whose bytes may include a `NUL`.
 *
 * @param root_dir      Fixture root directory.
 * @param relative_path Relative fixture path below `root_dir`.
 * @param data          Bytes to write.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
static int write_fixture_bytes(const char* root_dir, const char* relative_path, struct Str data) {
  char fixture_path[256];
  const int n = snprintf(fixture_path, sizeof(fixture_path), "%s/%s", root_dir, relative_path);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(fixture_path));
  if (n <= 0 || (size_t)n >= sizeof(fixture_path)) {
    return -1;
  }
  struct Log log = test_log();
  const int rc = os_write(os_posix(), str_from_cstr(fixture_path), (struct OsSource){.bytes = data},
                          S("failed to write fixture"), &log);
  TEST_CHECK(rc == 0);
  return rc;
}

/**
 * @brief Reports whether a log holds exactly one page-render failure.
 *
 * A page render, which every failing render here is, attributes the failure to the entry it was
 * rendering, so the expected line is the failure followed by `(while rendering '<source>')`.
 *
 * @param log     Log the render wrote. Must not be `NULL`.
 * @param entry   Entry the render was for. Must not be `NULL`.
 * @param failure Terminated failure the log must hold, without the attribution. Must not be `NULL`.
 * @return `true` when the log holds that one failure.
 */
static bool is_log_one_failure(const struct Log* log,
                               const struct ContentEntry* entry,
                               const char* failure) {
  return test_log_is(log, "%s (while rendering '%.*s')\n", failure, STR_FMT(entry->source_path)) &&
         TEST_CHECK(log->errors == 1);
}

/**
 * @brief Renders through `template_render_file_limited` and hands back the HTML as a C string.
 *
 * This is a deliberate test-only bridge back to C strings. Every assertion below inspects rendered
 * text with C string functions such as `strcmp` and `strstr`, so the helper hands it back
 * terminated, copied after the HTML in `perm`. Rendered output holds no `NUL` byte, so the copy
 * loses nothing.
 *
 * @param templates_dir  Terminated template directory root.
 * @param template_name  Terminated template name.
 * @param context        Template context. Must not be `NULL`.
 * @param output_len_max Largest rendered output accepted, in bytes.
 * @param perm           Arena that owns the HTML and its terminated copy. Must not be `NULL`.
 * @param html_out       Receives the terminated HTML on success, or `NULL` when the test asserts
 *                       only the result.
 * @param log            Log that receives the failure, or `NULL` when the test asserts only the
 *                       result.
 * @return `0` on success, or `-1` when the render failed.
 */
static int render_limited(const char* templates_dir,
                          const char* template_name,
                          const struct TemplateContext* context,
                          ptrdiff_t output_len_max,
                          struct Arena* perm,
                          char** html_out,
                          struct Log* log) {
  struct Log discarded = {0};
  if (log == NULL) {
    discarded = test_log();
    log = &discarded;
  }
  struct Str html = {0};
  if (template_render_file_limited(str_from_cstr(templates_dir), str_from_cstr(template_name),
                                   context, output_len_max, os_posix(), perm, test_arena(), &html,
                                   log) != 0) {
    return -1;
  }
  if (html_out != NULL) {
    *html_out = str_cstr(html, perm);
  }
  return 0;
}

/**
 * @brief Renders through `template_render_file` with the production bound and hands back the HTML
 *        as a C string, the same test-only bridge as `render_limited`.
 *
 * @param templates_dir Terminated template directory root.
 * @param template_name Terminated template name.
 * @param context       Template context. Must not be `NULL`.
 * @param perm          Arena that owns the HTML and its terminated copy. Must not be `NULL`.
 * @param html_out      Receives the terminated HTML on success, or `NULL` when the test asserts
 *                      only the result.
 * @param log           Log that receives the failure, or `NULL` when the test asserts only the
 *                      result.
 * @return `0` on success, or `-1` when the render failed.
 */
static int render(const char* templates_dir,
                  const char* template_name,
                  const struct TemplateContext* context,
                  struct Arena* perm,
                  char** html_out,
                  struct Log* log) {
  struct Log discarded = {0};
  if (log == NULL) {
    discarded = test_log();
    log = &discarded;
  }
  struct Str html = {0};
  if (template_render_file(str_from_cstr(templates_dir), str_from_cstr(template_name), context,
                           os_posix(), perm, test_arena(), &html, log) != 0) {
    return -1;
  }
  if (html_out != NULL) {
    *html_out = str_cstr(html, perm);
  }
  return 0;
}

// `{{title}}` is escaped and `{{{body}}}` is not in the same render, so escaping is chosen per tag
// rather than per template. This renders the real `templates/content.html`, whose output, including
// the feed `<link rel="alternate">`, is compared end to end with the expected output in
// `tests/expected/site_file_permalink` by the golden test.
static void test_renders_escaped_title_and_raw_body(void) {
  struct Arena perm = test_arena();
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = S("Content Entry <One>");
  const char* templates_dir = "tests/fixtures/site_file_permalink/templates";
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  TEST_ASSERT(render(templates_dir, "content.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strstr(rendered_html, "Content Entry &lt;One&gt;") != NULL);
  TEST_CHECK(strstr(rendered_html, "<p>raw</p>") != NULL);
  TEST_CHECK(strstr(rendered_html, "rel=\"alternate\"") != NULL);
}

// Content entries and their tag lists iterate with escaping.
static void test_iterates_content_entries_and_tags(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir,
                             "{{#content_entries}}{{title}}: {{#tags}}[{{.}}]{{/tags}}\n"
                             "{{/content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entries = content_entries,
                                          .content_entry_count = 2,
                                          .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  static struct Str first_tags[] = {STR_INIT("c"), STR_INIT("x&y")};
  static struct Str second_tags[] = {STR_INIT("z")};
  first.title = S("One <A>");
  first.tags = first_tags;
  first.tag_count = 2;
  second.title = S("Two");
  second.tags = second_tags;
  second.tag_count = 1;
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "One &lt;A&gt;: [c][x&amp;y]\nTwo: [z]\n") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An empty tag renders as an empty item and does not end the iteration. `node_scalar` maps `""` to
// `NULL` so an unset optional *field* is falsey, but a tag list must not inherit that rule: `NULL`
// from the child callback means end-of-list to mustache4c, so an empty tag would silently drop
// every tag after it. Frontmatter accepts `tags = ["", "x"]`, so this is reachable from a real
// site.
static void test_empty_tag_does_not_truncate_tag_list(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(
          root_dir, "{{#content_entries}}{{#tags}}[{{.}}]{{/tags}}{{/content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entries = content_entries,
                                          .content_entry_count = 1,
                                          .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  static struct Str tags[] = {STR_INIT("a"), STR_INIT(""), STR_INIT("b")};
  entry.tags = tags;
  entry.tag_count = 3;
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[a][][b]") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// Nested sections re-iterate content entries in an inner context.
static void test_nested_section_reiterates_content_entries(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir,
                             "{{#content_entries}}"
                             "["
                             "{{title}}:"
                             "{{#content_entries}}"
                             "{{title}},"
                             "{{/content_entries}}"
                             "]"
                             "{{/content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entries = content_entries,
                                          .content_entry_count = 2,
                                          .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  first.title = S("One");
  second.title = S("Two");
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[One:One,Two,][Two:One,Two,]") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// Double-brace variables are escaped and triple-brace variables stay raw.
static void test_escapes_double_brace_and_leaves_triple_brace_raw(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{title}} {{{title}}}\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  entry.title = S("Content Entry <One>");
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "Content Entry &lt;One&gt; Content Entry <One>\n") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// The feed template renders through the same name-based entry point as an HTML page. A body that
// already holds HTML is escaped again for XML. This renders the real `templates/atom.xml`, so the
// golden test compares its output end to end with `tests/expected/site_file_permalink/atom.xml`.
static void test_renders_atom_feed(void) {
  struct Arena perm = test_arena();
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = S("Content Entry <One>");
  entry.description = S("Desc & More");
  const struct ContentEntry* content_entries[] = {&entry};
  const char* templates_dir = "tests/fixtures/site_file_permalink/templates";
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entry_current = NULL,
                                          .content_entries = content_entries,
                                          .content_entry_count = 1,
                                          .site_updated = entry.date};
  char* rendered_html = NULL;
  TEST_ASSERT(render(templates_dir, "atom.xml", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strstr(rendered_html, "<title>Content Entry &lt;One&gt;</title>") != NULL);
  TEST_CHECK(strstr(rendered_html, "<content type=\"html\">&lt;p&gt;raw&lt;/p&gt;</content>") !=
             NULL);
  TEST_CHECK(strstr(rendered_html, "<updated>2026-07-01T00:00:00Z</updated>") != NULL);
}

// `generator` names the tool and its version, at the root and inside a section.
static void test_renders_generator(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(
          root_dir, "{{generator}}|{{#content_entries}}{{generator}}{{/content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entries = content_entries,
                                          .content_entry_count = 1,
                                          .site_updated = SITE_UPDATED};
  const struct Str version = sosig_version_string();
  char* rendered_html = NULL;
  char expected[64];
  int expected_len = 0;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  expected_len = snprintf(expected, sizeof(expected), "sosig %.*s|sosig %.*s", STR_FMT(version),
                          STR_FMT(version));
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, expected) == 0);
  TEST_MSG("rendered: '%s'", rendered_html);

cleanup:
  remove_fixture_tree(root_dir);
}

// A safe partial loads and renders in the parent context, with its values still escaped.
static void test_renders_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "before {{> card}} after") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "{{title}}") == 0)) {
    goto cleanup;
  }
  entry.title = S("Content Entry <One>");
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "before Content Entry &lt;One&gt; after") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial referenced twice per entry across three entries expands six times and renders each
// entry's own content each time. The compile cache is not observable through the API, so this pins
// what a broken cache would break, stale or empty expansions, rather than the number of compiles.
static void test_repeated_partial_renders_same_content_each_time(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir,
                             "{{#content_entries}}{{>card}}{{>card}}{{/content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entries[3];
  static const struct Str titles[] = {STR_INIT("p1"), STR_INIT("p2"), STR_INIT("p3")};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(entries) / sizeof(entries[0])); i++) {
    init_test_content_entry(&entries[i]);
    entries[i].title = titles[i];
  }
  const struct ContentEntry* content_entries[] = {&entries[0], &entries[1], &entries[2]};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entries = content_entries,
                                          .content_entry_count = 3,
                                          .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char* rendered_html = NULL;
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "[{{title}}]") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(strcmp(rendered_html, "[p1][p1][p2][p2][p3][p3]") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// Referencing one partial more times than the distinct-partial limit allows still builds, because
// every reference after the first is a cache hit and only a miss consumes a slot. This is the
// assertion the sibling above cannot make: expansions are counted before the cache lookup, so a
// cache that never hit would still expand the right number of times and render the right bytes.
// Exhausting the distinct-partial budget is the only difference observable through the API, so the
// reference count sits above the limit rather than at two.
//
// `RENDER_PARTIAL_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing it
// there must update this. A cache that never hit would fail on reference 65.
static void test_repeated_partial_stays_under_distinct_limit(void) {
  struct Arena perm = test_arena();
  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  enum { REFERENCE_COUNT = RENDER_PARTIAL_COUNT_MAX + 6 };
  enum { REFERENCE_LEN = sizeof("{{>card}}") - 1 };
  enum { EXPANSION_LEN = sizeof("[Content Entry]") - 1 };
  char source[REFERENCE_COUNT * REFERENCE_LEN + 1];
  char expected[REFERENCE_COUNT * EXPANSION_LEN + 1];
  for (ptrdiff_t i = 0; i < REFERENCE_COUNT; i++) {
    memcpy(source + i * REFERENCE_LEN, "{{>card}}", REFERENCE_LEN);
    memcpy(expected + i * EXPANSION_LEN, "[Content Entry]", EXPANSION_LEN);
  }
  source[REFERENCE_COUNT * REFERENCE_LEN] = '\0';
  expected[REFERENCE_COUNT * EXPANSION_LEN] = '\0';

  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, source) != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "[{{title}}]") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(strcmp(rendered_html, expected) == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An empty template renders as empty output.
static void test_empty_template_yields_empty_string(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An unknown variable renders as empty text per the Mustache spec, at the root and in a subtree.
static void test_unknown_variable_renders_empty(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "[{{typo}}|{{site.typo}}]\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[|]\n") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An inverted section renders only when the list is empty.
static void test_inverted_section_renders_when_list_empty(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir,
                             "{{#content_entries}}x{{/content_entries}}{{^content_entries}}none{{/"
                             "content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context_empty = {.site_config = &site_config,
                                                .site_updated = SITE_UPDATED};
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context_full = {.site_config = &site_config,
                                               .content_entries = content_entries,
                                               .content_entry_count = 1,
                                               .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);

  if (!TEST_CHECK(render(root_dir, "index.html", &context_empty, &perm, &rendered_html, NULL) ==
                  0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "none") == 0);

  if (!TEST_CHECK(render(root_dir, "index.html", &context_full, &perm, &rendered_html, NULL) ==
                  0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "x") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// An empty field value is falsey as a section, so an unset optional field reads as absent. Were an
// empty scalar truthy, `{{#description}}` would render for every entry and `{{^description}}` for
// none, so both branches are asserted here.
static void test_empty_field_is_falsey_as_section(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(
          root_dir,
          "{{#content_entries}}[{{#description}}D:{{description}}{{/description}}"
          "{{^description}}NONE{{/description}}]{{/content_entries}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  // An empty `description` is the shape a content file that omits the key leaves on its zeroed
  // entry.
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entries = content_entries,
                                          .content_entry_count = 2,
                                          .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  first.description = S("");
  second.description = S("Desc");
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "[NONE][D:Desc]") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// Comments are dropped from the rendered output.
static void test_comments_are_dropped(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "a{{! ignored }}b") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, NULL) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(rendered_html, "ab") == 0);

cleanup:
  remove_fixture_tree(root_dir);
}

// The output bound belongs to the call. `template_render_file_limited` accepts output exactly at
// the bound it is given, reporting that length, and rejects one byte more, while
// `template_render_file` keeps the production bound and renders that same page.
static void test_output_bound_applies_per_call(void) {
  struct Arena perm = test_arena();
  enum { OUTPUT_LEN_MAX = 64 * 1024 };
  char* padding = arena_new(&perm, OUTPUT_LEN_MAX + 2, char);
  memset(padding, 'x', (size_t)OUTPUT_LEN_MAX + 1);
  padding[OUTPUT_LEN_MAX + 1] = '\0';
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, padding) != 0) {
    return;
  }
  padding[OUTPUT_LEN_MAX] = '\0';

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char* rendered_html = NULL;
  char expected_err[EXPECTED_SIZE];
  int expected_err_len = 0;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "at.html", padding) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(render_limited(root_dir, "at.html", &context, OUTPUT_LEN_MAX, &perm,
                                 &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strlen(rendered_html) == (size_t)OUTPUT_LEN_MAX);

  TEST_CHECK(render_limited(root_dir, "index.html", &context, OUTPUT_LEN_MAX, &perm, NULL, &log) ==
             -1);
  expected_err_len =
      snprintf(expected_err, sizeof(expected_err),
               "render exceeds max rendered output (%d bytes) at %d bytes (in 'index.html')",
               OUTPUT_LEN_MAX, OUTPUT_LEN_MAX + 1);
  TEST_CHECK(expected_err_len > 0 && (size_t)expected_err_len < sizeof(expected_err));
  TEST_CHECK(is_log_one_failure(&log, &entry, expected_err));

  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(strlen(rendered_html) == (size_t)OUTPUT_LEN_MAX + 1);

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial name that could escape the partials directory is rejected, with a diagnostic. A name
// holding `..` never reaches the provider: mustache4c's own tag-name validation rejects it while
// compiling, so the failure surfaces as a compile error for the enclosing template.
static void test_rejects_unsafe_partial_name(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{> ../secret}}\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char* rendered_html = NULL;
  char expected_invalid_tag[EXPECTED_SIZE];
  char expected_unsafe_name[EXPECTED_SIZE];
  int n = 0;
  init_test_content_entry(&entry);
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, &log) == -1);
  // Rejected by mustache4c's own tag validation, so the leading `%s` is its text and only the
  // position wrapper is this project's. Composed through the same format string as
  // `test_rejects_unclosed_section`, which is the other assertion of that wrapper.
  n = snprintf(expected_invalid_tag, sizeof(expected_invalid_tag),
               "%s at line %u, column %u (in '%s')", "tag name is invalid", 1U, 1U, "index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_invalid_tag));
  TEST_CHECK(is_log_one_failure(&log, &entry, expected_invalid_tag));

  // A name Mustache accepts but that is not a safe identifier is rejected by the provider, which
  // names the offending partial and the template that references it.
  if (!TEST_CHECK(write_fixture_file(root_dir, "index.html", "{{>nested.name}}\n") == 0)) {
    goto cleanup;
  }
  log = test_log();
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, &rendered_html, &log) == -1);
  n = snprintf(
      expected_unsafe_name, sizeof(expected_unsafe_name),
      "partial name must contain only letters, digits, '_' and '-': '%s' (in 'index.html')",
      "nested.name");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_unsafe_name));
  TEST_CHECK(is_log_one_failure(&log, &entry, expected_unsafe_name));

cleanup:
  remove_fixture_tree(root_dir);
}

// A bad partial reference names the file it is written in. Inside a partial that is the partial,
// not the template including it. mustache4c resolves the reference while it renders and says
// nothing when a partial ends, so the second case puts a reference in the template after a partial
// has finished, where the template holds it again.
static void test_rejects_unsafe_partial_name_in_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "outer {{>a/b}}\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  TEST_CHECK(
      is_log_one_failure(&log, &entry,
                         "partial name must contain only letters, digits, '_' and '-': 'a/b' "
                         "(in 'partials/card.html')"));

  if (!TEST_CHECK(write_fixture_file(root_dir, "index.html", "{{>card}}\n{{>a/b}}\n") == 0) ||
      !TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", "card\n") == 0)) {
    goto cleanup;
  }
  log = test_log();
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  TEST_CHECK(is_log_one_failure(
      &log, &entry,
      "partial name must contain only letters, digits, '_' and '-': 'a/b' (in 'index.html')"));

cleanup:
  remove_fixture_tree(root_dir);
}

// An unreadable partial fails the render and reports the cause. It does not publish a page with a
// missing partial. mustache4c interprets a `NULL` callback result as an absent partial and reports
// success. Only the provider's failure flag converts this result to a failed render. A typo in
// `{{>card}}` is a typical cause.
static void test_rejects_unreadable_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  // No `partials/card.html` is written, so resolving the reference fails on the read.
  if (write_template_fixture(root_dir, "before {{>card}} after") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected), "failed to read partial: %s ('%s/%s')",
                          error_system_message(reason, sizeof(reason), ENOENT), root_dir,
                          "partials/card.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A template holding a `NUL` byte is rejected at the read, naming the template file. Rendering past
// it would emit a page with a raw `NUL` in it.
static void test_rejects_template_with_nul_byte(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  static const char bytes[] = {'a', '\0', 'b'};
  if (!TEST_CHECK(write_fixture_bytes(root_dir, "index.html", (struct Str){bytes, sizeof(bytes)}) ==
                  0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read template: contains an embedded NUL byte ('%s/index.html')", root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial holding a `NUL` byte is rejected on the same grounds as a template, naming the partial
// file rather than the template that included it.
static void test_rejects_partial_with_nul_byte(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "before {{>card}} after") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  static const char bytes[] = {'a', '\0', 'b'};
  if (!TEST_CHECK(write_fixture_bytes(root_dir, "partials/card.html",
                                      (struct Str){bytes, sizeof(bytes)}) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read partial: contains an embedded NUL byte ('%s/partials/card.html')", root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A template or partial larger than `TEMPLATE_FILE_LEN_MAX` fails the render at the read, from its
// size, naming the limit and the size. The files are sparse, so the fixture costs no disk.
static void test_rejects_oversize_template_and_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "before {{>big}} after") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Arena arena = test_arena();
  const off_t file_len = (off_t)TEMPLATE_FILE_LEN_MAX + 1;
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "big.html", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/big.html", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(
          truncate(str_cstr(path_join(str_from_cstr(root_dir), S("big.html"), &arena), &arena),
                   file_len) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(
          truncate(
              str_cstr(path_join(str_from_cstr(root_dir), S("partials/big.html"), &arena), &arena),
              file_len) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(render(root_dir, "big.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read template: exceeds max file size (%d bytes) at %jd bytes "
                          "('%s/big.html')",
                          TEMPLATE_FILE_LEN_MAX, (intmax_t)file_len, root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

  log = test_log();
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "failed to read partial: exceeds max file size (%d bytes) at %jd bytes "
                          "('%s/partials/big.html')",
                          TEMPLATE_FILE_LEN_MAX, (intmax_t)file_len, root_dir);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// One distinct partial past the limit is rejected, naming the limit and the partial that reached
// it. `test_repeated_partial_stays_under_distinct_limit` covers the cache-hit side, where
// references beyond the limit cost nothing. This covers the miss side, where each distinct name
// consumes a slot. The guard is the only thing between a 65th distinct partial and a write past two
// fixed 64-element arrays living in `template_render_file`'s stack frame, so both sides of the
// comparison are asserted: exactly the limit renders, one more fails.
//
// `RENDER_PARTIAL_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing it
// there must update this.
static void test_rejects_partial_count_past_limit(void) {
  struct Arena perm = test_arena();
  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  char at_limit[RENDER_PARTIAL_COUNT_MAX * 10];
  char over_limit[(RENDER_PARTIAL_COUNT_MAX + 1) * 10];
  size_t at_limit_len = 0;
  size_t over_limit_len = 0;
  struct Log at_log = test_log();
  char* at_rendered = NULL;
  // Every partial renders as one `x`.
  char at_expected[RENDER_PARTIAL_COUNT_MAX + 1];
  struct Log over_log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  memset(at_expected, 'x', RENDER_PARTIAL_COUNT_MAX);
  at_expected[RENDER_PARTIAL_COUNT_MAX] = '\0';
  // One partial file per distinct name, plus the one that overflows the budget.
  for (ptrdiff_t i = 0; i <= RENDER_PARTIAL_COUNT_MAX; i++) {
    char partial_name[32];
    const int partial_name_len =
        snprintf(partial_name, sizeof(partial_name), "partials/p%td.html", i);
    if (!TEST_CHECK(partial_name_len > 0 && (size_t)partial_name_len < sizeof(partial_name))) {
      goto cleanup;
    }
    if (!TEST_CHECK(write_fixture_file(root_dir, partial_name, "x") == 0)) {
      goto cleanup;
    }
  }

  // `index.html` references exactly the limit. `over.html` references one more.
  for (ptrdiff_t i = 0; i <= RENDER_PARTIAL_COUNT_MAX; i++) {
    char reference[16];
    const int n = snprintf(reference, sizeof(reference), "{{>p%td}}", i);
    if (!TEST_CHECK(n > 0 && (size_t)n < sizeof(reference))) {
      goto cleanup;
    }
    if (i < RENDER_PARTIAL_COUNT_MAX) {
      memcpy(at_limit + at_limit_len, reference, (size_t)n);
      at_limit_len += (size_t)n;
    }
    memcpy(over_limit + over_limit_len, reference, (size_t)n);
    over_limit_len += (size_t)n;
  }
  at_limit[at_limit_len] = '\0';
  over_limit[over_limit_len] = '\0';
  if (!TEST_CHECK(write_fixture_file(root_dir, "index.html", at_limit) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "over.html", over_limit) == 0)) {
    goto cleanup;
  }

  if (!TEST_CHECK(render(root_dir, "index.html", &context, &perm, &at_rendered, &at_log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&at_log));
  TEST_CHECK(strcmp(at_rendered, at_expected) == 0);

  TEST_CHECK(render(root_dir, "over.html", &context, &perm, NULL, &over_log) == -1);
  expected_len = snprintf(expected, sizeof(expected),
                          "render exceeds max distinct partials (%d) while resolving partial 'p%d' "
                          "(in 'over.html')",
                          RENDER_PARTIAL_COUNT_MAX, RENDER_PARTIAL_COUNT_MAX);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&over_log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A partial that includes itself is bounded by the expansion count rather than recursing until the
// stack runs out. The diagnostic names the limit and the likely cause.
//
// Unlike the output bound, this test uses the real limit. Reaching it proves that the selected
// constant has an acceptable cost. A full 300,000 expansions of this bare `{{>loop}}` uses
// single-digit MB and finishes in a fraction of a second. A cheaper override would test the
// mechanism without testing the number. This template resolves no names, so it measures the floor
// rather than the ceiling. A runaway partial referencing names costs a `struct Node` per name per
// expansion, which is the term `template.c`'s limits comment sizes the constant against.
static void test_rejects_recursive_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>loop}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/loop.html", "{{>loop}}") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  // `RENDER_EXPANSION_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing
  // it there must update this.
  TEST_CHECK(
      is_log_one_failure(&log, &entry,
                         "render exceeds max partial expansions (300000); check for a partial that "
                         "includes itself (in 'index.html')"));

cleanup:
  remove_fixture_tree(root_dir);
}

// Two partials that include each other are bounded by the same expansion count, so the bound is not
// specific to direct self-inclusion.
static void test_rejects_mutual_partial_cycle(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>ping}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/ping.html", "{{>pong}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/pong.html", "{{>ping}}") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  // `RENDER_EXPANSION_COUNT_MAX` is spelled out here for the same reason as in
  // `test_rejects_recursive_partial`. Changing it in `template.c` must update this.
  TEST_CHECK(
      is_log_one_failure(&log, &entry,
                         "render exceeds max partial expansions (300000); check for a partial that "
                         "includes itself (in 'index.html')"));

cleanup:
  remove_fixture_tree(root_dir);
}

// An oversize render is rejected once the accumulated output passes the byte bound it was given.
//
// Reaching a byte-count limit means accumulating that many bytes. The production bound is sized for
// the largest real site, so asserting it at that value would cost most of a gigabyte of peak RSS
// for one diagnostic. This case passes a 64 KiB bound to `template_render_file_limited` instead.
static void test_rejects_oversize_render_output(void) {
  struct Arena perm = test_arena();
  enum { OUTPUT_LEN_MAX = 64 * 1024 };

  // One kilobyte per expansion, so the bound is passed in far fewer expansions than the expansion
  // limit allows and this case lands on the output limit.
  enum { PADDING_LEN = 1024 };
  char partial[PADDING_LEN + sizeof("{{>card}}")];
  memset(partial, 'x', (size_t)PADDING_LEN);
  memcpy(partial + PADDING_LEN, "{{>card}}", sizeof("{{>card}}"));

  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected_head[EXPECTED_SIZE];
  int expected_head_len = 0;
  struct Str failure = {0};
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html", partial) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render_limited(root_dir, "index.html", &context, OUTPUT_LEN_MAX, &perm, NULL, &log) ==
             -1);
  // The measured length is whichever append first crossed the bound, so it depends on the chunking
  // mustache4c happens to use. The limit clause and the template attribution are deterministic and
  // are asserted instead.
  expected_head_len = snprintf(expected_head, sizeof(expected_head),
                               "render exceeds max rendered output (%d bytes) at ", OUTPUT_LEN_MAX);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  failure = buf_str(&log.buf);
  TEST_CHECK(str_startswith(failure, (struct Str){expected_head, expected_head_len}));
  // Anchored at the end rather than searched for: the head above pins the start and the measured
  // length sits between them, so a floating `strstr` would leave anything trailing the message
  // undetected. The entry has no source path, so the page attribution names an empty one.
  static const char expected_tail[] = " bytes (in 'index.html') (while rendering '')\n";
  TEST_CHECK(
      str_endswith(failure, (struct Str){expected_tail, (ptrdiff_t)sizeof(expected_tail) - 1}));
  TEST_CHECK(log.errors == 1);
  TEST_MSG("actual: '%.*s'", STR_FMT(failure));

cleanup:
  remove_fixture_tree(root_dir);
}

// Compilation rejects an unclosed section. The diagnostic gives the line and column of the
// incorrect tag, not only the template name. mustache4c reports this position through its parser
// callback. The section opens on line 3, so a hardcoded line 1 cannot pass.
static void test_rejects_unclosed_section(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "first\nsecond\n{{#content_entries}}{{title}}\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  // The leading `%s` is mustache4c's own parser text. Only the position and template around it are
  // this project's wording. Composing through the same format string keeps the two distinguishable
  // and keeps that wording greppable from `template.c`.
  expected_len = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                          "section-opening tag has no closer", 3U, 1U, "index.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A mismatched section closing tag fails to compile. The diagnostic says the closer does not match
// its opener rather than reporting a generic compile failure. The leading `%s` is mustache4c's own
// parser text. Composing through the same format string as `test_rejects_unclosed_section` keeps
// that wording distinguishable from this project's position wrapper. The closer sits at column 30,
// so a hardcoded 1 cannot pass.
static void test_rejects_section_name_mismatch(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{#content_entries}}{{title}}{{/wrong}}") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len =
      snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
               "name of section-closing tag does not match corresponding section-opening "
               "tag",
               1U, 30U, "index.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// Malformed tag syntax and unsupported block commands are each rejected with the library's own
// reason and the column of the offending tag, so one malformed shape cannot pass on another's
// failure. Every reason below is mustache4c's text and only the position wrapper is this project's,
// composed through the same format string as `test_rejects_unclosed_section`. Each source is a
// single line, so the line is 1 by construction and the column is what distinguishes the cases.
static void test_rejects_malformed_tags(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  init_test_content_entry(&entry);

  static const struct {
    const char* source;
    const char* reason;
    unsigned column;
  } cases[] = {
      {"{{title\n", "tag opener has no closer", 1U},
      {"{{{title}}\n", "tag closer is incompatible with its opener", 11U},
      {"{{/each}}\n", "section-closing tag has no opener", 1U},
      {"{{#if content_entry}}\n", "tag name is invalid", 1U},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    if (!TEST_CHECK(write_fixture_file(root_dir, "index.html", cases[i].source) == 0)) {
      goto cleanup;
    }
    struct Log log = test_log();
    TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')", cases[i].reason,
                 1U, cases[i].column, "index.html");
    if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
      goto cleanup;
    }
    TEST_CHECK(is_log_one_failure(&log, &entry, expected));
  }

cleanup:
  remove_fixture_tree(root_dir);
}

// A syntax error inside a partial names the partial's *file*, not the bare tag name.
// `(in '<file>')` must denote a file, and `card` is not one. It is also ambiguous with a top-level
// template of the same name, which reaches the identical format string. The position is the
// partial's own, so line 2 here is line 2 of `partials/card.html`, not of the `index.html` that
// included it.
static void test_rejects_syntax_error_in_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html",
                                     "first\n{{#content_entries}}{{title}}\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                          "section-opening tag has no closer", 2U, 1U, "partials/card.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

// A syntax error in the middle of a partial is reported like one at its end, as the first error of
// the partial. mustache4c goes on scanning the partial after it reports an error, so the diagnostic
// must be allocated past the partial's bytes rather than over them. The text after the error is
// longer than the diagnostic and holds a second error, so under ASan an overlapping message makes
// the scan read poisoned memory.
static void test_rejects_syntax_error_inside_partial(void) {
  struct Arena perm = test_arena();
  char root_dir[] = "/tmp/sosig-template-XXXXXX";
  if (write_template_fixture(root_dir, "{{>card}}\n") != 0) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entry_current = &entry, .site_updated = SITE_UPDATED};
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  int expected_len = 0;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(write_fixture_file(root_dir, "partials/card.html",
                                     "first\n{{title\n"
                                     "a line of ordinary text that runs on well past the length "
                                     "of the diagnostic for the error above it\n"
                                     "{{/each}}\n"
                                     "and one more line of text after the second error\n") == 0)) {
    goto cleanup;
  }
  TEST_CHECK(render(root_dir, "index.html", &context, &perm, NULL, &log) == -1);
  expected_len = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                          "tag opener has no closer", 2U, 1U, "partials/card.html");
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(is_log_one_failure(&log, &entry, expected));

cleanup:
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"renders escaped title and raw body", test_renders_escaped_title_and_raw_body},
    {"iterates content entries and tags", test_iterates_content_entries_and_tags},
    {"empty tag does not truncate tag list", test_empty_tag_does_not_truncate_tag_list},
    {"nested section reiterates content entries", test_nested_section_reiterates_content_entries},
    {"escapes double brace and leaves triple brace raw",
     test_escapes_double_brace_and_leaves_triple_brace_raw},
    {"renders atom feed", test_renders_atom_feed},
    {"renders generator", test_renders_generator},
    {"renders partial", test_renders_partial},
    {"repeated partial renders same content each time",
     test_repeated_partial_renders_same_content_each_time},
    {"repeated partial stays under distinct limit",
     test_repeated_partial_stays_under_distinct_limit},
    {"empty template yields empty string", test_empty_template_yields_empty_string},
    {"unknown variable renders empty", test_unknown_variable_renders_empty},
    {"inverted section renders when list empty", test_inverted_section_renders_when_list_empty},
    {"empty field is falsey as section", test_empty_field_is_falsey_as_section},
    {"comments are dropped", test_comments_are_dropped},
    {"output bound applies per call", test_output_bound_applies_per_call},
    {"rejects unsafe partial name", test_rejects_unsafe_partial_name},
    {"rejects unsafe partial name in partial", test_rejects_unsafe_partial_name_in_partial},
    {"rejects unreadable partial", test_rejects_unreadable_partial},
    {"rejects template with NUL byte", test_rejects_template_with_nul_byte},
    {"rejects partial with NUL byte", test_rejects_partial_with_nul_byte},
    {"rejects oversize template and partial", test_rejects_oversize_template_and_partial},
    {"rejects partial count past limit", test_rejects_partial_count_past_limit},
    {"rejects recursive partial", test_rejects_recursive_partial},
    {"rejects mutual partial cycle", test_rejects_mutual_partial_cycle},
    {"rejects oversize render output", test_rejects_oversize_render_output},
    {"rejects unclosed section", test_rejects_unclosed_section},
    {"rejects section name mismatch", test_rejects_section_name_mismatch},
    {"rejects malformed tags", test_rejects_malformed_tags},
    {"rejects syntax error in partial", test_rejects_syntax_error_in_partial},
    {"rejects syntax error inside partial", test_rejects_syntax_error_inside_partial},
    {NULL, NULL},
};
