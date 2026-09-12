#ifndef SOSIG_MARKDOWN_H
#define SOSIG_MARKDOWN_H

#include <stddef.h>

/**
 * Upper bound on Markdown input passed to `markdown_to_html`, excluding a terminator.
 *
 * `RENDER_OUTPUT_LEN_MAX` is 256 MiB because an aggregate of 10,000 entries at ~6.7 KB is 64 MB.
 * That bound sits four times above it. HTML is not smaller than the Markdown that produced it, so a
 * body at that render cap already fills the budget before the template adds a byte. An aggregate or
 * feed that embeds the body could then reach the render limit. At that point, this function has
 * already parsed and retained the HTML. The same 256 MiB cap rejects that body at the converter and
 * reports the limit. This prevents the body from dominating RSS before a generic render error.
 *
 * The md4c API addresses input with `MD_SIZE` (`unsigned`). The 256 MiB cap is far below that
 * range. The API can address all accepted input.
 */
enum { MARKDOWN_INPUT_LEN_MAX = 256 * 1024 * 1024 };

/**
 * @brief Converts Markdown to HTML using the configured md4c extension set.
 *
 * Ownership of the returned HTML passes to the caller, who must `free` it. Empty input yields an
 * empty, terminated string rather than `NULL`.
 *
 * This is reentrant, so content jobs may render in parallel across the worker pool. md4c holds no
 * writable global state, and all per-render state lives in this function's frame. A vendor bump
 * that introduced a global would invalidate that without any call site changing.
 *
 * @param markdown     Input bytes of Markdown. Must hold at least `markdown_len` bytes. May be
 *                     `NULL` only when `markdown_len` is 0, so the declaration has no `nonnull`
 *                     attribute.
 * @param markdown_len Number of input bytes.
 * @param err          Receives a diagnostic when the input exceeds `MARKDOWN_INPUT_LEN_MAX`.
 *                     Untouched on success and on a parser or allocation failure, which have no
 *                     message of their own. May be `NULL` only when `err_len` is 0.
 * @param err_len      Size of `err` in bytes.
 * @return Terminated HTML the caller must `free`, or `NULL` on oversize input, a parser failure, or
 *         allocation failure.
 */
char* markdown_to_html(const char* markdown, size_t markdown_len, char* err, size_t err_len);

#endif
