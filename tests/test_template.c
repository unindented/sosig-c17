#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "build/template.h"
#include "core/error.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"

// Initializes the shared site config metadata used by template tests. The fields below are string
// literals, not arena copies, which is safe only because `site_config_free` releases the arena and
// never the field pointers. Assigning a heap string here would leak. Making the free release these
// would be a `free` of a string literal. `init_test_content_entry` relies on the same rule.
static void init_test_site_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->base_url = "https://example.com";
  site_config->title = "Site";
  site_config->author = "Author";
}

// Initializes the shared content entry metadata used by template tests.
static void init_test_content_entry(struct ContentEntry* entry) {
  content_entry_init(entry);
  entry->title = "Content Entry";
  entry->date = "2026-07-01T00:00:00Z";
  entry->description = "Desc";
  entry->slug = "content-entry";
  entry->url_path = "/content-entry.html";
  entry->body_html = "<p>raw</p>";
}

// Creates a temporary template root. Returns `NULL` once `TEST_CHECK` has failed the test, so a
// caller's `return` only suppresses cascading noise. The result aliases the caller's `dir` rather
// than being owned.
static const char* init_template_fixture(char root_dir[static 1]) {
  char* tmp = mkdtemp(root_dir);
  TEST_CHECK(tmp != NULL);
  return tmp;
}

// Formats a fixture path inside a temporary template root.
static int template_fixture_path(const char* root_dir,
                                 const char* template_name,
                                 char* fixture_path_out,
                                 size_t fixture_path_out_len) {
  const int n = snprintf(fixture_path_out, fixture_path_out_len, "%s/%s", root_dir, template_name);
  TEST_CHECK(n > 0 && (size_t)n < fixture_path_out_len);
  return n > 0 && (size_t)n < fixture_path_out_len ? 0 : -1;
}

// Writes one template fixture file, creating parent directories as needed. The `NULL, 0` reason
// arguments are the option `fs_write_file`'s contract allows: a failed fixture write is a broken
// test, not behavior under test. Callers assert the non-zero return.
static int write_template_fixture(const char* root_dir,
                                  const char* template_name,
                                  const char* contents) {
  char fixture_path[256];
  if (template_fixture_path(root_dir, template_name, fixture_path, sizeof(fixture_path)) != 0) {
    return -1;
  }
  return fs_write_file(fixture_path, contents, strlen(contents), NULL, 0);
}

// Removes the fixture paths used by these tests.
static void cleanup_template_fixture(const char* root_dir) {
  static const char* const names[] = {
      "partials/card.html", "partials/loop.html", "partials/ping.html",
      "partials/pong.html", "content-entry.html", "index.html",
  };

  char fixture_path[256];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (template_fixture_path(root_dir, names[i], fixture_path, sizeof(fixture_path)) == 0) {
      (void)unlink(fixture_path);
    }
  }
  if (template_fixture_path(root_dir, "partials", fixture_path, sizeof(fixture_path)) == 0) {
    (void)rmdir(fixture_path);
  }
  (void)rmdir(root_dir);
}

// `{{title}}` is escaped and `{{{body}}}` is not in the same render, so escaping is chosen per tag
// rather than per template. This renders the real `templates/content.html`, whose output, including
// the feed `<link rel="alternate">`, is diffed end to end by the golden files in
// `tests/expected/site-file-permalink`.
static void test_renders_escaped_title_and_raw_body(void) {
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = "Content Entry <One>";
  const char* templates_dir = "tests/fixtures/site-file-permalink/templates";
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html =
                  template_render_file(templates_dir, "content.html", &context, NULL, 0)) != NULL);
  TEST_CHECK(strstr(rendered_html, "Content Entry &lt;One&gt;") != NULL);
  TEST_CHECK(strstr(rendered_html, "<p>raw</p>") != NULL);
  TEST_CHECK(strstr(rendered_html, "rel=\"alternate\"") != NULL);
  free(rendered_html);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// Content entries and their tag lists iterate with escaping.
static void test_iterates_content_entries_and_tags(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  const char template[] =
      "{{#content_entries}}{{title}}: {{#tags}}[{{.}}]{{/tags}}\n{{/content_entries}}";
  TEST_CHECK(write_template_fixture(tmp, "index.html", template) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  static const char* first_tags[] = {"c", "x&y"};
  static const char* second_tags[] = {"z"};
  first.title = "One <A>";
  first.tags = first_tags;
  first.tag_count = 2;
  second.title = "Two";
  second.tags = second_tags;
  second.tag_count = 1;
  const struct ContentEntry* content_entries[] = {&first, &second};
  const char* templates_dir = tmp;
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 2};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html =
                  template_render_file(templates_dir, "index.html", &context, NULL, 0)) != NULL);
  TEST_CHECK(strcmp(rendered_html, "One &lt;A&gt;: [c][x&amp;y]\nTwo: [z]\n") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&second);
  content_entry_free(&first);
  site_config_free(&site_config);
}

// An empty tag renders as an empty item and does not end the iteration. `node_scalar` maps `""` to
// `NULL` so an unset optional *field* is falsey, but a tag list must not inherit that rule: `NULL`
// from the child callback means end-of-list to mustache4c, so an empty tag would silently drop
// every tag after it. Frontmatter accepts `tags = ["", "x"]`, so this is reachable from a real
// site.
static void test_empty_tag_does_not_truncate_tag_list(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(
                 tmp, "index.html",
                 "{{#content_entries}}{{#tags}}[{{.}}]{{/tags}}{{/content_entries}}") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  static const char* tags[] = {"a", "", "b"};
  entry.tags = tags;
  entry.tag_count = 3;
  const struct ContentEntry* content_entries[] = {&entry};
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(tmp, "index.html", &context, NULL, 0)) != NULL);
  TEST_CHECK(strcmp(rendered_html, "[a][][b]") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// Nested sections re-iterate content entries in an inner context.
static void test_nested_section_reiterates_content_entries(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  const char template[] =
      "{{#content_entries}}"
      "["
      "{{title}}:"
      "{{#content_entries}}"
      "{{title}},"
      "{{/content_entries}}"
      "]"
      "{{/content_entries}}";
  TEST_CHECK(write_template_fixture(tmp, "index.html", template) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  first.title = "One";
  second.title = "Two";
  const struct ContentEntry* content_entries[] = {&first, &second};
  const char* templates_dir = tmp;
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 2};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html =
                  template_render_file(templates_dir, "index.html", &context, NULL, 0)) != NULL);
  TEST_CHECK(strcmp(rendered_html, "[One:One,Two,][Two:One,Two,]") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&second);
  content_entry_free(&first);
  site_config_free(&site_config);
}

// Double-brace variables are escaped and triple-brace variables stay raw.
static void test_escapes_double_brace_and_leaves_triple_brace_raw(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  const char template[] = "{{title}} {{{title}}}\n";
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", template) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = "Content Entry <One>";
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   NULL, 0)) != NULL);
  TEST_CHECK(strcmp(rendered_html, "Content Entry &lt;One&gt; Content Entry <One>\n") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A safe partial loads and renders in the parent context.
static void test_renders_partial(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "before {{> card}} after") == 0);
  TEST_CHECK(write_template_fixture(tmp, "partials/card.html", "{{title}}") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = "Content Entry <One>";
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   NULL, 0)) != NULL);
  TEST_CHECK(strcmp(rendered_html, "before Content Entry &lt;One&gt; after") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A partial referenced twice per entry across three entries expands six times and renders the same
// content each time. The compile cache is not observable through the API, so this pins what a
// broken cache would break, stale or empty expansions, rather than the number of compiles.
static void test_repeated_partial_renders_same_content_each_time(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "partials/card.html", "[{{title}}]") == 0);
  TEST_CHECK(write_template_fixture(tmp, "index.html",
                                    "{{#content_entries}}{{>card}}{{>card}}{{/content_entries}}") ==
             0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry, &entry, &entry};
  const char* templates_dir = tmp;
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 3};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "index.html", &context, err,
                                                   sizeof(err))) != NULL);
  TEST_CHECK(err[0] == '\0');
  if (rendered_html != NULL) {
    // Three entries x two references each, so the partial is expanded six times off one compile.
    size_t expansions = 0;
    for (const char* p = rendered_html; (p = strstr(p, "[Content Entry]")) != NULL; p++) {
      expansions++;
    }
    TEST_CHECK(expansions == 6);
  }
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
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
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  enum { REFERENCE_COUNT = RENDER_PARTIAL_COUNT_MAX + 6 };
  enum { REFERENCE_LEN = sizeof("{{>card}}") - 1 };
  TEST_CHECK(write_template_fixture(tmp, "partials/card.html", "[{{title}}]") == 0);
  char source[REFERENCE_COUNT * REFERENCE_LEN + 1];
  for (size_t i = 0; i < (size_t)REFERENCE_COUNT; i++) {
    memcpy(source + i * REFERENCE_LEN, "{{>card}}", REFERENCE_LEN);
  }
  source[REFERENCE_COUNT * REFERENCE_LEN] = '\0';
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", source) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   err, sizeof(err))) != NULL);
  TEST_CHECK(err[0] == '\0');
  if (rendered_html != NULL) {
    size_t expansions = 0;
    for (const char* p = rendered_html; (p = strstr(p, "[Content Entry]")) != NULL; p++) {
      expansions++;
    }
    TEST_CHECK(expansions == (size_t)REFERENCE_COUNT);
  }
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// The feed template renders through the same name-based entry point as an HTML page. A body that
// already holds HTML is escaped again for XML. This renders the real `templates/atom.xml`, so the
// end-to-end assertion lives in the `tests/expected/site-file-permalink/atom.xml` diff.
static void test_renders_atom_feed(void) {
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = "Content Entry <One>";
  entry.description = "Desc & More";
  const struct ContentEntry* content_entries[] = {&entry};
  const char* templates_dir = "tests/fixtures/site-file-permalink/templates";
  struct TemplateContext context = {.site_config = &site_config,
                                    .content_entry_current = NULL,
                                    .content_entries = content_entries,
                                    .content_entry_count = 1,
                                    .site_updated = entry.date};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "atom.xml", &context, NULL, 0)) !=
             NULL);
  TEST_CHECK(strstr(rendered_html, "<title>Content Entry &lt;One&gt;</title>") != NULL);
  TEST_CHECK(strstr(rendered_html, "<content type=\"html\">&lt;p&gt;raw&lt;/p&gt;</content>") !=
             NULL);
  TEST_CHECK(strstr(rendered_html, "<updated>2026-07-01T00:00:00Z</updated>") != NULL);
  free(rendered_html);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// An empty template returns an allocated empty string.
static void test_empty_template_yields_empty_string(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   NULL, 0)) != NULL);
  TEST_CHECK(strcmp(rendered_html, "") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// An unknown variable renders as empty text per the Mustache spec.
static void test_unknown_variable_renders_empty(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "[{{typo}}]\n") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   NULL, 0)) != NULL);
  TEST_CHECK(rendered_html != NULL && strcmp(rendered_html, "[]\n") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// An inverted section renders only when the list is empty.
static void test_inverted_section_renders_when_list_empty(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  const char template[] =
      "{{#content_entries}}x{{/content_entries}}{{^content_entries}}none{{/"
      "content_entries}}";
  TEST_CHECK(write_template_fixture(tmp, "index.html", template) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  const char* templates_dir = tmp;
  struct TemplateContext context_empty = {.site_config = &site_config, .content_entry_count = 0};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "index.html", &context_empty,
                                                   NULL, 0)) != NULL);
  TEST_CHECK(rendered_html != NULL && strcmp(rendered_html, "none") == 0);
  free(rendered_html);

  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry};
  struct TemplateContext context_full = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "index.html", &context_full, NULL,
                                                   0)) != NULL);
  TEST_CHECK(rendered_html != NULL && strcmp(rendered_html, "x") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// An empty field value is falsey as a section, so an unset optional field reads as absent. Were an
// empty scalar truthy, `{{#description}}` would render for every entry and `{{^description}}` for
// none, so both branches are asserted here.
static void test_empty_field_is_falsey_as_section(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  const char template[] =
      "{{#content_entries}}[{{#description}}D:{{description}}{{/description}}"
      "{{^description}}NONE{{/description}}]{{/content_entries}}";
  TEST_CHECK(write_template_fixture(tmp, "index.html", template) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);

  // `content_entry_init` defaults `description` to an empty string rather than `NULL`, so this is
  // the shape produced by a content file that omits the key.
  struct ContentEntry unset_entry;
  init_test_content_entry(&unset_entry);
  unset_entry.description = "";
  struct ContentEntry set_entry;
  init_test_content_entry(&set_entry);
  set_entry.description = "Desc";

  const struct ContentEntry* content_entries[] = {&unset_entry, &set_entry};
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 2};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(tmp, "index.html", &context, NULL, 0)) != NULL);
  TEST_CHECK(rendered_html != NULL && strcmp(rendered_html, "[NONE][D:Desc]") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&unset_entry);
  content_entry_free(&set_entry);
  site_config_free(&site_config);
}

// Comments are dropped from the rendered output.
static void test_comments_are_dropped(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "a{{! ignored }}b") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   NULL, 0)) != NULL);
  TEST_CHECK(rendered_html != NULL && strcmp(rendered_html, "ab") == 0);
  free(rendered_html);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A template name that could escape the root is rejected, naming the offending template. The first
// call passes a real `err` buffer and asserts the whole message. The calls below pass `NULL, 0` and
// assert only the `NULL` return. That is deliberate rather than an oversight. The message is pinned
// once here, and repeating it per variant would couple every rejection variant to its wording.
static void test_rejects_unsafe_template_name(void) {
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  const char* templates_dir = "tests/fixtures/site-file-permalink/templates";
  struct TemplateContext context = {.site_config = &site_config};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "../content-entry.html", &context,
                                                   err, sizeof(err))) == NULL);
  TEST_CHECK(
      strcmp(err, "template must be a safe relative template name: '../content-entry.html'") == 0);
  TEST_CHECK((rendered_html =
                  template_render_file(templates_dir, "../atom.xml", &context, NULL, 0)) == NULL);
  // An embedded `..` segment is rejected by the safety check itself (a distinct path from a leading
  // `..`), independent of whether the target exists.
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "sub/../../secret.xml", &context,
                                                   NULL, 0)) == NULL);
  site_config_free(&site_config);
}

// A partial name that could escape the partials directory is rejected, with a diagnostic. A name
// containing `/` never reaches the provider: mustache4c's own tag-name validation rejects it while
// compiling, so the failure surfaces as a compile error for the enclosing template.
static void test_rejects_unsafe_partial_name(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "{{> ../secret}}\n") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   err, sizeof(err))) == NULL);
  // Rejected by mustache4c's own tag validation, so the leading `%s` is its text and only the
  // position wrapper is this project's. Composed through the same format string as
  // `test_rejects_unclosed_section`, which is the other assertion of that wrapper.
  char expected_invalid_tag[ERROR_MESSAGE_SIZE];
  int n = snprintf(expected_invalid_tag, sizeof(expected_invalid_tag),
                   "%s at line %u, column %u (in '%s')", "tag name is invalid", 1U, 1U,
                   "content-entry.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_invalid_tag));
  TEST_CHECK(strcmp(err, expected_invalid_tag) == 0);

  // A name Mustache accepts but that is not a safe identifier is rejected by the provider, which
  // names the offending partial.
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "{{>nested.name}}\n") == 0);
  err[0] = '\0';
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   err, sizeof(err))) == NULL);
  char expected_unsafe_name[ERROR_MESSAGE_SIZE];
  n = snprintf(expected_unsafe_name, sizeof(expected_unsafe_name),
               "partial name must contain only letters, digits, '_' and '-': '%s'", "nested.name");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_unsafe_name));
  TEST_CHECK(strcmp(err, expected_unsafe_name) == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A partial name too long for the path buffer names that limit, instead of reporting the same
// generic failure as an allocation error. The name is a legal identifier, so nothing rejects it
// before the path is built.
static void test_rejects_oversize_partial_name(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // `PARTIAL_PATH_SIZE` is file-local to `template.c`, so it is spelled out here. Changing it there
  // must update this. A name of exactly this length is the first that `partials/<name>.html` cannot
  // hold, one byte past the longest that fits.
  enum { PARTIAL_PATH_SIZE = 256 };
  const size_t name_len = PARTIAL_PATH_SIZE - (sizeof("partials/") - 1) - (sizeof(".html") - 1);
  char name[PARTIAL_PATH_SIZE];
  memset(name, 'a', name_len);
  name[name_len] = '\0';
  char source[PARTIAL_PATH_SIZE + 32];
  int n = snprintf(source, sizeof(source), "{{>%s}}\n", name);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(source));
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", source) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(templates_dir, "content-entry.html", &context, err,
                                  sizeof(err)) == NULL);
  char expected[ERROR_MESSAGE_SIZE];
  n = snprintf(expected, sizeof(expected),
               "partial path exceeds max partial path length (%zu bytes) at %zu bytes: '%s'",
               (size_t)PARTIAL_PATH_SIZE - 1, strlen("partials/") + strlen(name) + strlen(".html"),
               name);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// An unreadable partial fails the render and reports the cause. It does not publish a page with a
// missing partial. mustache4c interprets a `NULL` callback result as an absent partial and reports
// success. Only the provider's failure flag converts this result to a failed render. A typo in
// `{{>card}}` is a typical cause.
static void test_rejects_unreadable_partial(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // No `partials/card.html` is written, so resolving the reference fails on the read.
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "before {{>card}} after") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(tmp, "content-entry.html", &context, err, sizeof(err)) == NULL);

  // The path embeds the temporary directory, so the test checks two fixed parts instead of the
  // complete value. The first part is the reason and cause at the start. The second part is the
  // path suffix. Each part is a complete claim, not a single word.
  char reason[FS_REASON_SIZE];
  (void)snprintf(reason, sizeof(reason), "%s", strerror(ENOENT));
  char expected_head[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected_head, sizeof(expected_head), "failed to read partial: %s ('", reason);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, strlen(expected_head)) == 0);
  TEST_CHECK(strstr(err, "/partials/card.html')") != NULL);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// One distinct partial past the limit is rejected, naming the limit and the partial that reached
// it. The sibling above covers the cache-hit side, where references beyond the limit cost nothing.
// This covers the miss side, where each distinct name consumes a slot. The guard is the only thing
// between a 65th distinct partial and a write past two fixed 64-element arrays living in
// `template_render_file`'s stack frame, so both sides of the comparison are asserted: exactly the
// limit renders, one more fails.
//
// `RENDER_PARTIAL_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing it
// there must update this.
static void test_rejects_partial_count_past_limit(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  enum { RENDER_PARTIAL_COUNT_MAX = 64 };
  // One partial file per distinct name, plus the one that overflows the budget.
  for (size_t i = 0; i <= (size_t)RENDER_PARTIAL_COUNT_MAX; i++) {
    char partial_name[32];
    TEST_ASSERT(snprintf(partial_name, sizeof(partial_name), "partials/p%zu.html", i) > 0);
    TEST_CHECK(write_template_fixture(tmp, partial_name, "x") == 0);
  }

  // `index.html` references exactly the limit. `content-entry.html` references one more.
  char at_limit[RENDER_PARTIAL_COUNT_MAX * 10];
  char over_limit[(RENDER_PARTIAL_COUNT_MAX + 1) * 10];
  size_t at_limit_len = 0;
  size_t over_limit_len = 0;
  for (size_t i = 0; i <= (size_t)RENDER_PARTIAL_COUNT_MAX; i++) {
    char reference[16];
    const int n = snprintf(reference, sizeof(reference), "{{>p%zu}}", i);
    TEST_ASSERT(n > 0);
    if (i < (size_t)RENDER_PARTIAL_COUNT_MAX) {
      memcpy(at_limit + at_limit_len, reference, (size_t)n);
      at_limit_len += (size_t)n;
    }
    memcpy(over_limit + over_limit_len, reference, (size_t)n);
    over_limit_len += (size_t)n;
  }
  at_limit[at_limit_len] = '\0';
  over_limit[over_limit_len] = '\0';
  TEST_CHECK(write_template_fixture(tmp, "index.html", at_limit) == 0);
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", over_limit) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};

  char at_err[ERROR_MESSAGE_SIZE] = "";
  char* at_rendered = template_render_file(tmp, "index.html", &context, at_err, sizeof(at_err));
  TEST_CHECK(at_rendered != NULL);
  TEST_CHECK(at_err[0] == '\0');
  free(at_rendered);

  char over_err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(tmp, "content-entry.html", &context, over_err,
                                  sizeof(over_err)) == NULL);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected),
                         "render exceeds max distinct partials (%d) while resolving partial 'p%d' "
                         "(in 'content-entry.html')",
                         RENDER_PARTIAL_COUNT_MAX, RENDER_PARTIAL_COUNT_MAX);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(over_err, expected) == 0);

  // The generated partials are not in `cleanup_template_fixture`'s fixed list, so they go here.
  for (size_t i = 0; i <= (size_t)RENDER_PARTIAL_COUNT_MAX; i++) {
    char partial_name[32];
    char partial_path[256];
    if (snprintf(partial_name, sizeof(partial_name), "partials/p%zu.html", i) > 0 &&
        template_fixture_path(tmp, partial_name, partial_path, sizeof(partial_path)) == 0) {
      (void)unlink(partial_path);
    }
  }
  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A partial that includes itself is bounded by the expansion count rather than recursing until the
// stack runs out. The diagnostic names the limit and the likely cause.
//
// Unlike the output bound, this test uses the real limit. Reaching it proves that the selected
// constant has an acceptable cost. A full 300,000 expansions of this bare `{{>loop}}` uses
// single-digit MB and finishes in less than one tenth of a second. A cheaper override would test
// the mechanism without testing the number. This template resolves no names, so it measures the
// floor rather than the ceiling. A runaway partial referencing names costs a `struct Node` per name
// per expansion, which is the term `template.c`'s limits comment sizes the constant against.
static void test_rejects_recursive_partial(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "{{>loop}}") == 0);
  TEST_CHECK(write_template_fixture(tmp, "partials/loop.html", "{{>loop}}") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   err, sizeof(err))) == NULL);
  // `RENDER_EXPANSION_COUNT_MAX` is file-local to `template.c`, so it is spelled out here. Changing
  // it there must update this.
  TEST_CHECK(strcmp(err,
                    "render exceeds max partial expansions (300000); check for a partial that "
                    "includes itself (in 'content-entry.html')") == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// Two partials that include each other are bounded by the same expansion count, so the bound is not
// specific to direct self-inclusion.
static void test_rejects_mutual_partial_cycle(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "{{>ping}}") == 0);
  TEST_CHECK(write_template_fixture(tmp, "partials/ping.html", "{{>pong}}") == 0);
  TEST_CHECK(write_template_fixture(tmp, "partials/pong.html", "{{>ping}}") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   err, sizeof(err))) == NULL);
  // `RENDER_EXPANSION_COUNT_MAX` is spelled out here for the same reason as in
  // `test_rejects_recursive_partial`. Changing it in `template.c` must update this.
  TEST_CHECK(strcmp(err,
                    "render exceeds max partial expansions (300000); check for a partial that "
                    "includes itself (in 'content-entry.html')") == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// An oversize render is rejected once the accumulated output passes the configured byte bound.
//
// `template.c` enforces `RENDER_OUTPUT_LEN_MAX`, which this binary overrides down to
// `SOSIG_RENDER_OUTPUT_LEN_MAX` (see the `Makefile`). Reaching a byte-count limit means
// accumulating that many bytes. The production bound is sized for the largest real site, so
// asserting it at that value would cost most of a gigabyte of peak RSS for one diagnostic.
static void test_rejects_oversize_render_output(void) {
  _Static_assert(SOSIG_RENDER_OUTPUT_LEN_MAX <= 1024 * 1024,
                 "this binary's render output bound must stay small enough to reach cheaply");

  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  // One kilobyte per expansion, so the bound is passed in far fewer expansions than the expansion
  // limit allows and this case lands on the output limit.
  enum { PADDING_LEN = 1024 };
  char partial[PADDING_LEN + sizeof("{{>card}}")];
  memset(partial, 'x', (size_t)PADDING_LEN);
  memcpy(partial + PADDING_LEN, "{{>card}}", sizeof("{{>card}}"));
  TEST_CHECK(write_template_fixture(tmp, "content-entry.html", "{{>card}}") == 0);
  TEST_CHECK(write_template_fixture(tmp, "partials/card.html", partial) == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "content-entry.html", &context,
                                                   err, sizeof(err))) == NULL);
  // The measured length is whichever append first crossed the bound, so it depends on the chunking
  // mustache4c happens to use. The limit clause and the template attribution are deterministic and
  // are asserted instead.
  char expected_head[ERROR_MESSAGE_SIZE];
  const int expected_head_len =
      snprintf(expected_head, sizeof(expected_head),
               "render exceeds max rendered output (%d bytes) at ", SOSIG_RENDER_OUTPUT_LEN_MAX);
  TEST_CHECK(expected_head_len > 0 && (size_t)expected_head_len < sizeof(expected_head));
  TEST_CHECK(strncmp(err, expected_head, (size_t)expected_head_len) == 0);
  // Anchored at the end rather than searched for: the head above pins the start and the measured
  // length sits between them, so a floating `strstr` would leave anything trailing the message
  // undetected.
  static const char expected_tail[] = " bytes (in 'content-entry.html')";
  const size_t err_actual_len = strlen(err);
  TEST_CHECK(err_actual_len >= sizeof(expected_tail) - 1 &&
             strcmp(err + err_actual_len - (sizeof(expected_tail) - 1), expected_tail) == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// Compilation rejects an unclosed section. The diagnostic gives the line and column of the
// incorrect tag, not only the template name. mustache4c reports this position through its parser
// callback. The section opens on line 3, so a hardcoded line 1 cannot pass.
static void test_rejects_unclosed_section(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "index.html",
                                    "first\nsecond\n{{#content_entries}}{{title}}\n") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry};
  const char* templates_dir = tmp;
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "index.html", &context, err,
                                                   sizeof(err))) == NULL);
  // The leading `%s` is mustache4c's own parser text. Only the position and template around it are
  // this project's wording. Composing through the same format string keeps the two distinguishable
  // and keeps that wording greppable from `template.c`.
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                         "section-opening tag has no closer", 3U, 1U, "index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A mismatched section closing tag fails to compile. The diagnostic says the closer does not match
// its opener rather than reporting a generic compile failure. The leading `%s` is mustache4c's own
// parser text. Composing through the same format string as `test_rejects_unclosed_section` keeps
// that wording distinguishable from this project's position wrapper. The closer sits at column 30,
// so a hardcoded 1 cannot pass.
static void test_rejects_section_name_mismatch(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "index.html", "{{#content_entries}}{{title}}{{/wrong}}") ==
             0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry};
  const char* templates_dir = tmp;
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(template_render_file(templates_dir, "index.html", &context, err, sizeof(err)) == NULL);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                         "name of section-closing tag does not match corresponding section-opening "
                         "tag",
                         1U, 30U, "index.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// Malformed tag syntax and unsupported block commands are each rejected with the library's own
// reason and the column of the offending tag, so one malformed shape cannot pass on another's
// failure. Every reason below is mustache4c's text and only the position wrapper is this project's,
// composed through the same format string as `test_rejects_unclosed_section`. Each source is a
// single line, so the line is 1 by construction and the column is what distinguishes the cases.
static void test_rejects_malformed_tags(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const char* templates_dir = tmp;
  struct TemplateContext context = {.site_config = &site_config, .content_entry_current = &entry};

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
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    TEST_CHECK(write_template_fixture(tmp, "content-entry.html", cases[i].source) == 0);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(template_render_file(templates_dir, "content-entry.html", &context, err,
                                    sizeof(err)) == NULL);
    char expected[ERROR_MESSAGE_SIZE];
    const int n = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                           cases[i].reason, 1U, cases[i].column, "content-entry.html");
    TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
    TEST_CHECK(strcmp(err, expected) == 0);
  }

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

// A syntax error inside a partial names the partial's *file*, not the bare tag name.
// `(in '<file>')` must denote a file, and `bad` is not one. It is also ambiguous with a top-level
// template of the same name, which reaches the identical format string. The position is the
// partial's own, so line 2 here is line 2 of `partials/card.html`, not of the `index.html` that
// included it.
static void test_rejects_syntax_error_in_partial(void) {
  char dir[] = "/tmp/sosig-template-test.XXXXXX";
  const char* tmp = init_template_fixture(dir);
  if (tmp == NULL) {
    return;
  }
  TEST_CHECK(write_template_fixture(tmp, "index.html", "{{>card}}\n") == 0);
  TEST_CHECK(write_template_fixture(tmp, "partials/card.html",
                                    "first\n{{#content_entries}}{{title}}\n") == 0);

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry};
  const char* templates_dir = tmp;
  struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* rendered_html = NULL;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK((rendered_html = template_render_file(templates_dir, "index.html", &context, err,
                                                   sizeof(err))) == NULL);
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected), "%s at line %u, column %u (in '%s')",
                         "section-opening tag has no closer", 2U, 1U, "partials/card.html");
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  cleanup_template_fixture(tmp);
  content_entry_free(&entry);
  site_config_free(&site_config);
}

TEST_LIST = {
    {"renders escaped title and raw body", test_renders_escaped_title_and_raw_body},
    {"iterates content entries and tags", test_iterates_content_entries_and_tags},
    {"empty tag does not truncate tag list", test_empty_tag_does_not_truncate_tag_list},
    {"nested section reiterates content entries", test_nested_section_reiterates_content_entries},
    {"escapes double brace and leaves triple brace raw",
     test_escapes_double_brace_and_leaves_triple_brace_raw},
    {"renders partial", test_renders_partial},
    {"repeated partial renders same content each time",
     test_repeated_partial_renders_same_content_each_time},
    {"repeated partial stays under distinct limit",
     test_repeated_partial_stays_under_distinct_limit},
    {"renders atom feed", test_renders_atom_feed},
    {"empty template yields empty string", test_empty_template_yields_empty_string},
    {"unknown variable renders empty", test_unknown_variable_renders_empty},
    {"inverted section renders when list empty", test_inverted_section_renders_when_list_empty},
    {"empty field is falsey as section", test_empty_field_is_falsey_as_section},
    {"comments are dropped", test_comments_are_dropped},
    {"rejects unsafe template name", test_rejects_unsafe_template_name},
    {"rejects unsafe partial name", test_rejects_unsafe_partial_name},
    {"rejects oversize partial name", test_rejects_oversize_partial_name},
    {"rejects unreadable partial", test_rejects_unreadable_partial},
    {"rejects partial count past limit", test_rejects_partial_count_past_limit},
    {"rejects recursive partial", test_rejects_recursive_partial},
    {"rejects mutual partial cycle", test_rejects_mutual_partial_cycle},
    {"rejects oversize render output", test_rejects_oversize_render_output},
    {"rejects unclosed section", test_rejects_unclosed_section},
    {"rejects section name mismatch", test_rejects_section_name_mismatch},
    {"rejects malformed tags", test_rejects_malformed_tags},
    {"rejects syntax error in partial", test_rejects_syntax_error_in_partial},
    {NULL, NULL}};
