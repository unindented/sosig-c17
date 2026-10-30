#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "formats/org.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

// Common Org constructs render to their expected HTML elements, and a top-level headline is an
// `<h1>`, as Markdown's `#` is. The golden test compares a whole Org note end to end with
// `tests/expected/site_file_permalink/org-document.html`, including its top property drawer and
// `* Footnotes` headline, which render nothing. This checks the module boundary: `org_to_html`
// leaves `ORG_HTML_FLAG_TOPLEVEL_H2` off.
static void test_renders_basic_html(void) {
  struct Arena perm = test_arena();
  const struct Str html = org_to_html(S("* Title\n/em/ and =code=\n"), &perm);

  TEST_CHECK(str_eq(html, S("<h1>Title</h1>\n<p><i>em</i> and <code>code</code></p>\n")));
}

// Braced sub/superscripts render, and an unbraced one such as an identifier with an underscore
// stays text, which is `ORG_FLAG_SUBSUPERSCRIPTS_BRACED`, Org's `^:{}`.
static void test_renders_braced_subscripts_only(void) {
  struct Arena perm = test_arena();
  const struct Str html = org_to_html(S("a_{b} and a_b\n"), &perm);

  TEST_CHECK(str_eq(html, S("<p>a<sub>b</sub> and a_b</p>\n")));
}

// A file's `#+OPTIONS:` line overrides the defaults, which is `ORG_HTML_FLAG_IN_BUFFER_OPTIONS`:
// `^:nil` turns the braced subscripts off.
static void test_honors_in_buffer_options(void) {
  struct Arena perm = test_arena();
  const struct Str html = org_to_html(S("#+OPTIONS: ^:nil\na_{b}\n"), &perm);

  TEST_CHECK(str_eq(html, S("<p>a_{b}</p>\n")));
}

// An HTML export block passes through verbatim. `org_to_html` deliberately omits
// `ORG_FLAG_NOEXPORTBLOCKS` because the Org is first-party and the content template emits the body
// unescaped as `{{{body}}}`. This pins the same trust decision as Markdown's raw HTML.
static void test_passes_through_export_blocks(void) {
  struct Arena perm = test_arena();
  const struct Str html = org_to_html(
      S("#+BEGIN_EXPORT html\n<div class=\"note\">raw <b>x</b></div>\n#+END_EXPORT\n"), &perm);

  TEST_CHECK(str_eq(html, S("<div class=\"note\">raw <b>x</b></div>\n")));
}

// A body beginning with a UTF-8 BOM renders without it, which is `ORG_HTML_FLAG_SKIP_UTF8_BOM`. An
// Org source is converted whole, so a BOM at the start of the file reaches the converter.
static void test_skips_leading_byte_order_mark(void) {
  struct Arena perm = test_arena();
  const struct Str html = org_to_html(S("\xef\xbb\xbfhello\n"), &perm);

  TEST_CHECK(str_eq(html, S("<p>hello</p>\n")));
}

// Empty input succeeds with empty HTML.
static void test_empty_input_yields_empty_string(void) {
  struct Arena perm = test_arena();
  const struct Str html = org_to_html(S(""), &perm);

  TEST_CHECK(html.len == 0);
}

// The HTML grows in place at the start of `perm`'s free space, so a body that outgrows many
// capacities is never copied: it starts where the free space did. org4c hands the output over in
// small chunks, so two thousand paragraphs force many growths.
static void test_html_grows_in_place_in_perm(void) {
  enum { PARAGRAPH_COUNT = 2000, PARAGRAPH_LEN = 4 };
  struct Arena scratch = test_arena();
  char* org = arena_new(&scratch, PARAGRAPH_COUNT * PARAGRAPH_LEN, char);
  for (ptrdiff_t i = 0; i < PARAGRAPH_COUNT; i++) {
    memcpy(org + i * PARAGRAPH_LEN, "ab\n\n", PARAGRAPH_LEN);
  }

  struct Arena perm = test_arena();
  // The first allocation lands where an empty one from a copy of the arena does, past any gap the
  // arena keeps between allocations.
  struct Arena probe = perm;
  char* const start = arena_new(&probe, 0, char);
  const struct Str html = org_to_html((struct Str){org, PARAGRAPH_COUNT * PARAGRAPH_LEN}, &perm);
  TEST_CHECK(html.data == start);
  TEST_CHECK(html.len == PARAGRAPH_COUNT * S("<p>ab</p>\n").len);
}

TEST_LIST = {
    {"renders basic html", test_renders_basic_html},
    {"renders braced subscripts only", test_renders_braced_subscripts_only},
    {"honors in-buffer options", test_honors_in_buffer_options},
    {"passes through export blocks", test_passes_through_export_blocks},
    {"skips leading byte order mark", test_skips_leading_byte_order_mark},
    {"empty input yields empty string", test_empty_input_yields_empty_string},
    {"html grows in place in perm", test_html_grows_in_place_in_perm},
    {NULL, NULL},
};
