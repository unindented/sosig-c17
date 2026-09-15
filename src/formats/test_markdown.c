#include <acutest.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "formats/markdown.h"

// Common Markdown constructs render to their expected HTML elements.
static void test_renders_basic_html(void) {
  const char markdown[] = "# Title\n\n*em* and `code`\n";
  char* html = NULL;
  TEST_ASSERT((html = markdown_to_html(markdown, sizeof(markdown) - 1, NULL, 0)) != NULL);

  TEST_CHECK(strstr(html, "<h1>Title</h1>") != NULL);
  TEST_CHECK(strstr(html, "<em>em</em>") != NULL);
  TEST_CHECK(strstr(html, "<code>code</code>") != NULL);

  free(html);
}

// The enabled extension set renders GitHub-style tables. The golden test compares the rendered
// table end to end with `tests/expected/site-file-permalink/table.html`. This checks the module
// boundary: `markdown_to_html` enables the extension and returns an owned string.
static void test_renders_github_tables(void) {
  const char markdown[] = "| a | b |\n|---|---|\n| 1 | 2 |\n";
  char* html = NULL;
  TEST_ASSERT((html = markdown_to_html(markdown, sizeof(markdown) - 1, NULL, 0)) != NULL);

  TEST_CHECK(strstr(html, "<table>") != NULL);

  free(html);
}

// Raw HTML in a body passes through verbatim. `markdown_to_html` deliberately omits
// `MD_FLAG_NOHTML` because the Markdown is first-party and the content template emits the body
// unescaped as `{{{body}}}`. This pins a trust decision rather than an incidental default: enabling
// that flag would turn every author-written HTML block into visible escaped source text on the
// page.
static void test_passes_through_raw_html(void) {
  const char markdown[] = "<div class=\"note\">raw <b>x</b></div>\n";
  char* html = NULL;
  TEST_ASSERT((html = markdown_to_html(markdown, sizeof(markdown) - 1, NULL, 0)) != NULL);

  TEST_CHECK(strcmp(html, "<div class=\"note\">raw <b>x</b></div>\n") == 0);

  free(html);
}

// Runs of whitespace inside a paragraph collapse to one space, which is
// `MD_FLAG_COLLAPSEWHITESPACE` and affects the text content of every generated page. No fixture
// body has a whitespace run, so the golden test suite cannot detect this flag.
static void test_collapses_whitespace_runs(void) {
  const char markdown[] = "a    b   c\n";
  char* html = NULL;
  TEST_ASSERT((html = markdown_to_html(markdown, sizeof(markdown) - 1, NULL, 0)) != NULL);

  TEST_CHECK(strcmp(html, "<p>a b c</p>\n") == 0);

  free(html);
}

// A body beginning with a UTF-8 BOM renders without it, which is `MD_HTML_FLAG_SKIP_UTF8_BOM`. This
// is a reachable production case rather than a theoretical one. `frontmatter_split` strips only the
// file's leading BOM, so an author who writes one immediately after the closing fence hands md4c a
// body slice that still starts with it. Without the flag, a stray U+FEFF lands in the page text.
static void test_skips_leading_byte_order_mark(void) {
  const char markdown[] = "\xef\xbb\xbfhello\n";
  char* html = NULL;
  TEST_ASSERT((html = markdown_to_html(markdown, sizeof(markdown) - 1, NULL, 0)) != NULL);

  TEST_CHECK(strcmp(html, "<p>hello</p>\n") == 0);

  free(html);
}

// Empty input yields a non-`NULL`, empty HTML string rather than `NULL`.
static void test_empty_input_yields_empty_string(void) {
  char* html = NULL;
  TEST_ASSERT((html = markdown_to_html("", 0, NULL, 0)) != NULL);
  TEST_CHECK(html[0] == '\0');

  free(html);
}

// `markdown_to_html` rejects a source longer than `MARKDOWN_INPUT_LEN_MAX` rather than handing it
// to md4c. The length is a lie about a two-byte literal, which is safe and allocates nothing,
// because the check returns before the function reads `markdown`. The expected diagnostic derives
// the limit from the same constant the function enforces.
static void test_rejects_oversize_input(void) {
  char err[ERROR_MESSAGE_SIZE] = "untouched";
  TEST_CHECK(markdown_to_html("x", (size_t)MARKDOWN_INPUT_LEN_MAX + 1u, err, sizeof(err)) == NULL);

  char expected[ERROR_MESSAGE_SIZE];
  const int n =
      snprintf(expected, sizeof(expected),
               "Markdown input exceeds max Markdown input length (%zu bytes) at %zu bytes",
               (size_t)MARKDOWN_INPUT_LEN_MAX, (size_t)MARKDOWN_INPUT_LEN_MAX + 1u);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);

  TEST_CHECK(markdown_to_html("x", SIZE_MAX, NULL, 0) == NULL);
}

TEST_LIST = {
    {"renders basic html", test_renders_basic_html},
    {"renders github tables", test_renders_github_tables},
    {"passes through raw html", test_passes_through_raw_html},
    {"collapses whitespace runs", test_collapses_whitespace_runs},
    {"skips leading byte order mark", test_skips_leading_byte_order_mark},
    {"empty input yields empty string", test_empty_input_yields_empty_string},
    {"rejects oversize input", test_rejects_oversize_input},
    {NULL, NULL},
};
