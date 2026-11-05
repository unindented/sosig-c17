#include "formats/highlight.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>
#include <tree_sitter_grammars.h>

#include "core/ascii.h"
#include "formats/html.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

/** Prefix a Markdown fence's language may carry, which names the same language without it. */
#define LANGUAGE_PREFIX S("language-")

/**
 * Bytes before each allocation tree-sitter takes from the arena: its requested size, then the next
 * free allocation of that size while it is free. Sixteen keeps the allocation aligned as `malloc`
 * aligns one.
 */
enum { TREE_MEMORY_HEADER_LEN = 16 };

/** Slots of the table of free lists, one per allocation size; a power of two. */
enum { TREE_MEMORY_FREE_LIST_COUNT = 256 };

/**
 * Bytes of tree-sitter memory from which a block gives its pages back once it is written: parsing a
 * block costs more than faulting the pages in again, and its pages would otherwise stay resident in
 * the worker for the rest of the build.
 */
enum { TREE_MEMORY_DISCARD_LEN_MIN = 16 * 1024 * 1024 };

/**
 * Most times the query cursor may report its progress for each KiB of a block, past which the block
 * is written plain. The cursor reports after each thousand units of work, a unit for each of the
 * matches it is following at each step, so the count grows with the work rather than with the time.
 * Real code takes at most three per KiB, and nested brackets, which the cursor follows as matches
 * at every level, take over a thousand.
 */
enum { CURSOR_REPORT_PER_KIB_MAX = 64 };

/**
 * Most children one error node of a block's tree may have, past which the block is written plain.
 * Tree-sitter recovers from tens of thousands of unclosed brackets with one error node of that many
 * children, and its query steps over a node's later siblings at each of them, which takes seconds.
 * A well-formed node that wide, such as a long table of numbers, takes the query no longer than any
 * other code.
 */
enum { NODE_CHILD_COUNT_MAX = 4096 };

/** Capture index that names no capture: no span is open. */
#define CAPTURE_NONE UINT32_MAX

/** One grammar: the names that find it, its language, and its highlight query. */
struct HighlightGrammar {
  /** Names in lowercase, such as `c`. */
  const struct Str* names;
  ptrdiff_t name_count;

  /** Returns the grammar's static language. */
  const TSLanguage* (*language)(void);

  /** Text of the highlight query, and its length. */
  const char* query_source;
  const uint32_t* query_source_len;

  /** Compiled query, or `NULL` when the query does not load. Set once, under `grammar_lock`. */
  TSQuery* query;

  /**
   * Whether the query has been compiled, or found not to load. Read and set under `grammar_lock`.
   */
  bool is_loaded;
};

/** Test of a `#match?` pattern against a node's text. */
typedef bool (*PatternFn)(struct Str text);

/** A `#match?` pattern of a vendored query, and the C test that matches as the pattern does. */
struct Pattern {
  /** The pattern as the query spells it, after its string escapes. */
  struct Str text;

  /** Test that holds where the pattern matches the text, as Rust's `is_match` would. */
  PatternFn is_match;
};

/** List of the free allocations of one size, chained through their headers. */
struct TreeMemoryFreeList {
  /** Size of the list's allocations plus one, or zero for an unused slot. */
  ptrdiff_t size_key;

  /** First free allocation, or `NULL`. */
  char* head;
};

/** Tree-sitter's memory for one block, which the allocator serves while a block is written. */
struct TreeMemory {
  /** Arena the allocations come from. */
  struct Arena* arena;

  /** Bytes taken from the arena so far, headers included. */
  ptrdiff_t len;

  /** Bytes past which the parse or the query is cancelled. */
  ptrdiff_t len_max;

  /** Times the query cursor has reported its progress, and the most it may. */
  ptrdiff_t cursor_report_count;
  ptrdiff_t cursor_report_count_max;

  /** Free lists, by size, in open addressing. */
  struct TreeMemoryFreeList free_lists[TREE_MEMORY_FREE_LIST_COUNT];
};

/** One capture the writer has opened and not yet passed: a node, its byte range and its name. */
struct Capture {
  /** The node's `id`, which only the same node shares. */
  const void* node_id;

  ptrdiff_t start;
  ptrdiff_t end;
  uint32_t index;
};

/** State of one block being written. */
struct Writer {
  /** Destination buffer. */
  struct Buf* buffer;

  /** Query whose capture names the classes, or `NULL` when the block is written plain. */
  const TSQuery* query;

  /** The block's code. */
  struct Str code;

  /** Callbacks around each line, or `NULL`. */
  const struct HighlightLines* lines;

  /** Bytes of `code` written so far. */
  ptrdiff_t pos;

  /** Index of the line being written. */
  ptrdiff_t line;

  /** Whether the current line's open callback has run. */
  bool is_line_open;

  /** Capture whose span is open in the output, or `CAPTURE_NONE`. */
  uint32_t span_capture;

  /** Captures that cover `pos`, in arrival order, so the last is the innermost. */
  struct Capture* stack;
  ptrdiff_t stack_count;
  ptrdiff_t stack_capacity;

  /** Arena the stack grows in. */
  struct Arena* arena;
};

/**
 * The grammars, by the names a code block gives its language: those `SOSIG_GRAMMARS` selects, which
 * `vendor/CMakeLists.txt` declares in `tree_sitter_grammars.h`, with their names.
 */
static struct HighlightGrammar grammars[] = {TREE_SITTER_GRAMMARS};

/**
 * Guards loading the grammars and installing the allocator. A grammar's query is compiled at its
 * first lookup, while every other lookup waits, so a site compiles only the grammars it names. The
 * compiled queries live until the process exits: they are immutable once compiled, every worker
 * reads them, and freeing them at exit could race a worker still highlighting when `os_oom` ends
 * the process. That is the one tree-sitter allocation no call deletes.
 */
static pthread_mutex_t grammar_lock = PTHREAD_MUTEX_INITIALIZER;

/** Whether tree-sitter's allocator has been replaced. Read and set under `grammar_lock`. */
static bool is_allocator_installed;

/**
 * Memory of the block this thread is writing, or `NULL` outside one. While it is set, the allocator
 * serves tree-sitter from its arena; outside one, as for a query compiled at a lookup, from
 * `malloc`.
 */
static _Thread_local struct TreeMemory* tree_memory;

/**
 * @brief Reports whether text is an uppercase letter followed by uppercase letters, digits and
 *        underscores: `^[A-Z][A-Z\d_]*$`.
 *
 * @param text Node text.
 * @return `true` when it is, `false` otherwise.
 */
static bool is_upper_identifier(struct Str text);

/**
 * @brief Reports whether text starts with `-`: `^-`.
 *
 * @param text Node text.
 * @return `true` when it does, `false` otherwise.
 */
static bool is_dash_led(struct Str text);

/**
 * @brief Reports whether text starts with `--`: `^--`.
 *
 * @param text Node text.
 * @return `true` when it does, `false` otherwise.
 */
static bool is_double_dash_led(struct Str text);

/**
 * @brief Reports whether text starts with an uppercase letter: `^[A-Z]`.
 *
 * @param text Node text.
 * @return `true` when it does, `false` otherwise.
 */
static bool is_upper_led(struct Str text);

/**
 * @brief Reports whether text is an uppercase letter or underscore followed by at least one
 *        uppercase letter, digit or underscore: `^[A-Z_][A-Z\d_]+$`.
 *
 * @param text Node text.
 * @return `true` when it is, `false` otherwise.
 */
static bool is_upper_constant(struct Str text);

/**
 * @brief Reports whether text is one of JavaScript's builtin names:
 *        `^(arguments|module|console|window|document)$`.
 *
 * @param text Node text.
 * @return `true` when it is, `false` otherwise.
 */
static bool is_javascript_builtin(struct Str text);

/**
 * @brief Reports whether text starts with a lowercase letter and holds no `.`: `^[a-z][^.]*$`.
 *
 * @param text Node text.
 * @return `true` when it does, `false` otherwise.
 */
static bool is_lowercase_tag(struct Str text);

/**
 * The `#match?` patterns of the vendored queries. A query with any other pattern does not load, so
 * a grammar bump that brings one fails the lookup test, and its pattern needs a row here. The tests
 * read `\d` as an ASCII digit, where Rust's also takes the digits of other scripts, which no
 * identifier these patterns test is likely to hold.
 */
static const struct Pattern patterns[] = {
    {STR_INIT("^[A-Z][A-Z\\d_]*$"), is_upper_identifier},
    {STR_INIT("^-"), is_dash_led},
    {STR_INIT("^--"), is_double_dash_led},
    {STR_INIT("^[A-Z]"), is_upper_led},
    {STR_INIT("^[A-Z_][A-Z\\d_]+$"), is_upper_constant},
    {STR_INIT("^(arguments|module|console|window|document)$"), is_javascript_builtin},
    {STR_INIT("^[a-z][^.]*$"), is_lowercase_tag},
};

/**
 * @brief Compiles a grammar's query unless it has been, and reports whether it loaded. A step of
 *        `highlight_grammar_find`.
 *
 * @param grammar Grammar to load. Must not be `NULL`.
 * @return `true` when its query loaded, `false` otherwise.
 */
static bool highlight_grammar_find_load(struct HighlightGrammar* grammar)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether two names are equal once the first is folded to lowercase in ASCII.
 *
 * @param text Text to compare.
 * @param name Lowercase name.
 * @return `true` when they match, `false` otherwise.
 */
static bool is_equal_folded(struct Str text, struct Str name);

/**
 * @brief Reports whether every predicate of a query is one the writer evaluates: `#match?` or
 *        `#not-match?` with a capture and a pattern of `patterns`, `#eq?` or `#not-eq?` with a
 *        capture and a string, `#any-of?` or `#not-any-of?` with a capture and one or more strings,
 *        or `#is-not? local`.
 *
 * @param query Query to check. Must not be `NULL`.
 * @return `true` when every predicate is, `false` otherwise.
 */
static bool is_query_supported(const TSQuery* query) __attribute__((nonnull(1)));

/**
 * @brief Finds the test of a `#match?` pattern.
 *
 * @param pattern Pattern as the query spells it.
 * @return The pattern's test, or `NULL` when no row of `patterns` holds it.
 */
static PatternFn find_pattern(struct Str pattern);

/**
 * @brief Returns the text of a query's string or capture name.
 *
 * @param query Query that holds it. Must not be `NULL`.
 * @param step  Step naming it. Must not be `NULL`.
 * @return The string, which the query owns.
 */
static struct Str step_text(const TSQuery* query, const TSQueryPredicateStep* step)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Replaces tree-sitter's allocator, whose failures abort, with this module's.
 */
static void install_allocator(void);

/**
 * @brief Allocates for tree-sitter, from the block's arena while one is written.
 *
 * @param size Bytes to allocate.
 * @return The allocation. Calls `os_oom` instead of returning `NULL`.
 */
static void* tree_memory_malloc(size_t size);

/**
 * @brief Allocates zeroed storage for tree-sitter, from the block's arena while one is written.
 *
 * @param count Number of objects.
 * @param size  Bytes of one object.
 * @return The allocation. Calls `os_oom` instead of returning `NULL`.
 */
static void* tree_memory_calloc(size_t count, size_t size);

/**
 * @brief Resizes a tree-sitter allocation, in the block's arena while one is written.
 *
 * @param ptr  Allocation to resize, or `NULL` to allocate.
 * @param size New size in bytes.
 * @return The resized allocation. Calls `os_oom` instead of returning `NULL`.
 */
static void* tree_memory_realloc(void* ptr, size_t size);

/**
 * @brief Frees a tree-sitter allocation, onto its size's free list while a block is written.
 *
 * @param ptr Allocation to free, or `NULL`.
 */
static void tree_memory_free(void* ptr);

/**
 * @brief Takes an allocation from a block's memory: a free one of the same size, or a new one from
 *        its arena.
 *
 * @param state Block's memory. Must not be `NULL`.
 * @param size  Bytes to allocate.
 * @return The allocation, after its header. Not zeroed when it is reused.
 */
static char* tree_memory_take(struct TreeMemory* state, size_t size) __attribute__((nonnull(1)));

/**
 * @brief Gives an allocation back to a block's memory, onto its size's free list.
 *
 * @param state Block's memory. Must not be `NULL`.
 * @param ptr   Allocation, after its header. Must not be `NULL`.
 */
static void tree_memory_give(struct TreeMemory* state, char* ptr) __attribute__((nonnull(1, 2)));

/**
 * @brief Finds the free list of a size, claiming a slot for it when it has none.
 *
 * @param state Block's memory. Must not be `NULL`.
 * @param size  Allocation size.
 * @return The list, or `NULL` when every slot holds another size, and the allocation is not kept.
 */
static struct TreeMemoryFreeList* tree_memory_free_list(struct TreeMemory* state, ptrdiff_t size)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether a parse has taken more memory than its block may, so it stops. This is the
 *        parse's progress callback.
 *
 * @param state Parse state, whose payload is the block's `struct TreeMemory`. Must not be `NULL`.
 * @return `true` to cancel the parse, `false` to go on.
 */
static bool is_tree_memory_exceeded(TSParseState* state) __attribute__((nonnull(1)));

/**
 * @brief Reports whether an error node of a tree has more than `NODE_CHILD_COUNT_MAX` children.
 *
 * @param tree Tree to walk. Must not be `NULL`.
 * @return `true` when one has, `false` otherwise.
 */
static bool is_tree_too_wide(const TSTree* tree) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a query has taken more memory, or more work, than its block may, so it
 *        stops. This is the query cursor's progress callback.
 *
 * @param state Cursor state, whose payload is the block's `struct TreeMemory`. Must not be `NULL`.
 * @return `true` to cancel the query, `false` to go on.
 */
static bool is_query_exceeded(TSQueryCursorState* state) __attribute__((nonnull(1)));

/**
 * @brief Hands tree-sitter the code from a byte on. This is the parse's input callback.
 *
 * @param payload    The block's `struct Str` code. Must not be `NULL`.
 * @param byte_index Offset of the first byte wanted.
 * @param position   Row and column of that byte, unused.
 * @param bytes_read Receives the number of bytes returned. Must not be `NULL`.
 * @return The code from `byte_index` on, or an empty string past its end.
 */
static const char* read_code(void* payload,
                             uint32_t byte_index,
                             TSPoint position,
                             uint32_t* bytes_read) __attribute__((nonnull(1, 4)));

/**
 * @brief Writes a parsed block's captures through the writer. A step of `highlight_write_limited`.
 *
 * @param writer       Writer of the block. Must not be `NULL`.
 * @param grammar      Grammar of the block. Must not be `NULL`.
 * @param tree         The block's tree. Must not be `NULL`.
 * @param block_memory The block's memory, which counts the query's work. Must not be `NULL`.
 */
static void highlight_write_limited_captures(struct Writer* writer,
                                             const struct HighlightGrammar* grammar,
                                             const TSTree* tree,
                                             struct TreeMemory* block_memory)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Reports whether a match's predicates hold for the nodes it captured.
 *
 * @param query Query of the match. Must not be `NULL`.
 * @param match Match to check. Must not be `NULL`.
 * @param code  The block's code, whose bytes the nodes span.
 * @return `true` when every predicate holds, `false` otherwise.
 */
static bool is_match_accepted(const TSQuery* query, const TSQueryMatch* match, struct Str code)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a text predicate holds for every node a match took under its capture.
 *
 * @param query      Query of the match. Must not be `NULL`.
 * @param match      Match to check. Must not be `NULL`.
 * @param predicate  The predicate's steps: its name, its capture, then its strings. Must not be
 *                   `NULL`.
 * @param step_count Number of the predicate's steps, at least 3.
 * @param code       The block's code, whose bytes the nodes span.
 * @return `true` when it holds for each node, or the match took none, `false` otherwise.
 */
static bool is_predicate_holding(const TSQuery* query,
                                 const TSQueryMatch* match,
                                 const TSQueryPredicateStep* predicate,
                                 ptrdiff_t step_count,
                                 struct Str code) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reports whether a node's text passes a text predicate.
 *
 * @param query      Query of the predicate. Must not be `NULL`.
 * @param predicate  The predicate's steps, as `is_predicate_holding` takes them. Must not be
 *                   `NULL`.
 * @param step_count Number of the predicate's steps, at least 3.
 * @param text       Text of the node.
 * @return `true` when it passes, `false` otherwise.
 */
static bool is_text_passing(const TSQuery* query,
                            const TSQueryPredicateStep* predicate,
                            ptrdiff_t step_count,
                            struct Str text) __attribute__((nonnull(1, 2)));

/**
 * @brief Writes up to a capture's start, then makes it the innermost capture.
 *
 * A capture that comes straight after a capture of the same node replaces it, so a later pattern
 * wins. A private capture is never written, but still replaces that capture, which leaves the
 * node's text to the capture around it, as tree-sitter's own highlighter does with a capture whose
 * name it does not recognize.
 *
 * @param writer             Writer of the block. Must not be `NULL`.
 * @param capture            Capture to push.
 * @param is_after_same_node Whether the capture comes straight after a capture of its node.
 */
static void writer_push(struct Writer* writer, struct Capture capture, bool is_after_same_node)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether a capture's name starts with `_`, which marks a capture a query takes only
 *        for a predicate to test.
 *
 * @param query   Query of the capture. Must not be `NULL`.
 * @param capture Capture index.
 * @return `true` when it is, `false` otherwise.
 */
static bool is_capture_private(const TSQuery* query, uint32_t capture) __attribute__((nonnull(1)));

/**
 * @brief Writes the code up to an offset, dropping each capture that ends on the way.
 *
 * A capture is dropped wherever it sits in the stack, so one that arrived out of order costs a
 * color, never well-formed HTML.
 *
 * @param writer Writer of the block. Must not be `NULL`.
 * @param end    Offset to write up to, at least `writer->pos`.
 */
static void writer_advance(struct Writer* writer, ptrdiff_t end) __attribute__((nonnull(1)));

/**
 * @brief Writes the code up to an offset, inside the innermost capture's span.
 *
 * Opens a span only before text it covers, and closes it before each line end, so every line of the
 * output is whole HTML.
 *
 * @param writer Writer of the block. Must not be `NULL`.
 * @param end    Offset to write up to, at least `writer->pos`.
 */
static void writer_text(struct Writer* writer, ptrdiff_t end) __attribute__((nonnull(1)));

/**
 * @brief Writes the rest of the code and closes the last line.
 *
 * @param writer Writer of the block. Must not be `NULL`.
 */
static void writer_finish(struct Writer* writer) __attribute__((nonnull(1)));

/**
 * @brief Runs the open callback of the current line, unless it has run.
 *
 * @param writer Writer of the block. Must not be `NULL`.
 */
static void writer_open_line(struct Writer* writer) __attribute__((nonnull(1)));

/**
 * @brief Closes the open span, then runs the close callback of the current line.
 *
 * @param writer Writer of the block. Must not be `NULL`.
 */
static void writer_close_line(struct Writer* writer) __attribute__((nonnull(1)));

/**
 * @brief Makes the open span the given capture's, closing the one open before.
 *
 * @param writer  Writer of the block. Must not be `NULL`.
 * @param capture Capture whose span is wanted, or `CAPTURE_NONE` for none.
 */
static void writer_span(struct Writer* writer, uint32_t capture) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a byte is an uppercase ASCII letter, a digit or an underscore.
 *
 * @param c Byte to check.
 * @return `true` when it is, `false` otherwise.
 */
static bool is_upper_word_byte(char c);

struct Str highlight_grammar_list(void) {
  return S(TREE_SITTER_GRAMMAR_LIST);
}

const struct HighlightGrammar* highlight_grammar_find(struct Str language) {
  if (str_startswith(language, LANGUAGE_PREFIX)) {
    language = str_tail(language, LANGUAGE_PREFIX.len);
  }
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(grammars) / sizeof(grammars[0])); i++) {
    struct HighlightGrammar* grammar = &grammars[i];
    for (ptrdiff_t j = 0; j < grammar->name_count; j++) {
      if (is_equal_folded(language, grammar->names[j])) {
        return highlight_grammar_find_load(grammar) ? grammar : NULL;
      }
    }
  }
  return NULL;
}

void highlight_write(struct Buf* buffer,
                     const struct HighlightGrammar* grammar,
                     struct Str code,
                     const struct HighlightLines* lines,
                     struct Arena scratch) {
  highlight_write_limited(buffer, grammar, code, lines, HIGHLIGHT_MEMORY_LEN_MAX, scratch);
}

void highlight_write_limited(struct Buf* buffer,
                             const struct HighlightGrammar* grammar,
                             struct Str code,
                             const struct HighlightLines* lines,
                             ptrdiff_t memory_len_max,
                             struct Arena scratch) {
  assert(code.len >= 0 && (uintmax_t)code.len <= UINT32_MAX);
  assert(memory_len_max > 0);
  const char* region_start = scratch.beg;
  struct TreeMemory block_memory = {
      .arena = &scratch,
      .len_max = memory_len_max,
      .cursor_report_count_max = CURSOR_REPORT_PER_KIB_MAX * (code.len / 1024 + 1),
  };
  struct Writer writer = {
      .buffer = buffer,
      .code = code,
      .lines = lines,
      .span_capture = CAPTURE_NONE,
      .arena = &scratch,
  };
  TSParser* parser = NULL;
  TSTree* tree = NULL;
  if (grammar != NULL) {
    tree_memory = &block_memory;
    parser = ts_parser_new();
    // The lookup test loads every grammar, which checks its ABI against the runtime's.
    const bool is_language_set = ts_parser_set_language(parser, grammar->language());
    assert(is_language_set);
    (void)is_language_set;
    const TSInput input = {.payload = &code, .read = read_code, .encoding = TSInputEncodingUTF8};
    const TSParseOptions options = {.payload = &block_memory,
                                    .progress_callback = is_tree_memory_exceeded};
    // A parse cancelled past the limit returns no tree, and the block is written plain. Failing the
    // build instead, as STYLE asks of an exceeded limit, would fail it over a code block's colors,
    // and `formats/` has no log to report it in.
    tree = ts_parser_parse_with_options(parser, NULL, input, options);
  }
  if (tree != NULL && !is_tree_too_wide(tree)) {
    writer.query = grammar->query;
    // A query cancelled past a limit leaves the rest of the block plain, at a point that depends
    // only on the work and memory it counted, so the same block comes out the same every time.
    highlight_write_limited_captures(&writer, grammar, tree, &block_memory);
  }
  writer_finish(&writer);
  if (tree != NULL) {
    ts_tree_delete(tree);
  }
  if (parser != NULL) {
    ts_parser_delete(parser);
  }
  // Everything tree-sitter allocated is deleted above, while the allocator still serves this block,
  // so nothing reaches `free` with an arena pointer.
  tree_memory = NULL;
  if (block_memory.len >= TREE_MEMORY_DISCARD_LEN_MIN) {
    arena_discard(&scratch, region_start, scratch.beg - region_start);
  }
}

int highlight_write_query(struct Buf* buffer,
                          struct Str language,
                          struct Str query_source,
                          struct Str code,
                          struct Arena scratch) {
  assert(query_source.len >= 0 && (uintmax_t)query_source.len <= UINT32_MAX);
  const struct HighlightGrammar* found = highlight_grammar_find(language);
  if (found == NULL) {
    return -1;
  }
  struct HighlightGrammar grammar = *found;
  uint32_t error_offset = 0;
  TSQueryError error = TSQueryErrorNone;
  grammar.query = ts_query_new(grammar.language(), query_source.data, (uint32_t)query_source.len,
                               &error_offset, &error);
  if (grammar.query == NULL) {
    return -1;
  }
  int rc = -1;
  if (is_query_supported(grammar.query)) {
    highlight_write_limited(buffer, &grammar, code, NULL, HIGHLIGHT_MEMORY_LEN_MAX, scratch);
    rc = 0;
  }
  ts_query_delete(grammar.query);
  return rc;
}

static bool highlight_grammar_find_load(struct HighlightGrammar* grammar) {
  (void)pthread_mutex_lock(&grammar_lock);
  if (!is_allocator_installed) {
    install_allocator();
    is_allocator_installed = true;
  }
  if (!grammar->is_loaded) {
    uint32_t error_offset = 0;
    TSQueryError error = TSQueryErrorNone;
    TSQuery* query = ts_query_new(grammar->language(), grammar->query_source,
                                  *grammar->query_source_len, &error_offset, &error);
    // A query that does not compile against its grammar, or uses a predicate the writer does not
    // evaluate, leaves the grammar unhighlighted rather than writing spans it cannot vouch for. The
    // lookup test loads every grammar, so a vendor bump that breaks one fails it.
    if (query != NULL && !is_query_supported(query)) {
      ts_query_delete(query);
      query = NULL;
    }
    grammar->query = query;
    grammar->is_loaded = true;
  }
  const bool is_loaded = grammar->query != NULL;
  (void)pthread_mutex_unlock(&grammar_lock);
  return is_loaded;
}

static bool is_equal_folded(struct Str text, struct Str name) {
  if (text.len != name.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < text.len; i++) {
    if (ascii_to_lower((unsigned char)text.data[i]) != (unsigned char)name.data[i]) {
      return false;
    }
  }
  return true;
}

static bool is_query_supported(const TSQuery* query) {
  const ptrdiff_t pattern_count = (ptrdiff_t)ts_query_pattern_count(query);
  for (ptrdiff_t pattern = 0; pattern < pattern_count; pattern++) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep* steps =
        ts_query_predicates_for_pattern(query, (uint32_t)pattern, &step_count);
    ptrdiff_t i = 0;
    while (i < (ptrdiff_t)step_count) {
      ptrdiff_t end = i;
      while (end < (ptrdiff_t)step_count && steps[end].type != TSQueryPredicateStepTypeDone) {
        end++;
      }
      const ptrdiff_t arg_count = end - i;
      if (arg_count == 0 || steps[i].type != TSQueryPredicateStepTypeString) {
        return false;
      }
      const struct Str name = step_text(query, &steps[i]);
      const bool is_capture_and_string = arg_count == 3 &&
                                         steps[i + 1].type == TSQueryPredicateStepTypeCapture &&
                                         steps[i + 2].type == TSQueryPredicateStepTypeString;
      bool is_supported = false;
      if (str_eq(name, S("match?")) || str_eq(name, S("not-match?"))) {
        is_supported =
            is_capture_and_string && find_pattern(step_text(query, &steps[i + 2])) != NULL;
      } else if (str_eq(name, S("eq?")) || str_eq(name, S("not-eq?"))) {
        is_supported = is_capture_and_string;
      } else if (str_eq(name, S("any-of?")) || str_eq(name, S("not-any-of?"))) {
        is_supported = arg_count >= 3 && steps[i + 1].type == TSQueryPredicateStepTypeCapture;
        for (ptrdiff_t arg = i + 2; arg < end; arg++) {
          is_supported = is_supported && steps[arg].type == TSQueryPredicateStepTypeString;
        }
      } else if (str_eq(name, S("is-not?"))) {
        is_supported = arg_count == 2 && steps[i + 1].type == TSQueryPredicateStepTypeString &&
                       str_eq(step_text(query, &steps[i + 1]), S("local"));
      }
      if (!is_supported) {
        return false;
      }
      i = end + 1;
    }
  }
  return true;
}

static PatternFn find_pattern(struct Str pattern) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(patterns) / sizeof(patterns[0])); i++) {
    if (str_eq(pattern, patterns[i].text)) {
      return patterns[i].is_match;
    }
  }
  return NULL;
}

static struct Str step_text(const TSQuery* query, const TSQueryPredicateStep* step) {
  uint32_t len = 0;
  const char* text = step->type == TSQueryPredicateStepTypeCapture
                         ? ts_query_capture_name_for_id(query, step->value_id, &len)
                         : ts_query_string_value_for_id(query, step->value_id, &len);
  return (struct Str){text, (ptrdiff_t)len};
}

static void install_allocator(void) {
  ts_set_allocator(tree_memory_malloc, tree_memory_calloc, tree_memory_realloc, tree_memory_free);
}

// Tree-sitter frees and reallocates as it parses, so a `free` that did nothing would let text a
// grammar cannot parse commit memory up to the block's limit. The free lists reuse each freed
// allocation for the next of the same size, and keep it within the bytes the sanitizer left
// addressable for it. Everything goes when the block's scratch does.

static void* tree_memory_malloc(size_t size) {
  if (tree_memory != NULL) {
    return tree_memory_take(tree_memory, size);
  }
  void* result = malloc(size);
  if (result == NULL && size > 0) {
    os_oom();
  }
  return result;
}

static void* tree_memory_calloc(size_t count, size_t size) {
  if (tree_memory != NULL) {
    // A product past `SIZE_MAX` could never fit an arena, so it is running out of memory.
    size_t len = 0;
    if (__builtin_mul_overflow(count, size, &len)) {
      os_oom();
    }
    char* result = tree_memory_take(tree_memory, len);
    memset(result, 0, len);
    return result;
  }
  void* result = calloc(count, size);
  if (result == NULL && count > 0 && size > 0) {
    os_oom();
  }
  return result;
}

static void* tree_memory_realloc(void* ptr, size_t size) {
  if (tree_memory != NULL) {
    if (ptr == NULL) {
      return tree_memory_take(tree_memory, size);
    }
    ptrdiff_t old_size = 0;
    memcpy(&old_size, (char*)ptr - TREE_MEMORY_HEADER_LEN, sizeof(old_size));
    if (size <= (size_t)old_size) {
      return ptr;
    }
    char* result = tree_memory_take(tree_memory, size);
    memcpy(result, ptr, (size_t)old_size);
    tree_memory_give(tree_memory, ptr);
    return result;
  }
  void* result = realloc(ptr, size);
  if (result == NULL && size > 0) {
    os_oom();
  }
  return result;
}

static void tree_memory_free(void* ptr) {
  if (tree_memory != NULL) {
    if (ptr != NULL) {
      tree_memory_give(tree_memory, ptr);
    }
    return;
  }
  free(ptr);
}

static char* tree_memory_take(struct TreeMemory* state, size_t size) {
  // A size no `ptrdiff_t` holds could never fit an arena, so it is running out of memory.
  if (size > (size_t)(PTRDIFF_MAX - TREE_MEMORY_HEADER_LEN)) {
    os_oom();
  }
  const ptrdiff_t len = (ptrdiff_t)size;
  struct TreeMemoryFreeList* list = tree_memory_free_list(state, len);
  if (list != NULL && list->head != NULL) {
    char* result = list->head;
    memcpy(&list->head, result - TREE_MEMORY_HEADER_LEN + sizeof(ptrdiff_t), sizeof(list->head));
    return result;
  }
  // The count is of bytes requested, headers included, rather than of the arena's movement, which
  // the sanitizer's gaps between allocations change, so the limit cuts at the same place on every
  // host.
  char* block = arena_alloc(state->arena, 1, TREE_MEMORY_HEADER_LEN + len, TREE_MEMORY_HEADER_LEN);
  state->len += TREE_MEMORY_HEADER_LEN + len;
  memcpy(block, &len, sizeof(len));
  return block + TREE_MEMORY_HEADER_LEN;
}

static void tree_memory_give(struct TreeMemory* state, char* ptr) {
  ptrdiff_t size = 0;
  memcpy(&size, ptr - TREE_MEMORY_HEADER_LEN, sizeof(size));
  struct TreeMemoryFreeList* list = tree_memory_free_list(state, size);
  if (list == NULL) {
    return;
  }
  memcpy(ptr - TREE_MEMORY_HEADER_LEN + sizeof(ptrdiff_t), &list->head, sizeof(list->head));
  list->head = ptr;
}

static struct TreeMemoryFreeList* tree_memory_free_list(struct TreeMemory* state, ptrdiff_t size) {
  // A parse makes a few dozen distinct sizes, so the table rarely fills; Knuth's multiplicative
  // hash spreads sizes that differ by a multiple of eight.
  const ptrdiff_t first = (ptrdiff_t)(((uint32_t)size * 2654435761U) >> 24);
  for (ptrdiff_t i = 0; i < TREE_MEMORY_FREE_LIST_COUNT; i++) {
    struct TreeMemoryFreeList* list = &state->free_lists[(first + i) % TREE_MEMORY_FREE_LIST_COUNT];
    if (list->size_key == size + 1) {
      return list;
    }
    if (list->size_key == 0) {
      list->size_key = size + 1;
      return list;
    }
  }
  return NULL;
}

static bool is_tree_memory_exceeded(TSParseState* state) {
  const struct TreeMemory* block_memory = state->payload;
  return block_memory->len > block_memory->len_max;
}

static bool is_tree_too_wide(const TSTree* tree) {
  TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
  bool is_wide = false;
  bool is_walking = true;
  while (is_walking && !is_wide) {
    const TSNode node = ts_tree_cursor_current_node(&cursor);
    is_wide = ts_node_is_error(node) && ts_node_child_count(node) > NODE_CHILD_COUNT_MAX;
    if (ts_tree_cursor_goto_first_child(&cursor)) {
      continue;
    }
    while (is_walking && !ts_tree_cursor_goto_next_sibling(&cursor)) {
      is_walking = ts_tree_cursor_goto_parent(&cursor);
    }
  }
  ts_tree_cursor_delete(&cursor);
  return is_wide;
}

static bool is_query_exceeded(TSQueryCursorState* state) {
  struct TreeMemory* block_memory = state->payload;
  block_memory->cursor_report_count++;
  return block_memory->len > block_memory->len_max ||
         block_memory->cursor_report_count > block_memory->cursor_report_count_max;
}

static const char* read_code(void* payload,
                             uint32_t byte_index,
                             TSPoint position,
                             uint32_t* bytes_read) {
  (void)position;
  const struct Str* code = payload;
  if ((ptrdiff_t)byte_index >= code->len) {
    *bytes_read = 0;
    return "";
  }
  *bytes_read = (uint32_t)(code->len - (ptrdiff_t)byte_index);
  return code->data + byte_index;
}

static void highlight_write_limited_captures(struct Writer* writer,
                                             const struct HighlightGrammar* grammar,
                                             const TSTree* tree,
                                             struct TreeMemory* block_memory) {
  TSQueryCursor* cursor = ts_query_cursor_new();
  const TSQueryCursorOptions options = {.payload = block_memory,
                                        .progress_callback = is_query_exceeded};
  ts_query_cursor_exec_with_options(cursor, grammar->query, ts_tree_root_node(tree), &options);
  TSQueryMatch match = {0};
  uint32_t capture_index = 0;
  const void* node_id_last = NULL;
  uint32_t match_id_last = 0;
  while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
    if (!is_match_accepted(grammar->query, &match, writer->code)) {
      ts_query_cursor_remove_match(cursor, match.id);
      continue;
    }
    const TSQueryCapture* found = &match.captures[capture_index];
    const struct Capture capture = {.node_id = found->node.id,
                                    .start = (ptrdiff_t)ts_node_start_byte(found->node),
                                    .end = (ptrdiff_t)ts_node_end_byte(found->node),
                                    .index = found->index};
    // Of a run of captures of one node, the last counts, and each capture before it drops its own
    // match, with that match's captures of other nodes, as in tree-sitter's highlighter.
    const bool is_after_same_node = capture.node_id == node_id_last;
    if (is_after_same_node) {
      ts_query_cursor_remove_match(cursor, match_id_last);
    }
    node_id_last = capture.node_id;
    match_id_last = match.id;
    // A zero-width node, such as one tree-sitter inserts to recover from an error, covers no text.
    if (capture.start < capture.end && capture.start >= writer->pos) {
      writer_push(writer, capture, is_after_same_node);
    }
  }
  ts_query_cursor_delete(cursor);
}

static bool is_match_accepted(const TSQuery* query, const TSQueryMatch* match, struct Str code) {
  uint32_t step_count = 0;
  const TSQueryPredicateStep* steps =
      ts_query_predicates_for_pattern(query, match->pattern_index, &step_count);
  // `is_query_supported` has checked the shape of every predicate.
  ptrdiff_t i = 0;
  while (i < (ptrdiff_t)step_count) {
    const struct Str name = step_text(query, &steps[i]);
    ptrdiff_t end = i;
    while (steps[end].type != TSQueryPredicateStepTypeDone) {
      end++;
    }
    // `#is-not? local` holds, because no local variables are tracked.
    if (!str_eq(name, S("is-not?")) &&
        !is_predicate_holding(query, match, &steps[i], end - i, code)) {
      return false;
    }
    i = end + 1;
  }
  return true;
}

static bool is_predicate_holding(const TSQuery* query,
                                 const TSQueryMatch* match,
                                 const TSQueryPredicateStep* predicate,
                                 ptrdiff_t step_count,
                                 struct Str code) {
  // As in tree-sitter's own highlighter, every node the match took under the capture must pass.
  for (ptrdiff_t i = 0; i < (ptrdiff_t)match->capture_count; i++) {
    const TSQueryCapture* capture = &match->captures[i];
    if (capture->index != predicate[1].value_id) {
      continue;
    }
    const ptrdiff_t start = (ptrdiff_t)ts_node_start_byte(capture->node);
    const ptrdiff_t end = (ptrdiff_t)ts_node_end_byte(capture->node);
    if (!is_text_passing(query, predicate, step_count,
                         str_head(str_tail(code, start), end - start))) {
      return false;
    }
  }
  return true;
}

static bool is_text_passing(const TSQuery* query,
                            const TSQueryPredicateStep* predicate,
                            ptrdiff_t step_count,
                            struct Str text) {
  const struct Str name = step_text(query, &predicate[0]);
  bool is_positive_passing = false;
  if (str_eq(name, S("match?")) || str_eq(name, S("not-match?"))) {
    is_positive_passing = find_pattern(step_text(query, &predicate[2]))(text);
  } else if (str_eq(name, S("eq?")) || str_eq(name, S("not-eq?"))) {
    is_positive_passing = str_eq(text, step_text(query, &predicate[2]));
  } else {
    for (ptrdiff_t i = 2; i < step_count && !is_positive_passing; i++) {
      is_positive_passing = str_eq(text, step_text(query, &predicate[i]));
    }
  }
  // A predicate whose name starts with `not-` holds where its positive form does not.
  return is_positive_passing != str_startswith(name, S("not-"));
}

static void writer_push(struct Writer* writer, struct Capture capture, bool is_after_same_node) {
  writer_advance(writer, capture.start);
  if (is_after_same_node && writer->stack_count > 0 &&
      writer->stack[writer->stack_count - 1].node_id == capture.node_id) {
    writer->stack_count--;
  }
  if (is_capture_private(writer->query, capture.index)) {
    return;
  }
  if (writer->stack_count == writer->stack_capacity) {
    writer->stack = arena_grow(writer->arena, writer->stack, &writer->stack_capacity,
                               (ptrdiff_t)sizeof(struct Capture), _Alignof(struct Capture));
  }
  writer->stack[writer->stack_count] = capture;
  writer->stack_count++;
}

static bool is_capture_private(const TSQuery* query, uint32_t capture) {
  uint32_t name_len = 0;
  const char* name = ts_query_capture_name_for_id(query, capture, &name_len);
  return name_len > 0 && name[0] == '_';
}

static void writer_advance(struct Writer* writer, ptrdiff_t end) {
  while (writer->stack_count > 0) {
    ptrdiff_t ended = PTRDIFF_MAX;
    for (ptrdiff_t i = 0; i < writer->stack_count; i++) {
      if (writer->stack[i].end < ended) {
        ended = writer->stack[i].end;
      }
    }
    if (ended > end) {
      break;
    }
    writer_text(writer, ended);
    ptrdiff_t kept = 0;
    for (ptrdiff_t i = 0; i < writer->stack_count; i++) {
      if (writer->stack[i].end > ended) {
        writer->stack[kept] = writer->stack[i];
        kept++;
      }
    }
    writer->stack_count = kept;
  }
  writer_text(writer, end);
}

static void writer_text(struct Writer* writer, ptrdiff_t end) {
  const uint32_t capture =
      writer->stack_count > 0 ? writer->stack[writer->stack_count - 1].index : CAPTURE_NONE;
  while (writer->pos < end) {
    writer_open_line(writer);
    const struct Str rest = str_head(str_tail(writer->code, writer->pos), end - writer->pos);
    const struct StrCut line = str_cut(rest, '\n');
    if (line.head.len > 0) {
      writer_span(writer, capture);
      html_escape_append(writer->buffer, line.head);
      writer->pos += line.head.len;
    }
    if (line.is_found) {
      writer_close_line(writer);
      buf_append(writer->buffer, S("\n"));
      writer->pos++;
      writer->line++;
    }
  }
}

static void writer_finish(struct Writer* writer) {
  writer_advance(writer, writer->code.len);
  if (writer->is_line_open) {
    writer_close_line(writer);
  }
}

static void writer_open_line(struct Writer* writer) {
  if (writer->is_line_open) {
    return;
  }
  writer->is_line_open = true;
  if (writer->lines != NULL) {
    writer->lines->open(writer->buffer, writer->line, writer->lines->userdata);
  }
}

static void writer_close_line(struct Writer* writer) {
  writer_span(writer, CAPTURE_NONE);
  writer->is_line_open = false;
  if (writer->lines != NULL) {
    writer->lines->close(writer->buffer, writer->line, writer->lines->userdata);
  }
}

static void writer_span(struct Writer* writer, uint32_t capture) {
  if (writer->span_capture == capture) {
    return;
  }
  if (writer->span_capture != CAPTURE_NONE) {
    buf_append(writer->buffer, S("</span>"));
  }
  writer->span_capture = capture;
  if (capture == CAPTURE_NONE) {
    return;
  }
  // A capture name, such as `punctuation.bracket`, becomes the class `tok-punctuation-bracket`.
  uint32_t name_len = 0;
  const char* name = ts_query_capture_name_for_id(writer->query, capture, &name_len);
  buf_append(writer->buffer, S("<span class=\"tok-"));
  for (ptrdiff_t i = 0; i < (ptrdiff_t)name_len; i++) {
    buf_append_char(writer->buffer, name[i] == '.' ? '-' : name[i]);
  }
  buf_append(writer->buffer, S("\">"));
}

static bool is_upper_word_byte(char c) {
  return (c >= 'A' && c <= 'Z') || ascii_is_digit((unsigned char)c) || c == '_';
}

static bool is_upper_identifier(struct Str text) {
  if (text.len == 0 || !(text.data[0] >= 'A' && text.data[0] <= 'Z')) {
    return false;
  }
  for (ptrdiff_t i = 1; i < text.len; i++) {
    if (!is_upper_word_byte(text.data[i])) {
      return false;
    }
  }
  return true;
}

static bool is_dash_led(struct Str text) {
  return text.len > 0 && text.data[0] == '-';
}

static bool is_double_dash_led(struct Str text) {
  return str_startswith(text, S("--"));
}

static bool is_upper_led(struct Str text) {
  return text.len > 0 && text.data[0] >= 'A' && text.data[0] <= 'Z';
}

static bool is_upper_constant(struct Str text) {
  if (text.len < 2 || !((text.data[0] >= 'A' && text.data[0] <= 'Z') || text.data[0] == '_')) {
    return false;
  }
  for (ptrdiff_t i = 1; i < text.len; i++) {
    if (!is_upper_word_byte(text.data[i])) {
      return false;
    }
  }
  return true;
}

static bool is_javascript_builtin(struct Str text) {
  return str_eq(text, S("arguments")) || str_eq(text, S("module")) || str_eq(text, S("console")) ||
         str_eq(text, S("window")) || str_eq(text, S("document"));
}

static bool is_lowercase_tag(struct Str text) {
  if (text.len == 0 || !(text.data[0] >= 'a' && text.data[0] <= 'z')) {
    return false;
  }
  for (ptrdiff_t i = 1; i < text.len; i++) {
    if (text.data[i] == '.') {
      return false;
    }
  }
  return true;
}
