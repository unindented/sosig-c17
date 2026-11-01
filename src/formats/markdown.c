#include "formats/markdown.h"

#include <assert.h>
#include <md4c.h>
#include <md4c/entity.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ascii.h"
#include "formats/html.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

_Static_assert((uintmax_t)MARKDOWN_INPUT_LEN_MAX <= (uintmax_t)(MD_SIZE)-1,
               "the product cap must stay inside md4c's addressable input range");

/** UTF-8 byte order mark, skipped at the start of a body. */
#define UTF8_BOM S("\xEF\xBB\xBF")

/** UTF-8 encoding of U+FFFD, written for a `NUL` and for a reference to an invalid code point. */
#define UTF8_REPLACEMENT S("\xEF\xBF\xBD")

/** Uppercase hex digits of a percent-encoded URL byte. */
static const char HEX_DIGITS[] = "0123456789ABCDEF";

/**
 * @brief Appends text to a buffer, escaped for the context it is written in.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param text   Text to escape and append.
 */
typedef void EscapeFn(struct Buf* buffer, struct Str text);

/** State of one render. It lives in `markdown_to_html`'s frame. */
struct MarkdownRender {
  /** HTML written so far, growing in `perm`. */
  struct Buf html;

  /**
   * Count of images whose label is being written, zero outside one. An image's label becomes its
   * `alt` attribute, so nothing but its text is written there.
   */
  ptrdiff_t image_depth;
};

/** Opening and closing tags of an element whose detail its HTML does not depend on. */
struct MarkdownTags {
  /** Written when the element starts, or empty. */
  struct Str open;

  /** Written when the element ends, or empty. */
  struct Str close;
};

/**
 * Tags of each block type, by type. A block whose open or close tag depends on its detail leaves
 * that tag empty here and writes it in the callback.
 */
static const struct MarkdownTags block_tags[MD_BLOCK_BLANK + 1] = {
    [MD_BLOCK_QUOTE] = {STR_INIT("<blockquote>\n"), STR_INIT("</blockquote>\n")},
    [MD_BLOCK_UL] = {STR_INIT("<ul>\n"), STR_INIT("</ul>\n")},
    [MD_BLOCK_OL] = {STR_INIT(""), STR_INIT("</ol>\n")},
    [MD_BLOCK_LI] = {STR_INIT(""), STR_INIT("</li>\n")},
    [MD_BLOCK_HR] = {STR_INIT("<hr>\n"), STR_INIT("")},
    [MD_BLOCK_CODE] = {STR_INIT(""), STR_INIT("</code></pre>\n")},
    [MD_BLOCK_P] = {STR_INIT("<p>"), STR_INIT("</p>\n")},
    [MD_BLOCK_TABLE] = {STR_INIT("<table>\n"), STR_INIT("</table>\n")},
    [MD_BLOCK_THEAD] = {STR_INIT("<thead>\n"), STR_INIT("</thead>\n")},
    [MD_BLOCK_TBODY] = {STR_INIT("<tbody>\n"), STR_INIT("</tbody>\n")},
    [MD_BLOCK_TR] = {STR_INIT("<tr>\n"), STR_INIT("</tr>\n")},
    [MD_BLOCK_TH] = {STR_INIT(""), STR_INIT("</th>\n")},
    [MD_BLOCK_TD] = {STR_INIT(""), STR_INIT("</td>\n")},
    [MD_BLOCK_FOOTNOTE_DEF_SECTION] = {STR_INIT("<section class=\"footnotes\">\n<ol>\n"),
                                       STR_INIT("</ol>\n</section>\n")},
    [MD_BLOCK_ADMONITION] = {STR_INIT(""), STR_INIT("</div>\n")},
};

/**
 * Tags of each span type, by type. A span whose open or close tag depends on its detail leaves that
 * tag empty here and writes it in the callback.
 */
static const struct MarkdownTags span_tags[MD_SPAN_MARK + 1] = {
    [MD_SPAN_EM] = {STR_INIT("<em>"), STR_INIT("</em>")},
    [MD_SPAN_STRONG] = {STR_INIT("<strong>"), STR_INIT("</strong>")},
    [MD_SPAN_A] = {STR_INIT(""), STR_INIT("</a>")},
    [MD_SPAN_CODE] = {STR_INIT("<code>"), STR_INIT("</code>")},
    [MD_SPAN_DEL] = {STR_INIT("<del>"), STR_INIT("</del>")},
    [MD_SPAN_LATEXMATH] = {STR_INIT("<x-equation>"), STR_INIT("</x-equation>")},
    [MD_SPAN_LATEXMATH_DISPLAY] = {STR_INIT("<x-equation type=\"display\">"),
                                   STR_INIT("</x-equation>")},
    [MD_SPAN_SPOILER] = {STR_INIT("<x-spoiler>"), STR_INIT("</x-spoiler>")},
    [MD_SPAN_SUPERSCRIPT] = {STR_INIT("<sup>"), STR_INIT("</sup>")},
    [MD_SPAN_SUBSCRIPT] = {STR_INIT("<sub>"), STR_INIT("</sub>")},
    [MD_SPAN_MARK] = {STR_INIT("<mark>"), STR_INIT("</mark>")},
};

/**
 * @brief Writes the HTML that starts a block.
 *
 * This is installed as md4c's `enter_block` callback.
 *
 * @param type     Block type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that ends a block.
 *
 * This is installed as md4c's `leave_block` callback.
 *
 * @param type     Block type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that starts a span.
 *
 * Inside an image's label it writes nothing, because the label is the image's `alt` text.
 *
 * This is installed as md4c's `enter_span` callback.
 *
 * @param type     Span type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_enter_span(MD_SPANTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that ends a span.
 *
 * This is installed as md4c's `leave_span` callback.
 *
 * @param type     Span type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_leave_span(MD_SPANTYPE type, void* detail, void* userdata);

/**
 * @brief Writes a run of text, escaped for where it falls.
 *
 * This is installed as md4c's `text` callback.
 *
 * @param type     Text type.
 * @param text     Text bytes, valid only for the duration of this call.
 * @param size     Number of bytes in `text`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_text(MD_TEXTTYPE type,
                                 const MD_CHAR* text,
                                 MD_SIZE size,
                                 void* userdata);

/**
 * @brief Writes the opening tag of a list item, with a disabled checkbox for a task item.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param item   List item detail. Must not be `NULL`.
 */
static void write_list_item_open(struct Buf* buffer, const MD_BLOCK_LI_DETAIL* item)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the opening tags of a code block, with a `language-` class when it names one.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param code   Code block detail. Must not be `NULL`.
 */
static void write_code_block_open(struct Buf* buffer, const MD_BLOCK_CODE_DETAIL* code)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the opening tag of a table cell, with its alignment.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param tag    Cell tag name, `th` or `td`.
 * @param cell   Cell detail. Must not be `NULL`.
 */
static void write_table_cell_open(struct Buf* buffer,
                                  struct Str tag,
                                  const MD_BLOCK_TD_DETAIL* cell) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the opening of an admonition: its `admonition-<type>` block and its title.
 *
 * @param buffer     Destination buffer. Must not be `NULL`.
 * @param admonition Admonition detail. Must not be `NULL`.
 */
static void write_admonition_open(struct Buf* buffer, const MD_BLOCK_ADMONITION_DETAIL* admonition)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the back-references that end a footnote, one for each reference to it.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param footnote Footnote definition detail. Must not be `NULL`.
 */
static void write_footnote_close(struct Buf* buffer, const MD_BLOCK_FOOTNOTE_DEF_DETAIL* footnote)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the opening tag of a link.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param link   Link detail. Must not be `NULL`.
 */
static void write_link_open(struct Buf* buffer, const MD_SPAN_A_DETAIL* link)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the end of an image's tag, after its `alt` text.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param image  Image detail. Must not be `NULL`.
 */
static void write_image_close(struct Buf* buffer, const MD_SPAN_IMG_DETAIL* image)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes an attribute's text, decoding its entities, escaped by `escape`.
 *
 * @param buffer    Destination buffer. Must not be `NULL`.
 * @param attribute Attribute to write. Must not be `NULL`.
 * @param escape    Escaping for the context the attribute is written in. Must not be `NULL`.
 */
static void write_attribute(struct Buf* buffer, const MD_ATTRIBUTE* attribute, EscapeFn* escape)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes an entity as the UTF-8 text it stands for, escaped by `escape`.
 *
 * A numeric reference gives its code point. A named one is looked up in md4c's table of HTML
 * entities, and an unknown name is written as it is.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param entity Entity as written, from `&` to `;`.
 * @param escape Escaping for the context the entity is written in. Must not be `NULL`.
 */
static void write_entity(struct Buf* buffer, struct Str entity, EscapeFn* escape)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Writes a code point as UTF-8, escaped by `escape`, or U+FFFD when it is not a valid one.
 *
 * @param buffer     Destination buffer. Must not be `NULL`.
 * @param code_point Code point to encode.
 * @param escape     Escaping for the context the text is written in. Must not be `NULL`.
 */
static void write_code_point(struct Buf* buffer, uint32_t code_point, EscapeFn* escape)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Appends text to a URL attribute, percent-encoding every byte a URL may not hold as it is.
 *
 * `&` becomes `&amp;`, so the attribute is still valid HTML.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param text   URL bytes to escape and append.
 */
static void url_escape_append(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a URL may hold a byte as it is.
 *
 * @param c Byte to check.
 * @return `true` for a letter, a digit, or one of `~-_.+!*(),%#@?=;:/$`, `false` otherwise.
 */
static bool is_url_byte_kept(char c);

/**
 * @brief Returns the value of a hexadecimal digit.
 *
 * @param c Hexadecimal digit, in either case.
 * @return The digit's value, from 0 to 15.
 */
static uint32_t hex_digit_value(char c);

struct Str markdown_to_html(struct Str markdown, struct Arena* perm) {
  struct MarkdownRender render = {.html = {.arena = perm}};

  // This names extensions one at a time rather than through an `MD_DIALECT_*` constant, because
  // md4c documents that a dialect constant's meaning changes as it implements more of that dialect.
  // A vendor bump would then silently change how existing content renders. Raw HTML blocks and
  // spans stay enabled (no `MD_FLAG_NOHTML`) because the Markdown is first-party and the content
  // template emits the body unescaped as `{{{body}}}`. Md4c does no sanitizing, so this is a trust
  // decision about who authors content, not an omission. The callbacks write only the block, span
  // and text types these extensions produce.
  const MD_PARSER parser = {
      .flags = MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
               MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS | MD_FLAG_SPOILERS |
               MD_FLAG_SUPERSCRIPTS | MD_FLAG_SUBSCRIPTS | MD_FLAG_ADMONITIONS | MD_FLAG_FOOTNOTES |
               MD_FLAG_HIGHLIGHT,
      .enter_block = markdown_to_html_enter_block,
      .leave_block = markdown_to_html_leave_block,
      .enter_span = markdown_to_html_enter_span,
      .leave_span = markdown_to_html_leave_span,
      .text = markdown_to_html_text,
  };
  // `frontmatter_split` skips a BOM at the start of the file, but one right after the closing fence
  // starts the body, and would otherwise put a stray U+FEFF in the page text.
  if (str_startswith(markdown, UTF8_BOM)) {
    markdown = str_tail(markdown, UTF8_BOM.len);
  }
  // This is the one boundary where md4c's error convention becomes this project's. `md_parse` fails
  // only when its own allocation fails or a callback aborts, and these callbacks never abort, so a
  // failure here is the same condition as `perm` running out.
  assert(markdown.len >= 0 && markdown.len <= MARKDOWN_INPUT_LEN_MAX);
  if (md_parse(markdown.data, (MD_SIZE)markdown.len, &parser, &render) != 0) {
    os_oom();
  }
  return buf_str(&render.html);
}

static int markdown_to_html_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  struct Buf* html = &render->html;
  switch (type) {
    case MD_BLOCK_OL: {
      const MD_BLOCK_OL_DETAIL* list = detail;
      if (list->start == 1) {
        buf_append(html, S("<ol>\n"));
      } else {
        buf_printf(html, "<ol start=\"%u\">\n", list->start);
      }
      break;
    }
    case MD_BLOCK_LI:
      write_list_item_open(html, detail);
      break;
    case MD_BLOCK_H:
      buf_printf(html, "<h%u>", ((const MD_BLOCK_H_DETAIL*)detail)->level);
      break;
    case MD_BLOCK_CODE:
      write_code_block_open(html, detail);
      break;
    case MD_BLOCK_TH:
      write_table_cell_open(html, S("th"), detail);
      break;
    case MD_BLOCK_TD:
      write_table_cell_open(html, S("td"), detail);
      break;
    case MD_BLOCK_FOOTNOTE_DEF:
      buf_printf(html, "<li id=\"fn-%u\">\n", ((const MD_BLOCK_FOOTNOTE_DEF_DETAIL*)detail)->id);
      break;
    case MD_BLOCK_ADMONITION:
      write_admonition_open(html, detail);
      break;
    default:
      buf_append(html, block_tags[type].open);
      break;
  }
  return 0;
}

static int markdown_to_html_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  struct Buf* html = &render->html;
  switch (type) {
    case MD_BLOCK_H:
      buf_printf(html, "</h%u>\n", ((const MD_BLOCK_H_DETAIL*)detail)->level);
      break;
    case MD_BLOCK_FOOTNOTE_DEF:
      write_footnote_close(html, detail);
      break;
    default:
      buf_append(html, block_tags[type].close);
      break;
  }
  return 0;
}

static int markdown_to_html_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  struct Buf* html = &render->html;
  const bool is_inside_image = render->image_depth > 0;
  if (type == MD_SPAN_IMG) {
    render->image_depth++;
  }
  if (is_inside_image) {
    return 0;
  }
  switch (type) {
    case MD_SPAN_A:
      write_link_open(html, detail);
      break;
    case MD_SPAN_IMG:
      buf_append(html, S("<img src=\""));
      write_attribute(html, &((const MD_SPAN_IMG_DETAIL*)detail)->src, url_escape_append);
      buf_append(html, S("\" alt=\""));
      break;
    case MD_SPAN_FOOTNOTE_REF: {
      const MD_SPAN_FOOTNOTE_REF_DETAIL* reference = detail;
      buf_printf(html, "<sup><a href=\"#fn-%u\" id=\"fnref-%u-%u\">%u</a></sup>", reference->id,
                 reference->id, reference->ref_id, reference->id);
      break;
    }
    default:
      buf_append(html, span_tags[type].open);
      break;
  }
  return 0;
}

static int markdown_to_html_leave_span(MD_SPANTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  if (type == MD_SPAN_IMG) {
    render->image_depth--;
  }
  if (render->image_depth > 0) {
    return 0;
  }
  if (type == MD_SPAN_IMG) {
    write_image_close(&render->html, detail);
  } else {
    buf_append(&render->html, span_tags[type].close);
  }
  return 0;
}

static int markdown_to_html_text(MD_TEXTTYPE type,
                                 const MD_CHAR* text,
                                 MD_SIZE size,
                                 void* userdata) {
  struct MarkdownRender* render = userdata;
  struct Buf* html = &render->html;
  const struct Str run = {text, (ptrdiff_t)size};
  const bool is_inside_image = render->image_depth > 0;
  switch (type) {
    case MD_TEXT_NULLCHAR:
      buf_append(html, UTF8_REPLACEMENT);
      break;
    case MD_TEXT_BR:
      buf_append(html, is_inside_image ? S(" ") : S("<br>\n"));
      break;
    case MD_TEXT_SOFTBR:
      buf_append(html, is_inside_image ? S(" ") : S("\n"));
      break;
    case MD_TEXT_HTML:
      // Inside an image's label the text falls into its `alt` attribute, where raw HTML would end
      // the attribute, so it is escaped like any other text there.
      if (is_inside_image) {
        html_escape_append(html, run);
      } else {
        buf_append(html, run);
      }
      break;
    case MD_TEXT_ENTITY:
      write_entity(html, run, html_escape_append);
      break;
    default:
      html_escape_append(html, run);
      break;
  }
  return 0;
}

static void write_list_item_open(struct Buf* buffer, const MD_BLOCK_LI_DETAIL* item) {
  if (!item->is_task) {
    buf_append(buffer, S("<li>"));
    return;
  }
  buf_append(buffer, S("<li class=\"task-list-item\">"
                       "<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled"));
  if (item->task_mark == 'x' || item->task_mark == 'X') {
    buf_append(buffer, S(" checked"));
  }
  buf_append(buffer, S(">"));
}

static void write_code_block_open(struct Buf* buffer, const MD_BLOCK_CODE_DETAIL* code) {
  buf_append(buffer, S("<pre><code"));
  if (code->lang.text != NULL) {
    buf_append(buffer, S(" class=\""));
    if (!str_startswith((struct Str){code->lang.text, (ptrdiff_t)code->lang.size},
                        S("language-"))) {
      buf_append(buffer, S("language-"));
    }
    write_attribute(buffer, &code->lang, html_escape_append);
    buf_append(buffer, S("\""));
  }
  buf_append(buffer, S(">"));
}

static void write_table_cell_open(struct Buf* buffer,
                                  struct Str tag,
                                  const MD_BLOCK_TD_DETAIL* cell) {
  buf_append(buffer, S("<"));
  buf_append(buffer, tag);
  switch (cell->align) {
    case MD_ALIGN_LEFT:
      buf_append(buffer, S(" align=\"left\">"));
      break;
    case MD_ALIGN_CENTER:
      buf_append(buffer, S(" align=\"center\">"));
      break;
    case MD_ALIGN_RIGHT:
      buf_append(buffer, S(" align=\"right\">"));
      break;
    default:
      buf_append(buffer, S(">"));
      break;
  }
}

static void write_admonition_open(struct Buf* buffer,
                                  const MD_BLOCK_ADMONITION_DETAIL* admonition) {
  buf_append(buffer, S("<div class=\"admonition-"));
  write_attribute(buffer, &admonition->type, html_escape_append);
  buf_append(buffer, S("\"><p class=\"admonition-title\">"));
  write_attribute(buffer, &admonition->type, html_escape_append);
  buf_append(buffer, S("</p>"));
}

static void write_footnote_close(struct Buf* buffer, const MD_BLOCK_FOOTNOTE_DEF_DETAIL* footnote) {
  for (unsigned ref_id = 1; ref_id <= footnote->ref_count; ref_id++) {
    if (ref_id > 1) {
      buf_append(buffer, S(" "));
    }
    buf_printf(buffer, "<a href=\"#fnref-%u-%u\" class=\"footnote-backref\">&#8617;</a>",
               footnote->id, ref_id);
  }
  buf_append(buffer, S("\n</li>\n"));
}

static void write_link_open(struct Buf* buffer, const MD_SPAN_A_DETAIL* link) {
  buf_append(buffer, S("<a href=\""));
  write_attribute(buffer, &link->href, url_escape_append);
  if (link->title.text != NULL) {
    buf_append(buffer, S("\" title=\""));
    write_attribute(buffer, &link->title, html_escape_append);
  }
  buf_append(buffer, S("\">"));
}

static void write_image_close(struct Buf* buffer, const MD_SPAN_IMG_DETAIL* image) {
  if (image->title.text != NULL) {
    buf_append(buffer, S("\" title=\""));
    write_attribute(buffer, &image->title, html_escape_append);
  }
  buf_append(buffer, S("\">"));
}

static void write_attribute(struct Buf* buffer, const MD_ATTRIBUTE* attribute, EscapeFn* escape) {
  for (ptrdiff_t i = 0; attribute->substr_offsets[i] < attribute->size; i++) {
    const MD_OFFSET offset = attribute->substr_offsets[i];
    const struct Str text = {attribute->text + offset,
                             (ptrdiff_t)(attribute->substr_offsets[i + 1] - offset)};
    switch (attribute->substr_types[i]) {
      case MD_TEXT_NULLCHAR:
        buf_append(buffer, UTF8_REPLACEMENT);
        break;
      case MD_TEXT_ENTITY:
        write_entity(buffer, text, escape);
        break;
      default:
        escape(buffer, text);
        break;
    }
  }
}

static void write_entity(struct Buf* buffer, struct Str entity, EscapeFn* escape) {
  // md4c reports only well-formed references, `&#` and digits or `&#x` and hex digits, each ending
  // in `;`, or a name.
  if (entity.len > 3 && entity.data[1] == '#') {
    uint32_t code_point = 0;
    if (entity.data[2] == 'x' || entity.data[2] == 'X') {
      for (ptrdiff_t i = 3; i < entity.len - 1; i++) {
        code_point = 16 * code_point + hex_digit_value(entity.data[i]);
      }
    } else {
      for (ptrdiff_t i = 2; i < entity.len - 1; i++) {
        code_point = 10 * code_point + (uint32_t)(entity.data[i] - '0');
      }
    }
    write_code_point(buffer, code_point, escape);
    return;
  }
  const ENTITY* named = entity_lookup(entity.data, (size_t)entity.len);
  if (named == NULL) {
    escape(buffer, entity);
    return;
  }
  write_code_point(buffer, named->codepoints[0], escape);
  if (named->codepoints[1] != 0) {
    write_code_point(buffer, named->codepoints[1], escape);
  }
}

static void write_code_point(struct Buf* buffer, uint32_t code_point, EscapeFn* escape) {
  if (code_point == 0 || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
    escape(buffer, UTF8_REPLACEMENT);
    return;
  }
  char utf8[4] = {0};
  ptrdiff_t len = 0;
  if (code_point <= 0x7F) {
    utf8[0] = (char)code_point;
    len = 1;
  } else if (code_point <= 0x7FF) {
    utf8[0] = (char)(0xC0 | (code_point >> 6));
    utf8[1] = (char)(0x80 | (code_point & 0x3F));
    len = 2;
  } else if (code_point <= 0xFFFF) {
    utf8[0] = (char)(0xE0 | (code_point >> 12));
    utf8[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    utf8[2] = (char)(0x80 | (code_point & 0x3F));
    len = 3;
  } else {
    utf8[0] = (char)(0xF0 | (code_point >> 18));
    utf8[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
    utf8[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
    utf8[3] = (char)(0x80 | (code_point & 0x3F));
    len = 4;
  }
  escape(buffer, (struct Str){utf8, len});
}

static void url_escape_append(struct Buf* buffer, struct Str text) {
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

static uint32_t hex_digit_value(char c) {
  if (ascii_is_digit((unsigned char)c)) {
    return (uint32_t)(c - '0');
  }
  return (uint32_t)(ascii_to_lower((unsigned char)c) - 'a' + 10);
}
