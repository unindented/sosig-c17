#include "formats/markdown.h"

#include <md4c-html.h>
#include <md4c.h>
#include <stdbool.h>

#include "core/error.h"
#include "shared/string_buffer.h"

_Static_assert((size_t)MARKDOWN_INPUT_LEN_MAX <= (size_t)((MD_SIZE)-1),
               "the product cap must stay inside md4c's addressable input range");

/** md4c callback state for one Markdown render. */
struct MarkdownRenderState {
  /** Accumulates rendered HTML chunks. */
  struct StringBuffer buf;

  /** Records append failure because the callback has no return value. */
  bool has_failed;
};

/**
 * @brief Appends one rendered HTML chunk to the render buffer.
 *
 * This is installed as md4c's output callback. Because that callback cannot return an error, it
 * records an append failure on the render state for the caller to check after rendering.
 *
 * @param chunk_data     Rendered HTML bytes for this chunk. A slice into md4c's own buffer: not
 *                       terminated, and valid only for the duration of this call. Because of this
 *                       representation, the body uses `string_buffer_append_len`.
 * @param chunk_data_len Number of bytes in `chunk_data`.
 * @param userdata       Pointer to the `struct MarkdownRenderState` for this render.
 */
static void markdown_to_html_append_chunk(const MD_CHAR* chunk_data,
                                          MD_SIZE chunk_data_len,
                                          void* userdata);

char* markdown_to_html(const char* markdown, size_t markdown_len, char* err, size_t err_len) {
  if (markdown_len > (size_t)MARKDOWN_INPUT_LEN_MAX) {
    (void)error_report(err, err_len,
                       "Markdown input exceeds max Markdown input length (%zu bytes) at %zu bytes",
                       (size_t)MARKDOWN_INPUT_LEN_MAX, markdown_len);
    return NULL;
  }
  struct MarkdownRenderState render;
  string_buffer_init(&render.buf);
  render.has_failed = false;

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
  char* html = NULL;
  const int rc = md_html(markdown, (MD_SIZE)markdown_len, markdown_to_html_append_chunk, &render,
                         parser_flags, MD_HTML_FLAG_SKIP_UTF8_BOM);
  // This is the one boundary where md4c's error convention becomes this project's. `md_html`
  // returns `0` or `-1`, and that result or a recorded append failure becomes a `NULL` producer
  // return. `md_html` fails only when `md_parse` does, and carries no message of its own, so a
  // parser or allocation failure leaves `err` untouched. The product-cap rejection above is this
  // function's own diagnostic.
  if (rc != 0 || render.has_failed) {
    goto cleanup;
  }
  // An empty document still yields an allocated, terminated buffer to steal.
  html = string_buffer_steal(&render.buf);

cleanup:
  string_buffer_free(&render.buf);
  return html;
}

static void markdown_to_html_append_chunk(const MD_CHAR* chunk_data,
                                          MD_SIZE chunk_data_len,
                                          void* userdata) {
  struct MarkdownRenderState* render = userdata;
  if (string_buffer_append_len(&render->buf, chunk_data, chunk_data_len) != 0) {
    render->has_failed = true;
  }
}
