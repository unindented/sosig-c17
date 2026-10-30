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

#ifndef ORG4C_H
#define ORG4C_H

#ifdef __cplusplus
    extern "C" {
#endif

#if defined ORG4C_USE_UTF16
    /* Magic to support UTF-16. Note that in order to use it, you have to define
     * the macro ORG4C_USE_UTF16 both when building ORG4C as well as when
     * including this header in your code. */
    #ifdef _WIN32
        #include <windows.h>
        typedef WCHAR       ORG_CHAR;
    #else
        #error ORG4C_USE_UTF16 is only supported on Windows.
    #endif
#else
    typedef char            ORG_CHAR;
#endif

typedef unsigned ORG_SIZE;
typedef unsigned ORG_OFFSET;


/* Block represents a part of document hierarchy structure like a paragraph
 * or list item.
 */
typedef enum ORG_BLOCKTYPE {
    /* <body>...</body> */
    ORG_BLOCK_DOC = 0,

    /* #+BEGIN_QUOTE ... #+END_QUOTE
     * <blockquote>...</blockquote> */
    ORG_BLOCK_QUOTE,

    /* <ul>...</ul>
     * Detail: Structure ORG_BLOCK_LIST_DETAIL. */
    ORG_BLOCK_UL,

    /* <ol>...</ol>
     * Detail: Structure ORG_BLOCK_LIST_DETAIL. */
    ORG_BLOCK_OL,

    /* <li>...</li> (item of ORG_BLOCK_UL or ORG_BLOCK_OL)
     * Detail: Structure ORG_BLOCK_LI_DETAIL.
     * Note (as ox-html does), if the first paragraph of the item is its only
     * contents (ignoring its sub-list), it is not wrapped in ORG_BLOCK_P. The
     * same applies to ORG_BLOCK_DD. */
    ORG_BLOCK_LI,

    /* <hr> */
    ORG_BLOCK_HR,

    /* <h1>...</h1> (for levels up to 6)
     * Detail: Structure ORG_BLOCK_HEADLINE_DETAIL. */
    ORG_BLOCK_HEADLINE,

    /* #+BEGIN_SRC ... #+END_SRC
     * <pre><code>...</code></pre>
     * Detail: Structure ORG_BLOCK_SRC_DETAIL.
     * Note the text lines within source blocks are terminated with '\n'. */
    ORG_BLOCK_SRC,

    /* #+BEGIN_EXPORT backend ... #+END_EXPORT
     * Raw contents to be passed to the export backend verbatim.
     * Detail: Structure ORG_BLOCK_EXPORT_DETAIL.
     * Note the text lines within export blocks are terminated with '\n'. */
    ORG_BLOCK_EXPORT,

    /* <p>...</p>
     * Detail: Structure ORG_BLOCK_P_DETAIL. */
    ORG_BLOCK_P,

    /* <table>...</table> and its contents.
     * Detail: Structure ORG_BLOCK_TABLE_DETAIL (for ORG_BLOCK_TABLE),
     *         structure ORG_BLOCK_TD_DETAIL (for ORG_BLOCK_TH and ORG_BLOCK_TD) */
    ORG_BLOCK_TABLE,
    ORG_BLOCK_THEAD,
    ORG_BLOCK_TBODY,
    ORG_BLOCK_TR,
    ORG_BLOCK_TH,
    ORG_BLOCK_TD,

    /* Container for all referenced footnote definitions, and a single
     * footnote definition.
     * Detail: NULL (for ORG_BLOCK_FOOTNOTE_DEF_SECTION),
     *         structure ORG_BLOCK_FOOTNOTE_DEF_DETAIL (for ORG_BLOCK_FOOTNOTE_DEF).
     * Note footnote definitions are not reported where they are in the
     * document. Instead, all the footnotes which are referenced are reported
     * (in the order of their first reference) at the end of the document, as
     * children of a single ORG_BLOCK_FOOTNOTE_DEF_SECTION. Contents of an
     * inline footnote definition (e.g. "[fn::text]") is reported as a single
     * ORG_BLOCK_P. */
    ORG_BLOCK_FOOTNOTE_DEF_SECTION,
    ORG_BLOCK_FOOTNOTE_DEF,

    /* Section: A headline together with all its contents (including all the
     * nested sections of deeper levels).
     * Detail: Structure ORG_BLOCK_SECTION_DETAIL. */
    ORG_BLOCK_SECTION,

    /* <dl>...</dl>
     * Detail: Structure ORG_BLOCK_LIST_DETAIL. */
    ORG_BLOCK_DL,

    /* <dt>...</dt> (tag of an item of ORG_BLOCK_DL)
     * Detail: Structure ORG_BLOCK_LI_DETAIL. */
    ORG_BLOCK_DT,

    /* <dd>...</dd> (contents of an item of ORG_BLOCK_DL)
     * Detail: Structure ORG_BLOCK_LI_DETAIL. */
    ORG_BLOCK_DD,

    /* #+BEGIN_EXAMPLE ... #+END_EXAMPLE
     * <pre class="example">...</pre>
     * Detail: Structure ORG_BLOCK_EXAMPLE_DETAIL.
     * Note the text lines within example blocks are terminated with '\n'. */
    ORG_BLOCK_EXAMPLE,

    /* #+BEGIN_VERSE ... #+END_VERSE
     * <p class="verse">...</p>
     * Note line breaks within verse blocks are reported as ORG_TEXT_BR. */
    ORG_BLOCK_VERSE,

    /* #+BEGIN_CENTER ... #+END_CENTER
     * <div class="org-center">...</div> */
    ORG_BLOCK_CENTER,

    /* #+BEGIN_name ... #+END_name (any other name)
     * <div class="name">...</div>
     * Detail: Structure ORG_BLOCK_SPECIAL_DETAIL. */
    ORG_BLOCK_SPECIAL,

    /* Lines starting with '#' followed by a blank (or alone), or a block
     * #+BEGIN_COMMENT ... #+END_COMMENT. Not meant to be exported.
     * Note the text lines within comments are terminated with '\n'. */
    ORG_BLOCK_COMMENT,

    /* Lines starting with ':' followed by a space (or alone).
     * <pre class="example">...</pre>
     * Note the text lines within fixed-width areas are terminated with '\n'. */
    ORG_BLOCK_FIXED_WIDTH,

    /* :NAME: ... :END:
     * Detail: Structure ORG_BLOCK_DRAWER_DETAIL. */
    ORG_BLOCK_DRAWER,

    /* :PROPERTIES: ... :END: (directly following a headline or its planning
     * line; or at the beginning of the document, preceded only by blank lines
     * and a comment). All its lines have to be node properties ":NAME: VALUE"
     * (otherwise it is an ORG_BLOCK_DRAWER). Not meant to be exported.
     * Note the text lines within property drawers are terminated with '\n'. */
    ORG_BLOCK_PROPERTY_DRAWER,

    /* SCHEDULED:/DEADLINE:/CLOSED: line directly following a headline.
     * Not meant to be exported by default.
     * Note the text lines within planning are terminated with '\n'. */
    ORG_BLOCK_PLANNING,

    /* #+KEY: VALUE
     * Detail: Structure ORG_BLOCK_KEYWORD_DETAIL. */
    ORG_BLOCK_KEYWORD,

    /* \begin{name} ... \end{name}
     * Detail: Structure ORG_BLOCK_LATEX_ENVIRONMENT_DETAIL.
     * Note the whole environment (including the \begin and \end lines) is
     * reported as ORG_TEXT_LATEX; the text lines are terminated with '\n'. */
    ORG_BLOCK_LATEX_ENVIRONMENT,

    /* Caption of an element (from the affiliated keyword "#+CAPTION:").
     * Note it is reported as the first child of the element (e.g.
     * ORG_BLOCK_TABLE, ORG_BLOCK_SRC or ORG_BLOCK_P); its contents is the
     * caption text. */
    ORG_BLOCK_CAPTION,

    /* Inline task: A headline of level at least 15, optionally with contents
     * and with a terminating line "*************** END". Its first child
     * (after ORG_BLOCK_CAPTION, if any) is the ORG_BLOCK_HEADLINE with its
     * title.
     * Detail: Structure ORG_BLOCK_HEADLINE_DETAIL.
     * Note: Recognized only when ORG_FLAG_INLINETASKS is enabled. */
    ORG_BLOCK_INLINETASK,

    /* #+BEGIN: name params ... #+END:
     * Dynamic block.
     * Detail: Structure ORG_BLOCK_DYNAMIC_DETAIL.
     * Note its contents is reported as it is; ORG4C never updates it. */
    ORG_BLOCK_DYNAMIC
} ORG_BLOCKTYPE;

/* Span represents an in-line piece of a document which should be rendered with
 * the same font, color and other attributes. A sequence of spans forms a block
 * like paragraph or list item. */
typedef enum ORG_SPANTYPE {
    /* <i>...</i> */
    ORG_SPAN_ITALIC = 0,

    /* <b>...</b> */
    ORG_SPAN_BOLD,

    /* <a href="xxx">...</a>
     * Detail: Structure ORG_SPAN_LINK_DETAIL. */
    ORG_SPAN_LINK,

    /* <code>...</code> (~code~) */
    ORG_SPAN_CODE,

    /* <del>...</del> */
    ORG_SPAN_STRIKE,

    /* LaTeX fragment, e.g. "\(x^2\)", "$x$", "\[x\]", "$$x$$" or "\cmd{arg}".
     * The raw fragment (including the delimiters) is reported as ORG_TEXT_LATEX. */
    ORG_SPAN_LATEX,

    /* <span class="underline">...</span> */
    ORG_SPAN_UNDERLINE,

    /* <sup>...</sup> and <sub>...</sub>
     * Note: Recognized only when ORG_FLAG_SUBSUPERSCRIPTS (or
     * ORG_FLAG_SUBSUPERSCRIPTS_BRACED) is enabled. */
    ORG_SPAN_SUPERSCRIPT,
    ORG_SPAN_SUBSCRIPT,

    /* Footnote reference "[fn:label]", "[fn:label:definition]" or
     * "[fn::definition]". The span has no contents.
     * Detail: Structure ORG_SPAN_FOOTNOTE_REF_DETAIL. */
    ORG_SPAN_FOOTNOTE_REF,

    /* <code>...</code> (=verbatim=) */
    ORG_SPAN_VERBATIM,

    /* Timestamp, e.g. "<2024-01-31 Wed>" or "[2024-01-31 Wed 10:00]".
     * The raw timestamp is reported as the text of the span.
     * Detail: Structure ORG_SPAN_TIMESTAMP_DETAIL. */
    ORG_SPAN_TIMESTAMP,

    /* Inline source block, e.g. "src_python{print(1)}".
     * The code is reported as ORG_TEXT_CODE.
     * Detail: Structure ORG_SPAN_INLINE_SRC_DETAIL. */
    ORG_SPAN_INLINE_SRC,

    /* Export snippet, e.g. "@@html:<b>@@".
     * The value is reported as ORG_TEXT_EXPORT.
     * Detail: Structure ORG_SPAN_EXPORT_SNIPPET_DETAIL. */
    ORG_SPAN_EXPORT_SNIPPET,

    /* Statistics cookie, e.g. "[2/3]" or "[66%]".
     * The raw cookie is reported as the text of the span. */
    ORG_SPAN_STATISTICS_COOKIE,

    /* Target "<<target>>" (the span has no contents), or a radio target
     * "<<<target>>>" (the target text is the contents of the span).
     * Detail: Structure ORG_SPAN_TARGET_DETAIL.
     * Note radio targets make all the occurrences of their text in the
     * document links; they are reported as ORG_SPAN_LINK of the type
     * "radio". */
    ORG_SPAN_TARGET,

    /* Macro "{{{name(args)}}}". The contents of the span is the (parsed)
     * expansion of the macro (see ORG4C's README for the supported macros).
     * Detail: Structure ORG_SPAN_MACRO_DETAIL.
     * Note unknown macros are reported as a normal text. */
    ORG_SPAN_MACRO,

    /* Citation "[cite/style:prefix @key suffix; ...]". Its only contents are
     * the ORG_SPAN_CITATION_REFERENCE spans (which have no contents).
     * Detail: Structure ORG_SPAN_CITATION_DETAIL (for ORG_SPAN_CITATION),
     *         structure ORG_SPAN_CITATION_REFERENCE_DETAIL (for
     *         ORG_SPAN_CITATION_REFERENCE). */
    ORG_SPAN_CITATION,
    ORG_SPAN_CITATION_REFERENCE,

    /* Inline Babel call "call_name(args)". It has no contents.
     * Detail: Structure ORG_SPAN_INLINE_BABEL_CALL_DETAIL.
     * Note ORG4C never evaluates any code. */
    ORG_SPAN_INLINE_BABEL_CALL,

    /* A line of ORG_BLOCK_SRC or ORG_BLOCK_EXAMPLE with a code reference
     * label (e.g. "(ref:name)"; see the switch "-l"). The label itself is
     * removed from the line.
     * Detail: Structure ORG_SPAN_CODEREF_DETAIL. */
    ORG_SPAN_CODEREF
} ORG_SPANTYPE;

/* Text is the actual textual contents of span. */
typedef enum ORG_TEXTTYPE {
    /* Normal text. */
    ORG_TEXT_NORMAL = 0,

    /* NULL character. It is reported separately, so this allows caller to
     * replace it with the replacement char U+FFFD easily. */
    ORG_TEXT_NULLCHAR,

    /* Line breaks.
     * Note these are not sent from blocks with verbatim output (e.g.
     * ORG_BLOCK_SRC or ORG_BLOCK_EXAMPLE). In such cases, '\n' is part of the
     * text itself. */
    ORG_TEXT_BR,        /* <br> (hard break; "\\" at the end of line, or a line break in ORG_BLOCK_VERSE) */
    ORG_TEXT_SOFTBR,    /* '\n' in source text where it is not semantically meaningful (soft break) */

    /* Entity, e.g. "\alpha", "\alpha{}" or "\_ " (the raw source text).
     * Only the entities known to Org mode (see the variable `org-entities'
     * in Emacs) are reported this way; ORG4C provides the function
     * org_entity_html() to translate them. */
    ORG_TEXT_ENTITY,

    /* Text in a verbatim-like block (ORG_BLOCK_SRC, ORG_BLOCK_EXAMPLE,
     * ORG_BLOCK_FIXED_WIDTH) or in the spans ORG_SPAN_VERBATIM, ORG_SPAN_CODE
     * and ORG_SPAN_INLINE_SRC. If it is inside a block, it includes spaces for
     * indentation and '\n' for new lines. */
    ORG_TEXT_CODE,

    /* Raw text meant for the export backend (contents of ORG_BLOCK_EXPORT
     * and ORG_SPAN_EXPORT_SNIPPET). The text contains verbatim '\n' for the
     * new lines. */
    ORG_TEXT_EXPORT,

    /* Raw LaTeX (inside ORG_SPAN_LATEX and ORG_BLOCK_LATEX_ENVIRONMENT). */
    ORG_TEXT_LATEX
} ORG_TEXTTYPE;


/* Alignment enumeration. */
typedef enum ORG_ALIGN {
    ORG_ALIGN_DEFAULT = 0,  /* When unspecified. */
    ORG_ALIGN_LEFT,
    ORG_ALIGN_CENTER,
    ORG_ALIGN_RIGHT
} ORG_ALIGN;


/* String attribute.
 *
 * This wraps strings which are outside of a normal text flow and which are
 * propagated within various detailed structures, but which still may contain
 * string portions of different types.
 *
 * Note that these invariants are always guaranteed:
 *  -- substr_offsets[0] == 0
 *  -- substr_offsets[LAST+1] == size
 *  -- Currently, only ORG_TEXT_NORMAL and ORG_TEXT_NULLCHAR substrings can
 *     appear. (Each NUL character forms an ORG_TEXT_NULLCHAR substring of
 *     its own.) This could change in the future (e.g. to support entities).
 *
 * If the attribute is not present (e.g. a source block without a language),
 * then text is NULL and size is zero.
 */
typedef struct ORG_ATTRIBUTE {
    const ORG_CHAR* text;
    ORG_SIZE size;
    const ORG_TEXTTYPE* substr_types;
    const ORG_OFFSET* substr_offsets;
} ORG_ATTRIBUTE;


/* Detailed info for ORG_BLOCK_UL, ORG_BLOCK_OL and ORG_BLOCK_DL. */
typedef struct ORG_BLOCK_LIST_DETAIL {
    ORG_CHAR mark;              /* Bullet character ('-', '+', '*') of the first item;
                                 * for ORG_BLOCK_OL the delimiter ('.' or ')'). */
} ORG_BLOCK_LIST_DETAIL;

/* Detailed info for ORG_BLOCK_LI, ORG_BLOCK_DT and ORG_BLOCK_DD. */
typedef struct ORG_BLOCK_LI_DETAIL {
    ORG_CHAR checkbox;          /* One of ' ', 'X' or '-' for "[ ]", "[X]", "[-]"; or zero if no checkbox. */
    unsigned counter;           /* Counter from the "[@N]" cookie; or zero if not specified. */
} ORG_BLOCK_LI_DETAIL;

/* Detailed info for ORG_BLOCK_HEADLINE. */
typedef struct ORG_BLOCK_HEADLINE_DETAIL {
    unsigned level;             /* Headline level (count of leading stars; 1 - N) */
    ORG_ATTRIBUTE todo;         /* TODO keyword, e.g. "TODO" or "DONE"; or empty. */
    int is_done;                /* Non-zero if the TODO keyword is a "done" keyword. */
    ORG_CHAR priority;          /* Priority cookie character, e.g. 'A' for "[#A]"; or zero. */
    int is_commented;           /* Non-zero if the headline starts with the COMMENT keyword. */
    ORG_ATTRIBUTE tags;         /* Tags, e.g. "work:urgent" (without the outer colons); or empty. */
} ORG_BLOCK_HEADLINE_DETAIL;

/* Detailed info for ORG_BLOCK_SRC. */
typedef struct ORG_BLOCK_SRC_DETAIL {
    ORG_ATTRIBUTE lang;         /* Language, e.g. "python"; or empty. */
    ORG_ATTRIBUTE params;       /* Everything after the language (switches and header arguments); or empty. */
    unsigned first_line_number; /* Number of the first line (for the switches "-n" and "+n"); zero if not numbered. */
    unsigned line_count;        /* Count of the lines. */
} ORG_BLOCK_SRC_DETAIL;

/* Detailed info for ORG_BLOCK_EXPORT. */
typedef struct ORG_BLOCK_EXPORT_DETAIL {
    ORG_ATTRIBUTE backend;      /* Export backend, e.g. "html" or "latex"; or empty. */
} ORG_BLOCK_EXPORT_DETAIL;

/* Detailed info for ORG_BLOCK_P. */
typedef struct ORG_BLOCK_P_DETAIL {
    int is_standalone_link;     /* Non-zero if the paragraph consists only of a link without any description
                                 * (e.g. an image to be rendered as a figure). */
} ORG_BLOCK_P_DETAIL;

/* Detailed info for ORG_BLOCK_TABLE. */
typedef struct ORG_BLOCK_TABLE_DETAIL {
    unsigned col_count;         /* Count of columns in the table. */
    unsigned head_row_count;    /* Count of rows in the table header */
    unsigned body_row_count;    /* Count of rows in the table body */
} ORG_BLOCK_TABLE_DETAIL;

/* Detailed info for ORG_BLOCK_TH and ORG_BLOCK_TD. */
typedef struct ORG_BLOCK_TD_DETAIL {
    ORG_ALIGN align;
} ORG_BLOCK_TD_DETAIL;

/* Detailed info for ORG_BLOCK_FOOTNOTE_DEF. */
typedef struct ORG_BLOCK_FOOTNOTE_DEF_DETAIL {
    unsigned id;                /* 1-based number of the footnote (in the order of the first reference). */
    unsigned ref_count;         /* Count of references to the footnote (not counting the ones
                                 * in the footnote definitions reported after this one). */
    ORG_ATTRIBUTE label;        /* Label, e.g. "1" or "note"; empty for anonymous footnotes. */
    int is_defined;             /* Zero if there is no definition of the footnote in the document. */
} ORG_BLOCK_FOOTNOTE_DEF_DETAIL;

/* Detailed info for ORG_BLOCK_SECTION. */
typedef struct ORG_BLOCK_SECTION_DETAIL {
    unsigned level;             /* Headline level (count of leading stars; 1 - N) */
    int is_commented;           /* Non-zero if the headline starts with the COMMENT keyword. */
    ORG_ATTRIBUTE tags;         /* Tags of the headline as written in the source, e.g. "work:urgent" (without the outer colons). */
    int is_footnote_section;    /* Non-zero if the title (without the TODO keyword, priority, COMMENT and tags)
                                 * is exactly "Footnotes" (the default org-footnote-section, where Org files
                                 * the footnote definitions). */
} ORG_BLOCK_SECTION_DETAIL;

/* Detailed info for ORG_BLOCK_EXAMPLE. */
typedef struct ORG_BLOCK_EXAMPLE_DETAIL {
    ORG_ATTRIBUTE switches;     /* Everything after "#+BEGIN_EXAMPLE"; or empty. */
    unsigned first_line_number; /* Number of the first line (for the switches "-n" and "+n"); zero if not numbered. */
    unsigned line_count;        /* Count of the lines. */
} ORG_BLOCK_EXAMPLE_DETAIL;

/* Detailed info for ORG_BLOCK_SPECIAL. */
typedef struct ORG_BLOCK_SPECIAL_DETAIL {
    ORG_ATTRIBUTE name;         /* Name of the block as written, e.g. "note" for "#+BEGIN_note". */
    ORG_ATTRIBUTE params;       /* Anything following the name on the #+BEGIN line; or empty. */
} ORG_BLOCK_SPECIAL_DETAIL;

/* Detailed info for ORG_BLOCK_DRAWER. */
typedef struct ORG_BLOCK_DRAWER_DETAIL {
    ORG_ATTRIBUTE name;         /* Name of the drawer, e.g. "LOGBOOK". */
} ORG_BLOCK_DRAWER_DETAIL;

/* Detailed info for ORG_BLOCK_KEYWORD. */
typedef struct ORG_BLOCK_KEYWORD_DETAIL {
    ORG_ATTRIBUTE key;          /* Key as written, e.g. "TITLE" or "CAPTION[short]". */
    ORG_ATTRIBUTE value;        /* Value, with the surrounding whitespace stripped. */
    int is_affiliated;          /* Non-zero for an affiliated keyword (e.g. "#+CAPTION:" or "#+ATTR_HTML:")
                                 * which belongs to the element directly following it. */
} ORG_BLOCK_KEYWORD_DETAIL;

/* Detailed info for ORG_BLOCK_LATEX_ENVIRONMENT. */
typedef struct ORG_BLOCK_LATEX_ENVIRONMENT_DETAIL {
    ORG_ATTRIBUTE name;         /* Name of the environment, e.g. "equation". */
} ORG_BLOCK_LATEX_ENVIRONMENT_DETAIL;

/* Detailed info for ORG_BLOCK_DYNAMIC. */
typedef struct ORG_BLOCK_DYNAMIC_DETAIL {
    ORG_ATTRIBUTE name;         /* Name of the dynamic block, e.g. "clocktable". */
    ORG_ATTRIBUTE params;       /* Anything following the name; or empty. */
} ORG_BLOCK_DYNAMIC_DETAIL;

/* Detailed info for ORG_SPAN_LINK. */
typedef struct ORG_SPAN_LINK_DETAIL {
    ORG_ATTRIBUTE type;         /* Link type, e.g. "https", "file", "id", "custom-id", "coderef", "fuzzy" or "radio". */
    ORG_ATTRIBUTE path;         /* Path, without the "type:" prefix (and without '#' for "custom-id",
                                 * and without the parentheses for "coderef"). */
    ORG_ATTRIBUTE raw;          /* The whole link target as written. */
    int is_plain;               /* Non-zero if this is a plain link (e.g. https://example.com). */
    int has_description;        /* Non-zero if the link has an explicit description. If not,
                                 * the raw link target is reported as the text of the span
                                 * (for "coderef", the name of the reference; or its line
                                 * number with the switches "-r" or "-k"). */
} ORG_SPAN_LINK_DETAIL;

/* Detailed info for ORG_SPAN_FOOTNOTE_REF. */
typedef struct ORG_SPAN_FOOTNOTE_REF_DETAIL {
    unsigned id;                /* 1-based number of the referenced footnote. */
    unsigned ref_id;            /* 1-based index of this reference among all references to the same footnote. */
    ORG_ATTRIBUTE label;        /* Label; empty for anonymous footnotes. */
} ORG_SPAN_FOOTNOTE_REF_DETAIL;

/* Detailed info for ORG_SPAN_TIMESTAMP. */
typedef struct ORG_SPAN_TIMESTAMP_DETAIL {
    int is_active;              /* Non-zero for "<...>", zero for "[...]". */
    int is_range;               /* Non-zero for a range, e.g. "<...>--<...>". */
} ORG_SPAN_TIMESTAMP_DETAIL;

/* Detailed info for ORG_SPAN_INLINE_SRC. */
typedef struct ORG_SPAN_INLINE_SRC_DETAIL {
    ORG_ATTRIBUTE lang;         /* Language, e.g. "python". */
    ORG_ATTRIBUTE params;       /* Header arguments (without the brackets); or empty. */
} ORG_SPAN_INLINE_SRC_DETAIL;

/* Detailed info for ORG_SPAN_EXPORT_SNIPPET. */
typedef struct ORG_SPAN_EXPORT_SNIPPET_DETAIL {
    ORG_ATTRIBUTE backend;      /* Export backend, e.g. "html". */
} ORG_SPAN_EXPORT_SNIPPET_DETAIL;

/* Detailed info for ORG_SPAN_TARGET. */
typedef struct ORG_SPAN_TARGET_DETAIL {
    ORG_ATTRIBUTE name;         /* Target name, e.g. "target" for "<<target>>". */
    int is_radio;               /* Non-zero for a radio target "<<<target>>>". */
} ORG_SPAN_TARGET_DETAIL;

/* Detailed info for ORG_SPAN_MACRO. */
typedef struct ORG_SPAN_MACRO_DETAIL {
    ORG_ATTRIBUTE name;         /* Macro name. */
    ORG_ATTRIBUTE args;         /* Raw arguments (without the parenthesis); or empty. */
} ORG_SPAN_MACRO_DETAIL;

/* Detailed info for ORG_SPAN_CITATION. */
typedef struct ORG_SPAN_CITATION_DETAIL {
    ORG_ATTRIBUTE style;        /* Citation style, e.g. "t" for "[cite/t:...]"; or empty. */
    ORG_ATTRIBUTE prefix;       /* Global prefix; or empty. */
    ORG_ATTRIBUTE suffix;       /* Global suffix; or empty. */
} ORG_SPAN_CITATION_DETAIL;

/* Detailed info for ORG_SPAN_CITATION_REFERENCE. */
typedef struct ORG_SPAN_CITATION_REFERENCE_DETAIL {
    ORG_ATTRIBUTE key;          /* Citation key, e.g. "smith2020" for "@smith2020". */
    ORG_ATTRIBUTE prefix;       /* Prefix, e.g. "see"; or empty. */
    ORG_ATTRIBUTE suffix;       /* Suffix, e.g. "p. 5"; or empty. */
} ORG_SPAN_CITATION_REFERENCE_DETAIL;

/* Detailed info for ORG_SPAN_INLINE_BABEL_CALL. */
typedef struct ORG_SPAN_INLINE_BABEL_CALL_DETAIL {
    ORG_ATTRIBUTE name;         /* Name of the called code block. */
    ORG_ATTRIBUTE args;         /* Raw arguments. */
} ORG_SPAN_INLINE_BABEL_CALL_DETAIL;

/* Detailed info for ORG_SPAN_CODEREF. */
typedef struct ORG_SPAN_CODEREF_DETAIL {
    ORG_ATTRIBUTE name;         /* Name of the reference, e.g. "name" for "(ref:name)". */
    unsigned line_number;       /* Line number (if the lines are numbered); or zero. */
    int is_label_retained;      /* Zero if the label should not be shown (switch "-r",
                                 * unless the lines are numbered and "-k" is used). */
} ORG_SPAN_CODEREF_DETAIL;


/* Flags specifying extensions/deviations from the default Org syntax handling.
 *
 * By default (when ORG_PARSER::flags == 0), we try to parse the document the
 * same way as Emacs Org mode (org-element) does, with its default settings;
 * except for sub/superscripts, which are recognized only with
 * ORG_FLAG_SUBSUPERSCRIPTS (Emacs' default is "^:t"). The following flags may
 * allow some extensions or deviations from it.
 */
#define ORG_FLAG_NOEXPORTBLOCKS             0x1      /* Do not report export blocks (#+BEGIN_EXPORT). */
#define ORG_FLAG_NOPLAINLINKS               0x2      /* Do not recognize plain links (e.g. https://example.com). */
#define ORG_FLAG_SUBSUPERSCRIPTS            0x4      /* Enable sub/superscripts ("a_b", "a^{b}"). */
#define ORG_FLAG_SUBSUPERSCRIPTS_BRACED     0x8      /* Enable only braced sub/superscripts ("a_{b}"), i.e. Org's "^:{}". Implies ORG_FLAG_SUBSUPERSCRIPTS. */
#define ORG_FLAG_NOLATEX                    0x10     /* Do not recognize LaTeX fragments and environments. (Entities still are.) */
#define ORG_FLAG_SKIPCOMMENTED              0x20     /* Do not report subtrees of headlines with the COMMENT keyword at all. */
#define ORG_FLAG_SKIPNOEXPORT               0x40     /* Do not report subtrees of headlines tagged "noexport" at all. */
#define ORG_FLAG_INLINETASKS                0x80     /* Enable inline tasks (headlines of level 15 or deeper; see Emacs `org-inlinetask'). */
#define ORG_FLAG_SKIPARCHIVED               0x100    /* Do not report subtrees of headlines tagged "ARCHIVE" at all. */
#define ORG_FLAG_SKIPARCHIVEDCONTENTS       0x200    /* Of subtrees of headlines tagged "ARCHIVE", report only the headlines (as Org's "arch:headline"). */

/* Parser structure.
 */
typedef struct ORG_PARSER {
    /* Reserved. Set to zero.
     */
    unsigned abi_version;

    /* Dialect options. Bitmask of ORG_FLAG_xxxx values.
     */
    unsigned flags;

    /* Caller-provided rendering callbacks.
     *
     * For some block/span types, more detailed information is provided in a
     * type-specific structure pointed by the argument 'detail'.
     *
     * The last argument of all callbacks, 'userdata', is just propagated from
     * org_parse() and is available for any use by the application.
     *
     * Note any strings provided to the callbacks as their arguments or as
     * members of any detail structure are generally not zero-terminated.
     * Application has to take the respective size information into account.
     *
     * Any rendering callback may abort further parsing of the document by
     * returning non-zero.
     */
    int (*enter_block)(ORG_BLOCKTYPE /*type*/, void* /*detail*/, void* /*userdata*/);
    int (*leave_block)(ORG_BLOCKTYPE /*type*/, void* /*detail*/, void* /*userdata*/);

    int (*enter_span)(ORG_SPANTYPE /*type*/, void* /*detail*/, void* /*userdata*/);
    int (*leave_span)(ORG_SPANTYPE /*type*/, void* /*detail*/, void* /*userdata*/);

    int (*text)(ORG_TEXTTYPE /*type*/, const ORG_CHAR* /*text*/, ORG_SIZE /*size*/, void* /*userdata*/);

    /* Debug callback. Optional (may be NULL).
     *
     * If provided and something goes wrong, this function gets called.
     * This is intended for debugging and problem diagnosis for developers;
     * it is not intended to provide any errors suitable for displaying to an
     * end user.
     */
    void (*debug_log)(const char* /*msg*/, void* /*userdata*/);

    /* Reserved. Set to NULL.
     */
    void (*syntax)(void);
} ORG_PARSER;


/* Parse the Org document stored in the string 'text' of size 'size'.
 * The parser provides callbacks to be called during the parsing so the
 * caller can render the document on the screen or convert the Org document
 * to another format.
 *
 * Zero is returned on success. If a runtime error occurs (e.g. a memory
 * fails), -1 is returned. If the processing is aborted due any callback
 * returning non-zero, the return value of the callback is returned.
 */
int org_parse(const ORG_CHAR* text, ORG_SIZE size, const ORG_PARSER* parser, void* userdata);


/* Translate an Org entity name (e.g. "alpha", without the leading backslash
 * and without any trailing "{}") into its HTML representation (e.g.
 * "&alpha;"). Returns NULL if the name is not known.
 *
 * Note the name is always in 8-bit characters (even when ORG4C_USE_UTF16 is
 * defined).
 */
const char* org_entity_html(const char* name, ORG_SIZE name_size);


#ifdef __cplusplus
    }  /* extern "C" { */
#endif

#endif  /* ORG4C_H */
