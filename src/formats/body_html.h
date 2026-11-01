#ifndef SOSIG_BODY_HTML_H
#define SOSIG_BODY_HTML_H

#include <stdbool.h>
#include <stddef.h>

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
 * @brief Writes a code block, HTML-escaped, with a `language-` class when it names its language.
 *
 * A language that already starts with `language-` does not get the prefix twice.
 *
 * @param buffer   Destination buffer. Must not be `NULL`.
 * @param language Language, such as `c`, or empty when the block names none.
 * @param code     Code, every line ending in `\n`.
 */
void body_html_code_block_write(struct Buf* buffer, struct Str language, struct Str code)
    __attribute__((nonnull(1)));

#endif
