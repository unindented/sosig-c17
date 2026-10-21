#include "formats/markdown.h"

#include <assert.h>
#include <md4c-html.h>
#include <md4c.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

_Static_assert((uintmax_t)MARKDOWN_INPUT_LEN_MAX <= (uintmax_t)(MD_SIZE)-1,
               "the product cap must stay inside md4c's addressable input range");

/**
 * @brief Appends one rendered HTML chunk to the render buffer.
 *
 * This is installed as md4c's output callback.
 *
 * @param chunk_data     Rendered HTML bytes for this chunk. A slice into md4c's own buffer: not
 *                       terminated, and valid only for the duration of this call.
 * @param chunk_data_len Number of bytes in `chunk_data`.
 * @param userdata       Pointer to the `struct Buf` the render appends to.
 */
static void markdown_to_html_append_chunk(const MD_CHAR* chunk_data,
                                          MD_SIZE chunk_data_len,
                                          void* userdata);

struct Str markdown_to_html(struct Str markdown, struct Arena* perm) {
  struct Buf html = {.arena = perm};

  // This names extensions one at a time rather than through an `MD_DIALECT_*` constant, because
  // md4c documents that a dialect constant's meaning changes as it implements more of that dialect.
  // A vendor bump would then silently change how existing content renders. Raw HTML blocks and
  // spans stay enabled (no `MD_FLAG_NOHTML`) because the Markdown is first-party and the content
  // template emits the body unescaped as `{{{body}}}`. Md4c does no sanitizing, so this is a trust
  // decision about who authors content, not an omission.
  const unsigned parser_flags = MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_TABLES |
                                MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS |
                                MD_FLAG_SPOILERS | MD_FLAG_SUPERSCRIPTS | MD_FLAG_SUBSCRIPTS |
                                MD_FLAG_ADMONITIONS | MD_FLAG_FOOTNOTES | MD_FLAG_HIGHLIGHT;
  // This is the one boundary where md4c's error convention becomes this project's. `md_html` fails
  // only when `md_parse` does, and the HTML renderer's callbacks never abort the parse, so a
  // failure here is md4c's own allocation failing. That is the same condition as `perm` running
  // out.
  assert(markdown.len >= 0 && markdown.len <= MARKDOWN_INPUT_LEN_MAX);
  if (md_html(markdown.data, (MD_SIZE)markdown.len, markdown_to_html_append_chunk, &html,
              parser_flags, MD_HTML_FLAG_SKIP_UTF8_BOM) != 0) {
    os_oom();
  }
  return buf_str(&html);
}

static void markdown_to_html_append_chunk(const MD_CHAR* chunk_data,
                                          MD_SIZE chunk_data_len,
                                          void* userdata) {
  buf_append(userdata, (struct Str){chunk_data, (ptrdiff_t)chunk_data_len});
}
