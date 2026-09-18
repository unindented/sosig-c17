#include "domain/permalink.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "shared/arena.h"

/**
 * @brief Expands a permalink pattern, writing into `url_out` when given and returning the length.
 *
 * One implementation both sizes the buffer and fills it. Called with `url_out` as `NULL` it writes
 * nothing and only counts, so the length and the bytes can never disagree.
 *
 * @param pattern Permalink pattern to expand. Must not be `NULL`.
 * @param section Section value substituted for `{section}`. Must not be `NULL`.
 * @param slug    Slug value substituted for `{slug}`. Must not be `NULL`.
 * @param url_out Destination for the expansion, excluding the terminator, or `NULL` to only
 *                measure.
 * @return Length of the expansion in bytes, excluding the terminator.
 */
static size_t permalink_expand_into(const char* pattern,
                                    const char* section,
                                    const char* slug,
                                    char* url_out) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes one byte at `*url_len` when `url` is given, and advances it either way.
 *
 * @param url     Destination buffer, or `NULL` to only advance the length.
 * @param url_len Running length, used as the write offset and incremented. Must not be `NULL`.
 * @param c       Byte to write.
 */
static void put_char(char* url, size_t* url_len, char c) __attribute__((nonnull(2)));

char* permalink_expand(const char* pattern,
                       const char* section,
                       const char* slug,
                       struct Arena* arena) {
  // This builds the URL directly in arena storage. The expansion is the entry's `url_path`, which
  // the arena owns for the rest of the build, so a growable heap buffer would only be copied there
  // and freed.
  const size_t url_len = permalink_expand_into(pattern, section, slug, NULL);
  char* url = arena_alloc(arena, url_len + 1);
  if (url == NULL) {
    return NULL;
  }
  (void)permalink_expand_into(pattern, section, slug, url);
  url[url_len] = '\0';
  return url;
}

static size_t permalink_expand_into(const char* pattern,
                                    const char* section,
                                    const char* slug,
                                    char* url_out) {
  size_t url_out_len = 0;

  // Root the URL at `/`, then substitute tokens while collapsing redundant separators so an empty
  // section (or a doubled `/` in the pattern) leaves no gap.
  put_char(url_out, &url_out_len, '/');
  bool is_after_slash = true;
  for (const char* p = pattern; *p != '\0';) {
    const char* replacement = NULL;
    // `strncmp` stops at a terminator in either argument, so matching the 9-byte `{section}`
    // against a shorter pattern tail such as `/{sec` compares only as far as that tail's `NUL` and
    // never reads past the pattern. `memcmp` has no such stop and would be an out-of-bounds read
    // here, which is the substitution a reader makes to avoid the terminator scan.
    if (strncmp(p, "{slug}", sizeof("{slug}") - 1) == 0) {
      replacement = slug;
      p += sizeof("{slug}") - 1;
    } else if (strncmp(p, "{section}", sizeof("{section}") - 1) == 0) {
      replacement = section;
      p += sizeof("{section}") - 1;
    }

    if (replacement != NULL) {
      // An empty replacement runs this loop zero times and so leaves `is_after_slash` exactly as it
      // was, which collapses the two `/` around an empty `{section}` into one. Resetting
      // `is_after_slash` once per token, the natural-looking change, emits `//` for every top-level
      // entry.
      for (const char* c = replacement; *c != '\0'; c++) {
        put_char(url_out, &url_out_len, *c);
        is_after_slash = *c == '/';
      }
      continue;
    }

    if (*p == '/') {
      if (!is_after_slash) {
        put_char(url_out, &url_out_len, '/');
        is_after_slash = true;
      }
    } else {
      put_char(url_out, &url_out_len, *p);
      is_after_slash = false;
    }
    p++;
  }

  // An expansion ending on a separator publishes the directory's index document. The condition is
  // the expansion's last byte, not the pattern's. An empty `{section}` in the last segment leaves
  // the expansion ending on the separator written before it, so `/{slug}/{section}` publishes an
  // index document for a top-level entry too.
  if (is_after_slash) {
    static const char INDEX_DOCUMENT[] = "index.html";
    for (size_t i = 0; i < sizeof(INDEX_DOCUMENT) - 1; i++) {
      put_char(url_out, &url_out_len, INDEX_DOCUMENT[i]);
    }
  }
  return url_out_len;
}

static void put_char(char* url, size_t* url_len, char c) {
  if (url != NULL) {
    url[*url_len] = c;
  }
  (*url_len)++;
}
