#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "formats/body_html.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

// A URL keeps its letters, digits and the punctuation a URL may hold as it is, so an ordinary link
// target is copied through unchanged.
static void test_url_escape_keeps_url_bytes(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_url_escape_append(&buf, S("https://example.com/a_b-c.d~e/f?g=h;i#j(k)*l!m,n+o$p@q%20"));
  TEST_CHECK(str_eq(buf_str(&buf), S("https://example.com/a_b-c.d~e/f?g=h;i#j(k)*l!m,n+o$p@q%20")));
}

// A heading's tags follow its level in the source, and the line end follows the closing tag.
static void test_heading_writes_level_tags(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_heading_open(&buf, 2);
  buf_append(&buf, S("Title"));
  body_html_heading_close(&buf, 2);
  TEST_CHECK(str_eq(buf_str(&buf), S("<h2>Title</h2>\n")));
}

// A footnote reference links to its footnote and carries the id its back-reference links to.
static void test_footnote_reference_links_both_ways(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_footnote_reference_write(&buf, 3, 2);
  TEST_CHECK(str_eq(buf_str(&buf), S("<sup><a href=\"#fn-3\" id=\"fnref-3-2\">3</a></sup>")));
}

// The footnote list is an ordered list in a `footnotes` section, and each footnote ends with one
// back-reference for each of its references.
static void test_footnotes_write_list_with_backrefs(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_footnotes_open(&buf);
  body_html_footnote_open(&buf, 1);
  buf_append(&buf, S("Note."));
  body_html_footnote_back_references_write(&buf, 1, 2);
  body_html_footnote_close(&buf);
  body_html_footnotes_close(&buf);
  TEST_CHECK(str_eq(buf_str(&buf),
                    S("<section class=\"footnotes\">\n<ol>\n<li id=\"fn-1\">\nNote."
                      "<a href=\"#fnref-1-1\" class=\"footnote-backref\">&#8617;</a> "
                      "<a href=\"#fnref-1-2\" class=\"footnote-backref\">&#8617;</a>\n</li>\n"
                      "</ol>\n</section>\n")));
}

// A checkbox is a disabled input, checked only for a done task.
static void test_checkbox_marks_done_tasks_checked(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_checkbox_write(&buf, true);
  body_html_checkbox_write(&buf, false);
  TEST_CHECK(str_eq(buf_str(&buf),
                    S("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>"
                      "<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>")));
}

// An admonition's type is both its class suffix and its title.
static void test_admonition_names_type_twice(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_admonition_open(&buf, S("warning"), S(""));
  buf_append(&buf, S("<p>Careful.</p>\n"));
  body_html_admonition_close(&buf);
  TEST_CHECK(
      str_eq(buf_str(&buf), S("<div class=\"admonition-warning\"><p class=\"admonition-title\">"
                              "warning</p><p>Careful.</p>\n</div>\n")));
}

// Inline and displayed math differ only in the displayed formula's `type` attribute.
static void test_math_marks_display_formulas(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_math_open(&buf, false);
  buf_append(&buf, S("a"));
  body_html_math_close(&buf);
  body_html_math_open(&buf, true);
  buf_append(&buf, S("b"));
  body_html_math_close(&buf);
  TEST_CHECK(str_eq(buf_str(&buf),
                    S("<x-equation>a</x-equation><x-equation type=\"display\">b</x-equation>")));
}

// A code block names its language in a `language-` class, and its code is escaped.
static void test_code_block_names_language(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_code_block_write(&buf, S("c"), S("a < b;\n"));
  TEST_CHECK(
      str_eq(buf_str(&buf), S("<pre><code class=\"language-c\">a &lt; b;\n</code></pre>\n")));
}

// A heading deeper than HTML's six levels stays at `<h6>`.
static void test_heading_caps_level_at_six(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_heading_open(&buf, 9);
  body_html_heading_close(&buf, 9);
  TEST_CHECK(str_eq(buf_str(&buf), S("<h6></h6>\n")));
}

// An admonition with an id carries it, so a link can point to it.
static void test_admonition_carries_id(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_admonition_open(&buf, S("note"), S("n1"));
  TEST_CHECK(
      str_eq(buf_str(&buf),
             S("<div class=\"admonition-note\" id=\"n1\"><p class=\"admonition-title\">note</p>")));
}

// A footnote nothing references has no back-reference.
static void test_footnote_without_references_has_no_backrefs(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_footnote_back_references_write(&buf, 1, 0);
  TEST_CHECK(buf.len == 0);
}

// A code block without a language has no class, and a language that already starts with `language-`
// does not get the prefix twice.
static void test_code_block_class_variants(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_code_block_write(&buf, S(""), S("x\n"));
  body_html_code_block_write(&buf, S("language-c"), S("y\n"));
  TEST_CHECK(str_eq(buf_str(&buf), S("<pre><code>x\n</code></pre>\n"
                                     "<pre><code class=\"language-c\">y\n</code></pre>\n")));
}

// `&` becomes a character reference, so the attribute stays valid HTML, and every other byte a URL
// may not hold becomes `%XX` in uppercase hexadecimal, a byte above 0x7F included.
static void test_url_escape_encodes_other_bytes(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_url_escape_append(&buf, S("a b&c\"d<e'f\xC3\xA9"));
  TEST_CHECK(str_eq(buf_str(&buf), S("a%20b&amp;c%22d%3Ce%27f%C3%A9")));
}

TEST_LIST = {
    {"url escape keeps url bytes", test_url_escape_keeps_url_bytes},
    {"heading writes level tags", test_heading_writes_level_tags},
    {"footnote reference links both ways", test_footnote_reference_links_both_ways},
    {"footnotes write list with backrefs", test_footnotes_write_list_with_backrefs},
    {"checkbox marks done tasks checked", test_checkbox_marks_done_tasks_checked},
    {"admonition names type twice", test_admonition_names_type_twice},
    {"math marks display formulas", test_math_marks_display_formulas},
    {"code block names language", test_code_block_names_language},
    {"heading caps level at six", test_heading_caps_level_at_six},
    {"admonition carries id", test_admonition_carries_id},
    {"footnote without references has no backrefs",
     test_footnote_without_references_has_no_backrefs},
    {"code block class variants", test_code_block_class_variants},
    {"url escape encodes other bytes", test_url_escape_encodes_other_bytes},
    {NULL, NULL},
};
