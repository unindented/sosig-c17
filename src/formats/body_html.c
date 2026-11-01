#include "formats/body_html.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/ascii.h"
#include "shared/buf.h"
#include "shared/str.h"

/** Uppercase hex digits of a percent-encoded URL byte. */
static const char HEX_DIGITS[] = "0123456789ABCDEF";

/**
 * @brief Reports whether a URL may hold a byte as it is.
 *
 * @param c Byte to check.
 * @return `true` for a letter, a digit, or one of `~-_.+!*(),%#@?=;:/$`, `false` otherwise.
 */
static bool is_url_byte_kept(char c);

void body_html_url_escape_append(struct Buf* buffer, struct Str text) {
  ptrdiff_t run_start = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    if (is_url_byte_kept((char)c)) {
      continue;
    }
    buf_append(buffer, str_head(str_tail(text, run_start), i - run_start));
    if (c == '&') {
      buf_append(buffer, S("&amp;"));
    } else {
      const char percent[] = {'%', HEX_DIGITS[c >> 4], HEX_DIGITS[c & 0xF]};
      buf_append(buffer, (struct Str){percent, (ptrdiff_t)sizeof(percent)});
    }
    run_start = i + 1;
  }
  buf_append(buffer, str_tail(text, run_start));
}

static bool is_url_byte_kept(char c) {
  if (ascii_is_alphanumeric((unsigned char)c)) {
    return true;
  }
  switch (c) {
    case '~':
    case '-':
    case '_':
    case '.':
    case '+':
    case '!':
    case '*':
    case '(':
    case ')':
    case ',':
    case '%':
    case '#':
    case '@':
    case '?':
    case '=':
    case ';':
    case ':':
    case '/':
    case '$':
      return true;
    default:
      return false;
  }
}
