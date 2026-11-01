#include "formats/body_html.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/ascii.h"
#include "formats/html.h"
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

/**
 * @brief Returns the HTML level of a heading: one below its level in the source, up to 6.
 *
 * @param level Heading level in the source, from 1.
 * @return The `N` of `<hN>`, from 2 to 6.
 */
static ptrdiff_t heading_html_level(ptrdiff_t level);

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

void body_html_heading_open(struct Buf* buffer, ptrdiff_t level) {
  buf_printf(buffer, "<h%td>", heading_html_level(level));
}

void body_html_heading_close(struct Buf* buffer, ptrdiff_t level) {
  buf_printf(buffer, "</h%td>\n", heading_html_level(level));
}

void body_html_footnote_reference_write(struct Buf* buffer, ptrdiff_t id, ptrdiff_t reference_id) {
  buf_printf(buffer, "<sup><a href=\"#fn-%td\" id=\"fnref-%td-%td\">%td</a></sup>", id, id,
             reference_id, id);
}

void body_html_footnotes_open(struct Buf* buffer) {
  buf_append(buffer, S("<section class=\"footnotes\">\n<ol>\n"));
}

void body_html_footnotes_close(struct Buf* buffer) {
  buf_append(buffer, S("</ol>\n</section>\n"));
}

void body_html_footnote_open(struct Buf* buffer, ptrdiff_t id) {
  buf_printf(buffer, "<li id=\"fn-%td\">\n", id);
}

void body_html_footnote_back_references_write(struct Buf* buffer,
                                              ptrdiff_t id,
                                              ptrdiff_t reference_count) {
  for (ptrdiff_t reference_id = 1; reference_id <= reference_count; reference_id++) {
    if (reference_id > 1) {
      buf_append(buffer, S(" "));
    }
    buf_printf(buffer, "<a href=\"#fnref-%td-%td\" class=\"footnote-backref\">&#8617;</a>", id,
               reference_id);
  }
}

void body_html_footnote_close(struct Buf* buffer) {
  buf_append(buffer, S("\n</li>\n"));
}

void body_html_checkbox_write(struct Buf* buffer, bool is_checked) {
  buf_append(buffer,
             is_checked
                 ? S("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled "
                     "checked>")
                 : S("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"));
}

void body_html_admonition_open(struct Buf* buffer, struct Str type, struct Str id) {
  buf_append(buffer, S("<div class=\"admonition-"));
  html_escape_append(buffer, type);
  if (id.len > 0) {
    buf_append(buffer, S("\" id=\""));
    html_escape_append(buffer, id);
  }
  buf_append(buffer, S("\"><p class=\"admonition-title\">"));
  html_escape_append(buffer, type);
  buf_append(buffer, S("</p>"));
}

void body_html_admonition_close(struct Buf* buffer) {
  buf_append(buffer, S("</div>\n"));
}

void body_html_math_open(struct Buf* buffer, bool is_display) {
  buf_append(buffer, is_display ? S("<x-equation type=\"display\">") : S("<x-equation>"));
}

void body_html_math_close(struct Buf* buffer) {
  buf_append(buffer, S("</x-equation>"));
}

void body_html_code_block_write(struct Buf* buffer, struct Str language, struct Str code) {
  buf_append(buffer, S("<pre><code"));
  if (language.len > 0) {
    buf_append(buffer, S(" class=\""));
    if (!str_startswith(language, S("language-"))) {
      buf_append(buffer, S("language-"));
    }
    html_escape_append(buffer, language);
    buf_append(buffer, S("\""));
  }
  buf_append(buffer, S(">"));
  html_escape_append(buffer, code);
  buf_append(buffer, S("</code></pre>\n"));
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

static ptrdiff_t heading_html_level(ptrdiff_t level) {
  return level < 5 ? level + 1 : 6;
}
