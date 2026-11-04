#ifndef SOSIG_HIGHLIGHT_H
#define SOSIG_HIGHLIGHT_H

#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Buf;
struct HighlightGrammar;

/**
 * Most bytes of tree-sitter memory one code block may take, headers included. Valid code takes 20
 * to 55 bytes for each byte of it, so this covers a block of 2 to 6 MiB; text a grammar cannot
 * parse can take thousands, and a block that passes the limit is written plain.
 */
enum { HIGHLIGHT_MEMORY_LEN_MAX = 128 * 1024 * 1024 };

/**
 * @brief Callback the writer calls around each line of a code block.
 *
 * Every span the writer opens on a line closes before the line ends, so the callback can write
 * markup around the line without crossing one.
 *
 * @param buffer   Destination buffer the code is written to. Must not be `NULL`.
 * @param line     Index of the line, from 0.
 * @param userdata Opaque pointer from `struct HighlightLines`.
 */
typedef void (*HighlightLineFn)(struct Buf* buffer, ptrdiff_t line, void* userdata);

/** Callbacks the writer calls around each line, in order, which decorate it, such as with its
 * number.
 */
struct HighlightLines {
  /** Called before a line's text. Must not be `NULL`. */
  HighlightLineFn open;

  /** Called after a line's text, before its `\n`, or at the end of a last line without one. */
  HighlightLineFn close;

  /** Opaque pointer passed to both callbacks. */
  void* userdata;
};

/**
 * @brief Finds the grammar a code block's language names.
 *
 * The name is compared in ASCII case-insensitively, after a `language-` prefix is dropped. The
 * first lookup of a grammar compiles its highlight query, once in the process, so the lookup may
 * run from several threads at once.
 *
 * @param language Language the block names, such as `c` or `language-sh`, or empty.
 * @return The grammar, which lives until the process exits, or `NULL` for an empty or unknown name
 *         and for a grammar whose query does not load.
 */
const struct HighlightGrammar* highlight_grammar_find(struct Str language);

/**
 * @brief Writes code HTML-escaped, with a `<span class="tok-…">` around each token the grammar's
 *        query captures.
 *
 * A capture whose name starts with `_`, which a query takes only for a predicate to test, is never
 * written, and hides a capture of the same node just before it, whose text the capture around it
 * then takes.
 *
 * Writes the code plain, only escaped, when `grammar` is `NULL`, when parsing it would take more
 * than `HIGHLIGHT_MEMORY_LEN_MAX` bytes, and when tree-sitter recovers from it with an error node
 * of thousands of children, as unclosed brackets by the tens of thousands give. A query that would
 * take more memory, or more work than the block's length allows, as brackets nested 65,536 deep do,
 * leaves the rest of the block plain. Every line goes through `lines`, once.
 *
 * @param buffer  Destination buffer. Must not be `NULL`.
 * @param grammar Grammar from `highlight_grammar_find`, or `NULL` to write the code plain.
 * @param code    Code, at most `UINT32_MAX` bytes.
 * @param lines   Callbacks around each line, or `NULL` for none.
 * @param scratch Arena for tree-sitter's memory while the code is parsed and its tokens found, in
 *                which nothing else grows while this runs.
 */
void highlight_write(struct Buf* buffer,
                     const struct HighlightGrammar* grammar,
                     struct Str code,
                     const struct HighlightLines* lines,
                     struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes code as `highlight_write` does, with a memory limit of the caller's.
 *
 * This lets a test reach the limit with a small block.
 *
 * @param buffer         Destination buffer. Must not be `NULL`.
 * @param grammar        Grammar from `highlight_grammar_find`, or `NULL` to write the code plain.
 * @param code           Code, at most `UINT32_MAX` bytes.
 * @param lines          Callbacks around each line, or `NULL` for none.
 * @param memory_len_max Most bytes of tree-sitter memory the block may take before it is written
 *                       plain. Must be positive.
 * @param scratch        Arena for tree-sitter's memory, in which nothing else grows while this
 *                       runs.
 */
void highlight_write_limited(struct Buf* buffer,
                             const struct HighlightGrammar* grammar,
                             struct Str code,
                             const struct HighlightLines* lines,
                             ptrdiff_t memory_len_max,
                             struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes code as `highlight_write` does, with a query of the caller's in place of the
 *        grammar's.
 *
 * This lets a test write code with a query no grammar embeds. The query is compiled for this call
 * alone.
 *
 * @param buffer       Destination buffer. Must not be `NULL`.
 * @param language     Language whose grammar parses the code, as `highlight_grammar_find` takes it.
 * @param query_source Highlight query, which may use the predicates a grammar's query may.
 * @param code         Code, at most `UINT32_MAX` bytes.
 * @param scratch      Arena for tree-sitter's memory, in which nothing else grows while this runs.
 * @return `0` on success, or `-1` when the language names no grammar whose own query loads, or
 *         `query_source` does not load, and nothing is written.
 */
int highlight_write_query(struct Buf* buffer,
                          struct Str language,
                          struct Str query_source,
                          struct Str code,
                          struct Arena scratch) __attribute__((nonnull(1)));

#endif
