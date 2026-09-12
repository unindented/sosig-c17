#ifndef SOSIG_PERMALINK_H
#define SOSIG_PERMALINK_H

struct Arena;

/**
 * @brief Expands a permalink pattern into a content entry's public URL.
 *
 * Substitutes the `{slug}` and `{section}` tokens, collapses redundant `/` (so an empty section
 * leaves no gap), roots the URL at `/`, and appends `index.html` when the *expansion* ends on a
 * separator. That covers a pattern ending in `/` and also a pattern whose last segment expanded to
 * nothing, as `{section}` does for a top-level entry. `/{slug}/{section}` publishes
 * `<slug>/index.html` there. It leaves an unrecognized `{token}` literal, which the caller's
 * path-safety check then rejects. The output-relative path is the returned URL without its leading
 * `/`.
 *
 * This is the one implementation of the pattern's meaning. `site_config_load` validates a
 * configured pattern by expanding it here and checking the result, rather than reasoning about the
 * pattern's bytes separately, so validation and rendering can never disagree.
 *
 * @param pattern Permalink pattern to expand. Must not be `NULL`.
 * @param section Slugified section. May be empty. Must not be `NULL`.
 * @param slug    Entry slug. Must not be `NULL`.
 * @param arena   Arena that owns the returned URL. Must not be `NULL`.
 * @return Terminated URL owned by `arena` with a leading `/`, or `NULL` on allocation failure.
 */
char* permalink_expand(const char* pattern,
                       const char* section,
                       const char* slug,
                       struct Arena* arena) __attribute__((nonnull(1, 2, 3, 4)));

#endif
