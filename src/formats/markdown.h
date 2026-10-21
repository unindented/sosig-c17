#ifndef SOSIG_MARKDOWN_H
#define SOSIG_MARKDOWN_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;

/**
 * Upper bound on Markdown input passed to `markdown_to_html`, in bytes.
 *
 * `RENDER_OUTPUT_LEN_MAX` is 256 MiB because an aggregate of 10,000 entries at ~6.7 KB is 64 MB.
 * That bound sits four times above it. HTML is not smaller than the Markdown that produced it, so a
 * body at that render cap already fills the budget before the template adds a byte. An aggregate or
 * feed that embeds the body could then reach the render limit. At that point, this function has
 * already parsed and retained the HTML. The same 256 MiB cap, applied by `entry_renderer` to the
 * whole source file at the read, rejects that body before conversion and reports the limit. This
 * prevents the body from dominating RSS before a generic render error.
 *
 * `entry_renderer` reads each content file with this as its size limit, so a source too large to
 * hold such a body is rejected from its size before it is loaded.
 *
 * The md4c API addresses input with `MD_SIZE` (`unsigned`). The 256 MiB cap is far below that
 * range. The API can address all accepted input.
 */
enum { MARKDOWN_INPUT_LEN_MAX = 256 * 1024 * 1024 };

/**
 * @brief Converts Markdown to HTML using the configured md4c extension set.
 *
 * The HTML is built in `perm`, growing in place while nothing else allocates there. md4c's own
 * working memory comes from `malloc`, and a failed allocation there ends in `os_oom`, like one from
 * `perm`.
 *
 * This is reentrant, so content jobs may render in parallel across the worker pool. md4c holds no
 * writable global state, and all per-render state lives in this function's frame. A vendor bump
 * that introduced a global would invalidate that without any call site changing.
 *
 * @param markdown Input bytes of Markdown, at most `MARKDOWN_INPUT_LEN_MAX`.
 * @param perm     Arena that owns the HTML. Must not be `NULL`.
 * @return The HTML, owned by `perm`.
 */
struct Str markdown_to_html(struct Str markdown, struct Arena* perm) __attribute__((nonnull(2)));

#endif
