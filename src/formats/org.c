#include "formats/org.h"

#include <assert.h>
#include <org4c-html.h>
#include <org4c.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

_Static_assert((uintmax_t)ORG_INPUT_LEN_MAX <= (uintmax_t)(ORG_SIZE)-1,
               "the product cap must stay inside org4c's addressable input range");

/**
 * @brief Appends one rendered HTML chunk to the render buffer.
 *
 * This is installed as org4c's output callback.
 *
 * @param chunk_data     Rendered HTML bytes for this chunk. A slice into org4c's own buffer: not
 *                       terminated, and valid only for the duration of this call.
 * @param chunk_data_len Number of bytes in `chunk_data`.
 * @param userdata       Pointer to the `struct Buf` the render appends to.
 */
static void org_to_html_append_chunk(const ORG_CHAR* chunk_data,
                                     ORG_SIZE chunk_data_len,
                                     void* userdata);

struct Str org_to_html(struct Str org, struct Arena* perm) {
  struct Buf html = {.arena = perm};

  // Braced sub/superscripts are Org's `^:{}`: `a_{b}` renders while `snake_case` stays text, the
  // usual setting for prose with identifiers in it. A file's own `#+OPTIONS:` line overrides these
  // defaults. Headlines start at `<h1>` (no `ORG_HTML_FLAG_TOPLEVEL_H2`), as Markdown's `#` does.
  // Export blocks and `@@html:...@@` snippets stay enabled (no `ORG_FLAG_NOEXPORTBLOCKS`) for the
  // reason `markdown_to_html` keeps raw HTML: the Org is first-party and the content template emits
  // the body unescaped as `{{{body}}}`. An Org file has no fence ahead of its body, so a BOM at the
  // start of the file reaches the converter, which skips it.
  const unsigned parser_flags = ORG_FLAG_SUBSUPERSCRIPTS_BRACED;
  const unsigned renderer_flags = ORG_HTML_FLAG_IN_BUFFER_OPTIONS | ORG_HTML_FLAG_SKIP_UTF8_BOM;
  // This is the one boundary where org4c's error convention becomes this project's. `org_html`
  // fails only when `org_parse` does, and `org_parse` fails only when its own allocation or the
  // renderer's fails. That is the same condition as `perm` running out.
  assert(org.len >= 0 && org.len <= ORG_INPUT_LEN_MAX);
  if (org_html(org.data, (ORG_SIZE)org.len, org_to_html_append_chunk, &html, parser_flags,
               renderer_flags) != 0) {
    os_oom();
  }
  return buf_str(&html);
}

static void org_to_html_append_chunk(const ORG_CHAR* chunk_data,
                                     ORG_SIZE chunk_data_len,
                                     void* userdata) {
  buf_append(userdata, (struct Str){chunk_data, (ptrdiff_t)chunk_data_len});
}
