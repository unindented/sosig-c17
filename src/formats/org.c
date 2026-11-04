#include "formats/org.h"

#include <assert.h>
#include <org4c.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/ascii.h"
#include "formats/body_html.h"
#include "formats/html.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

_Static_assert((uintmax_t)ORG_INPUT_LEN_MAX <= UINT32_MAX,
               "a code block must fit tree-sitter's 32-bit lengths");
_Static_assert((uintmax_t)ORG_INPUT_LEN_MAX <= (uintmax_t)(ORG_SIZE)-1,
               "the product cap must stay inside org4c's addressable input range");

/** UTF-8 byte order mark, skipped at the start of a file. */
#define UTF8_BOM S("\xEF\xBB\xBF")

/** UTF-8 encoding of U+FFFD, written for a `NUL`. */
#define UTF8_REPLACEMENT S("\xEF\xBF\xBD")

/**
 * Most attributes the `#+ATTR_HTML:` lines of one element keep. Any more are ignored.
 */
enum { ORG_ATTR_HTML_COUNT_MAX = 16 };

/**
 * Export settings a file's `#+OPTIONS:` line can change, beside the parser's flags. Each is off by
 * default, as Emacs exports.
 */
enum OrgExport {
  /** `-:nil`: `--`, `---`, `...` and `\-` stay as written. */
  ORG_EXPORT_SPECIAL_STRINGS_VERBATIM = 1 << 0,

  /** `e:nil`: entities such as `\alpha` stay as written. */
  ORG_EXPORT_ENTITIES_VERBATIM = 1 << 1,

  /** `todo:nil`: headlines drop their TODO keyword. */
  ORG_EXPORT_NO_TODO = 1 << 2,

  /** `pri:nil`: headlines drop their priority cookie. */
  ORG_EXPORT_NO_PRIORITY = 1 << 3,

  /** `tags:nil`: headlines drop their tags. */
  ORG_EXPORT_NO_TAGS = 1 << 4,

  /** `f:nil`: footnote references and definitions render nothing. */
  ORG_EXPORT_NO_FOOTNOTES = 1 << 5,

  /** `d:nil`: drawers render nothing. */
  ORG_EXPORT_NO_DRAWERS = 1 << 6,

  /** `\n:t`: every soft line break is a hard one. */
  ORG_EXPORT_HARD_BREAKS = 1 << 7,

  /** `arch:t`: an archived subtree renders whole, not only its headline. */
  ORG_EXPORT_ARCHIVED_TREES = 1 << 8,
};

/** Parts of an element's affiliated keywords that `write_affiliated_attributes` writes, as bits. */
enum OrgAffiliated {
  /** The `id` from `#+NAME:`, unless `ORG_AFFILIATED_ATTRS` is set and there is an `:id`. */
  ORG_AFFILIATED_ID = 1 << 0,

  /** The attributes of `#+ATTR_HTML:`. */
  ORG_AFFILIATED_ATTRS = 1 << 1,

  /** With `ORG_AFFILIATED_ATTRS`, all but `:src` and `:alt`, which an image writes itself. */
  ORG_AFFILIATED_IMAGE = 1 << 2,

  /** With `ORG_AFFILIATED_ATTRS`, all but `:href`, which a link writes itself. */
  ORG_AFFILIATED_LINK = 1 << 3,
};

/**
 * @brief Appends text to a buffer, escaped for the context it is written in.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param text   Text to escape and append.
 */
typedef void EscapeFn(struct Buf* buffer, struct Str text);

/** One `:key value` attribute of `#+ATTR_HTML:`. Both strings borrow the document. */
struct OrgAttrHtml {
  /** Key, without its leading `:`. */
  struct Str key;

  /** Value: every word up to the next key. */
  struct Str value;

  /** Whether a value follows the key. */
  bool has_value;
};

/** One `#+OPTIONS:` setting that turns a single flag on or off. */
struct OrgOption {
  /** Option name, before its `:`. */
  struct Str key;

  /** Flag the option sets or clears. */
  unsigned flag;

  /** Whether `flag` is one of org4c's parser flags rather than an `enum OrgExport` bit. */
  bool is_parser_flag;

  /** Whether the value `nil` sets the flag, rather than clearing it. */
  bool is_set_by_nil;
};

/**
 * State of one render. It lives in `org_to_html`'s frame. Every string it holds borrows the
 * document, which outlives the parse.
 */
struct OrgRender {
  /** HTML written so far, growing in `perm`. */
  struct Buf html;

  /** Caption of a figure, held in scratch until the image it follows is written. */
  struct Buf caption;

  /** Body of a footnote, held in scratch until its end tells how many paragraphs it has. */
  struct Buf footnote;

  /** Text of a plain source block, held in scratch and written whole when the block ends. */
  struct Buf code;

  /** Region of scratch a code block is highlighted in, in which no buffer of the render grows. */
  struct Arena code_scratch;

  /** Buffer every write goes to: `html`, `footnote` while one is held, or `caption`. */
  struct Buf* output;

  /** `enum OrgExport` bits. */
  unsigned export_flags;

  /** Count of links being skipped, inside an image or a link that writes its own path. */
  ptrdiff_t raw_link_depth;

  /** Count of blocks being skipped, because they are not exported. */
  ptrdiff_t suppress_depth;

  /** Count of verse blocks being written. */
  ptrdiff_t verse_depth;

  /** Count of tables, listings and figures captioned so far, which number the next caption. */
  ptrdiff_t table_count;
  ptrdiff_t listing_count;
  ptrdiff_t figure_count;

  /** Type of the last block entered, which a caption belongs to. */
  ORG_BLOCKTYPE last_block_type;

  /** Language of the pending source block, or empty when it names none. */
  struct Str source_language;

  /** Number of the code block's current line, or zero when its lines are not numbered. */
  ptrdiff_t code_line;

  /** Width of the code block's widest line number, in digits. */
  ptrdiff_t code_line_width;

  /** Length of the caption held for the open figure, or zero when it has none. */
  ptrdiff_t figure_caption_len;

  /** Count of references written in the current citation. */
  ptrdiff_t citation_reference_count;

  /**
   * Count of dashes, two or three, that end the last text and wait to learn whether a line end
   * follows, or zero.
   */
  ptrdiff_t pending_dash_count;

  /** Value of `#+NAME:` for the element being written, or empty. */
  struct Str affiliated_name;

  /** Attributes of `#+ATTR_HTML:` for the element being written. */
  struct OrgAttrHtml affiliated_attrs[ORG_ATTR_HTML_COUNT_MAX];
  ptrdiff_t affiliated_attr_count;

  /** Whether the outermost skipped link is an image, which has no closing tag. */
  bool is_raw_link_image;

  /** Whether the next text starts a verse line, whose leading spaces are kept. */
  bool is_at_verse_line_start;

  /** Whether the text is inside an export snippet for another backend. */
  bool is_in_hidden_snippet;

  /** Whether the text is inside a LaTeX environment. */
  bool is_in_latex_environment;

  /** Whether the text is inside a timestamp. */
  bool is_in_timestamp;

  /** Whether the last block entered is a paragraph holding only a link, which may be a figure. */
  bool is_last_block_standalone_link;

  /** Whether a source block's opening tags wait for its caption, which is its first child. */
  bool is_source_pending;

  /**
   * Whether the source block's text is held in `code`, because nothing so far needs Org's own
   * markup for it: a `#+NAME:`, a caption, line numbers or a code reference.
   */
  bool is_source_held;

  /** Whether the text is inside a footnote's body. */
  bool is_in_footnote;

  /** Whether a paragraph's opening tag waits to learn whether it is a figure. */
  bool is_paragraph_pending;

  /** Whether the open paragraph is a figure. */
  bool is_paragraph_figure;

  /** Whether the text is inside a caption. */
  bool is_in_caption;

  /** Whether the text is inside a source or example block. */
  bool is_in_code_block;

  /** Whether the next code text starts a line, which takes a line number. */
  bool is_at_code_line_start;

  /** Whether the affiliated keywords before an element are being reported. */
  bool is_collecting_affiliated;

  /** Whether the next link is a paragraph's first, which takes the `#+ATTR_HTML:` attributes. */
  bool is_first_link_affiliated;
};

/** Opening and closing tags of an element whose detail its HTML does not depend on. */
struct OrgTags {
  /** Written when the element starts, or empty. */
  struct Str open;

  /** Written when the element ends, or empty. */
  struct Str close;
};

/**
 * Tags of each block type, by type. A block whose open or close tag depends on its detail, its
 * affiliated keywords or the render's state, or one that `body_html` writes because Markdown has it
 * too, leaves that tag empty here and writes it in the callback.
 */
static const struct OrgTags block_tags[ORG_BLOCK_DYNAMIC + 1] = {
    [ORG_BLOCK_QUOTE] = {STR_INIT(""), STR_INIT("</blockquote>\n")},
    [ORG_BLOCK_UL] = {STR_INIT(""), STR_INIT("</ul>\n")},
    [ORG_BLOCK_OL] = {STR_INIT(""), STR_INIT("</ol>\n")},
    [ORG_BLOCK_LI] = {STR_INIT(""), STR_INIT("</li>\n")},
    [ORG_BLOCK_HR] = {STR_INIT("<hr>\n"), STR_INIT("")},
    [ORG_BLOCK_TABLE] = {STR_INIT(""), STR_INIT("</table>\n")},
    [ORG_BLOCK_THEAD] = {STR_INIT("<thead>\n"), STR_INIT("</thead>\n")},
    [ORG_BLOCK_TBODY] = {STR_INIT("<tbody>\n"), STR_INIT("</tbody>\n")},
    [ORG_BLOCK_TR] = {STR_INIT("<tr>\n"), STR_INIT("</tr>\n")},
    [ORG_BLOCK_TH] = {STR_INIT(""), STR_INIT("</th>\n")},
    [ORG_BLOCK_TD] = {STR_INIT(""), STR_INIT("</td>\n")},
    [ORG_BLOCK_DL] = {STR_INIT(""), STR_INIT("</dl>\n")},
    [ORG_BLOCK_DT] = {STR_INIT(""), STR_INIT("</dt>\n")},
    [ORG_BLOCK_DD] = {STR_INIT("<dd>"), STR_INIT("</dd>\n")},
    [ORG_BLOCK_EXAMPLE] = {STR_INIT(""), STR_INIT("</pre>\n")},
    [ORG_BLOCK_VERSE] = {STR_INIT(""), STR_INIT("</p>\n")},
    [ORG_BLOCK_CENTER] = {STR_INIT(""), STR_INIT("</div>\n")},
    [ORG_BLOCK_FIXED_WIDTH] = {STR_INIT(""), STR_INIT("</pre>\n")},
};

/**
 * Tags of each span type, by type. A span whose open or close tag depends on its detail or the
 * render's state leaves that tag empty here and writes it in the callback.
 */
static const struct OrgTags span_tags[ORG_SPAN_CODEREF + 1] = {
    [ORG_SPAN_ITALIC] = {STR_INIT("<em>"), STR_INIT("</em>")},
    [ORG_SPAN_BOLD] = {STR_INIT("<strong>"), STR_INIT("</strong>")},
    [ORG_SPAN_LINK] = {STR_INIT(""), STR_INIT("</a>")},
    [ORG_SPAN_CODE] = {STR_INIT("<code>"), STR_INIT("</code>")},
    [ORG_SPAN_STRIKE] = {STR_INIT("<del>"), STR_INIT("</del>")},
    [ORG_SPAN_UNDERLINE] = {STR_INIT("<span class=\"underline\">"), STR_INIT("</span>")},
    [ORG_SPAN_SUPERSCRIPT] = {STR_INIT("<sup>"), STR_INIT("</sup>")},
    [ORG_SPAN_SUBSCRIPT] = {STR_INIT("<sub>"), STR_INIT("</sub>")},
    [ORG_SPAN_VERBATIM] = {STR_INIT("<code>"), STR_INIT("</code>")},
    [ORG_SPAN_INLINE_SRC] = {STR_INIT(""), STR_INIT("</code>")},
    [ORG_SPAN_STATISTICS_COOKIE] = {STR_INIT("<code>"), STR_INIT("</code>")},
};

/**
 * Names of the special blocks that are admonitions, as Markdown's `> [!NOTE]` is. A block's name
 * matches whatever its case, so `#+BEGIN_NOTE` is a note.
 */
static const struct Str admonition_types[] = {
    STR_INIT("note"),    STR_INIT("tip"),     STR_INIT("important"),
    STR_INIT("warning"), STR_INIT("caution"),
};

/**
 * `#+OPTIONS:` settings that turn one flag on or off. `^:` and `arch:`, which choose among three
 * values, are read apart.
 */
static const struct OrgOption org_options[] = {
    {STR_INIT("tex"), ORG_FLAG_NOLATEX, true, true},
    {STR_INIT("-"), ORG_EXPORT_SPECIAL_STRINGS_VERBATIM, false, true},
    {STR_INIT("e"), ORG_EXPORT_ENTITIES_VERBATIM, false, true},
    {STR_INIT("todo"), ORG_EXPORT_NO_TODO, false, true},
    {STR_INIT("pri"), ORG_EXPORT_NO_PRIORITY, false, true},
    {STR_INIT("tags"), ORG_EXPORT_NO_TAGS, false, true},
    {STR_INIT("f"), ORG_EXPORT_NO_FOOTNOTES, false, true},
    {STR_INIT("d"), ORG_EXPORT_NO_DRAWERS, false, true},
    {STR_INIT("\\n"), ORG_EXPORT_HARD_BREAKS, false, false},
};

/**
 * @brief Applies the `#+OPTIONS:` lines of a document to the flags of its render.
 *
 * The lines are found without parsing, so one inside a block counts too.
 *
 * @param org          Document bytes.
 * @param parser_flags Flags for org4c's parser, updated in place. Must not be `NULL`.
 * @param export_flags `enum OrgExport` bits, updated in place. Must not be `NULL`.
 */
static void apply_in_buffer_options(struct Str org, unsigned* parser_flags, unsigned* export_flags)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Applies one `key:value` setting of `#+OPTIONS:`, ignoring one it does not know.
 *
 * @param option       Setting bytes.
 * @param parser_flags Flags for org4c's parser, updated in place. Must not be `NULL`.
 * @param export_flags `enum OrgExport` bits, updated in place. Must not be `NULL`.
 */
static void apply_option(struct Str option, unsigned* parser_flags, unsigned* export_flags)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Writes the HTML that starts a block, unless the block is not exported.
 *
 * This is installed as org4c's `enter_block` callback.
 *
 * @param type     Block type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct OrgRender`.
 * @return `0`, so the parse continues.
 */
static int org_to_html_enter_block(ORG_BLOCKTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that ends a block, unless the block is not exported.
 *
 * This is installed as org4c's `leave_block` callback.
 *
 * @param type     Block type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct OrgRender`.
 * @return `0`, so the parse continues.
 */
static int org_to_html_leave_block(ORG_BLOCKTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that starts a span.
 *
 * This is installed as org4c's `enter_span` callback.
 *
 * @param type     Span type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct OrgRender`.
 * @return `0`, so the parse continues.
 */
static int org_to_html_enter_span(ORG_SPANTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that ends a span.
 *
 * This is installed as org4c's `leave_span` callback.
 *
 * @param type     Span type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct OrgRender`.
 * @return `0`, so the parse continues.
 */
static int org_to_html_leave_span(ORG_SPANTYPE type, void* detail, void* userdata);

/**
 * @brief Writes a run of text, escaped and translated for where it falls.
 *
 * This is installed as org4c's `text` callback.
 *
 * @param type     Text type.
 * @param text     Text bytes, valid only for the duration of this call.
 * @param size     Number of bytes in `text`.
 * @param userdata Pointer to the `struct OrgRender`.
 * @return `0`, so the parse continues.
 */
static int org_to_html_text(ORG_TEXTTYPE type, const ORG_CHAR* text, ORG_SIZE size, void* userdata);

/**
 * @brief Reports whether a block renders nothing, with everything in it.
 *
 * @param render Render state. Must not be `NULL`.
 * @param type   Block type.
 * @param detail Type-specific detail, or `NULL`.
 * @return `true` when the block is not exported, `false` otherwise.
 */
static bool is_block_suppressed(const struct OrgRender* render,
                                ORG_BLOCKTYPE type,
                                const void* detail) __attribute__((nonnull(1)));

/**
 * @brief Records an affiliated keyword, `#+NAME:` or `#+ATTR_HTML:`, for the element after it.
 *
 * @param render  Render state. Must not be `NULL`.
 * @param keyword Keyword detail. Must not be `NULL`.
 */
static void collect_affiliated_keyword(struct OrgRender* render,
                                       const ORG_BLOCK_KEYWORD_DETAIL* keyword)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Records the `:key value` attributes of one `#+ATTR_HTML:` line.
 *
 * Text before the first key is ignored, and a later value of a key replaces the earlier one, as in
 * Emacs.
 *
 * @param render Render state. Must not be `NULL`.
 * @param value  Value of the `#+ATTR_HTML:` line.
 */
static void collect_attr_html(struct OrgRender* render, struct Str value)
    __attribute__((nonnull(1)));

/**
 * @brief Forgets the affiliated keywords of the last element.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void clear_affiliated(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Writes the `class`, `id` and other attributes an element's affiliated keywords give it.
 *
 * @param render     Render state. Must not be `NULL`.
 * @param base_class Class the element has anyway, or empty. The `:class` of `#+ATTR_HTML:` extends
 *                   it.
 * @param parts      `enum OrgAffiliated` bits naming what to write.
 */
static void write_affiliated_attributes(struct OrgRender* render,
                                        struct Str base_class,
                                        unsigned parts) __attribute__((nonnull(1)));

/**
 * @brief Writes an opening tag with the attributes its element's affiliated keywords give it.
 *
 * @param render     Render state. Must not be `NULL`.
 * @param tag        Tag name.
 * @param base_class Class the element has anyway, or empty.
 * @param parts      `enum OrgAffiliated` bits naming what to write.
 */
static void write_open_tag(struct OrgRender* render,
                           struct Str tag,
                           struct Str base_class,
                           unsigned parts) __attribute__((nonnull(1)));

/**
 * @brief Looks up an attribute of `#+ATTR_HTML:` by its key.
 *
 * @param render Render state. Must not be `NULL`.
 * @param key    Key, without its leading `:`.
 * @return The attribute, or `NULL` when the element has none with that key.
 */
static struct OrgAttrHtml* find_attr_html(struct OrgRender* render, struct Str key)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether an attribute of `#+ATTR_HTML:` is written: it has a value, not `nil`.
 *
 * @param attribute Attribute, or `NULL`.
 * @return `true` when it is written, `false` otherwise.
 */
static bool is_attr_html_written(const struct OrgAttrHtml* attribute);

/**
 * @brief Reports whether a word is a key of `#+ATTR_HTML:`: `:` and letters, digits, `-` or `_`.
 *
 * @param word Word bytes.
 * @return `true` when it is a key, `false` otherwise.
 */
static bool is_attr_html_key(struct Str word);

/**
 * @brief Writes the opening tag of a headline, with its TODO keyword and priority cookie.
 *
 * @param render   Render state. Must not be `NULL`.
 * @param headline Headline detail. Must not be `NULL`.
 */
static void write_headline_open(struct OrgRender* render, const ORG_BLOCK_HEADLINE_DETAIL* headline)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes a headline's tags and its closing tag.
 *
 * @param render   Render state. Must not be `NULL`.
 * @param headline Headline detail. Must not be `NULL`.
 */
static void write_headline_close(struct OrgRender* render,
                                 const ORG_BLOCK_HEADLINE_DETAIL* headline)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the opening tag of a list item or a term, with its counter and its checkbox.
 *
 * @param render Render state. Must not be `NULL`.
 * @param tag    Tag name, `li` or `dt`.
 * @param item   Item detail. Must not be `NULL`.
 */
static void write_list_item_open(struct OrgRender* render,
                                 struct Str tag,
                                 const ORG_BLOCK_LI_DETAIL* item) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the opening tags of a source block that waits for its caption, once it is known.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void flush_pending_source(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Writes the opening tag of a paragraph that waits to learn whether it is a figure.
 *
 * @param render    Render state. Must not be `NULL`.
 * @param is_figure Whether the paragraph is a figure: it opens with an image.
 */
static void flush_pending_paragraph(struct OrgRender* render, bool is_figure)
    __attribute__((nonnull(1)));

/**
 * @brief Writes the end of a paragraph, or of a figure with its caption.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void write_paragraph_close(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Enters a source or example block, and starts its line numbers when it has them.
 *
 * @param render     Render state. Must not be `NULL`.
 * @param first_line Number of the first line, or zero when the lines are not numbered.
 * @param line_count Count of the block's lines.
 */
static void start_code_block(struct OrgRender* render, ptrdiff_t first_line, ptrdiff_t line_count)
    __attribute__((nonnull(1)));

/**
 * @brief Writes the number of a code block's line, when the text starts a numbered line.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void write_code_line_number(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Writes the start of a caption: a table's, a listing's, or a figure's, which is held.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void write_caption_open(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Writes the end of a caption.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void write_caption_close(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Writes the opening tag of a table cell, with its alignment.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param tag    Cell tag name, `th` or `td`.
 * @param cell   Cell detail. Must not be `NULL`.
 */
static void write_table_cell_open(struct Buf* buffer,
                                  struct Str tag,
                                  const ORG_BLOCK_TD_DETAIL* cell) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the opening tag of a link, or a whole image.
 *
 * @param render               Render state. Must not be `NULL`.
 * @param link                 Link detail. Must not be `NULL`.
 * @param has_affiliated_attrs Whether the link takes its paragraph's `#+ATTR_HTML:` attributes.
 */
static void write_link_open(struct OrgRender* render,
                            const ORG_SPAN_LINK_DETAIL* link,
                            bool has_affiliated_attrs) __attribute__((nonnull(1, 2)));

/**
 * @brief Writes an image for a link to one.
 *
 * @param render               Render state. Must not be `NULL`.
 * @param link                 Link detail. Must not be `NULL`.
 * @param has_affiliated_attrs Whether the image takes its paragraph's `#+ATTR_HTML:` attributes.
 */
static void write_image(struct OrgRender* render,
                        const ORG_SPAN_LINK_DETAIL* link,
                        bool has_affiliated_attrs) __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the target of a link, as its `href` or `src` value.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param link   Link detail. Must not be `NULL`.
 */
static void write_link_target(struct Buf* buffer, const ORG_SPAN_LINK_DETAIL* link)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the path of a `file` link, with an Org file's `.org` made `.html`.
 *
 * Of the search options after `::`, only a custom ID (`file:a.org::#id`) is kept, as the fragment
 * `#id`. The others refer to the other file's contents, which the render does not see.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param path   Link path. Must not be `NULL`.
 * @param escape Escaping for the context the path is written in. Must not be `NULL`.
 */
static void write_file_link_path(struct Buf* buffer, const ORG_ATTRIBUTE* path, EscapeFn* escape)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reports whether a link's text is its written path rather than the link as typed.
 *
 * As in Emacs, a `file` link without a description shows the path it points to, so
 * `[[file:notes.org]]` shows `notes.html`.
 *
 * @param link Link detail. Must not be `NULL`.
 * @return `true` for such a link, `false` otherwise.
 */
static bool is_path_text_link(const ORG_SPAN_LINK_DETAIL* link) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a link is an image: a `file`, `http` or `https` link to an image file,
 *        without a description.
 *
 * @param link Link detail. Must not be `NULL`.
 * @return `true` for an image, `false` otherwise.
 */
static bool is_image_link(const ORG_SPAN_LINK_DETAIL* link) __attribute__((nonnull(1)));

/**
 * @brief Writes the start of a citation, with its prefix.
 *
 * @param render   Render state. Must not be `NULL`.
 * @param citation Citation detail. Must not be `NULL`.
 */
static void write_citation_open(struct OrgRender* render, const ORG_SPAN_CITATION_DETAIL* citation)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes one reference of a citation, with its prefix and suffix.
 *
 * @param render    Render state. Must not be `NULL`.
 * @param reference Citation reference detail. Must not be `NULL`.
 */
static void write_citation_reference(struct OrgRender* render,
                                     const ORG_SPAN_CITATION_REFERENCE_DETAIL* reference)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the dashes that ended the last text: as a dash entity when a line end follows, as
 *        written otherwise, as Emacs does.
 *
 * @param render         Render state. Must not be `NULL`.
 * @param is_at_line_end Whether what follows the dashes is a line end.
 */
static void flush_pending_dashes(struct OrgRender* render, bool is_at_line_end)
    __attribute__((nonnull(1)));

/**
 * @brief Writes ordinary text, escaped, with its special strings translated unless `-:nil`.
 *
 * `--` becomes an en dash, `---` an em dash, `...` an ellipsis and `\-` a soft hyphen, as in Emacs.
 * Dashes at the end of the text wait in `pending_dash_count`, because only what follows tells
 * whether they end a line.
 *
 * @param render Render state. Must not be `NULL`.
 * @param text   Text bytes.
 */
static void write_text(struct OrgRender* render, struct Str text) __attribute__((nonnull(1)));

/**
 * @brief Writes a timestamp, escaped, with the `--` of a range as an en dash.
 *
 * @param buffer    Destination buffer. Must not be `NULL`.
 * @param timestamp Timestamp text.
 */
static void write_timestamp(struct Buf* buffer, struct Str timestamp) __attribute__((nonnull(1)));

/**
 * @brief Writes an entity such as `\alpha` or `\alpha{}` as its HTML, or as written when Org does
 *        not know it.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param entity Entity as written.
 */
static void write_entity(struct Buf* buffer, struct Str entity) __attribute__((nonnull(1)));

/**
 * @brief Writes a LaTeX fragment: an inline formula `\(...\)` or `$...$`, or a displayed one
 *        `\[...\]` or `$$...$$`, as `body_html` writes math, and any other fragment as written.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param fragment Fragment as written, delimiters included.
 */
static void write_latex(struct Buf* buffer, struct Str fragment) __attribute__((nonnull(1)));

/**
 * @brief Writes the end of a footnote: its held body, shaped as a Markdown footnote is, and its
 *        back-references.
 *
 * A footnote of one paragraph, the common case, is written without its `<p>`. With several blocks
 * the back-references go inside the last one when it is a paragraph, and after it otherwise.
 *
 * @param render   Render state. Must not be `NULL`.
 * @param footnote Footnote definition detail. Must not be `NULL`.
 */
static void write_footnote_close(struct OrgRender* render,
                                 const ORG_BLOCK_FOOTNOTE_DEF_DETAIL* footnote)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Stops holding a source block's text, and writes it with Org's own markup.
 *
 * @param render Render state. Must not be `NULL`.
 */
static void release_held_source(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Finds the admonition type a special block's name names.
 *
 * @param special  Special block detail. Must not be `NULL`.
 * @param type_out Receives the type in lowercase when the block is an admonition. Must not be
 *                 `NULL`.
 * @return `true` when the block is an admonition, `false` otherwise.
 */
static bool find_admonition_type(const ORG_BLOCK_SPECIAL_DETAIL* special, struct Str* type_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the buffer the body is written to outside a caption: `footnote` while a footnote
 *        is held, and `html` otherwise.
 *
 * @param render Render state. Must not be `NULL`.
 * @return The buffer.
 */
static struct Buf* body_output(struct OrgRender* render) __attribute__((nonnull(1)));

/**
 * @brief Writes a slice of an attribute's text, escaped by `escape`.
 *
 * @param buffer    Destination buffer. Must not be `NULL`.
 * @param attribute Attribute to write. Must not be `NULL`.
 * @param begin     Offset of the slice's first byte.
 * @param end       Offset past the slice's last byte.
 * @param escape    Escaping for the context the attribute is written in. Must not be `NULL`.
 */
static void write_attribute_part(struct Buf* buffer,
                                 const ORG_ATTRIBUTE* attribute,
                                 ptrdiff_t begin,
                                 ptrdiff_t end,
                                 EscapeFn* escape) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Writes an attribute's text, escaped by `escape`.
 *
 * @param buffer    Destination buffer. Must not be `NULL`.
 * @param attribute Attribute to write. Must not be `NULL`.
 * @param escape    Escaping for the context the attribute is written in. Must not be `NULL`.
 */
static void write_attribute(struct Buf* buffer, const ORG_ATTRIBUTE* attribute, EscapeFn* escape)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns an attribute's text, `NUL` substrings included.
 *
 * @param attribute Attribute. Must not be `NULL`.
 * @return A view of its bytes, empty when it is absent.
 */
static struct Str attribute_text(const ORG_ATTRIBUTE* attribute) __attribute__((nonnull(1)));

/**
 * @brief Reports whether an attribute's text equals a lowercase name, folding ASCII case.
 *
 * @param attribute Attribute. Must not be `NULL`.
 * @param name      Lowercase name.
 * @return `true` when they match, `false` otherwise.
 */
static bool is_attribute_named(const ORG_ATTRIBUTE* attribute, struct Str name)
    __attribute__((nonnull(1)));

/**
 * @brief Finds the value of a header argument, such as `:exports`, of a source block.
 *
 * @param params    Everything after the block's language. Must not be `NULL`.
 * @param name      Header argument, `:` included.
 * @param value_out Receives the argument's first word, which may be empty. Must not be `NULL`.
 * @return `true` when the block has the argument, `false` otherwise.
 */
static bool find_header_argument(const ORG_ATTRIBUTE* params,
                                 struct Str name,
                                 struct Str* value_out) __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether a link path names an image file, by its extension in either case.
 *
 * @param path Link path. Must not be `NULL`.
 * @return `true` for an image file, `false` otherwise.
 */
static bool is_image_path(const ORG_ATTRIBUTE* path) __attribute__((nonnull(1)));

/**
 * @brief Reports whether text equals a lowercase name, folding the case of ASCII letters.
 *
 * @param text Text to compare.
 * @param name Lowercase name.
 * @return `true` when they match, `false` otherwise.
 */
static bool is_equal_folded(struct Str text, struct Str name);

/**
 * @brief Reports whether a byte is a blank: a space or a tab.
 *
 * @param c Byte to check.
 * @return `true` for a blank, `false` otherwise.
 */
static bool is_blank(char c);

/**
 * @brief Splits off the first blank-separated word of text, skipping the blanks before it.
 *
 * @param text Text to split.
 * @return The word, the bytes after it, and whether there is a word.
 */
static struct StrCut cut_word(struct Str text);

struct Str org_to_html(struct Str org, struct Arena* perm, struct Arena scratch) {
  // An Org file has no fence ahead of its body, so a BOM at the start of the file reaches the
  // converter. It is skipped before the `#+OPTIONS:` lines are looked for, so one on the first line
  // counts.
  if (str_startswith(org, UTF8_BOM)) {
    org = str_tail(org, UTF8_BOM.len);
  }
  // Braced sub/superscripts are Org's `^:{}`: `a_{b}` renders while `snake_case` stays text, the
  // usual setting for prose with identifiers in it. A file's own `#+OPTIONS:` line overrides these
  // defaults, as it does in Emacs's export, so one file can differ without a config key. Export
  // blocks and `@@html:...@@` snippets stay enabled (no `ORG_FLAG_NOEXPORTBLOCKS`) for the reason
  // `markdown_to_html` keeps raw HTML: the Org is first-party and the content template emits the
  // body unescaped as `{{{body}}}`.
  unsigned parser_flags = ORG_FLAG_SUBSUPERSCRIPTS_BRACED;
  unsigned export_flags = 0;
  apply_in_buffer_options(org, &parser_flags, &export_flags);
  // Commented and `noexport` subtrees never render, so the parser skips them, and the footnotes
  // only they reference are not numbered. So does the body of an archived subtree, unless `arch:t`
  // keeps it.
  parser_flags |= ORG_FLAG_SKIPCOMMENTED | ORG_FLAG_SKIPNOEXPORT;
  if ((export_flags & ORG_EXPORT_ARCHIVED_TREES) == 0) {
    parser_flags |= ORG_FLAG_SKIPARCHIVEDCONTENTS;
  }
  const ORG_PARSER parser = {
      .flags = parser_flags,
      .enter_block = org_to_html_enter_block,
      .leave_block = org_to_html_leave_block,
      .enter_span = org_to_html_enter_span,
      .leave_span = org_to_html_leave_span,
      .text = org_to_html_text,
  };
  struct OrgRender render = {
      .html = {.arena = perm},
      .caption = {.arena = &scratch},
      .footnote = {.arena = &scratch},
      .code = {.arena = &scratch},
      .export_flags = export_flags,
  };
  render.output = &render.html;
  render.code_scratch = body_html_code_block_scratch(&scratch);
  // This is the one boundary where org4c's error convention becomes this project's. `org_parse`
  // fails only when its own allocation fails or a callback aborts, and these callbacks never abort,
  // so a failure here is the same condition as `perm` running out.
  assert(org.len >= 0 && org.len <= ORG_INPUT_LEN_MAX);
  if (org_parse(org.data, (ORG_SIZE)org.len, &parser, &render) != 0) {
    os_oom();
  }
  return buf_str(&render.html);
}

static void apply_in_buffer_options(struct Str org,
                                    unsigned* parser_flags,
                                    unsigned* export_flags) {
  ptrdiff_t line_start = 0;
  while (line_start < org.len) {
    ptrdiff_t line_end = line_start;
    while (line_end < org.len && org.data[line_end] != '\n' && org.data[line_end] != '\r') {
      line_end++;
    }
    struct Str line = str_head(str_tail(org, line_start), line_end - line_start);
    while (line.len > 0 && is_blank(line.data[0])) {
      line = str_tail(line, 1);
    }
    const struct Str keyword = S("#+options:");
    if (line.len >= keyword.len && is_equal_folded(str_head(line, keyword.len), keyword)) {
      const struct Str options = str_tail(line, keyword.len);
      for (struct StrCut option = cut_word(options); option.is_found;
           option = cut_word(option.tail)) {
        apply_option(option.head, parser_flags, export_flags);
      }
    }
    line_start = line_end + 1;
  }
}

static void apply_option(struct Str option, unsigned* parser_flags, unsigned* export_flags) {
  const struct StrCut setting = str_cut(option, ':');
  if (!setting.is_found) {
    return;
  }
  const struct Str key = setting.head;
  const struct Str value = setting.tail;
  bool is_nil = str_eq(value, S("nil"));

  // Archived subtrees: `headline`, the default, `nil`, or anything else for `t`.
  if (str_eq(key, S("arch"))) {
    *parser_flags &= ~(unsigned)ORG_FLAG_SKIPARCHIVED;
    *export_flags &= ~(unsigned)ORG_EXPORT_ARCHIVED_TREES;
    if (is_nil) {
      *parser_flags |= ORG_FLAG_SKIPARCHIVED;
    } else if (!str_eq(value, S("headline"))) {
      *export_flags |= ORG_EXPORT_ARCHIVED_TREES;
    }
    return;
  }
  // Sub/superscripts: `t`, `nil` or `{}`.
  if (str_eq(key, S("^"))) {
    *parser_flags &= ~(unsigned)(ORG_FLAG_SUBSUPERSCRIPTS | ORG_FLAG_SUBSUPERSCRIPTS_BRACED);
    if (str_eq(value, S("{}"))) {
      *parser_flags |= ORG_FLAG_SUBSUPERSCRIPTS_BRACED;
    } else if (!is_nil) {
      *parser_flags |= ORG_FLAG_SUBSUPERSCRIPTS;
    }
    return;
  }
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(org_options) / sizeof(org_options[0])); i++) {
    const struct OrgOption* known = &org_options[i];
    if (!str_eq(key, known->key)) {
      continue;
    }
    // `tex:verbatim` keeps the LaTeX as written, which `tex:nil` does too.
    if (known->flag == ORG_FLAG_NOLATEX && known->is_parser_flag && str_eq(value, S("verbatim"))) {
      is_nil = true;
    }
    unsigned* flags = known->is_parser_flag ? parser_flags : export_flags;
    if (is_nil == known->is_set_by_nil) {
      *flags |= known->flag;
    } else {
      *flags &= ~known->flag;
    }
    return;
  }
}

static int org_to_html_enter_block(ORG_BLOCKTYPE type, void* detail, void* userdata) {
  struct OrgRender* render = userdata;
  // A block after the text of a list item, such as a sub-list, follows a line end.
  flush_pending_dashes(render, true);

  // The affiliated keywords are reported just before the element they belong to, and its caption as
  // its first child.
  if (type == ORG_BLOCK_KEYWORD && ((const ORG_BLOCK_KEYWORD_DETAIL*)detail)->is_affiliated) {
    if (render->suppress_depth == 0) {
      collect_affiliated_keyword(render, detail);
    }
  } else if (type != ORG_BLOCK_CAPTION) {
    if (!render->is_collecting_affiliated) {
      clear_affiliated(render);
    }
    render->is_collecting_affiliated = false;
  }

  if (render->suppress_depth > 0 || is_block_suppressed(render, type, detail)) {
    render->suppress_depth++;
    return 0;
  }

  // Only tables, source blocks and figures show their captions.
  if (type == ORG_BLOCK_CAPTION) {
    if (render->last_block_type != ORG_BLOCK_TABLE && render->last_block_type != ORG_BLOCK_SRC &&
        !(render->last_block_type == ORG_BLOCK_P && render->is_last_block_standalone_link)) {
      render->suppress_depth++;
      return 0;
    }
    write_caption_open(render);
    return 0;
  }

  render->last_block_type = type;
  render->is_last_block_standalone_link = type == ORG_BLOCK_P && detail != NULL &&
                                          ((const ORG_BLOCK_P_DETAIL*)detail)->is_standalone_link;

  switch (type) {
    case ORG_BLOCK_HEADLINE:
      write_headline_open(render, detail);
      break;
    case ORG_BLOCK_P:
      render->is_first_link_affiliated = render->affiliated_attr_count > 0;
      // A paragraph holding only a link may be a figure, which only the link tells.
      if (render->is_last_block_standalone_link) {
        render->is_paragraph_pending = true;
      } else {
        write_open_tag(render, S("p"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      }
      break;
    case ORG_BLOCK_UL:
      write_open_tag(render, S("ul"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      buf_append(render->output, S("\n"));
      break;
    case ORG_BLOCK_OL:
      write_open_tag(render, S("ol"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      buf_append(render->output, S("\n"));
      break;
    case ORG_BLOCK_DL:
      write_open_tag(render, S("dl"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      buf_append(render->output, S("\n"));
      break;
    case ORG_BLOCK_LI:
      write_list_item_open(render, S("li"), detail);
      break;
    case ORG_BLOCK_DT:
      write_list_item_open(render, S("dt"), detail);
      break;
    case ORG_BLOCK_TABLE:
      write_open_tag(render, S("table"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      buf_append(render->output, S("\n"));
      break;
    case ORG_BLOCK_TH:
      write_table_cell_open(render->output, S("th"), detail);
      break;
    case ORG_BLOCK_TD:
      write_table_cell_open(render->output, S("td"), detail);
      break;
    case ORG_BLOCK_SRC: {
      // The opening tags wait for a caption, which is the block's first child.
      const ORG_BLOCK_SRC_DETAIL* source = detail;
      render->is_source_pending = true;
      render->source_language = attribute_text(&source->lang);
      render->is_source_held = render->affiliated_name.len == 0 && source->first_line_number == 0;
      render->code.len = 0;
      start_code_block(render, (ptrdiff_t)source->first_line_number, (ptrdiff_t)source->line_count);
      break;
    }
    case ORG_BLOCK_EXAMPLE: {
      const ORG_BLOCK_EXAMPLE_DETAIL* example = detail;
      write_open_tag(render, S("pre"), S("example"), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      start_code_block(render, (ptrdiff_t)example->first_line_number,
                       (ptrdiff_t)example->line_count);
      break;
    }
    case ORG_BLOCK_FIXED_WIDTH:
      write_open_tag(render, S("pre"), S("example"), ORG_AFFILIATED_ID);
      break;
    case ORG_BLOCK_VERSE:
      write_open_tag(render, S("p"), S("verse"), ORG_AFFILIATED_ID);
      render->verse_depth++;
      render->is_at_verse_line_start = true;
      break;
    case ORG_BLOCK_QUOTE:
      write_open_tag(render, S("blockquote"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      buf_append(render->output, S("\n"));
      break;
    case ORG_BLOCK_CENTER:
      write_open_tag(render, S("div"), S("org-center"), ORG_AFFILIATED_ID);
      buf_append(render->output, S("\n"));
      break;
    case ORG_BLOCK_SPECIAL: {
      struct Str admonition_type = {0};
      if (find_admonition_type(detail, &admonition_type)) {
        body_html_admonition_open(render->output, admonition_type, render->affiliated_name);
        break;
      }
      buf_append(render->output, S("<div"));
      write_affiliated_attributes(render,
                                  attribute_text(&((const ORG_BLOCK_SPECIAL_DETAIL*)detail)->name),
                                  ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
      buf_append(render->output, S(">\n"));
      break;
    }
    case ORG_BLOCK_LATEX_ENVIRONMENT:
      // The whole environment is the formula, as Emacs hands it to MathJax.
      body_html_math_open(render->output, true);
      render->is_in_latex_environment = true;
      break;
    case ORG_BLOCK_FOOTNOTE_DEF_SECTION:
      body_html_footnotes_open(render->output);
      break;
    case ORG_BLOCK_FOOTNOTE_DEF:
      body_html_footnote_open(render->output,
                              (ptrdiff_t)((const ORG_BLOCK_FOOTNOTE_DEF_DETAIL*)detail)->id);
      render->footnote.len = 0;
      render->is_in_footnote = true;
      render->output = &render->footnote;
      break;
    default:
      buf_append(render->output, block_tags[type].open);
      break;
  }
  return 0;
}

static int org_to_html_leave_block(ORG_BLOCKTYPE type, void* detail, void* userdata) {
  struct OrgRender* render = userdata;
  // The end of a paragraph, or of a list item without one, is a line end. The end of a headline, a
  // term, a table cell or a caption is not.
  flush_pending_dashes(render, type != ORG_BLOCK_HEADLINE && type != ORG_BLOCK_DT &&
                                   type != ORG_BLOCK_TH && type != ORG_BLOCK_TD &&
                                   type != ORG_BLOCK_CAPTION);

  if (render->suppress_depth > 0) {
    render->suppress_depth--;
    return 0;
  }

  switch (type) {
    case ORG_BLOCK_HEADLINE:
      write_headline_close(render, detail);
      break;
    case ORG_BLOCK_P:
      write_paragraph_close(render);
      break;
    case ORG_BLOCK_SRC:
      if (render->is_source_held) {
        body_html_code_block_write(render->output, render->source_language, buf_str(&render->code),
                                   render->code_scratch);
        render->is_source_held = false;
        render->is_source_pending = false;
      } else {
        flush_pending_source(render);
        buf_append(render->output, S("</code></pre>\n"));
      }
      render->is_in_code_block = false;
      break;
    case ORG_BLOCK_EXAMPLE:
      buf_append(render->output, block_tags[type].close);
      render->is_in_code_block = false;
      break;
    case ORG_BLOCK_VERSE:
      buf_append(render->output, block_tags[type].close);
      render->verse_depth--;
      break;
    case ORG_BLOCK_SPECIAL: {
      struct Str admonition_type = {0};
      if (find_admonition_type(detail, &admonition_type)) {
        body_html_admonition_close(render->output);
      } else {
        buf_append(render->output, S("</div>\n"));
      }
      break;
    }
    case ORG_BLOCK_LATEX_ENVIRONMENT:
      body_html_math_close(render->output);
      buf_append(render->output, S("\n"));
      render->is_in_latex_environment = false;
      break;
    case ORG_BLOCK_FOOTNOTE_DEF_SECTION:
      body_html_footnotes_close(render->output);
      break;
    case ORG_BLOCK_FOOTNOTE_DEF:
      write_footnote_close(render, detail);
      break;
    case ORG_BLOCK_CAPTION:
      write_caption_close(render);
      break;
    default:
      buf_append(render->output, block_tags[type].close);
      break;
  }
  return 0;
}

static int org_to_html_enter_span(ORG_SPANTYPE type, void* detail, void* userdata) {
  struct OrgRender* render = userdata;
  flush_pending_dashes(render, false);
  if (render->suppress_depth > 0) {
    return 0;
  }
  // Inside an image, or a link that shows its path, the link as typed is not written: the image or
  // the path already has been.
  if (render->raw_link_depth > 0) {
    if (type == ORG_SPAN_LINK) {
      render->raw_link_depth++;
    }
    return 0;
  }

  render->is_at_verse_line_start = false;
  // The only span inside a source block is a code reference, which needs Org's own markup.
  if (render->is_source_held) {
    release_held_source(render);
  }
  flush_pending_source(render);
  flush_pending_paragraph(render, type == ORG_SPAN_LINK && is_image_link(detail));

  switch (type) {
    case ORG_SPAN_EXPORT_SNIPPET:
      if (!is_attribute_named(&((const ORG_SPAN_EXPORT_SNIPPET_DETAIL*)detail)->backend,
                              S("html"))) {
        render->is_in_hidden_snippet = true;
      }
      break;
    case ORG_SPAN_LINK: {
      const ORG_SPAN_LINK_DETAIL* link = detail;
      // Only a paragraph's first link takes its `#+ATTR_HTML:` attributes, as in Emacs, which is
      // how an inline image gets them. A link in a caption is not part of the paragraph.
      const bool has_affiliated_attrs = render->is_first_link_affiliated && !render->is_in_caption;
      if (has_affiliated_attrs) {
        render->is_first_link_affiliated = false;
      }
      write_link_open(render, link, has_affiliated_attrs);
      if (is_image_link(link) || is_path_text_link(link)) {
        render->is_raw_link_image = is_image_link(link);
        render->raw_link_depth++;
      }
      break;
    }
    case ORG_SPAN_FOOTNOTE_REF:
      if ((render->export_flags & ORG_EXPORT_NO_FOOTNOTES) == 0) {
        const ORG_SPAN_FOOTNOTE_REF_DETAIL* reference = detail;
        body_html_footnote_reference_write(render->output, (ptrdiff_t)reference->id,
                                           (ptrdiff_t)reference->ref_id);
      }
      break;
    case ORG_SPAN_TIMESTAMP:
      buf_append(render->output, S("<span class=\"timestamp-wrapper\"><span class=\"timestamp\">"));
      render->is_in_timestamp = true;
      break;
    case ORG_SPAN_INLINE_SRC:
      buf_append(render->output, S("<code class=\"src src-"));
      write_attribute(render->output, &((const ORG_SPAN_INLINE_SRC_DETAIL*)detail)->lang,
                      html_escape_append);
      buf_append(render->output, S("\">"));
      break;
    case ORG_SPAN_TARGET: {
      // A radio target holds its text, while a plain one is empty.
      const ORG_SPAN_TARGET_DETAIL* target = detail;
      buf_append(render->output, S("<a id=\""));
      write_attribute(render->output, &target->name, html_escape_append);
      buf_append(render->output, target->is_radio ? S("\">") : S("\"></a>"));
      break;
    }
    case ORG_SPAN_CITATION:
      write_citation_open(render, detail);
      break;
    case ORG_SPAN_CITATION_REFERENCE:
      write_citation_reference(render, detail);
      break;
    case ORG_SPAN_CODEREF:
      buf_append(render->output, S("<span id=\"coderef-"));
      write_attribute(render->output, &((const ORG_SPAN_CODEREF_DETAIL*)detail)->name,
                      html_escape_append);
      buf_append(render->output, S("\" class=\"coderef-off\">"));
      write_code_line_number(render);
      break;
    default:
      buf_append(render->output, span_tags[type].open);
      break;
  }
  return 0;
}

static int org_to_html_leave_span(ORG_SPANTYPE type, void* detail, void* userdata) {
  struct OrgRender* render = userdata;
  flush_pending_dashes(render, false);
  if (render->suppress_depth > 0) {
    return 0;
  }
  if (type == ORG_SPAN_EXPORT_SNIPPET) {
    render->is_in_hidden_snippet = false;
    return 0;
  }
  if (render->raw_link_depth > 0) {
    if (type == ORG_SPAN_LINK) {
      render->raw_link_depth--;
      if (render->raw_link_depth == 0 && !render->is_raw_link_image) {
        buf_append(render->output, S("</a>"));
      }
    }
    return 0;
  }

  switch (type) {
    case ORG_SPAN_TIMESTAMP:
      buf_append(render->output, S("</span></span>"));
      render->is_in_timestamp = false;
      break;
    case ORG_SPAN_TARGET:
      if (((const ORG_SPAN_TARGET_DETAIL*)detail)->is_radio) {
        buf_append(render->output, S("</a>"));
      }
      break;
    case ORG_SPAN_CITATION: {
      const ORG_SPAN_CITATION_DETAIL* citation = detail;
      if (citation->suffix.size > 0) {
        buf_append(render->output, S(" "));
        write_attribute(render->output, &citation->suffix, html_escape_append);
      }
      buf_append(render->output, S(")</span>"));
      break;
    }
    case ORG_SPAN_CODEREF: {
      const ORG_SPAN_CODEREF_DETAIL* coderef = detail;
      if (coderef->is_label_retained) {
        buf_append(render->output, S(" ("));
        write_attribute(render->output, &coderef->name, html_escape_append);
        buf_append(render->output, S(")"));
      }
      buf_append(render->output, S("</span>"));
      break;
    }
    default:
      buf_append(render->output, span_tags[type].close);
      break;
  }
  return 0;
}

static int org_to_html_text(ORG_TEXTTYPE type,
                            const ORG_CHAR* text,
                            ORG_SIZE size,
                            void* userdata) {
  struct OrgRender* render = userdata;
  struct Str run = {text, (ptrdiff_t)size};
  // Outside a verse, a hard break is `\\` at the end of a line, so an object follows the text. In a
  // verse it may be a line end too, which the callback cannot tell apart.
  flush_pending_dashes(render, type == ORG_TEXT_SOFTBR ||
                                   (type == ORG_TEXT_BR && render->verse_depth > 0) ||
                                   (type == ORG_TEXT_NORMAL && run.len > 0 && run.data[0] != '-'));
  if (render->suppress_depth > 0 || render->raw_link_depth > 0 || render->is_in_hidden_snippet) {
    return 0;
  }
  if (render->is_source_held) {
    buf_append(&render->code, type == ORG_TEXT_NULLCHAR ? UTF8_REPLACEMENT : run);
    return 0;
  }
  flush_pending_source(render);
  flush_pending_paragraph(render, false);

  if (render->is_in_code_block && type == ORG_TEXT_CODE) {
    write_code_line_number(render);
    if (str_eq(run, S("\n"))) {
      if (render->code_line > 0) {
        render->code_line++;
      }
      render->is_at_code_line_start = true;
    }
  }
  // A verse line keeps its indentation.
  if (render->is_at_verse_line_start && type == ORG_TEXT_NORMAL) {
    while (run.len > 0 && run.data[0] == ' ') {
      buf_append(render->output, S("&#xa0;"));
      run = str_tail(run, 1);
    }
  }
  render->is_at_verse_line_start = false;

  struct Buf* output = render->output;
  switch (type) {
    case ORG_TEXT_NULLCHAR:
      buf_append(output, UTF8_REPLACEMENT);
      break;
    case ORG_TEXT_BR:
      buf_append(output, S("<br>\n"));
      if (render->verse_depth > 0) {
        render->is_at_verse_line_start = true;
      }
      break;
    case ORG_TEXT_SOFTBR:
      buf_append(output,
                 (render->export_flags & ORG_EXPORT_HARD_BREAKS) != 0 ? S("<br>\n") : S("\n"));
      break;
    case ORG_TEXT_EXPORT:
      buf_append(output, run);
      break;
    case ORG_TEXT_ENTITY:
      if ((render->export_flags & ORG_EXPORT_ENTITIES_VERBATIM) != 0) {
        html_escape_append(output, run);
      } else {
        write_entity(output, run);
      }
      break;
    case ORG_TEXT_NORMAL:
      if (render->is_in_timestamp) {
        write_timestamp(output, run);
      } else {
        write_text(render, run);
      }
      break;
    case ORG_TEXT_LATEX:
      if (render->is_in_latex_environment) {
        html_escape_append(output, run);
      } else {
        write_latex(output, run);
      }
      break;
    default:
      html_escape_append(output, run);
      break;
  }
  return 0;
}

static bool is_block_suppressed(const struct OrgRender* render,
                                ORG_BLOCKTYPE type,
                                const void* detail) {
  switch (type) {
    case ORG_BLOCK_EXPORT:
      return !is_attribute_named(&((const ORG_BLOCK_EXPORT_DETAIL*)detail)->backend, S("html"));
    case ORG_BLOCK_SRC: {
      // Org Babel's `:exports`. Code is never evaluated, so `results` exports nothing either.
      struct Str exports = {0};
      return find_header_argument(&((const ORG_BLOCK_SRC_DETAIL*)detail)->params, S(":exports"),
                                  &exports) &&
             (is_equal_folded(exports, S("none")) || is_equal_folded(exports, S("results")));
    }
    case ORG_BLOCK_DRAWER:
      // Org does not export a `LOGBOOK` drawer by default.
      return (render->export_flags & ORG_EXPORT_NO_DRAWERS) != 0 ||
             is_attribute_named(&((const ORG_BLOCK_DRAWER_DETAIL*)detail)->name, S("logbook"));
    case ORG_BLOCK_FOOTNOTE_DEF_SECTION:
      return (render->export_flags & ORG_EXPORT_NO_FOOTNOTES) != 0;
    case ORG_BLOCK_SECTION:
      // As in Emacs, the `Footnotes` section renders nothing. Its definitions are written in the
      // footnote section at the end anyway.
      return ((const ORG_BLOCK_SECTION_DETAIL*)detail)->is_footnote_section;
    case ORG_BLOCK_COMMENT:
    case ORG_BLOCK_PROPERTY_DRAWER:
    case ORG_BLOCK_PLANNING:
    case ORG_BLOCK_KEYWORD:
      return true;
    default:
      return false;
  }
}

static void collect_affiliated_keyword(struct OrgRender* render,
                                       const ORG_BLOCK_KEYWORD_DETAIL* keyword) {
  if (!render->is_collecting_affiliated) {
    clear_affiliated(render);
    render->is_collecting_affiliated = true;
  }
  // Of several `#+NAME:` lines, the last one applies, as in Emacs.
  if (is_attribute_named(&keyword->key, S("name"))) {
    render->affiliated_name = attribute_text(&keyword->value);
  } else if (is_attribute_named(&keyword->key, S("attr_html"))) {
    collect_attr_html(render, attribute_text(&keyword->value));
  }
}

static void collect_attr_html(struct OrgRender* render, struct Str value) {
  struct OrgAttrHtml* attribute = NULL;
  for (struct StrCut word = cut_word(value); word.is_found; word = cut_word(word.tail)) {
    if (is_attr_html_key(word.head)) {
      const struct Str key = str_tail(word.head, 1);
      attribute = find_attr_html(render, key);
      if (attribute == NULL && render->affiliated_attr_count < ORG_ATTR_HTML_COUNT_MAX) {
        attribute = &render->affiliated_attrs[render->affiliated_attr_count];
        render->affiliated_attr_count++;
        attribute->key = key;
      }
      if (attribute != NULL) {
        attribute->has_value = false;
      }
    } else if (attribute != NULL) {
      // The value runs over every word up to the next key.
      if (!attribute->has_value) {
        attribute->value = word.head;
        attribute->has_value = true;
      }
      attribute->value.len = word.head.data + word.head.len - attribute->value.data;
    }
  }
}

static void clear_affiliated(struct OrgRender* render) {
  render->affiliated_name = (struct Str){0};
  render->affiliated_attr_count = 0;
}

static void write_affiliated_attributes(struct OrgRender* render,
                                        struct Str base_class,
                                        unsigned parts) {
  struct Buf* output = render->output;
  const struct OrgAttrHtml* class_attribute = NULL;
  if ((parts & ORG_AFFILIATED_ATTRS) != 0) {
    class_attribute = find_attr_html(render, S("class"));
  }
  if (!is_attr_html_written(class_attribute)) {
    class_attribute = NULL;
  }
  if (base_class.len > 0 || class_attribute != NULL) {
    buf_append(output, S(" class=\""));
    html_escape_append(output, base_class);
    if (base_class.len > 0 && class_attribute != NULL) {
      buf_append(output, S(" "));
    }
    if (class_attribute != NULL) {
      html_escape_append(output, class_attribute->value);
    }
    buf_append(output, S("\""));
  }

  // The `:id` of `#+ATTR_HTML:` takes precedence over `#+NAME:`. A link to a named element points
  // to `#NAME`, so the element gets the id even where Emacs gives it none, such as a paragraph.
  if ((parts & ORG_AFFILIATED_ID) != 0 &&
      ((parts & ORG_AFFILIATED_ATTRS) == 0 || find_attr_html(render, S("id")) == NULL) &&
      render->affiliated_name.len > 0) {
    buf_append(output, S(" id=\""));
    html_escape_append(output, render->affiliated_name);
    buf_append(output, S("\""));
  }

  if ((parts & ORG_AFFILIATED_ATTRS) == 0) {
    return;
  }
  for (ptrdiff_t i = 0; i < render->affiliated_attr_count; i++) {
    const struct OrgAttrHtml* attribute = &render->affiliated_attrs[i];
    const bool is_written_apart =
        str_eq(attribute->key, S("class")) ||
        ((parts & ORG_AFFILIATED_IMAGE) != 0 &&
         (str_eq(attribute->key, S("src")) || str_eq(attribute->key, S("alt")))) ||
        ((parts & ORG_AFFILIATED_LINK) != 0 && str_eq(attribute->key, S("href")));
    if (!is_attr_html_written(attribute) || is_written_apart) {
      continue;
    }
    buf_append(output, S(" "));
    buf_append(output, attribute->key);
    buf_append(output, S("=\""));
    html_escape_append(output, attribute->value);
    buf_append(output, S("\""));
  }
}

static void write_open_tag(struct OrgRender* render,
                           struct Str tag,
                           struct Str base_class,
                           unsigned parts) {
  buf_append(render->output, S("<"));
  buf_append(render->output, tag);
  write_affiliated_attributes(render, base_class, parts);
  buf_append(render->output, S(">"));
}

static struct OrgAttrHtml* find_attr_html(struct OrgRender* render, struct Str key) {
  for (ptrdiff_t i = 0; i < render->affiliated_attr_count; i++) {
    if (str_eq(render->affiliated_attrs[i].key, key)) {
      return &render->affiliated_attrs[i];
    }
  }
  return NULL;
}

static bool is_attr_html_written(const struct OrgAttrHtml* attribute) {
  return attribute != NULL && attribute->has_value && !str_eq(attribute->value, S("nil"));
}

static bool is_attr_html_key(struct Str word) {
  if (word.len < 2 || word.data[0] != ':') {
    return false;
  }
  for (ptrdiff_t i = 1; i < word.len; i++) {
    const unsigned char c = (unsigned char)word.data[i];
    if (!ascii_is_alphanumeric(c) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

static void write_headline_open(struct OrgRender* render,
                                const ORG_BLOCK_HEADLINE_DETAIL* headline) {
  struct Buf* output = render->output;
  body_html_heading_open(output, (ptrdiff_t)headline->level);
  if (headline->todo.size > 0 && (render->export_flags & ORG_EXPORT_NO_TODO) == 0) {
    buf_append(output, headline->is_done ? S("<span class=\"done ") : S("<span class=\"todo "));
    write_attribute(output, &headline->todo, html_escape_append);
    buf_append(output, S("\">"));
    write_attribute(output, &headline->todo, html_escape_append);
    buf_append(output, S("</span> "));
  }
  if (headline->priority != 0 && (render->export_flags & ORG_EXPORT_NO_PRIORITY) == 0) {
    buf_append(output, S("<span class=\"priority\">["));
    html_escape_append(output, (struct Str){&headline->priority, 1});
    buf_append(output, S("]</span> "));
  }
}

static void write_headline_close(struct OrgRender* render,
                                 const ORG_BLOCK_HEADLINE_DETAIL* headline) {
  struct Buf* output = render->output;
  if (headline->tags.size > 0 && (render->export_flags & ORG_EXPORT_NO_TAGS) == 0) {
    buf_append(output, S("&#xa0;&#xa0;&#xa0;<span class=\"tag\">"));
    bool is_first = true;
    struct Str tags = attribute_text(&headline->tags);
    while (tags.len > 0) {
      const struct StrCut tag = str_cut(tags, ':');
      tags = tag.tail;
      if (tag.head.len == 0) {
        continue;
      }
      if (!is_first) {
        buf_append(output, S("&#xa0;"));
      }
      buf_append(output, S("<span class=\""));
      html_escape_append(output, tag.head);
      buf_append(output, S("\">"));
      html_escape_append(output, tag.head);
      buf_append(output, S("</span>"));
      is_first = false;
    }
    buf_append(output, S("</span>"));
  }
  body_html_heading_close(output, (ptrdiff_t)headline->level);
}

static void write_list_item_open(struct OrgRender* render,
                                 struct Str tag,
                                 const ORG_BLOCK_LI_DETAIL* item) {
  struct Buf* output = render->output;
  buf_append(output, S("<"));
  buf_append(output, tag);
  if (item->counter > 0) {
    buf_printf(output, " value=\"%u\"", item->counter);
  }
  buf_append(output, S(">"));
  if (item->checkbox != 0) {
    body_html_checkbox_write(output, item->checkbox == 'X');
  }
}

static void flush_pending_source(struct OrgRender* render) {
  if (!render->is_source_pending || render->is_in_caption) {
    return;
  }
  render->is_source_pending = false;
  write_open_tag(render, S("pre"), S(""), ORG_AFFILIATED_ID);
  buf_append(render->output, S("<code"));
  // The class follows the rule of `body_html_code_block_write`, so a block reads the same whether
  // or not its name makes it take this path.
  if (render->source_language.len > 0) {
    buf_append(render->output, S(" class=\""));
    if (!str_startswith(render->source_language, S("language-"))) {
      buf_append(render->output, S("language-"));
    }
    html_escape_append(render->output, render->source_language);
    buf_append(render->output, S("\""));
  }
  buf_append(render->output, S(">"));
}

static void flush_pending_paragraph(struct OrgRender* render, bool is_figure) {
  if (!render->is_paragraph_pending || render->output == &render->caption) {
    return;
  }
  render->is_paragraph_pending = false;
  render->is_paragraph_figure = is_figure;
  if (is_figure) {
    // The `#+ATTR_HTML:` attributes go to the image.
    write_open_tag(render, S("div"), S("figure"), ORG_AFFILIATED_ID);
    buf_append(render->output, S("\n<p>"));
  } else {
    write_open_tag(render, S("p"), S(""), ORG_AFFILIATED_ID | ORG_AFFILIATED_ATTRS);
  }
}

static void write_paragraph_close(struct OrgRender* render) {
  flush_pending_paragraph(render, false);
  struct Buf* output = render->output;
  if (render->is_paragraph_figure) {
    buf_append(output, S("\n</p>\n"));
    if (render->figure_caption_len > 0) {
      render->figure_count++;
      buf_printf(output, "<p><span class=\"figure-number\">Figure %td: </span>",
                 render->figure_count);
      buf_append(output, str_head(buf_str(&render->caption), render->figure_caption_len));
      buf_append(output, S("</p>\n"));
    }
    buf_append(output, S("</div>\n"));
  } else {
    buf_append(output, S("</p>\n"));
  }
  render->is_paragraph_figure = false;
  render->figure_caption_len = 0;
  render->is_first_link_affiliated = false;
}

static void start_code_block(struct OrgRender* render, ptrdiff_t first_line, ptrdiff_t line_count) {
  render->is_in_code_block = true;
  render->code_line = first_line;
  render->is_at_code_line_start = true;
  if (first_line > 0) {
    ptrdiff_t last_line = first_line + (line_count > 0 ? line_count - 1 : 0);
    render->code_line_width = 1;
    while (last_line >= 10) {
      last_line /= 10;
      render->code_line_width++;
    }
  }
}

static void write_code_line_number(struct OrgRender* render) {
  if (!render->is_in_code_block || !render->is_at_code_line_start) {
    return;
  }
  render->is_at_code_line_start = false;
  if (render->code_line > 0) {
    // A `printf` field width is an `int`, and a line count fits one.
    buf_printf(render->output, "<span class=\"linenr\">%*td: </span>", (int)render->code_line_width,
               render->code_line);
  }
}

static void write_caption_open(struct OrgRender* render) {
  render->is_in_caption = true;
  switch (render->last_block_type) {
    case ORG_BLOCK_TABLE:
      render->table_count++;
      buf_printf(render->output,
                 "<caption class=\"t-above\"><span class=\"table-number\">Table %td:</span> ",
                 render->table_count);
      break;
    case ORG_BLOCK_SRC:
      render->is_source_held = false;
      render->listing_count++;
      buf_printf(render->output,
                 "<label class=\"org-src-name\"><span class=\"listing-number\">Listing %td: "
                 "</span>",
                 render->listing_count);
      break;
    case ORG_BLOCK_P:
      // A figure's caption follows its image, so it is held until the paragraph ends.
      render->caption.len = 0;
      render->output = &render->caption;
      break;
    default:
      break;
  }
}

static void write_caption_close(struct OrgRender* render) {
  render->is_in_caption = false;
  switch (render->last_block_type) {
    case ORG_BLOCK_TABLE:
      buf_append(render->output, S("</caption>\n"));
      break;
    case ORG_BLOCK_SRC:
      buf_append(render->output, S("</label>"));
      flush_pending_source(render);
      break;
    case ORG_BLOCK_P:
      render->output = body_output(render);
      render->figure_caption_len = render->caption.len;
      break;
    default:
      break;
  }
}

static void write_table_cell_open(struct Buf* buffer,
                                  struct Str tag,
                                  const ORG_BLOCK_TD_DETAIL* cell) {
  buf_append(buffer, S("<"));
  buf_append(buffer, tag);
  switch (cell->align) {
    case ORG_ALIGN_LEFT:
      buf_append(buffer, S(" align=\"left\">"));
      break;
    case ORG_ALIGN_CENTER:
      buf_append(buffer, S(" align=\"center\">"));
      break;
    case ORG_ALIGN_RIGHT:
      buf_append(buffer, S(" align=\"right\">"));
      break;
    default:
      buf_append(buffer, S(">"));
      break;
  }
}

static void write_link_open(struct OrgRender* render,
                            const ORG_SPAN_LINK_DETAIL* link,
                            bool has_affiliated_attrs) {
  if (is_image_link(link)) {
    write_image(render, link, has_affiliated_attrs);
    return;
  }
  struct Buf* output = render->output;
  buf_append(output, S("<a href=\""));
  write_link_target(output, link);
  buf_append(output, S("\""));
  write_affiliated_attributes(
      render, is_attribute_named(&link->type, S("coderef")) ? S("coderef") : S(""),
      has_affiliated_attrs ? ORG_AFFILIATED_ATTRS | ORG_AFFILIATED_LINK : 0);
  buf_append(output, S(">"));
  if (is_path_text_link(link)) {
    write_file_link_path(output, &link->path, html_escape_append);
  }
}

static void write_image(struct OrgRender* render,
                        const ORG_SPAN_LINK_DETAIL* link,
                        bool has_affiliated_attrs) {
  struct Buf* output = render->output;
  const struct OrgAttrHtml* source = has_affiliated_attrs ? find_attr_html(render, S("src")) : NULL;
  const struct OrgAttrHtml* alternative =
      has_affiliated_attrs ? find_attr_html(render, S("alt")) : NULL;
  buf_append(output, S("<img"));
  if (source == NULL) {
    buf_append(output, S(" src=\""));
    write_link_target(output, link);
    buf_append(output, S("\""));
  } else if (is_attr_html_written(source)) {
    buf_append(output, S(" src=\""));
    html_escape_append(output, source->value);
    buf_append(output, S("\""));
  }
  if (alternative == NULL) {
    // The default text is the file's name.
    const struct Str path = attribute_text(&link->path);
    ptrdiff_t name_start = path.len;
    while (name_start > 0 && path.data[name_start - 1] != '/') {
      name_start--;
    }
    buf_append(output, S(" alt=\""));
    write_attribute_part(output, &link->path, name_start, path.len, html_escape_append);
    buf_append(output, S("\""));
  } else if (is_attr_html_written(alternative)) {
    buf_append(output, S(" alt=\""));
    html_escape_append(output, alternative->value);
    buf_append(output, S("\""));
  }
  write_affiliated_attributes(
      render, S(""), has_affiliated_attrs ? ORG_AFFILIATED_ATTRS | ORG_AFFILIATED_IMAGE : 0);
  buf_append(output, S(">"));
}

static void write_link_target(struct Buf* buffer, const ORG_SPAN_LINK_DETAIL* link) {
  if (is_attribute_named(&link->type, S("file"))) {
    write_file_link_path(buffer, &link->path, body_html_url_escape_append);
  } else if (is_attribute_named(&link->type, S("coderef"))) {
    buf_append(buffer, S("#coderef-"));
    write_attribute(buffer, &link->path, body_html_url_escape_append);
  } else if (is_attribute_named(&link->type, S("custom-id")) ||
             is_attribute_named(&link->type, S("radio"))) {
    buf_append(buffer, S("#"));
    write_attribute(buffer, &link->path, body_html_url_escape_append);
  } else if (is_attribute_named(&link->type, S("id"))) {
    buf_append(buffer, S("#ID-"));
    write_attribute(buffer, &link->path, body_html_url_escape_append);
  } else if (is_attribute_named(&link->type, S("fuzzy"))) {
    // `*Heading` links to a headline.
    const struct Str path = attribute_text(&link->path);
    const ptrdiff_t begin = str_startswith(path, S("*")) ? 1 : 0;
    buf_append(buffer, S("#"));
    write_attribute_part(buffer, &link->path, begin, path.len, body_html_url_escape_append);
  } else if (is_attribute_named(&link->type, S("doi"))) {
    buf_append(buffer, S("https://doi.org/"));
    write_attribute(buffer, &link->path, body_html_url_escape_append);
  } else {
    write_attribute(buffer, &link->type, body_html_url_escape_append);
    buf_append(buffer, S(":"));
    write_attribute(buffer, &link->path, body_html_url_escape_append);
  }
}

static void write_file_link_path(struct Buf* buffer, const ORG_ATTRIBUTE* path, EscapeFn* escape) {
  const struct Str text = attribute_text(path);
  ptrdiff_t file_len = text.len;
  for (ptrdiff_t i = 0; i + 1 < text.len; i++) {
    if (text.data[i] == ':' && text.data[i + 1] == ':') {
      file_len = i;
      break;
    }
  }
  if (file_len > 4 && str_endswith(str_head(text, file_len), S(".org"))) {
    write_attribute_part(buffer, path, 0, file_len - 4, escape);
    buf_append(buffer, S(".html"));
  } else {
    write_attribute_part(buffer, path, 0, file_len, escape);
  }
  if (file_len + 2 < text.len && text.data[file_len + 2] == '#') {
    write_attribute_part(buffer, path, file_len + 2, text.len, escape);
  }
}

static bool is_path_text_link(const ORG_SPAN_LINK_DETAIL* link) {
  return !link->has_description && is_attribute_named(&link->type, S("file"));
}

static bool is_image_link(const ORG_SPAN_LINK_DETAIL* link) {
  return !link->has_description &&
         (is_attribute_named(&link->type, S("file")) ||
          is_attribute_named(&link->type, S("http")) ||
          is_attribute_named(&link->type, S("https"))) &&
         is_image_path(&link->path);
}

static void write_citation_open(struct OrgRender* render,
                                const ORG_SPAN_CITATION_DETAIL* citation) {
  buf_append(render->output, S("<span class=\"citation\">("));
  if (citation->prefix.size > 0) {
    write_attribute(render->output, &citation->prefix, html_escape_append);
    buf_append(render->output, S(" "));
  }
  render->citation_reference_count = 0;
}

static void write_citation_reference(struct OrgRender* render,
                                     const ORG_SPAN_CITATION_REFERENCE_DETAIL* reference) {
  struct Buf* output = render->output;
  if (render->citation_reference_count > 0) {
    buf_append(output, S("; "));
  }
  render->citation_reference_count++;
  if (reference->prefix.size > 0) {
    write_attribute(output, &reference->prefix, html_escape_append);
    buf_append(output, S(" "));
  }
  buf_append(output, S("<cite>"));
  write_attribute(output, &reference->key, html_escape_append);
  buf_append(output, S("</cite>"));
  if (reference->suffix.size > 0) {
    buf_append(output, S(" "));
    write_attribute(output, &reference->suffix, html_escape_append);
  }
}

static void flush_pending_dashes(struct OrgRender* render, bool is_at_line_end) {
  const ptrdiff_t dash_count = render->pending_dash_count;
  if (dash_count == 0) {
    return;
  }
  render->pending_dash_count = 0;
  if (is_at_line_end) {
    buf_append(render->output, dash_count == 3 ? S("&mdash;") : S("&ndash;"));
  } else {
    buf_append(render->output, dash_count == 3 ? S("---") : S("--"));
  }
}

static void write_text(struct OrgRender* render, struct Str text) {
  struct Buf* output = render->output;
  if ((render->export_flags & ORG_EXPORT_SPECIAL_STRINGS_VERBATIM) != 0) {
    html_escape_append(output, text);
    return;
  }
  ptrdiff_t run_start = 0;
  ptrdiff_t i = 0;
  while (i < text.len) {
    const struct Str rest = str_tail(text, i);
    struct Str special = {0};
    ptrdiff_t special_len = 0;
    if (str_startswith(rest, S("\\-"))) {
      special = S("&shy;");
      special_len = 2;
    } else if (str_startswith(rest, S("---")) && !str_startswith(rest, S("----"))) {
      special = S("&mdash;");
      special_len = 3;
    } else if (str_startswith(rest, S("--")) && !str_startswith(rest, S("---"))) {
      special = S("&ndash;");
      special_len = 2;
    } else if (str_startswith(rest, S("..."))) {
      special = S("&hellip;");
      special_len = 3;
    }
    if (special_len == 0) {
      i++;
      continue;
    }
    html_escape_append(output, str_head(str_tail(text, run_start), i - run_start));
    // Dashes that end the text become a dash entity only when a line end follows, which only the
    // next callback tells.
    if (rest.data[0] == '-' && special_len == rest.len) {
      render->pending_dash_count = special_len;
    } else {
      buf_append(output, special);
    }
    i += special_len;
    run_start = i;
  }
  html_escape_append(output, str_tail(text, run_start));
}

static void write_timestamp(struct Buf* buffer, struct Str timestamp) {
  for (ptrdiff_t i = 1; i + 1 < timestamp.len; i++) {
    const char before = timestamp.data[i - 1];
    if (timestamp.data[i] == '-' && timestamp.data[i + 1] == '-' &&
        (before == '>' || before == ']')) {
      html_escape_append(buffer, str_head(timestamp, i));
      buf_append(buffer, S("&ndash;"));
      html_escape_append(buffer, str_tail(timestamp, i + 2));
      return;
    }
  }
  html_escape_append(buffer, timestamp);
}

static void write_entity(struct Buf* buffer, struct Str entity) {
  ptrdiff_t name_len = entity.len - 1;
  if (name_len >= 2 && str_endswith(entity, S("{}"))) {
    name_len -= 2;
  }
  const char* html = entity.len > 1 ? org_entity_html(entity.data + 1, (ORG_SIZE)name_len) : NULL;
  if (html != NULL) {
    buf_append(buffer, str_from_cstr(html));
  } else {
    html_escape_append(buffer, entity);
  }
}

static void write_latex(struct Buf* buffer, struct Str fragment) {
  bool is_display = false;
  ptrdiff_t delimiter_len = 0;
  if (fragment.len >= 4 &&
      (str_startswith(fragment, S("$$")) ||
       (str_startswith(fragment, S("\\[")) && str_endswith(fragment, S("\\]"))))) {
    is_display = true;
    delimiter_len = 2;
  } else if (fragment.len >= 4 && str_startswith(fragment, S("\\(")) &&
             str_endswith(fragment, S("\\)"))) {
    delimiter_len = 2;
  } else if (fragment.len >= 2 && str_startswith(fragment, S("$"))) {
    delimiter_len = 1;
  }
  if (delimiter_len == 0) {
    html_escape_append(buffer, fragment);
    return;
  }
  body_html_math_open(buffer, is_display);
  html_escape_append(buffer,
                     str_head(str_tail(fragment, delimiter_len), fragment.len - 2 * delimiter_len));
  body_html_math_close(buffer);
}

static void write_footnote_close(struct OrgRender* render,
                                 const ORG_BLOCK_FOOTNOTE_DEF_DETAIL* footnote) {
  render->is_in_footnote = false;
  render->output = &render->html;
  const ptrdiff_t id = (ptrdiff_t)footnote->id;
  const ptrdiff_t reference_count = (ptrdiff_t)footnote->ref_count;
  const struct Str body = buf_str(&render->footnote);
  const struct Str paragraph_open = S("<p>");
  const struct Str paragraph_close = S("</p>\n");
  ptrdiff_t first_close = 0;
  while (first_close < body.len && !str_startswith(str_tail(body, first_close), S("</p>"))) {
    first_close++;
  }
  if (str_startswith(body, paragraph_open) && first_close == body.len - paragraph_close.len) {
    buf_append(render->output, str_head(str_tail(body, paragraph_open.len),
                                        body.len - paragraph_open.len - paragraph_close.len));
    body_html_footnote_back_references_write(render->output, id, reference_count);
  } else if (str_endswith(body, paragraph_close)) {
    buf_append(render->output, str_head(body, body.len - paragraph_close.len));
    body_html_footnote_back_references_write(render->output, id, reference_count);
    buf_append(render->output, S("</p>"));
  } else {
    buf_append(render->output, body);
    body_html_footnote_back_references_write(render->output, id, reference_count);
  }
  body_html_footnote_close(render->output);
}

static void release_held_source(struct OrgRender* render) {
  render->is_source_held = false;
  flush_pending_source(render);
  html_escape_append(render->output, buf_str(&render->code));
}

static bool find_admonition_type(const ORG_BLOCK_SPECIAL_DETAIL* special, struct Str* type_out) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(admonition_types) / sizeof(admonition_types[0]));
       i++) {
    if (is_attribute_named(&special->name, admonition_types[i])) {
      *type_out = admonition_types[i];
      return true;
    }
  }
  return false;
}

static struct Buf* body_output(struct OrgRender* render) {
  return render->is_in_footnote ? &render->footnote : &render->html;
}

static void write_attribute_part(struct Buf* buffer,
                                 const ORG_ATTRIBUTE* attribute,
                                 ptrdiff_t begin,
                                 ptrdiff_t end,
                                 EscapeFn* escape) {
  for (ptrdiff_t i = 0; attribute->substr_offsets[i] < attribute->size; i++) {
    ptrdiff_t part_begin = (ptrdiff_t)attribute->substr_offsets[i];
    ptrdiff_t part_end = (ptrdiff_t)attribute->substr_offsets[i + 1];
    if (part_begin < begin) {
      part_begin = begin;
    }
    if (part_end > end) {
      part_end = end;
    }
    if (part_begin >= part_end) {
      continue;
    }
    if (attribute->substr_types[i] == ORG_TEXT_NULLCHAR) {
      buf_append(buffer, UTF8_REPLACEMENT);
    } else {
      escape(buffer, (struct Str){attribute->text + part_begin, part_end - part_begin});
    }
  }
}

static void write_attribute(struct Buf* buffer, const ORG_ATTRIBUTE* attribute, EscapeFn* escape) {
  write_attribute_part(buffer, attribute, 0, (ptrdiff_t)attribute->size, escape);
}

static struct Str attribute_text(const ORG_ATTRIBUTE* attribute) {
  return (struct Str){attribute->text, (ptrdiff_t)attribute->size};
}

static bool is_attribute_named(const ORG_ATTRIBUTE* attribute, struct Str name) {
  return is_equal_folded(attribute_text(attribute), name);
}

static bool find_header_argument(const ORG_ATTRIBUTE* params,
                                 struct Str name,
                                 struct Str* value_out) {
  for (struct StrCut word = cut_word(attribute_text(params)); word.is_found;
       word = cut_word(word.tail)) {
    if (str_eq(word.head, name)) {
      const struct StrCut value = cut_word(word.tail);
      *value_out = value.head;
      return true;
    }
  }
  return false;
}

static bool is_image_path(const ORG_ATTRIBUTE* path) {
  static const struct Str extensions[] = {
      STR_INIT(".png"),  STR_INIT(".jpg"), STR_INIT(".jpeg"), STR_INIT(".gif"),  STR_INIT(".svg"),
      STR_INIT(".webp"), STR_INIT(".bmp"), STR_INIT(".tif"),  STR_INIT(".tiff"), STR_INIT(".xpm"),
      STR_INIT(".pbm"),  STR_INIT(".pgm"), STR_INIT(".ppm"),
  };
  const struct Str text = attribute_text(path);
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(extensions) / sizeof(extensions[0])); i++) {
    if (text.len > extensions[i].len &&
        is_equal_folded(str_tail(text, text.len - extensions[i].len), extensions[i])) {
      return true;
    }
  }
  return false;
}

static bool is_equal_folded(struct Str text, struct Str name) {
  if (text.len != name.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < text.len; i++) {
    if (ascii_to_lower((unsigned char)text.data[i]) != (unsigned char)name.data[i]) {
      return false;
    }
  }
  return true;
}

static bool is_blank(char c) {
  return c == ' ' || c == '\t';
}

static struct StrCut cut_word(struct Str text) {
  struct Str rest = text;
  while (rest.len > 0 && is_blank(rest.data[0])) {
    rest = str_tail(rest, 1);
  }
  ptrdiff_t word_len = 0;
  while (word_len < rest.len && !is_blank(rest.data[word_len])) {
    word_len++;
  }
  return (struct StrCut){
      .head = str_head(rest, word_len),
      .tail = str_tail(rest, word_len),
      .is_found = word_len > 0,
  };
}
