#include <acutest.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "formats/highlight.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

/** Bytes of the arena `run_out_of_memory` highlights in, too few for any tree. */
enum { TEST_TINY_ARENA_LEN = 4096 };

/** Lines of the block `test_write_limited_writes_plain_past_limit` parses past its limit. */
enum { TEST_LIMITED_LINE_COUNT = 1000 };

/**
 * Levels of the brackets `test_write_rest_plain_past_cursor_work` nests. Up to 65,535 levels the
 * query's work grows with the block; from 65,536 on, with the square of it.
 */
enum { TEST_NESTED_BRACKET_COUNT = 70000 };

/** Brackets `test_write_plain_for_wide_error_node` leaves open, which make one node of that
    many. */
enum { TEST_OPEN_BRACKET_COUNT = 10000 };

/** Numbers of the array `test_write_highlights_wide_node` writes, more than an error node may
    hold. */
enum { TEST_ARRAY_ITEM_COUNT = 5000 };

/** Threads `test_write_from_threads` highlights from at once. */
enum { TEST_THREAD_COUNT = 8 };

/** Times each thread of `test_write_from_threads` highlights every snippet. */
enum { TEST_THREAD_ROUND_COUNT = 20 };

/** A language and a snippet of code in it, with the HTML the writer makes of it. */
struct Snippet {
  const char* language;
  const char* code;
  const char* html;
};

/**
 * One snippet per grammar. Each holds a node that matches each `#match?` pattern of its query and
 * one that does not, and the `#eq?` of JavaScript's:
 *
 * - C's `^[A-Z][A-Z\d_]*$` makes `MAX_LEN` a constant and leaves `Max` a variable.
 * - bash's `^-` makes `-n` a constant and leaves `n` plain.
 * - HTML has no `#match?` pattern.
 * - CSS's `^--` makes `--gap` a variable and leaves `auto` plain.
 * - JavaScript's `^[A-Z_][A-Z\d_]+$` makes `CONST_A` a constant, `^[A-Z]` makes `Foo` a
 *   constructor, `^(arguments|…|document)$` makes `console` a builtin and leaves `consoles` a
 *   variable, `#eq? "require"` makes `require` a builtin function, and JSX's `^[a-z][^.]*$` makes
 *   `div` a tag and leaves the component `Foo` a constructor.
 * - TypeScript's `^[A-Z]` makes `X` a type, and leaves each capitalized parameter a parameter:
 *   `Foo`, `C` in an object pattern, and `D` in the array pattern of an optional parameter.
 * - TSX takes JSX's and TypeScript's patterns together.
 * - JSON has no `#match?` pattern.
 *
 * Between them, TypeScript's and TSX's snippets hold each kind of parameter in an array or object
 * pattern, in a required parameter and in an optional one.
 */
static const struct Snippet snippets[] = {
    {"c", "#define MAX_LEN 10\nint main(void) { return Max + MAX_LEN; }\n",
     "<span class=\"tok-keyword\">#define</span> <span class=\"tok-constant\">MAX_LEN</span> 10\n"
     "<span class=\"tok-type\">int</span> <span class=\"tok-function\">main</span>(<span "
     "class=\"tok-type\">void</span>) { <span class=\"tok-keyword\">return</span> <span "
     "class=\"tok-variable\">Max</span> <span class=\"tok-operator\">+</span> <span "
     "class=\"tok-constant\">MAX_LEN</span><span class=\"tok-delimiter\">;</span> }\n"},
    {"sh", "echo \"$HOME\" -n n # c\n",
     "<span class=\"tok-function\">echo</span> <span class=\"tok-string\">&quot;</span><span "
     "class=\"tok-operator\">$</span><span class=\"tok-property\">HOME</span><span "
     "class=\"tok-string\">&quot;</span> <span class=\"tok-constant\">-n</span> n <span "
     "class=\"tok-comment\"># c</span>\n"},
    {"html", "<!DOCTYPE html>\n<p class=\"x\">Hi<!-- c --></p>\n",
     "<span class=\"tok-constant\">&lt;!DOCTYPE html</span><span "
     "class=\"tok-punctuation-bracket\">&gt;</span>\n<span "
     "class=\"tok-punctuation-bracket\">&lt;</span><span class=\"tok-tag\">p</span> <span "
     "class=\"tok-attribute\">class</span>=&quot;<span class=\"tok-string\">x</span>&quot;<span "
     "class=\"tok-punctuation-bracket\">&gt;</span>Hi<span class=\"tok-comment\">&lt;!-- c "
     "--&gt;</span><span class=\"tok-punctuation-bracket\">&lt;/</span><span "
     "class=\"tok-tag\">p</span><span class=\"tok-punctuation-bracket\">&gt;</span>\n"},
    {"css", "a { margin: var(--gap) auto; }\n",
     "<span class=\"tok-tag\">a</span> <span class=\"tok-punctuation-bracket\">{</span> <span "
     "class=\"tok-property\">margin</span><span class=\"tok-punctuation-delimiter\">:</span> <span "
     "class=\"tok-function\">var</span><span class=\"tok-punctuation-bracket\">(</span><span "
     "class=\"tok-variable\">--gap</span><span class=\"tok-punctuation-bracket\">)</span> "
     "auto<span class=\"tok-punctuation-delimiter\">;</span> <span "
     "class=\"tok-punctuation-bracket\">}</span>\n"},
    {"js",
     "const CONST_A = Foo(console, consoles);\nrequire(\"x\");\nconst e = <div><Foo /></div>;\n",
     "<span class=\"tok-keyword\">const</span> <span class=\"tok-constant\">CONST_A</span> <span "
     "class=\"tok-operator\">=</span> <span class=\"tok-constructor\">Foo</span><span "
     "class=\"tok-punctuation-bracket\">(</span><span "
     "class=\"tok-variable-builtin\">console</span><span "
     "class=\"tok-punctuation-delimiter\">,</span> <span "
     "class=\"tok-variable\">consoles</span><span class=\"tok-punctuation-bracket\">)</span><span "
     "class=\"tok-punctuation-delimiter\">;</span>\n"
     "<span class=\"tok-function-builtin\">require</span><span "
     "class=\"tok-punctuation-bracket\">(</span><span "
     "class=\"tok-string\">&quot;x&quot;</span><span "
     "class=\"tok-punctuation-bracket\">)</span><span "
     "class=\"tok-punctuation-delimiter\">;</span>\n"
     "<span class=\"tok-keyword\">const</span> <span class=\"tok-variable\">e</span> <span "
     "class=\"tok-operator\">=</span> <span class=\"tok-punctuation-bracket\">&lt;</span><span "
     "class=\"tok-tag\">div</span><span class=\"tok-punctuation-bracket\">&gt;&lt;</span><span "
     "class=\"tok-constructor\">Foo</span> <span "
     "class=\"tok-punctuation-bracket\">/&gt;&lt;/</span><span class=\"tok-tag\">div</span><span "
     "class=\"tok-punctuation-bracket\">&gt;</span><span "
     "class=\"tok-punctuation-delimiter\">;</span>\n"},
    {"ts", "function f(x: number, Foo: Bar, { a, b: C }: T, [D]?: U) { return X; }\n",
     "<span class=\"tok-keyword\">function</span> <span class=\"tok-function\">f</span><span "
     "class=\"tok-punctuation-bracket\">(</span><span class=\"tok-variable-parameter\">x</span>: "
     "<span class=\"tok-type-builtin\">number</span><span "
     "class=\"tok-punctuation-delimiter\">,</span> <span "
     "class=\"tok-variable-parameter\">Foo</span>: <span class=\"tok-type\">Bar</span><span "
     "class=\"tok-punctuation-delimiter\">,</span> <span "
     "class=\"tok-punctuation-bracket\">{</span> <span "
     "class=\"tok-variable-parameter\">a</span><span class=\"tok-punctuation-delimiter\">,</span> "
     "<span class=\"tok-property\">b</span>: <span class=\"tok-variable-parameter\">C</span> <span "
     "class=\"tok-punctuation-bracket\">}</span>: <span class=\"tok-type\">T</span><span "
     "class=\"tok-punctuation-delimiter\">,</span> <span "
     "class=\"tok-punctuation-bracket\">[</span><span "
     "class=\"tok-variable-parameter\">D</span><span class=\"tok-punctuation-bracket\">]</span>?: "
     "<span class=\"tok-type\">U</span><span class=\"tok-punctuation-bracket\">)</span> <span "
     "class=\"tok-punctuation-bracket\">{</span> <span class=\"tok-keyword\">return</span> <span "
     "class=\"tok-type\">X</span><span class=\"tok-punctuation-delimiter\">;</span> <span "
     "class=\"tok-punctuation-bracket\">}</span>\n"},
    {"tsx", "const A = (p: P, { q, s: t }?: Q, [r]: R) => <div className=\"x\"><Foo /></div>;\n",
     "<span class=\"tok-keyword\">const</span> <span class=\"tok-type\">A</span> <span "
     "class=\"tok-operator\">=</span> <span class=\"tok-punctuation-bracket\">(</span><span "
     "class=\"tok-variable-parameter\">p</span>: <span class=\"tok-type\">P</span><span "
     "class=\"tok-punctuation-delimiter\">,</span> <span "
     "class=\"tok-punctuation-bracket\">{</span> <span "
     "class=\"tok-variable-parameter\">q</span><span class=\"tok-punctuation-delimiter\">,</span> "
     "<span class=\"tok-property\">s</span>: <span class=\"tok-variable-parameter\">t</span> <span "
     "class=\"tok-punctuation-bracket\">}</span>?: <span class=\"tok-type\">Q</span><span "
     "class=\"tok-punctuation-delimiter\">,</span> <span "
     "class=\"tok-punctuation-bracket\">[</span><span "
     "class=\"tok-variable-parameter\">r</span><span class=\"tok-punctuation-bracket\">]</span>: "
     "<span class=\"tok-type\">R</span><span class=\"tok-punctuation-bracket\">)</span> <span "
     "class=\"tok-operator\">=&gt;</span> <span class=\"tok-punctuation-bracket\">&lt;</span><span "
     "class=\"tok-tag\">div</span> <span class=\"tok-attribute\">className</span><span "
     "class=\"tok-operator\">=</span><span class=\"tok-string\">&quot;x&quot;</span><span "
     "class=\"tok-punctuation-bracket\">&gt;&lt;</span><span class=\"tok-type\">Foo</span> <span "
     "class=\"tok-punctuation-bracket\">/&gt;&lt;/</span><span class=\"tok-tag\">div</span><span "
     "class=\"tok-punctuation-bracket\">&gt;</span><span "
     "class=\"tok-punctuation-delimiter\">;</span>\n"},
    {"json", "{\"a\": [\"b\\n\", 1, null]}\n",
     "{<span class=\"tok-string\">&quot;a&quot;</span>: [<span "
     "class=\"tok-string\">&quot;b</span><span class=\"tok-escape\">\\n</span><span "
     "class=\"tok-string\">&quot;</span>, <span class=\"tok-number\">1</span>, <span "
     "class=\"tok-constant-builtin\">null</span>]}\n"},
};

/**
 * @brief Highlights code in a language, as a code block of that language is.
 *
 * @param language Language the block names.
 * @param code     Code to highlight.
 * @param lines    Callbacks around each line, or `NULL`.
 * @return The HTML, in an arena of its own.
 */
static struct Str highlight(const char* language,
                            const char* code,
                            const struct HighlightLines* lines) {
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  highlight_write(&buffer, highlight_grammar_find(str_from_cstr(language)), str_from_cstr(code),
                  lines, test_arena());
  return buf_str(&buffer);
}

/**
 * @brief Highlights code in a language with a query of the test's in place of the grammar's.
 *
 * @param language Language whose grammar parses the code.
 * @param query    Highlight query, which must load.
 * @param code     Code to highlight.
 * @return The HTML, in an arena of its own.
 */
static struct Str highlight_with_query(const char* language, const char* query, const char* code) {
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  TEST_CHECK(highlight_write_query(&buffer, str_from_cstr(language), str_from_cstr(query),
                                   str_from_cstr(code), test_arena()) == 0);
  return buf_str(&buffer);
}

/**
 * @brief Reports whether a query is refused: `highlight_write_query` fails and writes nothing.
 *
 * @param language Language whose grammar parses the code.
 * @param query    Highlight query.
 * @return `true` when the query is refused, `false` otherwise.
 */
static bool is_query_refused(const char* language, const char* query) {
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  const int rc = highlight_write_query(&buffer, str_from_cstr(language), str_from_cstr(query),
                                       S("int foo;\n"), test_arena());
  return rc == -1 && buf_str(&buffer).len == 0;
}

/**
 * @brief Returns text repeated, in an arena of its own.
 *
 * @param prefix Text before the repetitions.
 * @param open   Text repeated `count` times after `prefix`.
 * @param close  Text repeated `count` times after those, or empty.
 * @param count  Times each is repeated. Must not be negative.
 * @return The text.
 */
static struct Str repeat(const char* prefix, const char* open, const char* close, ptrdiff_t count) {
  struct Arena arena = test_arena();
  struct Buf text = {.arena = &arena};
  buf_append(&text, str_from_cstr(prefix));
  for (ptrdiff_t i = 0; i < count; i++) {
    buf_append(&text, str_from_cstr(open));
  }
  for (ptrdiff_t i = 0; i < count; i++) {
    buf_append(&text, str_from_cstr(close));
  }
  return buf_str(&text);
}

/**
 * @brief Highlights code in a grammar, or plain, through line callbacks or none, in a region as
 *        large as a converter's.
 *
 * @param grammar Grammar, or `NULL` to write the code plain.
 * @param code    Code to highlight.
 * @param lines   Callbacks around each line, or `NULL`.
 * @return The HTML, in an arena of its own.
 */
static struct Str highlight_code(const struct HighlightGrammar* grammar,
                                 struct Str code,
                                 const struct HighlightLines* lines) {
  // As large as the region a converter gives its code blocks, so the block reaches a limit before
  // its arena is full. A reservation costs address space only, and acutest's child process for the
  // test returns it.
  struct Arena region = {0};
  TEST_ASSERT(arena_reserve(2 * (ptrdiff_t)HIGHLIGHT_MEMORY_LEN_MAX, &region) == 0);
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  highlight_write(&buffer, grammar, code, lines, region);
  return buf_str(&buffer);
}

/**
 * @brief Writes `[` and the line's index before each line. A `HighlightLineFn`.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param line     Index of the line.
 * @param userdata Unused.
 */
static void open_line(struct Buf* buffer, ptrdiff_t line, void* userdata) {
  (void)userdata;
  buf_printf(buffer, "[%td", line);
}

/**
 * @brief Writes `]` after each line. A `HighlightLineFn`.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param line     Index of the line.
 * @param userdata Unused.
 */
static void close_line(struct Buf* buffer, ptrdiff_t line, void* userdata) {
  (void)line;
  (void)userdata;
  buf_append(buffer, S("]"));
}

/**
 * @brief Highlights C in an arena too small for its tree. Run through `test_is_oom`.
 *
 * @param userdata Unused.
 */
static void run_out_of_memory(void* userdata) {
  (void)userdata;
  const struct HighlightGrammar* c = highlight_grammar_find(S("c"));
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  static char tiny[TEST_TINY_ARENA_LEN];
  highlight_write(&buffer, c, S("int main(void) { return 0; }\n"), NULL,
                  (struct Arena){.beg = tiny, .end = tiny + sizeof(tiny)});
}

/**
 * @brief Highlights every snippet many times, and counts the outputs that differ from the expected.
 *        A thread of `test_write_from_threads`.
 *
 * @param userdata `ptrdiff_t` that receives the count. Must not be `NULL`.
 * @return `NULL`.
 */
static void* highlight_snippets(void* userdata) {
  ptrdiff_t* mismatches = userdata;
  for (ptrdiff_t round = 0; round < TEST_THREAD_ROUND_COUNT; round++) {
    for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(snippets) / sizeof(snippets[0])); i++) {
      if (!str_eq(highlight(snippets[i].language, snippets[i].code, NULL),
                  str_from_cstr(snippets[i].html))) {
        (*mismatches)++;
      }
    }
  }
  return NULL;
}

// Each snippet's language finds its grammar, whose query and predicates load, and its code comes
// out with each token in the span of the capture its query gives it.
static void test_write_snippets(void) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(snippets) / sizeof(snippets[0])); i++) {
    TEST_CASE(snippets[i].language);
    TEST_CHECK(str_eq(highlight(snippets[i].language, snippets[i].code, NULL),
                      str_from_cstr(snippets[i].html)));
  }
}

// Every name of a grammar finds the same grammar, and two grammars' names find different ones.
static void test_find_every_name(void) {
  static const char* const shell_names[] = {
      "sh",    "shell", "bash",  "bash2", "ash",  "dash", "jsh",   "ksh", "ksh88",
      "pdksh", "mksh",  "dtksh", "oash",  "wksh", "wsh",  "posix", "zsh",
  };
  const struct HighlightGrammar* shell = highlight_grammar_find(S("sh"));
  TEST_ASSERT(shell != NULL);
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(shell_names) / sizeof(shell_names[0])); i++) {
    TEST_CASE(shell_names[i]);
    TEST_CHECK(highlight_grammar_find(str_from_cstr(shell_names[i])) == shell);
  }
  const struct HighlightGrammar* javascript = highlight_grammar_find(S("javascript"));
  TEST_CHECK(javascript != NULL);
  TEST_CHECK(highlight_grammar_find(S("js")) == javascript);
  TEST_CHECK(highlight_grammar_find(S("jsx")) == javascript);
  const struct HighlightGrammar* typescript = highlight_grammar_find(S("typescript"));
  TEST_CHECK(typescript != NULL);
  TEST_CHECK(highlight_grammar_find(S("ts")) == typescript);
  const struct HighlightGrammar* c = highlight_grammar_find(S("c"));
  const struct HighlightGrammar* html = highlight_grammar_find(S("html"));
  const struct HighlightGrammar* css = highlight_grammar_find(S("css"));
  const struct HighlightGrammar* tsx = highlight_grammar_find(S("tsx"));
  const struct HighlightGrammar* json = highlight_grammar_find(S("json"));
  TEST_CHECK(c != NULL && html != NULL && css != NULL && tsx != NULL && json != NULL);
  const struct HighlightGrammar* const found[] = {c,          shell,      html, css,
                                                  javascript, typescript, tsx,  json};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(found) / sizeof(found[0])); i++) {
    for (ptrdiff_t j = i + 1; j < (ptrdiff_t)(sizeof(found) / sizeof(found[0])); j++) {
      TEST_CHECK(found[i] != found[j]);
    }
  }
}

// A name finds its grammar whatever its ASCII case, as Org's `C` does, and after a `language-`
// prefix, as a Markdown fence may write it.
static void test_find_folds_case_and_prefix(void) {
  const struct HighlightGrammar* c = highlight_grammar_find(S("c"));
  TEST_CHECK(c != NULL);
  TEST_CHECK(highlight_grammar_find(S("C")) == c);
  TEST_CHECK(highlight_grammar_find(S("language-c")) == c);
  TEST_CHECK(highlight_grammar_find(S("language-C")) == c);
  TEST_CHECK(highlight_grammar_find(S("BASH")) == highlight_grammar_find(S("bash")));
}

// HTML in code is escaped, inside a span and outside one.
static void test_write_escapes(void) {
  TEST_CHECK(str_eq(highlight("c", "a<b; s = \"&\";\n", NULL),
                    S("<span class=\"tok-variable\">a</span><span "
                      "class=\"tok-operator\">&lt;</span><span "
                      "class=\"tok-variable\">b</span><span class=\"tok-delimiter\">;</span> "
                      "<span class=\"tok-variable\">s</span> <span "
                      "class=\"tok-operator\">=</span> <span "
                      "class=\"tok-string\">&quot;&amp;&quot;</span><span "
                      "class=\"tok-delimiter\">;</span>\n")));
}

// A capture that spans lines closes its span before each line end and opens it again after, and an
// empty line inside it gets no span.
static void test_write_splits_spans_at_line_ends(void) {
  TEST_CHECK(str_eq(highlight("c", "/* a\n\n b */\n", NULL),
                    S("<span class=\"tok-comment\">/* a</span>\n\n<span class=\"tok-comment\"> b "
                      "*/</span>\n")));
}

// The innermost capture covering text gives it its class: a string's quotes are the string's, and a
// command inside it is the command's.
static void test_write_innermost_capture_wins(void) {
  TEST_CHECK(
      str_eq(highlight("sh", "echo \"$(ls)\"\n", NULL),
             S("<span class=\"tok-function\">echo</span> <span "
               "class=\"tok-string\">&quot;</span><span "
               "class=\"tok-embedded\">$(</span><span class=\"tok-function\">ls</span><span "
               "class=\"tok-embedded\">)</span><span class=\"tok-string\">&quot;</span>\n")));
}

// Of two patterns that capture one node, the later in the query wins: a called name is a function,
// although an earlier pattern makes every identifier a variable.
static void test_write_later_pattern_wins(void) {
  TEST_CHECK(str_eq(highlight("c", "f(x);\n", NULL),
                    S("<span class=\"tok-function\">f</span>(<span "
                      "class=\"tok-variable\">x</span>)<span class=\"tok-delimiter\">;</span>\n")));
}

// Of two captures of one node in a row, the later drops the earlier's whole match, as in
// tree-sitter's highlighter, so `b`, which only the earlier pattern captures, is written plain. A
// pattern that captures `a` three times keeps the second, whose own match goes with the third.
static void test_write_capture_drops_superseded_match(void) {
  TEST_CHECK(
      str_eq(highlight_with_query(
                 "c",
                 "(binary_expression left: (identifier) @variable right: (identifier) @constant)\n"
                 "(binary_expression left: (identifier) @function)\n",
                 "int x = a + b;\n"),
             S("int x = <span class=\"tok-function\">a</span> + b;\n")));
  TEST_CHECK(
      str_eq(highlight_with_query("c",
                                  "(binary_expression left: (identifier) @variable @function "
                                  "@keyword right: (identifier) @constant)\n",
                                  "int x = a + b;\n"),
             S("int x = <span class=\"tok-function\">a</span> + b;\n")));
}

// A capture whose name starts with `_` writes no class, and hides the capture of its node just
// before it, so the node's text takes the capture around it, as in tree-sitter's highlighter: `foo`
// is the declaration's, where `bar` is still a variable.
static void test_write_private_capture_hides_node_capture(void) {
  TEST_CHECK(str_eq(
      highlight_with_query("c",
                           "(declaration) @type\n"
                           "(identifier) @variable\n"
                           "((identifier) @_word (#eq? @_word \"foo\"))\n",
                           "int foo, bar = sizeof(int);\n"),
      S("<span class=\"tok-type\">int foo, </span><span class=\"tok-variable\">bar</span><span "
        "class=\"tok-type\"> = sizeof(int);</span>\n")));
}

// A capture whose name starts with `_` hides only a capture of its own node just before it: a later
// pattern's capture of `foo` stands, and so does the capture of the type descriptor around `int`, a
// node of the same range as the private one's. A call's name keeps its capture when a private
// capture of the call comes between it and a private capture of the name, as in tree-sitter's
// highlighter.
static void test_write_private_capture_spares_other_captures(void) {
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "((identifier) @_word (#eq? @_word \"foo\"))\n"
                                         "(identifier) @variable\n"
                                         "(type_descriptor) @constant\n"
                                         "(primitive_type) @_type\n",
                                         "int foo, bar = sizeof(int);\n"),
                    S("int <span class=\"tok-variable\">foo</span>, <span "
                      "class=\"tok-variable\">bar</span> = sizeof(<span "
                      "class=\"tok-constant\">int</span>);\n")));
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(identifier) @variable\n"
                                         "((call_expression function: (identifier) @_word) @_call "
                                         "(#eq? @_word \"foo\"))\n",
                                         "foo(x);\n"),
                    S("<span class=\"tok-variable\">foo</span>(<span "
                      "class=\"tok-variable\">x</span>);\n")));
}

// `#any-of?` keeps a match whose text is one of its strings and drops, with all its captures, one
// whose text is none of them: `foo` and its value are highlighted, and `bar` and its value are not.
static void test_write_any_of_keeps_listed_text(void) {
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(init_declarator declarator: (identifier) @variable "
                                         "value: (number_literal) @number "
                                         "(#any-of? @variable \"foo\" \"baz\"))\n",
                                         "int foo = 1, bar = 2;\n"),
                    S("int <span class=\"tok-variable\">foo</span> = <span "
                      "class=\"tok-number\">1</span>, bar = 2;\n")));
}

// `#not-any-of?` keeps a match whose text is none of its strings and drops one whose text is one of
// them: `bar` and its value are highlighted, and `foo` and its value are not.
static void test_write_not_any_of_keeps_unlisted_text(void) {
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(init_declarator declarator: (identifier) @variable "
                                         "value: (number_literal) @number "
                                         "(#not-any-of? @variable \"foo\" \"baz\"))\n",
                                         "int foo = 1, bar = 2;\n"),
                    S("int foo = 1, <span class=\"tok-variable\">bar</span> = <span "
                      "class=\"tok-number\">2</span>;\n")));
}

// `#not-eq?` keeps a match whose text differs from its string and drops one whose text equals it:
// `y` and its value are highlighted, and `x` and its value are not.
static void test_write_not_eq_keeps_other_text(void) {
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(init_declarator declarator: (identifier) @variable "
                                         "value: (number_literal) @number "
                                         "(#not-eq? @variable \"x\"))\n",
                                         "int x = 1, y = 2;\n"),
                    S("int x = 1, <span class=\"tok-variable\">y</span> = <span "
                      "class=\"tok-number\">2</span>;\n")));
}

// `#not-match?` keeps a match whose text its pattern does not match and drops one whose text it
// does: `bar` and its value are highlighted, and `Foo` and its value are not.
static void test_write_not_match_keeps_unmatched_text(void) {
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(init_declarator declarator: (identifier) @constant "
                                         "value: (number_literal) @number "
                                         "(#not-match? @constant \"^[A-Z]\"))\n",
                                         "int Foo = 1, bar = 2;\n"),
                    S("int Foo = 1, <span class=\"tok-constant\">bar</span> = <span "
                      "class=\"tok-number\">2</span>;\n")));
}

// A text predicate holds only when every node the match took under its capture passes it, as in
// tree-sitter's highlighter: the first call, whose arguments both pass, is highlighted, and the
// second, with one that does not, is not.
static void test_write_text_predicate_tests_every_node(void) {
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(argument_list (identifier) @variable (identifier) "
                                         "@variable (#any-of? @variable \"a\" \"b\"))\n",
                                         "f(a, b);\ng(a, c);\n"),
                    S("f(<span class=\"tok-variable\">a</span>, <span "
                      "class=\"tok-variable\">b</span>);\ng(a, c);\n")));
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(argument_list (identifier) @variable (identifier) "
                                         "@variable (#not-any-of? @variable \"c\"))\n",
                                         "f(a, b);\ng(a, c);\n"),
                    S("f(<span class=\"tok-variable\">a</span>, <span "
                      "class=\"tok-variable\">b</span>);\ng(a, c);\n")));
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(argument_list (identifier) @constant (identifier) "
                                         "@constant (#eq? @constant \"a\"))\n",
                                         "f(a, a);\ng(a, b);\n"),
                    S("f(<span class=\"tok-constant\">a</span>, <span "
                      "class=\"tok-constant\">a</span>);\ng(a, b);\n")));
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(argument_list (identifier) @variable (identifier) "
                                         "@variable (#not-eq? @variable \"c\"))\n",
                                         "f(a, b);\ng(a, c);\n"),
                    S("f(<span class=\"tok-variable\">a</span>, <span "
                      "class=\"tok-variable\">b</span>);\ng(a, c);\n")));
  TEST_CHECK(str_eq(highlight_with_query("c",
                                         "(argument_list (identifier) @variable (identifier) "
                                         "@variable (#not-match? @variable \"^[A-Z]\"))\n",
                                         "f(a, b);\ng(a, C);\n"),
                    S("f(<span class=\"tok-variable\">a</span>, <span "
                      "class=\"tok-variable\">b</span>);\ng(a, C);\n")));
}

// A letter outside ASCII is no letter to a grammar's scanner on any host, so a word holding one is
// highlighted the same on macOS, glibc and musl, whose own character classes disagree on it.
static void test_write_classifies_non_ascii_alike(void) {
  TEST_CHECK(
      str_eq(highlight("sh", "café=1\n", NULL), S("<span class=\"tok-function\">café=1</span>\n")));
}

// A character past the ASCII range where the bash scanner looks for a digit, as in a brace
// expansion, is no digit, and reads no table of the C library's out of its bounds.
static void test_write_reads_no_digit_past_ascii(void) {
  TEST_CHECK(str_eq(highlight("sh", "x {\xF3\xA0\x80\x80\n", NULL),
                    S("<span class=\"tok-function\">x</span> {\xF3\xA0\x80\x80\n")));
}

// A node tree-sitter inserts to recover from an error has no width, and writes nothing.
static void test_write_skips_zero_width_capture(void) {
  TEST_CHECK(str_eq(highlight("c", "int x\n", NULL), S("<span class=\"tok-type\">int</span> <span "
                                                       "class=\"tok-variable\">x</span>\n")));
}

// With no grammar the code is written plain, only escaped.
static void test_write_plain_without_grammar(void) {
  TEST_CHECK(str_eq(highlight("unknown", "a < b\n", NULL), S("a &lt; b\n")));
}

// The line callbacks run around each line, highlighted or plain, with the line's index, outside
// every span, and around a last line that has no line end.
static void test_write_calls_line_callbacks(void) {
  const struct HighlightLines lines = {.open = open_line, .close = close_line};
  TEST_CHECK(str_eq(highlight("c", "/* a\n\n b */\nx", &lines),
                    S("[0<span class=\"tok-comment\">/* a</span>]\n[1]\n[2<span "
                      "class=\"tok-comment\"> b */</span>]\n[3<span "
                      "class=\"tok-variable\">x</span>]")));
  TEST_CHECK(str_eq(highlight("unknown", "a\n\nb\n", &lines), S("[0a]\n[1]\n[2b]\n")));
}

// A block whose parse takes more memory than the limit is written plain, and the same block under a
// larger limit is highlighted. Tree-sitter checks the limit every hundred steps of a parse, so the
// block is long enough to reach a check.
static void test_write_limited_writes_plain_past_limit(void) {
  const struct HighlightGrammar* c = highlight_grammar_find(S("c"));
  TEST_ASSERT(c != NULL);
  struct Arena arena = test_arena();
  struct Buf code = {.arena = &arena};
  for (ptrdiff_t i = 0; i < TEST_LIMITED_LINE_COUNT; i++) {
    buf_append(&code, S("int x;\n"));
  }
  struct Buf small = {.arena = &arena};
  highlight_write_limited(&small, c, buf_str(&code), NULL, 1, test_arena());
  TEST_CHECK(str_eq(buf_str(&small), buf_str(&code)));
  struct Buf large = {.arena = &arena};
  highlight_write_limited(&large, c, S("int x;\n"), NULL, HIGHLIGHT_MEMORY_LEN_MAX, test_arena());
  TEST_CHECK(str_eq(buf_str(&large), S("<span class=\"tok-type\">int</span> <span "
                                       "class=\"tok-variable\">x</span><span "
                                       "class=\"tok-delimiter\">;</span>\n")));
}

// Brackets nested by the tens of thousands make the query follow a match at every level, so the
// query stops past the work the block's length allows, and the rest of the block is written plain,
// the same on every run.
static void test_write_rest_plain_past_cursor_work(void) {
  const struct Str code = repeat("x = ", "[", "]", TEST_NESTED_BRACKET_COUNT);
  const struct HighlightGrammar* javascript = highlight_grammar_find(S("js"));
  const struct Str html = highlight_code(javascript, code, NULL);
  TEST_CHECK(str_endswith(html, str_tail(code, code.len - TEST_NESTED_BRACKET_COUNT)));
  TEST_CHECK(str_eq(html, highlight_code(javascript, code, NULL)));
}

// Brackets left open by the thousands give one error node that many children, which the query would
// take seconds over, so the block is written plain.
static void test_write_plain_for_wide_error_node(void) {
  const struct Str code = repeat("x = ", "[", "", TEST_OPEN_BRACKET_COUNT);
  TEST_CHECK(str_eq(highlight_code(highlight_grammar_find(S("js")), code, NULL), code));
}

// A well-formed node of thousands of children, such as a table of numbers, is highlighted as any
// other code is.
static void test_write_highlights_wide_node(void) {
  struct Arena arena = test_arena();
  struct Buf code = {.arena = &arena};
  buf_append(&code, S("a = ["));
  for (ptrdiff_t i = 0; i < TEST_ARRAY_ITEM_COUNT; i++) {
    buf_printf(&code, "%td, ", i);
  }
  buf_append(&code, S("];\n"));
  const struct Str html = highlight_code(highlight_grammar_find(S("js")), buf_str(&code), NULL);
  TEST_CHECK(str_endswith(html, S("<span class=\"tok-number\">4999</span><span "
                                  "class=\"tok-punctuation-delimiter\">,</span> <span "
                                  "class=\"tok-punctuation-bracket\">]</span><span "
                                  "class=\"tok-punctuation-delimiter\">;</span>\n")));
}

// Several threads highlight every grammar at once and each gets the same HTML, so the grammars and
// queries they share hold still and each thread's tree-sitter memory stays its own.
static void test_write_from_threads(void) {
  pthread_t threads[TEST_THREAD_COUNT];
  ptrdiff_t mismatches[TEST_THREAD_COUNT] = {0};
  for (ptrdiff_t i = 0; i < TEST_THREAD_COUNT; i++) {
    TEST_ASSERT(pthread_create(&threads[i], NULL, highlight_snippets, &mismatches[i]) == 0);
  }
  for (ptrdiff_t i = 0; i < TEST_THREAD_COUNT; i++) {
    TEST_CHECK(pthread_join(threads[i], NULL) == 0);
    TEST_CHECK(mismatches[i] == 0);
  }
}

// An empty name, an unknown one, a csh name Emacs gives `sh-mode` and a bare prefix find no
// grammar.
static void test_find_rejects_unknown_names(void) {
  TEST_CHECK(highlight_grammar_find(S("")) == NULL);
  TEST_CHECK(highlight_grammar_find(S("unknown")) == NULL);
  TEST_CHECK(highlight_grammar_find(S("csh")) == NULL);
  TEST_CHECK(highlight_grammar_find(S("language-")) == NULL);
  TEST_CHECK(highlight_grammar_find(S("cc")) == NULL);
}

// A predicate in the wrong shape is refused, as a grammar's query with one would be: `#not-eq?` and
// `#not-match?` with a string where the capture should be or with no string, and `#any-of?` and
// `#not-any-of?` with a string where the capture should be, with no strings, or with a capture
// among the strings.
static void test_write_query_refuses_malformed_predicates(void) {
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#not-eq? \"x\" @variable))\n"));
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#not-eq? @variable))\n"));
  TEST_CHECK(
      is_query_refused("c", "((identifier) @variable (#not-match? \"^[A-Z]\" @variable))\n"));
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#not-match? @variable))\n"));
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#any-of? \"foo\" \"bar\"))\n"));
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#any-of? @variable))\n"));
  TEST_CHECK(
      is_query_refused("c", "((identifier) @variable (#any-of? @variable \"foo\" @variable))\n"));
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#not-any-of? \"foo\" \"bar\"))\n"));
  TEST_CHECK(is_query_refused("c", "((identifier) @variable (#not-any-of? @variable))\n"));
  TEST_CHECK(is_query_refused(
      "c", "((identifier) @variable (#not-any-of? @variable \"foo\" @variable))\n"));
  TEST_CHECK(!is_query_refused("c", "((identifier) @variable (#any-of? @variable \"foo\"))\n"));
  TEST_CHECK(!is_query_refused("c", "((identifier) @variable (#not-eq? @variable \"foo\"))\n"));
  TEST_CHECK(
      !is_query_refused("c", "((identifier) @variable (#not-match? @variable \"^[A-Z]\"))\n"));
}

// Tree-sitter running out of a block's arena ends in `os_oom`, as an arena allocation does.
static void test_write_out_of_memory(void) {
  TEST_CHECK(test_is_oom(run_out_of_memory, NULL));
}

TEST_LIST = {
    {"write snippets", test_write_snippets},
    {"find every name", test_find_every_name},
    {"find folds case and prefix", test_find_folds_case_and_prefix},
    {"write escapes", test_write_escapes},
    {"write splits spans at line ends", test_write_splits_spans_at_line_ends},
    {"write innermost capture wins", test_write_innermost_capture_wins},
    {"write later pattern wins", test_write_later_pattern_wins},
    {"write capture drops superseded match", test_write_capture_drops_superseded_match},
    {"write private capture hides node capture", test_write_private_capture_hides_node_capture},
    {"write private capture spares other captures",
     test_write_private_capture_spares_other_captures},
    {"write any of keeps listed text", test_write_any_of_keeps_listed_text},
    {"write not any of keeps unlisted text", test_write_not_any_of_keeps_unlisted_text},
    {"write not eq keeps other text", test_write_not_eq_keeps_other_text},
    {"write not match keeps unmatched text", test_write_not_match_keeps_unmatched_text},
    {"write text predicate tests every node", test_write_text_predicate_tests_every_node},
    {"write classifies non-ascii alike", test_write_classifies_non_ascii_alike},
    {"write reads no digit past ascii", test_write_reads_no_digit_past_ascii},
    {"write skips zero-width capture", test_write_skips_zero_width_capture},
    {"write plain without grammar", test_write_plain_without_grammar},
    {"write calls line callbacks", test_write_calls_line_callbacks},
    {"write limited writes plain past limit", test_write_limited_writes_plain_past_limit},
    {"write rest plain past cursor work", test_write_rest_plain_past_cursor_work},
    {"write plain for wide error node", test_write_plain_for_wide_error_node},
    {"write highlights wide node", test_write_highlights_wide_node},
    {"write from threads", test_write_from_threads},
    {"find rejects unknown names", test_find_rejects_unknown_names},
    {"write query refuses malformed predicates", test_write_query_refuses_malformed_predicates},
    {"write out of memory", test_write_out_of_memory},
    {NULL, NULL},
};
