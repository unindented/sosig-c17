/*
 * ORG4C: Org-mode parser for C
 * (https://github.com/unindented/org4c)
 *
 * Copyright (c) 2026 Daniel Perez Alvarez
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef ORG4C_HTML_H
#define ORG4C_HTML_H

#include "org4c.h"

#ifdef __cplusplus
    extern "C" {
#endif


/* If set, debug output from org_parse() is sent to stderr. */
#define ORG_HTML_FLAG_DEBUG                     0x0001
#define ORG_HTML_FLAG_VERBATIM_ENTITIES         0x0002  /* Output the entities as written (e.g. "\alpha"). */
#define ORG_HTML_FLAG_SKIP_UTF8_BOM             0x0004  /* Skip the UTF-8 byte order mark at the beginning of the input. */
#define ORG_HTML_FLAG_XHTML                     0x0008  /* Output XHTML (e.g. "<br />") instead of HTML. */
/* If set, the special strings are not translated. (By default, as ox-html
 * does, "--" is rendered as en dash, "---" as em dash, "..." as ellipsis and
 * "\-" as soft hyphen.) */
#define ORG_HTML_FLAG_VERBATIM_SPECIAL_STRINGS  0x0010
#define ORG_HTML_FLAG_NO_TODO                   0x0020  /* Do not output the TODO keywords of headlines. */
#define ORG_HTML_FLAG_NO_PRIORITY               0x0040  /* Do not output the priority cookies of headlines. */
#define ORG_HTML_FLAG_NO_TAGS                   0x0080  /* Do not output the tags of headlines. */
#define ORG_HTML_FLAG_NO_FOOTNOTES              0x0100  /* Do not output the footnotes. */
#define ORG_HTML_FLAG_NO_DRAWERS                0x0200  /* Do not output any drawers. */
#define ORG_HTML_FLAG_HARD_SOFT_BREAKS          0x0400  /* Render all the soft line breaks as hard ones. */

/* If set, org_html() honors the in-buffer "#+OPTIONS:" of the document, as
 * far as they are supported: "^:" (sub/superscripts), "tex:", "-:" (special
 * strings), "e:" (entities), "todo:", "pri:", "tags:", "f:" (footnotes),
 * "d:" (drawers), "\n:" (line breaks) and "arch:" (archived subtrees). They
 * override the respective flags.
 *
 * Note the lines "#+OPTIONS:" are looked for without parsing the document,
 * so they are honored even inside a block (e.g. #+BEGIN_SRC ... #+END_SRC),
 * unlike in Emacs. */
#define ORG_HTML_FLAG_IN_BUFFER_OPTIONS         0x0800

/* If set, the headlines of level 1 are rendered as <h2> (and of level N as
 * <hN+1>, still capped at <h6>), as ox-html does, reserving <h1> for the
 * document title. */
#define ORG_HTML_FLAG_TOPLEVEL_H2               0x1000

/* If set, the subtrees tagged "ARCHIVE" are output in full (as Org's
 * "arch:t"). By default, only their headlines are output, as ox-html does. */
#define ORG_HTML_FLAG_ARCHIVED_TREES            0x2000


/* Render Org document into HTML.
 *
 * Note only contents of <body> tag is generated. Caller must generate
 * HTML header/footer manually before/after calling org_html().
 *
 * Params input and input_size specify the Org input.
 * Callback process_output() gets called with chunks of HTML output.
 * (Typical implementation may just output the bytes to a file or append to
 * some buffer).
 * Param userdata is just propagated back to process_output() callback.
 * Param parser_flags are flags from org4c.h propagated to org_parse().
 * (ORG_FLAG_SKIPCOMMENTED and ORG_FLAG_SKIPNOEXPORT are always added, as the
 * commented subtrees and the ones tagged "noexport" are never rendered; and
 * so is ORG_FLAG_SKIPARCHIVEDCONTENTS, unless ORG_HTML_FLAG_ARCHIVED_TREES is
 * set.)
 * Param renderer_flags is bitmask of ORG_HTML_FLAG_xxxx.
 *
 * Returns -1 on error (if org_parse() fails.)
 * Returns 0 on success.
 */
int org_html(const ORG_CHAR* input, ORG_SIZE input_size,
             void (*process_output)(const ORG_CHAR*, ORG_SIZE, void*),
             void* userdata, unsigned parser_flags, unsigned renderer_flags);


#ifdef __cplusplus
    }  /* extern "C" { */
#endif

#endif  /* ORG4C_HTML_H */
