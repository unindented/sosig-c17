#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "formats/markdown.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

// Common Markdown constructs render to their expected HTML elements.
static void test_renders_basic_html(void) {
  const struct Str markdown = S("# Title\n\n*em* and `code`\n");
  struct Arena perm = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, &perm);

  TEST_CHECK(strstr(str_cstr(html, &perm), "<h1>Title</h1>") != NULL);
  TEST_CHECK(strstr(str_cstr(html, &perm), "<em>em</em>") != NULL);
  TEST_CHECK(strstr(str_cstr(html, &perm), "<code>code</code>") != NULL);
}

// The enabled extension set renders GitHub-style tables. The golden test compares the rendered
// table end to end with `tests/expected/site_file_permalink/table.html`. This checks the module
// boundary: `markdown_to_html` enables the extension.
static void test_renders_github_tables(void) {
  const struct Str markdown = S("| a | b |\n|---|---|\n| 1 | 2 |\n");
  struct Arena perm = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, &perm);

  TEST_CHECK(strstr(str_cstr(html, &perm), "<table>") != NULL);
}

// Raw HTML in a body passes through verbatim. `markdown_to_html` deliberately omits
// `MD_FLAG_NOHTML` because the Markdown is first-party and the content template emits the body
// unescaped as `{{{body}}}`. This pins a trust decision rather than an incidental default: enabling
// that flag would turn every author-written HTML block into visible escaped source text on the
// page.
static void test_passes_through_raw_html(void) {
  const struct Str markdown = S("<div class=\"note\">raw <b>x</b></div>\n");
  struct Arena perm = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, &perm);

  TEST_CHECK(str_eq(html, S("<div class=\"note\">raw <b>x</b></div>\n")));
}

// Runs of whitespace inside a paragraph collapse to one space, which is
// `MD_FLAG_COLLAPSEWHITESPACE` and affects the text content of every generated page. No fixture
// body has a whitespace run, so the golden test suite cannot detect this flag.
static void test_collapses_whitespace_runs(void) {
  const struct Str markdown = S("a    b   c\n");
  struct Arena perm = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, &perm);

  TEST_CHECK(str_eq(html, S("<p>a b c</p>\n")));
}

// A body beginning with a UTF-8 BOM renders without it, which is `MD_HTML_FLAG_SKIP_UTF8_BOM`. This
// is a reachable production case rather than a theoretical one. `frontmatter_split` strips only the
// file's leading BOM, so an author who writes one immediately after the closing fence hands md4c a
// body slice that still starts with it. Without the flag, a stray U+FEFF lands in the page text.
static void test_skips_leading_byte_order_mark(void) {
  const struct Str markdown = S("\xef\xbb\xbfhello\n");
  struct Arena perm = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, &perm);

  TEST_CHECK(str_eq(html, S("<p>hello</p>\n")));
}

// Empty input succeeds with empty HTML.
static void test_empty_input_yields_empty_string(void) {
  struct Arena perm = test_arena();
  struct Str html = S("untouched");
  html = markdown_to_html(S(""), &perm);
  TEST_CHECK(html.len == 0);
}

// The HTML grows in place at the start of `perm`'s free space, so a body that outgrows many
// capacities is never copied: it starts where the free space did. md4c hands the output over in
// small chunks, so two thousand paragraphs force many growths.
static void test_html_grows_in_place_in_perm(void) {
  enum { PARAGRAPH_COUNT = 2000, PARAGRAPH_LEN = 4 };
  struct Arena scratch = test_arena();
  char* markdown = arena_new(&scratch, PARAGRAPH_COUNT * PARAGRAPH_LEN, char);
  for (ptrdiff_t i = 0; i < PARAGRAPH_COUNT; i++) {
    memcpy(markdown + i * PARAGRAPH_LEN, "ab\n\n", PARAGRAPH_LEN);
  }

  struct Arena perm = test_arena();
  // The first allocation lands where an empty one from a copy of the arena does, past any gap the
  // arena keeps between allocations.
  struct Arena probe = perm;
  char* const start = arena_new(&probe, 0, char);
  struct Str html = {0};
  html = markdown_to_html((struct Str){markdown, PARAGRAPH_COUNT * PARAGRAPH_LEN}, &perm);
  TEST_CHECK(html.data == start);
  TEST_CHECK(html.len == PARAGRAPH_COUNT * S("<p>ab</p>\n").len);
}

TEST_LIST = {
    {"renders basic html", test_renders_basic_html},
    {"renders github tables", test_renders_github_tables},
    {"passes through raw html", test_passes_through_raw_html},
    {"collapses whitespace runs", test_collapses_whitespace_runs},
    {"skips leading byte order mark", test_skips_leading_byte_order_mark},
    {"empty input yields empty string", test_empty_input_yields_empty_string},
    {"html grows in place in perm", test_html_grows_in_place_in_perm},
    {NULL, NULL},
};
