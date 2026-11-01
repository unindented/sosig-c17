#ifndef SOSIG_BODY_HTML_H
#define SOSIG_BODY_HTML_H

#include "shared/str.h"

struct Buf;

/**
 * @brief Appends `text` to a URL attribute of `buffer`, percent-encoding every byte a URL may not
 *        hold as it is.
 *
 * A letter, a digit, and each of `~-_.+!*(),%#@?=;:/$` are copied, `&` becomes `&amp;`, so the
 * attribute stays valid HTML, and every other byte becomes `%XX` in uppercase hexadecimal. The
 * Markdown and Org renderers write every link and image target taken from the source through it.
 *
 * Like `html_escape_append`, this does not make a URL safe: a `javascript:` scheme survives it.
 *
 * @param buffer Destination buffer to append to. Must not be `NULL`.
 * @param text   URL bytes to escape and append.
 */
void body_html_url_escape_append(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

#endif
