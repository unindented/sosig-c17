#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "formats/org.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Converts Org to HTML, in arenas of its own.
 *
 * @param org Terminated Org text. Must not be `NULL`.
 * @return The HTML, in an arena that lives until the test exits.
 */
static struct Str render_org(const char* org) {
  struct Arena perm = test_arena();
  return org_to_html(str_from_cstr(org), &perm, test_arena());
}

// Common Org constructs render to their expected HTML elements, and a top-level headline is an
// `<h2>`, as Markdown's `#` is. The golden test compares a whole Org note end to end with
// `tests/expected/site_file_permalink/org-document.html`, including its top property drawer and
// `* Footnotes` headline, which render nothing.
static void test_renders_basic_html(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("* Title\n/em/ and =code=\n"), &perm, scratch);

  TEST_CHECK(str_eq(html, S("<h2>Title</h2>\n<p><em>em</em> and <code>code</code></p>\n")));
}

// Bold and italics give the same HTML as in Markdown, which `test_markdown.c` checks with the same
// expected HTML.
static void test_renders_bold_and_italics(void) {
  TEST_CHECK(str_eq(render_org("*b* /i/\n"), S("<p><strong>b</strong> <em>i</em></p>\n")));
}

// A one-paragraph footnote with two references gives the same HTML as in Markdown, which
// `test_markdown.c` checks with the same expected HTML.
static void test_renders_footnote(void) {
  TEST_CHECK(str_eq(render_org("A[fn:1] B[fn:1].\n\n[fn:1] Note.\n"),
                    S("<p>A<sup><a href=\"#fn-1\" id=\"fnref-1-1\">1</a></sup> B<sup><a "
                      "href=\"#fn-1\" id=\"fnref-1-2\">1</a></sup>.</p>\n"
                      "<section class=\"footnotes\">\n<ol>\n<li id=\"fn-1\">\nNote."
                      "<a href=\"#fnref-1-1\" class=\"footnote-backref\">&#8617;</a> "
                      "<a href=\"#fnref-1-2\" "
                      "class=\"footnote-backref\">&#8617;</a>\n</li>\n</ol>\n</section>\n")));
}

// A done and an open task give the same HTML as in Markdown, which `test_markdown.c` checks with
// the same expected HTML.
static void test_renders_task_list_items(void) {
  TEST_CHECK(
      str_eq(render_org("- [X] a\n- [ ] b\n"),
             S("<ul>\n<li><input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled "
               "checked>a</li>\n"
               "<li><input type=\"checkbox\" "
               "class=\"task-list-item-checkbox\" disabled>b</li>\n</ul>\n")));
}

// A note gives the same HTML as in Markdown, which `test_markdown.c` checks with the same expected
// HTML.
static void test_renders_note_admonition(void) {
  TEST_CHECK(str_eq(render_org("#+begin_note\nBe careful.\n#+end_note\n"),
                    S("<div class=\"admonition-note\"><p class=\"admonition-title\">note</p><p>Be "
                      "careful.</p>\n</div>\n")));
}

// Inline and displayed math give the same HTML as in Markdown, which `test_markdown.c` checks with
// the same expected HTML.
static void test_renders_math(void) {
  TEST_CHECK(str_eq(
      render_org("\\(a\\) and \\[b\\]\n"),
      S("<p><x-equation>a</x-equation> and <x-equation type=\"display\">b</x-equation></p>\n")));
}

// A plain code block gives the same HTML as in Markdown, which `test_markdown.c` checks with the
// same expected HTML.
static void test_renders_code_block(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_SRC c\nint x;\n#+END_SRC\n"),
                    S("<pre><code class=\"language-c\">int x;\n</code></pre>\n")));
}

// Headings at the first two levels give the same HTML as in Markdown, which `test_markdown.c`
// checks with the same expected HTML.
static void test_renders_heading_levels(void) {
  TEST_CHECK(str_eq(render_org("* A\n** B\n"), S("<h2>A</h2>\n<h3>B</h3>\n")));
}

// Braced sub/superscripts render, and an unbraced one such as an identifier with an underscore
// stays text, which is `ORG_FLAG_SUBSUPERSCRIPTS_BRACED`, Org's `^:{}`.
static void test_renders_braced_subscripts_only(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("a_{b} and a_b\n"), &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>a<sub>b</sub> and a_b</p>\n")));
}

// A file's `#+OPTIONS:` line overrides the defaults: `^:nil` turns the braced subscripts off.
static void test_honors_in_buffer_options(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("#+OPTIONS: ^:nil\na_{b}\n"), &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>a_{b}</p>\n")));
}

// An HTML export block passes through verbatim. `org_to_html` deliberately omits
// `ORG_FLAG_NOEXPORTBLOCKS` because the Org is first-party and the content template emits the body
// unescaped as `{{{body}}}`. This pins the same trust decision as Markdown's raw HTML.
static void test_passes_through_export_blocks(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html =
      org_to_html(S("#+BEGIN_EXPORT html\n<div class=\"note\">raw <b>x</b></div>\n#+END_EXPORT\n"),
                  &perm, scratch);

  TEST_CHECK(str_eq(html, S("<div class=\"note\">raw <b>x</b></div>\n")));
}

// A footnote of several paragraphs keeps each one, and its back-references go inside the last, so
// they still follow its text.
static void test_renders_footnote_of_several_paragraphs(void) {
  TEST_CHECK(str_eq(render_org("A[fn:1].\n\n[fn:1] One.\n\nTwo.\n"),
                    S("<p>A<sup><a href=\"#fn-1\" id=\"fnref-1-1\">1</a></sup>.</p>\n"
                      "<section class=\"footnotes\">\n<ol>\n<li id=\"fn-1\">\n"
                      "<p>One.</p>\n"
                      "<p>Two.<a href=\"#fnref-1-1\" class=\"footnote-backref\">&#8617;</a></p>\n"
                      "</li>\n</ol>\n</section>\n")));
}

// A special block whose name is not an admonition type keeps its name as its class, as in Emacs.
static void test_keeps_other_special_blocks(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_aside\nA\n#+END_aside\n"),
                    S("<div class=\"aside\">\n<p>A</p>\n</div>\n")));
}

// A source block with a code reference keeps Org's own markup for it, which the shared code block
// has no place for.
static void test_keeps_code_references(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_SRC c\nint y; (ref:here)\n#+END_SRC\n"),
                    S("<pre><code class=\"language-c\"><span id=\"coderef-here\" "
                      "class=\"coderef-off\">int y; (here)</span>\n</code></pre>\n")));
}

// A special block names an admonition whatever its case, as Emacs reads a block's name.
static void test_matches_admonition_names_in_any_case(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_WARNING\nHot.\n#+END_WARNING\n"),
                    S("<div class=\"admonition-warning\"><p class=\"admonition-title\">warning</p>"
                      "<p>Hot.</p>\n</div>\n")));
}

// An admonition keeps the id its `#+NAME:` gives it, so a link to the name reaches it.
static void test_keeps_admonition_names(void) {
  TEST_CHECK(
      str_eq(render_org("#+NAME: n1\n#+begin_note\nA.\n#+end_note\nSee [[n1]].\n"),
             S("<div class=\"admonition-note\" id=\"n1\"><p class=\"admonition-title\">note</p>"
               "<p>A.</p>\n</div>\n<p>See <a href=\"#n1\">n1</a>.</p>\n")));
}

// Org's dollar delimiters are math too: `$...$` inline and `$$...$$` displayed.
static void test_renders_dollar_math(void) {
  TEST_CHECK(str_eq(
      render_org("$a$ and $$b$$\n"),
      S("<p><x-equation>a</x-equation> and <x-equation type=\"display\">b</x-equation></p>\n")));
}

// A body beginning with a UTF-8 BOM renders without it. An Org source is converted whole, so a BOM
// at the start of the file reaches the converter.
static void test_skips_leading_byte_order_mark(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("\xef\xbb\xbfhello\n"), &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>hello</p>\n")));
}

// Empty input succeeds with empty HTML.
static void test_empty_input_yields_empty_string(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S(""), &perm, scratch);

  TEST_CHECK(html.len == 0);
}

// The HTML grows in place at the start of `perm`'s free space, so a body that outgrows many
// capacities is never copied: it starts where the free space did. The renderer writes the output in
// small pieces, so two thousand paragraphs force many growths.
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
  const struct Str html =
      org_to_html((struct Str){org, PARAGRAPH_COUNT * PARAGRAPH_LEN}, &perm, scratch);
  TEST_CHECK(html.data == start);
  TEST_CHECK(html.len == PARAGRAPH_COUNT * S("<p>ab</p>\n").len);
}

TEST_LIST = {
    {"renders basic html", test_renders_basic_html},
    {"renders bold and italics", test_renders_bold_and_italics},
    {"renders footnote", test_renders_footnote},
    {"renders task list items", test_renders_task_list_items},
    {"renders note admonition", test_renders_note_admonition},
    {"renders math", test_renders_math},
    {"renders code block", test_renders_code_block},
    {"renders heading levels", test_renders_heading_levels},
    {"renders braced subscripts only", test_renders_braced_subscripts_only},
    {"honors in-buffer options", test_honors_in_buffer_options},
    {"passes through export blocks", test_passes_through_export_blocks},
    {"renders footnote of several paragraphs", test_renders_footnote_of_several_paragraphs},
    {"keeps other special blocks", test_keeps_other_special_blocks},
    {"keeps code references", test_keeps_code_references},
    {"matches admonition names in any case", test_matches_admonition_names_in_any_case},
    {"keeps admonition names", test_keeps_admonition_names},
    {"renders dollar math", test_renders_dollar_math},
    {"skips leading byte order mark", test_skips_leading_byte_order_mark},
    {"empty input yields empty string", test_empty_input_yields_empty_string},
    {"html grows in place in perm", test_html_grows_in_place_in_perm},
    {NULL, NULL},
};
