#include "domain/permalink.h"

#include <stdbool.h>
#include <stddef.h>

#include "shared/buf.h"
#include "shared/str.h"

/**
 * @brief Appends a token's replacement, tracking whether the URL now ends on a separator.
 *
 * An empty replacement leaves `*is_after_slash` exactly as it was, which collapses the two `/`
 * around an empty `{section}` into one. Resetting it once per token, the natural-looking change,
 * emits `//` for every top-level entry.
 *
 * @param buffer         URL being built. Must not be `NULL`.
 * @param replacement    Bytes substituted for the token.
 * @param is_after_slash Whether the URL ends on a separator, updated. Must not be `NULL`.
 */
static void permalink_expand_token(struct Buf* buffer, struct Str replacement, bool* is_after_slash)
    __attribute__((nonnull(1, 3)));

struct Str permalink_expand(struct Str pattern,
                            struct Str section,
                            struct Str slug,
                            struct Arena* arena) {
  // Root the URL at `/`, then substitute tokens while collapsing redundant separators so an empty
  // section (or a doubled `/` in the pattern) leaves no gap.
  struct Buf url = {.arena = arena};
  buf_append_char(&url, '/');
  bool is_after_slash = true;
  while (pattern.len > 0) {
    if (str_startswith(pattern, S("{slug}"))) {
      permalink_expand_token(&url, slug, &is_after_slash);
      pattern = str_tail(pattern, S("{slug}").len);
      continue;
    }
    if (str_startswith(pattern, S("{section}"))) {
      permalink_expand_token(&url, section, &is_after_slash);
      pattern = str_tail(pattern, S("{section}").len);
      continue;
    }
    const char c = pattern.data[0];
    if (c != '/' || !is_after_slash) {
      buf_append_char(&url, c);
    }
    is_after_slash = c == '/';
    pattern = str_tail(pattern, 1);
  }

  // An expansion ending on a separator publishes the directory's index document. The condition is
  // the expansion's last byte, not the pattern's. An empty `{section}` in the last segment leaves
  // the expansion ending on the separator written before it, so `/{slug}/{section}` publishes an
  // index document for a top-level entry too.
  if (is_after_slash) {
    buf_append(&url, S("index.html"));
  }
  return buf_str(&url);
}

static void permalink_expand_token(struct Buf* buffer,
                                   struct Str replacement,
                                   bool* is_after_slash) {
  if (replacement.len > 0) {
    buf_append(buffer, replacement);
    *is_after_slash = replacement.data[replacement.len - 1] == '/';
  }
}
