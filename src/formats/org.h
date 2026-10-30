#ifndef SOSIG_ORG_H
#define SOSIG_ORG_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;

/**
 * Upper bound on Org input passed to `org_to_html`, in bytes.
 *
 * It is 256 MiB for the reasons `MARKDOWN_INPUT_LEN_MAX` gives, and `entry_renderer` reads each
 * content file with the same limit.
 *
 * The org4c API addresses input with `ORG_SIZE` (`unsigned`). The 256 MiB cap is far below that
 * range. The API can address all accepted input.
 */
enum { ORG_INPUT_LEN_MAX = 256 * 1024 * 1024 };

/**
 * @brief Converts Org to HTML using the configured org4c flags.
 *
 * The HTML is built in `perm`, growing in place while nothing else allocates there. org4c's own
 * working memory comes from `malloc`, and a failed allocation there ends in `os_oom`, like one from
 * `perm`.
 *
 * This is reentrant, so content jobs may render in parallel across the worker pool. org4c holds no
 * writable global state, and all per-render state lives in this function's frame and `org_html`'s.
 * A vendor bump that introduced a global would invalidate that without any call site changing.
 *
 * @param org  Input bytes of Org, at most `ORG_INPUT_LEN_MAX`.
 * @param perm Arena that owns the HTML. Must not be `NULL`.
 * @return The HTML, owned by `perm`.
 */
struct Str org_to_html(struct Str org, struct Arena* perm) __attribute__((nonnull(2)));

#endif
