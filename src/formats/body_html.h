#ifndef SOSIG_BODY_HTML_H
#define SOSIG_BODY_HTML_H

#include <stdbool.h>
#include <stddef.h>

#include "formats/highlight.h"
#include "shared/arena.h"
#include "shared/str.h"

struct Buf;

// Each element takes the markup md4c's own HTML renderer gives it, apart from a heading's level, a
// task item's `<li>` class, the footnotes' roles and a code block's highlighting. A stylesheet or
// math script written for md4c's HTML then covers both formats, and changing an element is one edit
// for both renderers.

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

/**
 * @brief Writes a URL of the caller's in place of a link's or an image's target as written, or
 *        declines to.
 *
 * Like a `HighlightLineFn`, it appends to the buffer the HTML grows in, so the URL needs no memory
 * of its own.
 *
 * @param buffer         Buffer the HTML grows in, which receives the URL, escaped as
 *                       `body_html_url_escape_append` escapes it. The callback may use the bytes
 *                       past the buffer's end while it runs. Must not be `NULL`.
 * @param type           Link type as org4c names it, such as `file`, `fuzzy` or `https`, or empty
 *                       for a Markdown link.
 * @param target         Target as written: an Org link's path, after its type, or a Markdown
 *                       destination.
 * @param is_image       Whether the target is an image's `src` rather than a link's `href`.
 * @param is_written_out Receives `true` when the callback appended a URL, and `false` when it
 *                       declined or failed. Must not be `NULL`.
 * @param userdata       Opaque pointer from `struct LinkTargets`.
 * @return `0` after appending a URL or declining, or `-1` after the callback recorded a failure of
 *         its own or found nothing to link. On either path without a URL, the renderer drops what
 *         the callback appended and writes the target as it does without a callback.
 */
typedef int (*LinkTargetWriteFn)(struct Buf* buffer,
                                 struct Str type,
                                 struct Str target,
                                 bool is_image,
                                 bool* is_written_out,
                                 void* userdata);

/** Callback that both renderers give every link and image target. */
struct LinkTargets {
  /** Writes a URL for a target, or declines to. Must not be `NULL`. */
  LinkTargetWriteFn write;

  /** Opaque pointer passed to `write`. */
  void* userdata;
};

/**
 * @brief Gives a link's or an image's target to `link_targets`, which may write a URL of its own
 *        for it.
 *
 * @param buffer       Buffer the HTML grows in. Must not be `NULL`.
 * @param type         Link type, as for `LinkTargetWriteFn`.
 * @param target       Target as written.
 * @param is_image     Whether the target is an image's `src`.
 * @param link_targets Callback, or `NULL` for none.
 * @return `0` after the callback wrote a URL, or `-1` with nothing written when there is no
 *         callback, or it declined or failed. The caller then writes the target itself.
 */
int body_html_link_target_write(struct Buf* buffer,
                                struct Str type,
                                struct Str target,
                                bool is_image,
                                const struct LinkTargets* link_targets) __attribute__((nonnull(1)));

/**
 * @brief Writes the opening tag of a heading.
 *
 * Markdown's `#` and Org's `*` are level 1, which is `<h2>`, so a body's sections sit below the
 * `<h1>` a content template gives the page title, as in Emacs's export. Each level below adds one,
 * up to `<h6>`.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param level  Heading level in the source, from 1.
 */
void body_html_heading_open(struct Buf* buffer, ptrdiff_t level) __attribute__((nonnull(1)));

/**
 * @brief Writes the closing tag of a heading, and the line end after it.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param level  Heading level in the source, from 1, as passed to `body_html_heading_open`.
 */
void body_html_heading_close(struct Buf* buffer, ptrdiff_t level) __attribute__((nonnull(1)));

/**
 * @brief Writes a reference to a footnote, which links to it.
 *
 * The link has the DPUB-ARIA role `doc-noteref`, so a screen reader announces it as a note
 * reference.
 *
 * @param buffer       Destination buffer. Must not be `NULL`.
 * @param id           Footnote number, from 1, in the order of the first references.
 * @param reference_id Number of this reference among the footnote's references, from 1. The
 *                     footnote's back-reference with this number links back to it.
 */
void body_html_footnote_reference_write(struct Buf* buffer, ptrdiff_t id, ptrdiff_t reference_id)
    __attribute__((nonnull(1)));

/**
 * @brief Writes the start of the footnote list at the end of a body.
 *
 * The section has the DPUB-ARIA role `doc-endnotes`, notes collected at the end of a work. The
 * notes themselves take no role, because ARIA allows `doc-footnote` on no list item.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 */
void body_html_footnotes_open(struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Writes the end of the footnote list.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 */
void body_html_footnotes_close(struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Writes the start of one footnote in the list.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param id     Footnote number, from 1.
 */
void body_html_footnote_open(struct Buf* buffer, ptrdiff_t id) __attribute__((nonnull(1)));

/**
 * @brief Writes a footnote's links back to its references, one for each, after its text.
 *
 * Each link has the DPUB-ARIA role `doc-backlink`.
 *
 * @param buffer          Destination buffer. Must not be `NULL`.
 * @param id              Footnote number, from 1.
 * @param reference_count Count of the footnote's references.
 */
void body_html_footnote_back_references_write(struct Buf* buffer,
                                              ptrdiff_t id,
                                              ptrdiff_t reference_count)
    __attribute__((nonnull(1)));

/**
 * @brief Writes the end of one footnote in the list, after its back-references.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 */
void body_html_footnote_close(struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Writes the disabled checkbox that starts a task list item, after the item's opening tag.
 *
 * @param buffer     Destination buffer. Must not be `NULL`.
 * @param is_checked Whether the task is done. Org's partly done `[-]` is not.
 */
void body_html_checkbox_write(struct Buf* buffer, bool is_checked) __attribute__((nonnull(1)));

/**
 * @brief Writes the start of an admonition, such as a note or a warning, with its title.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param type   Admonition type in lowercase: `note`, `tip`, `important`, `warning` or `caution`.
 *               It is both the class suffix and the title.
 * @param id     Element id a link can point to, such as an Org block's `#+NAME:`, or empty for
 *               none.
 */
void body_html_admonition_open(struct Buf* buffer, struct Str type, struct Str id)
    __attribute__((nonnull(1)));

/**
 * @brief Writes the end of an admonition.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 */
void body_html_admonition_close(struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Writes the opening tag of a math formula, which a script such as MathJax or KaTeX renders.
 *
 * The formula's LaTeX follows, HTML-escaped and without its delimiters.
 *
 * @param buffer     Destination buffer. Must not be `NULL`.
 * @param is_display Whether the formula is displayed on a line of its own rather than inline.
 */
void body_html_math_open(struct Buf* buffer, bool is_display) __attribute__((nonnull(1)));

/**
 * @brief Writes the closing tag of a math formula.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 */
void body_html_math_close(struct Buf* buffer) __attribute__((nonnull(1)));

/**
 * @brief Splits off a converter's scratch the region its code blocks are highlighted in.
 *
 * The region is twice `HIGHLIGHT_MEMORY_LEN_MAX`, the most one block's highlighting may take, or
 * half the free space of `scratch` when that is smaller, as in a unit test or a halved reservation.
 * None of the converter's own buffers grows in it, so a block can be highlighted while one of them,
 * such as a footnote's, receives the HTML.
 *
 * @param scratch Converter's scratch, which keeps the rest of its free space. Must not be `NULL`.
 * @return An arena over the region, which `body_html_code_block_write` takes by value.
 */
struct Arena body_html_code_block_scratch(struct Arena* scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes a code block, with a `language-` class when it names its language, and its code
 *        highlighted when the language has a grammar.
 *
 * This is `body_html_code_block_open`, `body_html_code_write` with no line callbacks, and
 * `body_html_code_block_close` in order.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param language Language, such as `c`, or empty when the block names none.
 * @param id       Element id a link can point to, such as an Org block's `#+NAME:`, or empty for
 *                 none.
 * @param code     Code, every line ending in `\n`.
 * @param scratch  Region from `body_html_code_block_scratch`, for highlighting the code.
 */
void body_html_code_block_write(struct Buf* buffer,
                                struct Str language,
                                struct Str id,
                                struct Str code,
                                struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes the opening tags of a code block, with a `language-` class when it names its
 *        language.
 *
 * A language that already starts with `language-` does not get the prefix twice.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param language Language, such as `c`, or empty when the block names none.
 * @param id       Element id a link can point to, or empty for none.
 */
void body_html_code_block_open(struct Buf* buffer, struct Str language, struct Str id)
    __attribute__((nonnull(1)));

/**
 * @brief Writes code, highlighted when its language has a grammar, and only HTML-escaped otherwise.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param language Language, such as `c`, or empty when the code names none.
 * @param code     Code.
 * @param lines    Callbacks around each line, such as an Org block's line numbers, or `NULL`.
 * @param scratch  Region from `body_html_code_block_scratch`, for highlighting the code.
 */
void body_html_code_write(struct Buf* buffer,
                          struct Str language,
                          struct Str code,
                          const struct HighlightLines* lines,
                          struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes the closing tags of a code block, and the line end after them.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 */
void body_html_code_block_close(struct Buf* buffer) __attribute__((nonnull(1)));

#endif
