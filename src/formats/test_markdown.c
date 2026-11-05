#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "formats/body_html.h"
#include "formats/markdown.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

/** What `write_test_link_target` does with each target. */
enum TestLinkMode {
  /** Declines the target, so the renderer writes it. */
  TEST_LINK_DECLINE,

  /** Writes `u.html` for the target. */
  TEST_LINK_WRITE,

  /** Appends a part of a URL, then fails. */
  TEST_LINK_FAIL,
};

/** State of `write_test_link_target`. */
struct TestLinkTargets {
  /** What the callback does with each target. */
  enum TestLinkMode mode;

  /** Every call so far, as `type|target|link;` or `type|target|image;`. */
  struct Buf calls;
};

/**
 * @brief Converts Markdown to HTML, in arenas of its own.
 *
 * @param markdown Terminated Markdown text. Must not be `NULL`.
 * @return The HTML, in an arena that lives until the test exits.
 */
static struct Str render_markdown(const char* markdown) {
  struct Arena perm = test_arena();
  return markdown_to_html(str_from_cstr(markdown), NULL, &perm, test_arena());
}

/**
 * @brief Records a target, then writes a URL for it, declines it, or fails, as `mode` says. This is
 *        a `LinkTargetWriteFn`.
 *
 * @param buffer         Buffer the HTML grows in. Must not be `NULL`.
 * @param type           Link type the renderer gives.
 * @param target         Target as written.
 * @param is_image       Whether the target is an image's.
 * @param is_written_out Receives whether a URL was written. Must not be `NULL`.
 * @param userdata       `struct TestLinkTargets*`. Must not be `NULL`.
 * @return `-1` in `TEST_LINK_FAIL` mode, or `0`.
 */
static int write_test_link_target(struct Buf* buffer,
                                  struct Str type,
                                  struct Str target,
                                  bool is_image,
                                  bool* is_written_out,
                                  void* userdata) {
  struct TestLinkTargets* targets = userdata;
  buf_append(&targets->calls, type);
  buf_append(&targets->calls, S("|"));
  buf_append(&targets->calls, target);
  buf_append(&targets->calls, is_image ? S("|image;") : S("|link;"));
  *is_written_out = targets->mode == TEST_LINK_WRITE;
  switch (targets->mode) {
    case TEST_LINK_WRITE:
      buf_append(buffer, S("u.html"));
      return 0;
    case TEST_LINK_FAIL:
      buf_append(buffer, S("partial"));
      return -1;
    default:
      return 0;
  }
}

/**
 * @brief Converts Markdown to HTML with `write_test_link_target` as the link callback, in arenas of
 *        its own.
 *
 * @param markdown  Terminated Markdown text. Must not be `NULL`.
 * @param mode      What the callback does with each target.
 * @param calls_out Receives the calls the callback recorded. Must not be `NULL`.
 * @return The HTML, in an arena that lives until the test exits.
 */
static struct Str render_markdown_linked(const char* markdown,
                                         enum TestLinkMode mode,
                                         struct Str* calls_out) {
  struct Arena perm = test_arena();
  struct Arena calls_arena = test_arena();
  struct TestLinkTargets targets = {.mode = mode, .calls = {.arena = &calls_arena}};
  const struct LinkTargets link_targets = {.write = write_test_link_target, .userdata = &targets};
  const struct Str html =
      markdown_to_html(str_from_cstr(markdown), &link_targets, &perm, test_arena());
  *calls_out = buf_str(&targets.calls);
  return html;
}

// Common Markdown constructs render to their expected HTML elements.
static void test_renders_basic_html(void) {
  const struct Str markdown = S("# Title\n\n*em* and `code`\n");
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<h2>Title</h2>\n<p><em>em</em> and <code>code</code></p>\n")));
}

// Bold and italics give the same HTML as in Org, which `test_org.c` checks with the same expected
// HTML.
static void test_renders_bold_and_italics(void) {
  TEST_CHECK(str_eq(render_markdown("**b** *i*\n"), S("<p><strong>b</strong> <em>i</em></p>\n")));
}

// A one-paragraph footnote with two references gives the same HTML as in Org, which `test_org.c`
// checks with the same expected HTML.
static void test_renders_footnote(void) {
  TEST_CHECK(str_eq(
      render_markdown("A[^1] B[^1].\n\n[^1]: Note.\n"),
      S("<p>A<sup><a href=\"#fn-1\" id=\"fnref-1-1\" role=\"doc-noteref\">1</a></sup> B<sup><a "
        "href=\"#fn-1\" id=\"fnref-1-2\" role=\"doc-noteref\">1</a></sup>.</p>\n"
        "<section class=\"footnotes\" role=\"doc-endnotes\">\n<ol>\n<li id=\"fn-1\">\nNote."
        "<a href=\"#fnref-1-1\" class=\"footnote-backref\" role=\"doc-backlink\">&#8617;</a> "
        "<a href=\"#fnref-1-2\" "
        "class=\"footnote-backref\" "
        "role=\"doc-backlink\">&#8617;</a>\n</li>\n</ol>\n</section>\n")));
}

// A done and an open task give the same HTML as in Org, which `test_org.c` checks with the same
// expected HTML.
static void test_renders_task_list_items(void) {
  TEST_CHECK(
      str_eq(render_markdown("- [x] a\n- [ ] b\n"),
             S("<ul>\n<li><input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled "
               "checked>a</li>\n"
               "<li><input type=\"checkbox\" "
               "class=\"task-list-item-checkbox\" disabled>b</li>\n</ul>\n")));
}

// A note gives the same HTML as in Org, which `test_org.c` checks with the same expected HTML.
static void test_renders_note_admonition(void) {
  TEST_CHECK(str_eq(render_markdown("> [!NOTE]\n> Be careful.\n"),
                    S("<div class=\"admonition-note\"><p class=\"admonition-title\">note</p><p>Be "
                      "careful.</p>\n</div>\n")));
}

// Inline and displayed math give the same HTML as in Org, which `test_org.c` checks with the same
// expected HTML.
static void test_renders_math(void) {
  TEST_CHECK(str_eq(
      render_markdown("$a$ and $$b$$\n"),
      S("<p><x-equation>a</x-equation> and <x-equation type=\"display\">b</x-equation></p>\n")));
}

// A code block gives the same highlighted HTML as in Org, which `test_org.c` checks with the same
// expected HTML.
static void test_renders_code_block(void) {
  TEST_CHECK(str_eq(render_markdown("```c\nint x;\n```\n"),
                    S("<pre><code class=\"language-c\"><span class=\"tok-type\">int</span> <span "
                      "class=\"tok-variable\">x</span><span "
                      "class=\"tok-delimiter\">;</span>\n</code></pre>\n")));
}

// Headings at the first two levels give the same HTML as in Org, which `test_org.c` checks with the
// same expected HTML.
static void test_renders_heading_levels(void) {
  TEST_CHECK(str_eq(render_markdown("# A\n\n## B\n"), S("<h2>A</h2>\n<h3>B</h3>\n")));
}

// Entities give the same HTML as in Org, which `test_org.c` checks with the same expected HTML.
static void test_renders_entities(void) {
  TEST_CHECK(str_eq(render_markdown("&alpha; and &beta;\n"), S("<p>&alpha; and &beta;</p>\n")));
}

// The enabled extension set renders GitHub-style tables. The golden test compares the rendered
// table end to end with `tests/expected/site_file_permalink/table.html`. This checks the module
// boundary: `markdown_to_html` enables the extension.
static void test_renders_github_tables(void) {
  const struct Str markdown = S("| a | b |\n|---|---|\n| 1 | 2 |\n");
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n"
                            "<tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n</tr>\n</tbody>\n</table>\n")));
}

// Raw HTML in a body passes through verbatim. `markdown_to_html` deliberately omits
// `MD_FLAG_NOHTML` because the Markdown is first-party and the content template emits the body
// unescaped as `{{{body}}}`. This pins a trust decision rather than an incidental default: enabling
// that flag would turn every author-written HTML block into visible escaped source text on the
// page.
static void test_passes_through_raw_html(void) {
  const struct Str markdown = S("<div class=\"note\">raw <b>x</b></div>\n");
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<div class=\"note\">raw <b>x</b></div>\n")));
}

// Runs of whitespace inside a paragraph collapse to one space, which is
// `MD_FLAG_COLLAPSEWHITESPACE` and affects the text content of every generated page. No fixture
// body has a whitespace run, so the golden test suite cannot detect this flag.
static void test_collapses_whitespace_runs(void) {
  const struct Str markdown = S("a    b   c\n");
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>a b c</p>\n")));
}

// Every link's and image's destination reaches the callback as written, with an empty type, once,
// and a declined destination renders as it does without a callback.
static void test_gives_every_target_to_the_callback(void) {
  const char markdown[] = "[a](b.md#x \"t\") <denote:1> ![i](p.png) [w](https://e.com)\n";
  struct Str calls = {0};
  TEST_CHECK(str_eq(render_markdown_linked(markdown, TEST_LINK_DECLINE, &calls),
                    render_markdown(markdown)));
  TEST_CHECK(str_eq(calls, S("|b.md#x|link;|denote:1|link;|p.png|image;|https://e.com|link;")));
}

// A URL the callback writes is the link's `href`, before its title, and an autolink keeps its text.
static void test_writes_the_callback_url(void) {
  struct Str calls = {0};
  TEST_CHECK(str_eq(render_markdown_linked("[a](b.md \"t\") <denote:1>\n", TEST_LINK_WRITE, &calls),
                    S("<p><a href=\"u.html\" title=\"t\">a</a> <a "
                      "href=\"u.html\">denote:1</a></p>\n")));
}

// A body beginning with a UTF-8 BOM renders without it. This is a reachable production case rather
// than a theoretical one. `frontmatter_split` strips only the file's leading BOM, so an author who
// writes one immediately after the closing fence hands `markdown_to_html` a body slice that still
// starts with it. Without the skip, a stray U+FEFF lands in the page text.
static void test_skips_leading_byte_order_mark(void) {
  const struct Str markdown = S("\xef\xbb\xbfhello\n");
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  struct Str html = {0};
  html = markdown_to_html(markdown, NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>hello</p>\n")));
}

// Empty input succeeds with empty HTML.
static void test_empty_input_yields_empty_string(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  struct Str html = S("untouched");
  html = markdown_to_html(S(""), NULL, &perm, scratch);
  TEST_CHECK(html.len == 0);
}

// The HTML grows in place at the start of `perm`'s free space, so a body that outgrows many
// capacities is never copied: it starts where the free space did. The renderer writes the output in
// small pieces, so two thousand paragraphs force many growths.
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
  html = markdown_to_html((struct Str){markdown, PARAGRAPH_COUNT * PARAGRAPH_LEN}, NULL, &perm,
                          scratch);
  TEST_CHECK(html.data == start);
  TEST_CHECK(html.len == PARAGRAPH_COUNT * S("<p>ab</p>\n").len);
}

// A destination the callback fails on renders as it does without a callback, without the part of a
// URL the callback appended.
static void test_writes_a_failed_target_as_without_a_callback(void) {
  const char markdown[] = "[a](b.md) ![i](p.png)\n";
  struct Str calls = {0};
  TEST_CHECK(
      str_eq(render_markdown_linked(markdown, TEST_LINK_FAIL, &calls), render_markdown(markdown)));
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
    {"renders entities", test_renders_entities},
    {"renders github tables", test_renders_github_tables},
    {"passes through raw html", test_passes_through_raw_html},
    {"collapses whitespace runs", test_collapses_whitespace_runs},
    {"gives every target to the callback", test_gives_every_target_to_the_callback},
    {"writes the callback url", test_writes_the_callback_url},
    {"skips leading byte order mark", test_skips_leading_byte_order_mark},
    {"empty input yields empty string", test_empty_input_yields_empty_string},
    {"html grows in place in perm", test_html_grows_in_place_in_perm},
    {"writes a failed target as without a callback",
     test_writes_a_failed_target_as_without_a_callback},
    {NULL, NULL},
};
