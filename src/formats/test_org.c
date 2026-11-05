#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "formats/body_html.h"
#include "formats/org.h"
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

/** A source block in C, which has a grammar, and in `zz`, which has none, with the HTML of each. */
struct SourceCase {
  const char* name;
  const char* org;
  const char* html;
  const char* org_without_grammar;
  const char* html_without_grammar;
};

/**
 * Each path a source block can take: named, captioned, with line numbers, with code references,
 * with both, with its label removed, and in a footnote. A caption holds emphasis, a link and an
 * inline source, which stay the caption's.
 */
static const struct SourceCase source_cases[] = {
    {"named",
     "#+NAME: n\n"
     "#+BEGIN_SRC c\n"
     "x\n"
     "#+END_SRC\n",
     "<pre id=\"n\"><code class=\"language-c\"><span class=\"tok-variable\">x</span>\n"
     "</code></pre>\n",
     "#+NAME: n\n"
     "#+BEGIN_SRC zz\n"
     "x\n"
     "#+END_SRC\n",
     "<pre id=\"n\"><code class=\"language-zz\">x\n"
     "</code></pre>\n"},
    {"captioned",
     "#+CAPTION: A /b/ [[https://x.org][c]] src_c{x}\n"
     "#+BEGIN_SRC c\n"
     "x\n"
     "#+END_SRC\n",
     "<label class=\"org-src-name\"><span class=\"listing-number\">Listing 1: </span>A <em>b</em> "
     "<a href=\"https://x.org\">c</a> <code class=\"src src-c\"><span "
     "class=\"tok-variable\">x</span></code></label><pre><code class=\"language-c\"><span "
     "class=\"tok-variable\">x</span>\n"
     "</code></pre>\n",
     "#+CAPTION: A /b/ [[https://x.org][c]] src_zz{x}\n"
     "#+BEGIN_SRC zz\n"
     "x\n"
     "#+END_SRC\n",
     "<label class=\"org-src-name\"><span class=\"listing-number\">Listing 1: </span>A <em>b</em> "
     "<a href=\"https://x.org\">c</a> <code class=\"src src-zz\">x</code></label><pre><code "
     "class=\"language-zz\">x\n"
     "</code></pre>\n"},
    {"numbered",
     "#+BEGIN_SRC c -n\n"
     "x\n"
     "\n"
     "x\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-c\"><span class=\"linenr\">1: </span><span "
     "class=\"tok-type\">x</span>\n"
     "<span class=\"linenr\">2: </span>\n"
     "<span class=\"linenr\">3: </span><span class=\"tok-variable\">x</span>\n"
     "</code></pre>\n",
     "#+BEGIN_SRC zz -n\n"
     "x\n"
     "\n"
     "x\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-zz\"><span class=\"linenr\">1: </span>x\n"
     "<span class=\"linenr\">2: </span>\n"
     "<span class=\"linenr\">3: </span>x\n"
     "</code></pre>\n"},
    {"coderef",
     "#+BEGIN_SRC c\n"
     "x (ref:r)\n"
     "x\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-c\"><span id=\"coderef-r\" class=\"coderef-off\"><span "
     "class=\"tok-type\">x</span> (r)</span>\n"
     "<span class=\"tok-variable\">x</span>\n"
     "</code></pre>\n",
     "#+BEGIN_SRC zz\n"
     "x (ref:r)\n"
     "x\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-zz\"><span id=\"coderef-r\" class=\"coderef-off\">x (r)</span>\n"
     "x\n"
     "</code></pre>\n"},
    {"numbered-coderef",
     "#+BEGIN_SRC c -n 9\n"
     "x\n"
     "x (ref:r)\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-c\"><span class=\"linenr\"> 9: </span><span "
     "class=\"tok-type\">x</span>\n"
     "<span id=\"coderef-r\" class=\"coderef-off\"><span class=\"linenr\">10: </span><span "
     "class=\"tok-variable\">x</span> (r)</span>\n"
     "</code></pre>\n",
     "#+BEGIN_SRC zz -n 9\n"
     "x\n"
     "x (ref:r)\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-zz\"><span class=\"linenr\"> 9: </span>x\n"
     "<span id=\"coderef-r\" class=\"coderef-off\"><span class=\"linenr\">10: </span>x (r)</span>\n"
     "</code></pre>\n"},
    {"label-removed",
     "#+BEGIN_SRC c -r\n"
     "x (ref:r)\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-c\"><span id=\"coderef-r\" class=\"coderef-off\"><span "
     "class=\"tok-variable\">x</span></span>\n"
     "</code></pre>\n",
     "#+BEGIN_SRC zz -r\n"
     "x (ref:r)\n"
     "#+END_SRC\n",
     "<pre><code class=\"language-zz\"><span id=\"coderef-r\" class=\"coderef-off\">x</span>\n"
     "</code></pre>\n"},
    {"footnote",
     "A.[fn:1]\n"
     "\n"
     "[fn:1] B\n"
     "#+BEGIN_SRC c\n"
     "x\n"
     "#+END_SRC\n",
     "<p>A.<sup><a href=\"#fn-1\" id=\"fnref-1-1\" role=\"doc-noteref\">1</a></sup></p>\n"
     "<section class=\"footnotes\" role=\"doc-endnotes\">\n"
     "<ol>\n"
     "<li id=\"fn-1\">\n"
     "<p>B</p>\n"
     "<pre><code class=\"language-c\"><span class=\"tok-variable\">x</span>\n"
     "</code></pre>\n"
     "<a href=\"#fnref-1-1\" class=\"footnote-backref\" role=\"doc-backlink\">&#8617;</a>\n"
     "</li>\n"
     "</ol>\n"
     "</section>\n",
     "A.[fn:1]\n"
     "\n"
     "[fn:1] B\n"
     "#+BEGIN_SRC zz\n"
     "x\n"
     "#+END_SRC\n",
     "<p>A.<sup><a href=\"#fn-1\" id=\"fnref-1-1\" role=\"doc-noteref\">1</a></sup></p>\n"
     "<section class=\"footnotes\" role=\"doc-endnotes\">\n"
     "<ol>\n"
     "<li id=\"fn-1\">\n"
     "<p>B</p>\n"
     "<pre><code class=\"language-zz\">x\n"
     "</code></pre>\n"
     "<a href=\"#fnref-1-1\" class=\"footnote-backref\" role=\"doc-backlink\">&#8617;</a>\n"
     "</li>\n"
     "</ol>\n"
     "</section>\n"},
};

/**
 * @brief Converts Org to HTML, in arenas of its own.
 *
 * @param org Terminated Org text. Must not be `NULL`.
 * @return The HTML, in an arena that lives until the test exits.
 */
static struct Str render_org(const char* org) {
  struct Arena perm = test_arena();
  return org_to_html(str_from_cstr(org), NULL, &perm, test_arena());
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
 * @brief Converts Org to HTML with `write_test_link_target` as the link callback, in arenas of its
 *        own.
 *
 * @param org       Terminated Org text. Must not be `NULL`.
 * @param mode      What the callback does with each target.
 * @param calls_out Receives the calls the callback recorded. Must not be `NULL`.
 * @return The HTML, in an arena that lives until the test exits.
 */
static struct Str render_org_linked(const char* org,
                                    enum TestLinkMode mode,
                                    struct Str* calls_out) {
  struct Arena perm = test_arena();
  struct Arena calls_arena = test_arena();
  struct TestLinkTargets targets = {.mode = mode, .calls = {.arena = &calls_arena}};
  const struct LinkTargets link_targets = {.write = write_test_link_target, .userdata = &targets};
  const struct Str html = org_to_html(str_from_cstr(org), &link_targets, &perm, test_arena());
  *calls_out = buf_str(&targets.calls);
  return html;
}

// Common Org constructs render to their expected HTML elements, and a top-level headline is an
// `<h2>`, as Markdown's `#` is. The golden test compares a whole Org note end to end with
// `tests/expected/site_file_permalink/org-document.html`, including its top property drawer and
// `* Footnotes` headline, which render nothing.
static void test_renders_basic_html(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("* Title\n/em/ and =code=\n"), NULL, &perm, scratch);

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
  TEST_CHECK(str_eq(
      render_org("A[fn:1] B[fn:1].\n\n[fn:1] Note.\n"),
      S("<p>A<sup><a href=\"#fn-1\" id=\"fnref-1-1\" role=\"doc-noteref\">1</a></sup> B<sup><a "
        "href=\"#fn-1\" id=\"fnref-1-2\" role=\"doc-noteref\">1</a></sup>.</p>\n"
        "<section class=\"footnotes\" role=\"doc-endnotes\">\n<ol>\n<li id=\"fn-1\">\nNote."
        "<a href=\"#fnref-1-1\" class=\"footnote-backref\" role=\"doc-backlink\">&#8617;</a> "
        "<a href=\"#fnref-1-2\" "
        "class=\"footnote-backref\" "
        "role=\"doc-backlink\">&#8617;</a>\n</li>\n</ol>\n</section>\n")));
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

// A source block gives the same highlighted HTML as in Markdown, which `test_markdown.c` checks
// with the same expected HTML.
static void test_renders_code_block(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_SRC c\nint x;\n#+END_SRC\n"),
                    S("<pre><code class=\"language-c\"><span class=\"tok-type\">int</span> <span "
                      "class=\"tok-variable\">x</span><span "
                      "class=\"tok-delimiter\">;</span>\n</code></pre>\n")));
}

// Every path of a source block highlights its code as Emacs does, with its line numbers and code
// references around the highlighted lines.
static void test_highlights_every_source_block_path(void) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(source_cases) / sizeof(source_cases[0])); i++) {
    TEST_CASE(source_cases[i].name);
    TEST_CHECK(str_eq(render_org(source_cases[i].org), str_from_cstr(source_cases[i].html)));
  }
}

// An inline source is highlighted, and its `:exports` leaves it out when it says `none` or
// `results`, as Emacs and a source block do. It keeps it for `code`, and for a value in another
// case, such as `NONE`, which Emacs reads as no value it knows.
static void test_follows_inline_source_exports(void) {
  TEST_CHECK(
      str_eq(render_org("A src_c[:exports none]{x} src_c[:exports results]{y} B "
                        "src_c[:exports code]{z} C src_c[:exports NONE]{w} D\n"),
             S("<p>A   B <code class=\"src src-c\"><span class=\"tok-variable\">z</span></code> "
               "C <code class=\"src src-c\"><span class=\"tok-variable\">w</span></code> "
               "D</p>\n")));
}

// Code in the caption of a line-numbered block is the caption's, so it takes no line number, and
// the block's first line keeps its own.
static void test_numbers_no_caption_code(void) {
  TEST_CHECK(str_eq(render_org("#+CAPTION: a =v= b\n#+BEGIN_SRC zz -n\nx\n#+END_SRC\n"),
                    S("<label class=\"org-src-name\"><span class=\"listing-number\">Listing 1: "
                      "</span>a <code>v</code> b</label><pre><code class=\"language-zz\"><span "
                      "class=\"linenr\">1: </span>x\n</code></pre>\n")));
}

// Headings at the first two levels give the same HTML as in Markdown, which `test_markdown.c`
// checks with the same expected HTML.
static void test_renders_heading_levels(void) {
  TEST_CHECK(str_eq(render_org("* A\n** B\n"), S("<h2>A</h2>\n<h3>B</h3>\n")));
}

// Entities give the same HTML as in Markdown, which `test_markdown.c` checks with the same expected
// HTML.
static void test_renders_entities(void) {
  TEST_CHECK(str_eq(render_org("\\alpha and \\beta\n"), S("<p>&alpha; and &beta;</p>\n")));
}

// Braced sub/superscripts render, and an unbraced one such as an identifier with an underscore
// stays text, which is `ORG_FLAG_SUBSUPERSCRIPTS_BRACED`, Org's `^:{}`.
static void test_renders_braced_subscripts_only(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("a_{b} and a_b\n"), NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>a<sub>b</sub> and a_b</p>\n")));
}

// A file's `#+OPTIONS:` line overrides the defaults: `^:nil` turns the braced subscripts off.
static void test_honors_in_buffer_options(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S("#+OPTIONS: ^:nil\na_{b}\n"), NULL, &perm, scratch);

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
                  NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<div class=\"note\">raw <b>x</b></div>\n")));
}

// A footnote of several paragraphs keeps each one, and its back-references go inside the last, so
// they still follow its text.
static void test_renders_footnote_of_several_paragraphs(void) {
  TEST_CHECK(
      str_eq(render_org("A[fn:1].\n\n[fn:1] One.\n\nTwo.\n"),
             S("<p>A<sup><a href=\"#fn-1\" id=\"fnref-1-1\" role=\"doc-noteref\">1</a></sup>.</p>\n"
               "<section class=\"footnotes\" role=\"doc-endnotes\">\n<ol>\n<li id=\"fn-1\">\n"
               "<p>One.</p>\n"
               "<p>Two.<a href=\"#fnref-1-1\" class=\"footnote-backref\" "
               "role=\"doc-backlink\">&#8617;</a></p>\n"
               "</li>\n</ol>\n</section>\n")));
}

// A source block whose language has no grammar keeps the output of every path as it was before
// highlighting, byte for byte.
static void test_keeps_source_block_paths_without_grammar(void) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(source_cases) / sizeof(source_cases[0])); i++) {
    TEST_CASE(source_cases[i].name);
    TEST_CHECK(str_eq(render_org(source_cases[i].org_without_grammar),
                      str_from_cstr(source_cases[i].html_without_grammar)));
  }
}

// An example block and fixed-width lines stay plain whatever they hold, and an example block keeps
// its line numbers and code references, as in Emacs.
static void test_leaves_example_blocks_plain(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_EXAMPLE -n\n"
                               "int x; (ref:e)\n"
                               "#+END_EXAMPLE\n"
                               ": int x;\n"),
                    S("<pre class=\"example\"><span id=\"coderef-e\" class=\"coderef-off\"><span "
                      "class=\"linenr\">1: </span>int x; (e)</span>\n"
                      "</pre>\n"
                      "<pre class=\"example\">int x;\n"
                      "</pre>\n")));
}

// A special block whose name is not an admonition type keeps its name as its class, as in Emacs.
static void test_keeps_other_special_blocks(void) {
  TEST_CHECK(str_eq(render_org("#+BEGIN_aside\nA\n#+END_aside\n"),
                    S("<div class=\"aside\">\n<p>A</p>\n</div>\n")));
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

// A `file` link to an Org file points at that file's page, with `.org` in any letter case made
// `.html`, as ox-html writes it. A link to any other file keeps its path.
static void test_points_org_file_links_at_pages(void) {
  TEST_CHECK(str_eq(render_org("[[file:a.org][A]] [[file:b.ORG][B]] [[file:c.txt][C]]\n"),
                    S("<p><a href=\"a.html\">A</a> <a href=\"b.html\">B</a> "
                      "<a href=\"c.txt\">C</a></p>\n")));
}

// Every link's and image's target reaches the callback with its org4c type and its path as written,
// once, and a declined target renders as it does without a callback.
static void test_gives_every_target_to_the_callback(void) {
  const char org[] =
      "[[file:a.org::#c][A]] [[denote:1::*H]] [[https://e.com][W]] [[#cid][C]] [[file:p.png]]\n";
  struct Str calls = {0};
  TEST_CHECK(str_eq(render_org_linked(org, TEST_LINK_DECLINE, &calls), render_org(org)));
  TEST_CHECK(str_eq(calls, S("file|a.org::#c|link;fuzzy|denote:1::*H|link;https|//e.com|link;"
                             "custom-id|cid|link;file|p.png|image;")));
}

// A URL the callback writes is the link's `href`. A `file` link without a description shows that
// URL as its text, as Emacs shows the path it exports, while another link without one shows the
// target as typed. The callback sees each link once.
static void test_writes_the_callback_url(void) {
  struct Str calls = {0};
  TEST_CHECK(str_eq(
      render_org_linked("[[file:a.org]] [[file:a.org][A]] [[denote:1]]\n", TEST_LINK_WRITE, &calls),
      S("<p><a href=\"u.html\">u.html</a> <a href=\"u.html\">A</a> "
        "<a href=\"u.html\">denote:1</a></p>\n")));
  TEST_CHECK(str_eq(calls, S("file|a.org|link;file|a.org|link;fuzzy|denote:1|link;")));
}

// A link in a footnote, whose body the renderer holds in its scratch, takes the callback's URL too.
static void test_writes_the_callback_url_in_a_footnote(void) {
  struct Str calls = {0};
  TEST_CHECK(str_eq(
      render_org_linked("A[fn:1]\n\n[fn:1] See [[file:b.org][B]].\n", TEST_LINK_WRITE, &calls),
      S("<p>A<sup><a href=\"#fn-1\" id=\"fnref-1-1\" role=\"doc-noteref\">1</a></sup></p>\n"
        "<section class=\"footnotes\" role=\"doc-endnotes\">\n<ol>\n<li id=\"fn-1\">\nSee <a "
        "href=\"u.html\">B</a>.<a href=\"#fnref-1-1\" class=\"footnote-backref\" "
        "role=\"doc-backlink\">&#8617;</a>\n</li>\n</ol>\n</section>\n")));
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
  const struct Str html = org_to_html(S("\xef\xbb\xbfhello\n"), NULL, &perm, scratch);

  TEST_CHECK(str_eq(html, S("<p>hello</p>\n")));
}

// Empty input succeeds with empty HTML.
static void test_empty_input_yields_empty_string(void) {
  struct Arena perm = test_arena();
  struct Arena scratch = test_arena();
  const struct Str html = org_to_html(S(""), NULL, &perm, scratch);

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
      org_to_html((struct Str){org, PARAGRAPH_COUNT * PARAGRAPH_LEN}, NULL, &perm, scratch);
  TEST_CHECK(html.data == start);
  TEST_CHECK(html.len == PARAGRAPH_COUNT * S("<p>ab</p>\n").len);
}

// A target the callback fails on renders as it does without a callback, without the part of a URL
// the callback appended.
static void test_writes_a_failed_target_as_without_a_callback(void) {
  const char org[] = "[[file:a.org]] [[file:b.png]] [[denote:1][D]]\n";
  struct Str calls = {0};
  TEST_CHECK(str_eq(render_org_linked(org, TEST_LINK_FAIL, &calls), render_org(org)));
}

TEST_LIST = {
    {"renders basic html", test_renders_basic_html},
    {"renders bold and italics", test_renders_bold_and_italics},
    {"renders footnote", test_renders_footnote},
    {"renders task list items", test_renders_task_list_items},
    {"renders note admonition", test_renders_note_admonition},
    {"renders math", test_renders_math},
    {"renders code block", test_renders_code_block},
    {"highlights every source block path", test_highlights_every_source_block_path},
    {"follows inline source exports", test_follows_inline_source_exports},
    {"numbers no caption code", test_numbers_no_caption_code},
    {"renders heading levels", test_renders_heading_levels},
    {"renders entities", test_renders_entities},
    {"renders braced subscripts only", test_renders_braced_subscripts_only},
    {"honors in-buffer options", test_honors_in_buffer_options},
    {"passes through export blocks", test_passes_through_export_blocks},
    {"renders footnote of several paragraphs", test_renders_footnote_of_several_paragraphs},
    {"keeps source block paths without grammar", test_keeps_source_block_paths_without_grammar},
    {"leaves example blocks plain", test_leaves_example_blocks_plain},
    {"keeps other special blocks", test_keeps_other_special_blocks},
    {"matches admonition names in any case", test_matches_admonition_names_in_any_case},
    {"keeps admonition names", test_keeps_admonition_names},
    {"points org file links at pages", test_points_org_file_links_at_pages},
    {"gives every target to the callback", test_gives_every_target_to_the_callback},
    {"writes the callback url", test_writes_the_callback_url},
    {"writes the callback url in a footnote", test_writes_the_callback_url_in_a_footnote},
    {"renders dollar math", test_renders_dollar_math},
    {"skips leading byte order mark", test_skips_leading_byte_order_mark},
    {"empty input yields empty string", test_empty_input_yields_empty_string},
    {"html grows in place in perm", test_html_grows_in_place_in_perm},
    {"writes a failed target as without a callback",
     test_writes_a_failed_target_as_without_a_callback},
    {NULL, NULL},
};
