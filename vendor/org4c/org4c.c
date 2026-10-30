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

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "org4c.h"
#include "entity.h"


/*****************************
 ***  Miscellaneous Stuff  ***
 *****************************/

/* Make the UTF-8 support the default. */
#if !defined ORG4C_USE_ASCII && !defined ORG4C_USE_UTF8 && !defined ORG4C_USE_UTF16
    #define ORG4C_USE_UTF8
#endif

/* Magic for making wide literals with ORG4C_USE_UTF16. */
#ifdef _T
    #undef _T
#endif
#if defined ORG4C_USE_UTF16
    #define _T(x)           L##x
#else
    #define _T(x)           x
#endif

/* Misc. macros. */
#define SIZEOF_ARRAY(a)     (sizeof(a) / sizeof((a)[0]))

#define STRINGIZE_(x)       #x
#define STRINGIZE(x)        STRINGIZE_(x)

#define MAX(a,b)            ((a) > (b) ? (a) : (b))
#define MIN(a,b)            ((a) < (b) ? (a) : (b))

#define ORG_LOG(msg)                                                    \
    do {                                                                \
        if(ctx->parser.debug_log != NULL)                               \
            ctx->parser.debug_log((msg), ctx->userdata);                \
    } while(0)

#ifdef DEBUG
    #define ORG_ASSERT(cond)                                            \
            do {                                                        \
                if(!(cond)) {                                           \
                    ORG_LOG(__FILE__ ":" STRINGIZE(__LINE__) ": "       \
                           "Assertion '" STRINGIZE(cond) "' failed.");  \
                    exit(EXIT_FAILURE);                                 \
                }                                                       \
            } while(0)

    #define ORG_UNREACHABLE()       ORG_ASSERT(1 == 0)
#else
    #ifdef __GNUC__
        #define ORG_ASSERT(cond)    do { if(!(cond)) __builtin_unreachable(); } while(0)
        #define ORG_UNREACHABLE()   do { __builtin_unreachable(); } while(0)
    #elif defined _MSC_VER  &&  _MSC_VER > 120
        #define ORG_ASSERT(cond)    do { __assume(cond); } while(0)
        #define ORG_UNREACHABLE()   do { __assume(0); } while(0)
    #else
        #define ORG_ASSERT(cond)    do {} while(0)
        #define ORG_UNREACHABLE()   do {} while(0)
    #endif
#endif

/* For falling through case labels in switch statements. */
#if defined __clang__ && __clang_major__ >= 12
    #define ORG_FALLTHROUGH()       ;__attribute__((fallthrough))
#elif defined __GNUC__ && __GNUC__ >= 7
    #define ORG_FALLTHROUGH()       ;__attribute__((fallthrough))
#else
    #define ORG_FALLTHROUGH()       ((void)0)
#endif

/* Suppress "unused parameter" warnings. */
#define ORG_UNUSED(x)               ((void)(x))


/******************************
 ***  Some internal limits  ***
 ******************************/

/* Org uses tab stops of 8 columns when computing the indentation (this is
 * Emacs default value of `tab-width'). */
#define TAB_WIDTH               8

/* Headline level is stored in a 16-bit bit-field. Deeper headlines are
 * clamped (nobody should ever reach this anyway). */
#define HEADLINE_MAX_LEVEL      0xffff

/* Contents of a span may start with another span of the same kind, e.g.
 * "**a**" is "<b><b>a</b></b>". To avoid a stack overflow on a pathological
 * input like thousands of '*' around a word, we limit the nesting. */
#define SPAN_MAX_NESTING        32

/* Inline footnote definitions may be nested (e.g. "[fn::a [fn::b]]"). Each
 * of them is then processed separately, so a deep nesting would make us
 * quadratic. Deeper inline definitions are treated as an ordinary text. */
#define FOOTNOTE_MAX_NESTING    16

/* Macros may expand to other macros. We limit the nesting, as well as the
 * total size of all the expansions (similarly as MD4C limits the output of
 * the link reference definitions), so that a document like
 * "#+MACRO: a {{{b}}}{{{b}}}" (and so on) cannot make us explode. */
#define MACRO_MAX_NESTING       16
#define MACRO_MAX_OUTPUT(size)  (16 * MIN((size), (SZ)(1024 * 1024 / 16)) + 64 * 1024)

/* Minimal level of an inline task (Emacs `org-inlinetask-min-level'). */
#define INLINETASK_MIN_LEVEL    15

/* Every word in the document has to be checked against all the radio targets
 * (of the same first character); so we limit their count. Any more of them are
 * ignored. */
#define RADIO_TARGETS_MAX       1024

/* Maximal count of the different counters of the macro "{{{n}}}", and the
 * maximal length of the counter name. (The name is copied, as it may come
 * from a macro expansion; longer names are truncated.) */
#define MACRO_MAX_COUNTERS      64
#define MACRO_COUNTER_NAME_MAX  32

/* Maximal count of the macro arguments. Any more of them are ignored. */
#define MACRO_MAX_ARGS          9

/* The maximal numeric value of a list item counter ("[@N]") we care about.
 * (Bigger ones are clamped.) */
#define LIST_COUNTER_MAX        999999999

/* The longest name of an entity in src/entity.c (not counting "\_ " with the
 * spaces, which is recognized separately) is "Leftrightarrow". We need this
 * only to convert the names in the UTF-16 builds. */
#define ENTITY_NAME_MAX         16

/* Other numbers (the values of the macro "{{{n}}}" counters and the first
 * line numbers "-n N" of source blocks) are not parsed any further once they
 * reach this, so that they cannot overflow. */
#define NUMBER_MAX              100000000

/* Maximal count of the ';'-separated parts of a citation (i.e. of its
 * references, and its common prefix and suffix). Any more of them are
 * ignored. */
#define CITATION_MAX_PARTS      64


/************************
 ***  Internal Types  ***
 ************************/

/* These are omnipresent so lets save some typing. */
#define CHAR    ORG_CHAR
#define SZ      ORG_SIZE
#define OFF     ORG_OFFSET

typedef struct ORG_MARK_tag ORG_MARK;
typedef struct ORG_BLOCK_tag ORG_BLOCK;
typedef struct ORG_CONTAINER_tag ORG_CONTAINER;
typedef struct ORG_BLOCK_END_tag ORG_BLOCK_END;
typedef struct ORG_FOOTNOTE_tag ORG_FOOTNOTE;
typedef struct ORG_TODO_KEYWORD_tag ORG_TODO_KEYWORD;
typedef struct ORG_DOC_KEYWORD_tag ORG_DOC_KEYWORD;
typedef struct ORG_RADIO_TARGET_tag ORG_RADIO_TARGET;
typedef struct ORG_MACRO_COUNTER_tag ORG_MACRO_COUNTER;
typedef struct ORG_CODE_BLOCK_tag ORG_CODE_BLOCK;
typedef struct ORG_CODE_REF_tag ORG_CODE_REF;
typedef struct ORG_SCAN_LINE_tag ORG_SCAN_LINE;

/* A line scanned by org_is_cut_by_item_end(). */
struct ORG_SCAN_LINE_tag {
    OFF beg;
    OFF next;           /* Beginning of the next line to scan (after the skipped block or drawer, if any). */
    unsigned indent;    /* UINT_MAX for a blank line. */
};

/* Numbered source (or example) block. */
struct ORG_CODE_BLOCK_tag {
    int byte_off;           /* Offset of its record in ctx->block_bytes. */
    unsigned first_line;
};

/* Code reference label, e.g. "(ref:name)" in a source block. */
struct ORG_CODE_REF_tag {
    const CHAR* name;
    SZ name_size;
    unsigned line_number;
    int use_label;          /* Zero if the links show the line number instead of the label. */
};

/* Keyword line "#+KEY: VALUE" (for the macros like "{{{title}}}" and for the
 * macro definitions "#+MACRO: name template"). */
struct ORG_DOC_KEYWORD_tag {
    OFF beg;
    const CHAR* key;
    SZ key_size;
    const CHAR* value;
    SZ value_size;
};

/* Radio target "<<<text>>>". */
struct ORG_RADIO_TARGET_tag {
    const CHAR* text;
    SZ size;
};

/* Counter of the macro "{{{n(name)}}}". */
struct ORG_MACRO_COUNTER_tag {
    CHAR name[MACRO_COUNTER_NAME_MAX];
    SZ name_size;
    int value;
};

/* TODO keyword, as defined by "#+TODO:" (or "#+SEQ_TODO:", "#+TYP_TODO:"). */
struct ORG_TODO_KEYWORD_tag {
    const CHAR* name;
    SZ name_size;
    int is_done;
};

/* Record of a line "#+END_name" (or "\end{name}" of a LaTeX environment).
 * We collect all of them (and all the headlines and all the lines ":END:") in
 * a pre-pass over the whole document, so that when we see a line
 * "#+BEGIN_name" (or ":NAME:"), we can quickly check whether there is its
 * terminating counterpart, without rescanning the rest of the document again
 * and again (which could make us quadratic). */
struct ORG_BLOCK_END_tag {
    OFF beg;                /* Beginning of the line. */
    const CHAR* name;
    SZ name_size;
};

/* Inline mark. It represents either a (potential) emphasis marker, or a
 * whole (already recognized) inline object like a link or a timestamp. */
struct ORG_MARK_tag {
    OFF beg;
    OFF end;
    OFF sub_beg;        /* Object-specific sub-range (e.g. link target, footnote label, snippet backend). */
    OFF sub_end;
    OFF desc_beg;       /* Object-specific sub-range with inline contents or a value (e.g. link description). */
    OFF desc_end;
    OFF aux_beg;        /* Object-specific additional sub-range (e.g. inline source block parameters). */
    OFF aux_end;
    int index;          /* Object-specific index (e.g. of the radio target in ctx->radio_targets[]). */
    int next;           /* Emphasis: index of the nearest potential closer of the same kind after this mark; or -1. */
    CHAR ch;            /* Emphasis: '*', '/', '_', '+', '=', '~'. Objects: see ORG_MARK_xxx_OBJECT. */
    unsigned short flags;
};

/* Mark flags. */
#define ORG_MARK_POTENTIAL_OPENER           0x01
#define ORG_MARK_POTENTIAL_CLOSER           0x02
#define ORG_MARK_NONSPACE_BEFORE            0x04
#define ORG_MARK_NONSPACE_AFTER             0x08
#define ORG_MARK_ACTIVE_TIMESTAMP           0x10
#define ORG_MARK_TIMESTAMP_RANGE            0x20
#define ORG_MARK_INLINE_DEFINITION          0x40

/* Values of ORG_MARK::ch for the inline objects. */
#define ORG_MARK_BRACKET_LINK_OBJECT        _T('[')
#define ORG_MARK_PLAIN_LINK_OBJECT          _T('L')     /* Also angle links "<https://example.com>". */
#define ORG_MARK_ENTITY_OBJECT              _T('E')
#define ORG_MARK_LATEX_OBJECT               _T('X')
#define ORG_MARK_TIMESTAMP_OBJECT           _T('T')
#define ORG_MARK_FOOTNOTE_REF_OBJECT        _T('F')
#define ORG_MARK_STATISTICS_COOKIE_OBJECT   _T('S')
#define ORG_MARK_TARGET_OBJECT              _T('G')
#define ORG_MARK_EXPORT_SNIPPET_OBJECT      _T('P')
#define ORG_MARK_INLINE_SRC_OBJECT          _T('I')
#define ORG_MARK_SUBSCRIPT_OBJECT           _T('U')
#define ORG_MARK_SUPERSCRIPT_OBJECT         _T('V')
#define ORG_MARK_MACRO_OBJECT               _T('M')
#define ORG_MARK_RADIO_TARGET_OBJECT        _T('Q')
#define ORG_MARK_RADIO_LINK_OBJECT          _T('K')
#define ORG_MARK_CITATION_OBJECT            _T('C')
#define ORG_MARK_INLINE_BABEL_CALL_OBJECT   _T('B')

/* Storage of the substrings of a non-trivial attribute (see
 * org_build_attribute(ctx, )). */
typedef struct ORG_ATTRIBUTE_STORAGE_tag ORG_ATTRIBUTE_STORAGE;
struct ORG_ATTRIBUTE_STORAGE_tag {
    ORG_ATTRIBUTE_STORAGE* next;
    /* Followed by the arrays of the substring offsets and types. */
};

/* Context propagated through all the parsing. */
typedef struct ORG_CTX_tag ORG_CTX;
struct ORG_CTX_tag {
    /* Immutable stuff (parameters of org_parse()). */
    const CHAR* text;
    SZ size;
    ORG_PARSER parser;
    void* userdata;

    /* Document-wide character checks (set once by org_check_doc_chars()).
     * They also hold for the detached texts (macro expansions), which are
     * made only of the document characters and a few ASCII ones (spaces,
     * commas, backslashes, digits). */
    bool doc_has_nul;               /* The document contains some '\0'. */
    bool doc_has_radio_opener;      /* The document contains some "<<<". */

    /* For the optimized scan for ends of lines (see org_line_end()): There is
     * no '\r' in [cr_scan_beg, cr_horizon), and no '\n' in [lf_scan_beg,
     * lf_horizon). */
    OFF cr_scan_beg;
    OFF cr_horizon;
    OFF lf_scan_beg;
    OFF lf_horizon;

    /* Storages of the non-trivial attributes (i.e. those with some NUL). They
     * are kept until the end of the parsing, as a detail (and its attributes)
     * may be still in use by a callback of an outer element. */
    ORG_ATTRIBUTE_STORAGE* attribute_storages;

    /* Document-wide indexes (built by org_build_doc_index(); except the code
     * blocks and references, built by org_resolve_code_blocks()). They are
     * sorted by the offset unless noted otherwise. Each of the offsets is the
     * beginning of the respective line. */
    OFF* headlines;
    int n_headlines;
    int alloc_headlines;

    OFF* drawer_ends;
    int n_drawer_ends;
    int alloc_drawer_ends;

    OFF* inlinetask_ends;           /* All the lines of inline tasks (with ORG_FLAG_INLINETASKS) */
    int n_inlinetask_ends;
    int alloc_inlinetask_ends;

    OFF* dynamic_ends;              /* Lines "#+END:" */
    int n_dynamic_ends;
    int alloc_dynamic_ends;

    ORG_BLOCK_END* block_ends;      /* Lines "#+END_name" (sorted by the name). */
    int n_block_ends;
    int alloc_block_ends;

    ORG_TODO_KEYWORD* todo_keywords;    /* If none, the default "TODO" and "DONE" are used. */
    int n_todo_keywords;
    int alloc_todo_keywords;

    ORG_DOC_KEYWORD* doc_keywords;  /* All keyword lines (sorted by the key, then by the offset). */
    int n_doc_keywords;
    int alloc_doc_keywords;
    ORG_DOC_KEYWORD* macros;        /* Macro definitions: key is the name, value the template (sorted by the name). */
    int n_macros;
    int alloc_macros;
    ORG_RADIO_TARGET* radio_targets;    /* Sorted by the first (case-folded) character, then by the length (longest first). */
    int n_radio_targets;
    int alloc_radio_targets;
    char radio_first_char_map[256];
    int radio_first_index[256];         /* Index of the first radio target starting with the (case-folded) character. */

    ORG_CODE_BLOCK* code_blocks;    /* Numbered source blocks (sorted by the offset of their record in ctx->block_bytes). */
    int n_code_blocks;
    int alloc_code_blocks;
    ORG_CODE_REF* code_refs;        /* Code references (sorted by the name). */
    int n_code_refs;
    int alloc_code_refs;

    ORG_BLOCK_END* latex_ends;      /* Lines "\end{name}" (sorted by the name). */
    int n_latex_ends;
    int alloc_latex_ends;

    /* Footnotes (see ORG_FOOTNOTE). */
    ORG_FOOTNOTE* footnotes;
    int n_footnotes;
    int alloc_footnotes;
    int* footnote_buckets;          /* Hash table of the labeled footnotes (indexes into footnotes[]; -1 if empty). */
    int n_footnote_buckets;
    int n_labeled_footnotes;
    int* footnote_order;            /* Indexes of the referenced footnotes, in the order of their numbers. */
    int n_footnote_order;
    int alloc_footnote_order;
    int footnote_nesting_level;     /* Nesting level of the inline footnote definition being processed. */

    /* Stack of inline/span markers.
     * This is only used for parsing a single block contents but by storing it
     * here we may reuse the stack for subsequent blocks; i.e. we have fewer
     * (re)allocations. */
    ORG_MARK* marks;
    int n_marks;
    int alloc_marks;

#if defined ORG4C_USE_UTF16
    char mark_char_map[128];
#else
    char mark_char_map[256];
#endif

    /* Horizons for the inline analysis. When non-zero, we know there is no
     * respective closing string (e.g. "]]" for links) in the current block at
     * or after this offset; so we do not need to look for it again. */
    OFF link_close_horizon;
    OFF latex_paren_horizon;        /* "\)" */
    OFF latex_bracket_horizon;      /* "\]" */
    OFF latex_dollars_horizon;      /* "$$" */
    OFF snippet_horizon;            /* "@@" */
    OFF macro_horizon;              /* ")}}}" */

    /* Matching brackets ('[' and ']', '{' and '}', '(' and ')') within the
     * current block. Built lazily by org_build_bracket_matches() when first
     * needed (org_collect_marks() only resets it). For each opening bracket
     * at offset OFF, bracket_matches[OFF - bracket_region_beg] is the offset
     * of its matching closing bracket (or zero). */
    OFF* bracket_matches;
    int alloc_bracket_matches;
    OFF bracket_region_beg;
    OFF bracket_region_end;
    bool bracket_matches_valid;

    /* Current nesting level of the spans being processed. */
    int span_nesting_level;

    /* Index of the line where org_process_text() ended the last time. It is
     * only a hint (the lines may be different by now), so it is validated
     * before use. */
    SZ text_line_hint;

    /* For macro expansions. */
    int macro_nesting_level;
    SZ macro_output_budget;
    ORG_MACRO_COUNTER macro_counters[MACRO_MAX_COUNTERS];
    int n_macro_counters;
    int is_detached;                /* Non-zero when processing a text not from the document (a macro expansion). */

    /* For block analysis.
     * Notes:
     *   -- It holds ORG_BLOCK as well as ORG_LINE structures. After each
     *      ORG_BLOCK, its (multiple) ORG_LINE(s) follow.
     *   -- For the verbatim-like blocks (see org_block_has_verbatim_lines()),
     *      ORG_VERBATIMLINE(s) are used instead of ORG_LINE(s).
     */
    void* block_bytes;
    ORG_BLOCK* current_block;
    int n_block_bytes;
    int alloc_block_bytes;

    /* For container block analysis. */
    ORG_CONTAINER* containers;
    int n_containers;
    int alloc_containers;

    /* Contextual info for line analysis. */
    OFF verbatim_end;       /* When inside a verbatim-like block, beginning of the line which terminates it; zero otherwise. */
    int n_blank_lines;      /* Count of consecutive blank lines just seen. */
    int after_headline;     /* 1 if the previous line was a headline, 2 if it was its planning line. */
    int at_doc_start;       /* 1 before the first element, 2 just after the first one if it is a comment,
                             * 3 after it and some blank lines; zero otherwise. */

    /* Consecutive lines scanned by org_is_cut_by_item_end() (with skipping
     * the blocks and drawers before 'scan_limit'); so that each of them is
     * analyzed only once even for many nested list items. */
    ORG_SCAN_LINE* scan_lines;
    int n_scan_lines;
    int alloc_scan_lines;
    OFF scan_limit;

    /* The caption (value of an affiliated "#+CAPTION:") of the element being
     * processed; and of the next element. (Zero if none.) */
    OFF caption_beg;
    OFF caption_end;
    OFF next_caption_beg;
    OFF next_caption_end;

    /* Offsets (in block_bytes) of the preceding keywords which may be
     * affiliated to the next element. */
    int* pending_keywords;
    int n_pending_keywords;
    int alloc_pending_keywords;
};

enum ORG_LINETYPE_tag {
    ORG_LINE_BLANK,
    ORG_LINE_HEADLINE,
    ORG_LINE_PLANNING,
    ORG_LINE_BLOCKBEGIN,
    ORG_LINE_DRAWERBEGIN,
    ORG_LINE_PROPERTYDRAWERBEGIN,
    ORG_LINE_BLOCKEND,          /* Terminates a block or a drawer. */
    ORG_LINE_VERBATIM,          /* Line inside a verbatim-like block. */
    ORG_LINE_KEYWORD,
    ORG_LINE_COMMENT,
    ORG_LINE_FIXEDWIDTH,
    ORG_LINE_HR,
    ORG_LINE_TABLE,
    ORG_LINE_ITEM,
    ORG_LINE_FOOTNOTEDEF,
    ORG_LINE_INLINETASK,
    ORG_LINE_TEXT
};
typedef enum ORG_LINETYPE_tag ORG_LINETYPE;

typedef struct ORG_LINE_ANALYSIS_tag ORG_LINE_ANALYSIS;
struct ORG_LINE_ANALYSIS_tag {
    ORG_LINETYPE type;
    unsigned data;          /* Headline (or inline task) level; block type for ORG_LINE_BLOCKBEGIN; list type for ORG_LINE_ITEM. */
    OFF line_beg;           /* Beginning of the line (including any indentation). */
    OFF beg;                /* Beginning of the line contents (after the indentation). */
    OFF end;                /* End of the line contents (before trailing whitespace and the new line). */
    unsigned indent;        /* Indentation level (in columns). */
    OFF end_line;           /* ORG_LINE_BLOCKBEGIN, ORG_LINE_*DRAWERBEGIN and ORG_LINE_INLINETASK: Beginning of the terminating line. */
    OFF contents_beg;       /* Where the actual contents begins (e.g. after the bullet of a list item). */
};

typedef struct ORG_LINE_tag ORG_LINE;
struct ORG_LINE_tag {
    OFF beg;
    OFF end;
};

typedef struct ORG_VERBATIMLINE_tag ORG_VERBATIMLINE;
struct ORG_VERBATIMLINE_tag {
    OFF beg;
    OFF end;
    OFF indent_beg;     /* Beginning of the indentation, which ends at 'beg'. */
};


/*****************
 ***  Helpers  ***
 *****************/

/* Character accessors. */
#define CH(off)                 (ctx->text[(off)])
#define STR(off)                (ctx->text + (off))

/* Character classification.
 * Note we assume ASCII compatibility of code points < 128 here. */
#define ISIN_(ch, ch_min, ch_max)       ((ch_min) <= (unsigned)(ch) && (unsigned)(ch) <= (ch_max))
#define ISANYOF_(ch, palette)           ((ch) != _T('\0')  &&  org_strchr((palette), (ch)) != NULL)
#define ISANYOF2_(ch, ch1, ch2)         ((ch) == (ch1) || (ch) == (ch2))
#define ISANYOF3_(ch, ch1, ch2, ch3)    ((ch) == (ch1) || (ch) == (ch2) || (ch) == (ch3))
#define ISASCII_(ch)                    ((unsigned)(ch) <= 127)
#define ISBLANK_(ch)                    (ISANYOF2_((ch), _T(' '), _T('\t')))
#define ISNEWLINE_(ch)                  (ISANYOF2_((ch), _T('\r'), _T('\n')))
#define ISWHITESPACE_(ch)               (ISBLANK_(ch) || ISANYOF2_((ch), _T('\v'), _T('\f')))
#define ISCNTRL_(ch)                    ((unsigned)(ch) <= 31 || (unsigned)(ch) == 127)
#define ISPUNCT_(ch)                    (ISIN_(ch, 33, 47) || ISIN_(ch, 58, 64) || ISIN_(ch, 91, 96) || ISIN_(ch, 123, 126))
#define ISUPPER_(ch)                    (ISIN_(ch, _T('A'), _T('Z')))
#define ISLOWER_(ch)                    (ISIN_(ch, _T('a'), _T('z')))
#define ISALPHA_(ch)                    (ISUPPER_(ch) || ISLOWER_(ch))
#define ISDIGIT_(ch)                    (ISIN_(ch, _T('0'), _T('9')))
#define ISXDIGIT_(ch)                   (ISDIGIT_(ch) || ISIN_(ch, _T('A'), _T('F')) || ISIN_(ch, _T('a'), _T('f')))
#define ISALNUM_(ch)                    (ISALPHA_(ch) || ISDIGIT_(ch))

#define ISANYOF(off, palette)           ISANYOF_(CH(off), (palette))
#define ISANYOF2(off, ch1, ch2)         ISANYOF2_(CH(off), (ch1), (ch2))
#define ISANYOF3(off, ch1, ch2, ch3)    ISANYOF3_(CH(off), (ch1), (ch2), (ch3))
#define ISASCII(off)                    ISASCII_(CH(off))
#define ISBLANK(off)                    ISBLANK_(CH(off))
#define ISNEWLINE(off)                  ISNEWLINE_(CH(off))
#define ISWHITESPACE(off)               ISWHITESPACE_(CH(off))
#define ISCNTRL(off)                    ISCNTRL_(CH(off))
#define ISPUNCT(off)                    ISPUNCT_(CH(off))
#define ISUPPER(off)                    ISUPPER_(CH(off))
#define ISLOWER(off)                    ISLOWER_(CH(off))
#define ISALPHA(off)                    ISALPHA_(CH(off))
#define ISDIGIT(off)                    ISDIGIT_(CH(off))
#define ISXDIGIT(off)                   ISXDIGIT_(CH(off))
#define ISALNUM(off)                    ISALNUM_(CH(off))


#if defined ORG4C_USE_UTF16
    #include <wchar.h>  /* wmemchr() */

    #define org_memchr wmemchr
    #define org_strchr wcschr
    #define org_strlen wcslen
#else
    #define org_memchr memchr
    #define org_strchr strchr
    #define org_strlen strlen
#endif


/* Case insensitive check of string equality. */
static inline int
org_ascii_case_eq(const CHAR* s1, const CHAR* s2, SZ n)
{
    OFF i;
    for(i = 0; i < n; i++) {
        CHAR ch1 = s1[i];
        CHAR ch2 = s2[i];

        if(ISLOWER_(ch1))
            ch1 += ('A'-'a');
        if(ISLOWER_(ch2))
            ch2 += ('A'-'a');
        if(ch1 != ch2)
            return false;
    }
    return true;
}

static inline int
org_ascii_eq(const CHAR* s1, const CHAR* s2, SZ n)
{
    return memcmp(s1, s2, n * sizeof(CHAR)) == 0;
}

/* Case insensitive comparison of two strings (for sorting). */
static int
org_ascii_case_cmp(const CHAR* s1, SZ n1, const CHAR* s2, SZ n2)
{
    OFF i;
    for(i = 0; i < n1  &&  i < n2; i++) {
        unsigned ch1 = (unsigned) s1[i];
        unsigned ch2 = (unsigned) s2[i];

        if(ISLOWER_(ch1))
            ch1 += ('A'-'a');
        if(ISLOWER_(ch2))
            ch2 += ('A'-'a');
        if(ch1 != ch2)
            return (ch1 < ch2) ? -1 : +1;
    }
    if(n1 != n2)
        return (n1 < n2) ? -1 : +1;
    return 0;
}

/* Check whether the string at the given offset case-insensitively starts with
 * the (ASCII) literal. */
static int
org_is_prefix_i(ORG_CTX* ctx, OFF off, OFF end, const CHAR* literal)
{
    SZ n = (SZ) org_strlen(literal);
    if(off + n > end)
        return false;
    return org_ascii_case_eq(STR(off), literal, n);
}

/* Same as org_is_prefix_i() but case sensitive. */
static int
org_is_prefix(ORG_CTX* ctx, OFF off, OFF end, const CHAR* literal)
{
    SZ n = (SZ) org_strlen(literal);
    if(off + n > end)
        return false;
    return org_ascii_eq(STR(off), literal, n);
}

static int
org_text_with_null_replacement(ORG_CTX* ctx, ORG_TEXTTYPE type, const CHAR* str, SZ size)
{
    OFF off = 0;
    int ret = 0;

    /* Optimization: Most documents contain no NUL at all. */
    if(!ctx->doc_has_nul)
        return ctx->parser.text(type, str, size, ctx->userdata);

    while(1) {
#if defined ORG4C_USE_UTF16
        while(off < size  &&  str[off] != _T('\0'))
            off++;
#else
        /* Optimization: memchr() is much faster than a plain loop and most
         * texts contain no NUL at all. */
        {
            const CHAR* nul = (const CHAR*) memchr(str, '\0', size);
            off = (nul != NULL ? (OFF) (nul - str) : size);
        }
#endif

        if(off > 0) {
            ret = ctx->parser.text(type, str, off, ctx->userdata);
            if(ret != 0)
                return ret;

            str += off;
            size -= off;
            off = 0;
        }

        if(off >= size)
            return 0;

        ret = ctx->parser.text(ORG_TEXT_NULLCHAR, _T(""), 1, ctx->userdata);
        if(ret != 0)
            return ret;
        str++;
        size--;
    }
}


#define ORG_CHECK(func)                                                     \
    do {                                                                    \
        ret = (func);                                                       \
        if(ret != 0)                                                        \
            goto abort;                                                     \
    } while(0)


#define ORG_ENTER_BLOCK(type, arg)                                          \
    do {                                                                    \
        ret = ctx->parser.enter_block((type), (arg), ctx->userdata);        \
        if(ret != 0) {                                                      \
            ORG_LOG("Aborted from enter_block() callback.");                \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define ORG_LEAVE_BLOCK(type, arg)                                          \
    do {                                                                    \
        ret = ctx->parser.leave_block((type), (arg), ctx->userdata);        \
        if(ret != 0) {                                                      \
            ORG_LOG("Aborted from leave_block() callback.");                \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define ORG_ENTER_SPAN(type, arg)                                           \
    do {                                                                    \
        ret = ctx->parser.enter_span((type), (arg), ctx->userdata);         \
        if(ret != 0) {                                                      \
            ORG_LOG("Aborted from enter_span() callback.");                 \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define ORG_LEAVE_SPAN(type, arg)                                           \
    do {                                                                    \
        ret = ctx->parser.leave_span((type), (arg), ctx->userdata);         \
        if(ret != 0) {                                                      \
            ORG_LOG("Aborted from leave_span() callback.");                 \
            goto abort;                                                     \
        }                                                                   \
    } while(0)

#define ORG_TEXT(type, str, size)                                           \
    do {                                                                    \
        if((size) > 0) {                                                    \
            ret = ctx->parser.text((type), (str), (size), ctx->userdata);   \
            if(ret != 0) {                                                  \
                ORG_LOG("Aborted from text() callback.");                   \
                goto abort;                                                 \
            }                                                               \
        }                                                                   \
    } while(0)

#define ORG_TEXT_INSECURE(type, str, size)                                  \
    do {                                                                    \
        if((size) > 0) {                                                    \
            ret = org_text_with_null_replacement(ctx, type, str, size);     \
            if(ret != 0) {                                                  \
                ORG_LOG("Aborted from text() callback.");                   \
                goto abort;                                                 \
            }                                                               \
        }                                                                   \
    } while(0)


/* If the offset falls into a gap between line, we return the following
 * line. */
static const ORG_LINE*
org_lookup_line(OFF off, const ORG_LINE* lines, SZ n_lines, SZ* p_line_index)
{
    SZ lo, hi;
    SZ pivot;

    /* Find the first line whose end is not before the offset. */
    lo = 0;
    hi = n_lines;
    while(lo < hi) {
        pivot = lo + (hi - lo) / 2;
        if(lines[pivot].end < off)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    if(lo >= n_lines)
        return NULL;
    if(p_line_index != NULL)
        *p_line_index = lo;
    return &lines[lo];
}

/* Find the first element of the sorted array arr[] greater than off. */
static int
org_bsearch_offset_after(const OFF* arr, int n, OFF off)
{
    int lo = 0;
    int hi = n;

    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        if(arr[pivot] <= off)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    return lo;
}

/* Grow the array 'arr' (with 'alloc' items allocated) so it can hold at least
 * n+1 items. On a failure, it jumps to 'abort' (as ORG_CHECK() does). (It is
 * a macro so that realloc() is used with the right type of the pointer.) */
#define ORG_GROW_ARRAY(arr, alloc, n)                                       \
    do {                                                                    \
        if((n) >= (alloc)) {                                                \
            int new_alloc__ = ((alloc) > 0 ? (alloc) + (alloc) / 2 : 64);   \
            void* new_arr__ = realloc((arr), new_alloc__ * sizeof(*(arr))); \
            if(new_arr__ == NULL) {                                         \
                ORG_LOG("realloc() failed.");                               \
                ret = -1;                                                   \
                goto abort;                                                 \
            }                                                               \
            (arr) = new_arr__;                                              \
            (alloc) = new_alloc__;                                          \
        }                                                                   \
    } while(0)


/*************************
 ***  Unicode Support  ***
 *************************/

#if defined ORG4C_USE_UTF16 || defined ORG4C_USE_UTF8
    /* Binary search over sorted "map" of codepoints. Consecutive sequences
     * of codepoints may be encoded in the map by just using the
     * (MIN_CODEPOINT | 0x40000000) and (MAX_CODEPOINT | 0x80000000).
     *
     * Returns index of the found record in the map (in the case of ranges,
     * the minimal value is used); or -1 on failure. */
    static int
    org_unicode_bsearch__(unsigned codepoint, const unsigned* map, size_t map_size)
    {
        int beg, end;
        int pivot_beg, pivot_end;

        beg = 0;
        end = (int) map_size-1;
        while(beg <= end) {
            /* Pivot may be a range, not just a single value. */
            pivot_beg = pivot_end = (beg + end) / 2;
            if(map[pivot_end] & 0x40000000)
                pivot_end++;
            if(map[pivot_beg] & 0x80000000)
                pivot_beg--;

            if(codepoint < (map[pivot_beg] & 0x00ffffff))
                end = pivot_beg - 1;
            else if(codepoint > (map[pivot_end] & 0x00ffffff))
                beg = pivot_end + 1;
            else
                return pivot_beg;
        }

        return -1;
    }

    static int
    org_is_unicode_whitespace__(unsigned codepoint)
    {
#define R(cp_min, cp_max)   ((cp_min) | 0x40000000), ((cp_max) | 0x80000000)
#define S(cp)               (cp)
        /* Unicode "Zs" category.
         * (generated by scripts/build_whitespace_map.py) */
        static const unsigned WHITESPACE_MAP[] = {
            S(0x0020), S(0x00a0), S(0x1680), R(0x2000,0x200a), S(0x202f), S(0x205f), S(0x3000)
        };
#undef R
#undef S

        /* The ASCII ones are the most frequently used ones. Org also treats
         * new lines as whitespace in this context. */
        if(codepoint <= 0x7f)
            return ISWHITESPACE_(codepoint) || ISNEWLINE_(codepoint);

        return (org_unicode_bsearch__(codepoint, WHITESPACE_MAP, SIZEOF_ARRAY(WHITESPACE_MAP)) >= 0);
    }
#endif


#if defined ORG4C_USE_UTF16
    #define IS_UTF16_SURROGATE_HI(word)     (((WORD)(word) & 0xfc00) == 0xd800)
    #define IS_UTF16_SURROGATE_LO(word)     (((WORD)(word) & 0xfc00) == 0xdc00)
    #define UTF16_DECODE_SURROGATE(hi, lo)  (0x10000 + ((((unsigned)(hi) & 0x3ff) << 10) | (((unsigned)(lo) & 0x3ff) << 0)))

    /* No whitespace uses surrogates, so no decoding needed here. */
    #define ISUNICODEWHITESPACE_(codepoint) org_is_unicode_whitespace__(codepoint)
    #define ISUNICODEWHITESPACE(off)        org_is_unicode_whitespace__(CH(off))
    #define ISUNICODEWHITESPACEBEFORE(off)  org_is_unicode_whitespace__(CH((off)-1))
#elif defined ORG4C_USE_UTF8
    #define IS_UTF8_LEAD1(byte)     ((unsigned char)(byte) <= 0x7f)
    #define IS_UTF8_LEAD2(byte)     (((unsigned char)(byte) & 0xe0) == 0xc0)
    #define IS_UTF8_LEAD3(byte)     (((unsigned char)(byte) & 0xf0) == 0xe0)
    #define IS_UTF8_LEAD4(byte)     (((unsigned char)(byte) & 0xf8) == 0xf0)
    #define IS_UTF8_TAIL(byte)      (((unsigned char)(byte) & 0xc0) == 0x80)

    static unsigned
    org_decode_utf8__(const CHAR* str, SZ str_size, SZ* p_size)
    {
        if(!IS_UTF8_LEAD1(str[0])) {
            if(IS_UTF8_LEAD2(str[0])) {
                if(1 < str_size && IS_UTF8_TAIL(str[1])) {
                    if(p_size != NULL)
                        *p_size = 2;

                    return (((unsigned int)str[0] & 0x1f) << 6) |
                           (((unsigned int)str[1] & 0x3f) << 0);
                }
            } else if(IS_UTF8_LEAD3(str[0])) {
                if(2 < str_size && IS_UTF8_TAIL(str[1]) && IS_UTF8_TAIL(str[2])) {
                    if(p_size != NULL)
                        *p_size = 3;

                    return (((unsigned int)str[0] & 0x0f) << 12) |
                           (((unsigned int)str[1] & 0x3f) << 6) |
                           (((unsigned int)str[2] & 0x3f) << 0);
                }
            } else if(IS_UTF8_LEAD4(str[0])) {
                if(3 < str_size && IS_UTF8_TAIL(str[1]) && IS_UTF8_TAIL(str[2]) && IS_UTF8_TAIL(str[3])) {
                    if(p_size != NULL)
                        *p_size = 4;

                    return (((unsigned int)str[0] & 0x07) << 18) |
                           (((unsigned int)str[1] & 0x3f) << 12) |
                           (((unsigned int)str[2] & 0x3f) << 6) |
                           (((unsigned int)str[3] & 0x3f) << 0);
                }
            }
        }

        if(p_size != NULL)
            *p_size = 1;
        return (unsigned) str[0];
    }

    static unsigned
    org_decode_utf8_before__(ORG_CTX* ctx, OFF off)
    {
        if(!IS_UTF8_LEAD1(CH(off-1))) {
            if(off > 1 && IS_UTF8_LEAD2(CH(off-2)) && IS_UTF8_TAIL(CH(off-1)))
                return (((unsigned int)CH(off-2) & 0x1f) << 6) |
                       (((unsigned int)CH(off-1) & 0x3f) << 0);

            if(off > 2 && IS_UTF8_LEAD3(CH(off-3)) && IS_UTF8_TAIL(CH(off-2)) && IS_UTF8_TAIL(CH(off-1)))
                return (((unsigned int)CH(off-3) & 0x0f) << 12) |
                       (((unsigned int)CH(off-2) & 0x3f) << 6) |
                       (((unsigned int)CH(off-1) & 0x3f) << 0);

            if(off > 3 && IS_UTF8_LEAD4(CH(off-4)) && IS_UTF8_TAIL(CH(off-3)) && IS_UTF8_TAIL(CH(off-2)) && IS_UTF8_TAIL(CH(off-1)))
                return (((unsigned int)CH(off-4) & 0x07) << 18) |
                       (((unsigned int)CH(off-3) & 0x3f) << 12) |
                       (((unsigned int)CH(off-2) & 0x3f) << 6) |
                       (((unsigned int)CH(off-1) & 0x3f) << 0);
        }

        return (unsigned) CH(off-1);
    }

    /* Optimization: Most characters tested are ASCII, so avoid the decoding
     * and the table lookup for them. */
    #define ISUNICODEWHITESPACE_(codepoint) org_is_unicode_whitespace__(codepoint)
    #define ISUNICODEWHITESPACE(off)        (IS_UTF8_LEAD1(CH(off))                                        \
                                                ? (ISWHITESPACE(off) || ISNEWLINE(off))                     \
                                                : org_is_unicode_whitespace__(org_decode_utf8__(STR(off), ctx->size - (off), NULL)))
    #define ISUNICODEWHITESPACEBEFORE(off)  (IS_UTF8_LEAD1(CH((off)-1))                                    \
                                                ? (ISWHITESPACE((off)-1) || ISNEWLINE((off)-1))             \
                                                : org_is_unicode_whitespace__(org_decode_utf8_before__(ctx, off)))
#else
    #define ISUNICODEWHITESPACE_(codepoint) (ISWHITESPACE_(codepoint) || ISNEWLINE_(codepoint))
    #define ISUNICODEWHITESPACE(off)        (ISWHITESPACE(off) || ISNEWLINE(off))
    #define ISUNICODEWHITESPACEBEFORE(off)  (ISWHITESPACE((off)-1) || ISNEWLINE((off)-1))
#endif


/*************************************
 ***  Helper string manipulations  ***
 *************************************/

/* Skip blank characters (spaces and tabs) forward. */
static OFF
org_skip_blanks(ORG_CTX* ctx, OFF off, OFF end)
{
    while(off < end  &&  ISBLANK(off))
        off++;
    return off;
}

/* Column after the blank character 'ch' at the column 'col'. */
#define ORG_NEXT_INDENT_COL(col, ch)                                        \
        ((ch) == _T('\t') ? ((col) / TAB_WIDTH + 1) * TAB_WIDTH : (col) + 1)

/* Width (in columns) of the blank characters [beg, end), from column zero. */
static unsigned
org_indent_width(ORG_CTX* ctx, OFF beg, OFF end)
{
    unsigned col = 0;

    while(beg < end) {
        col = ORG_NEXT_INDENT_COL(col, CH(beg));
        beg++;
    }
    return col;
}

/* Skip blank characters (spaces and tabs) backward. */
static OFF
org_skip_blanks_backward(ORG_CTX* ctx, OFF beg, OFF off)
{
    while(off > beg  &&  ISBLANK(off-1))
        off--;
    return off;
}

/* Skip non-blank characters forward. */
static OFF
org_skip_word(ORG_CTX* ctx, OFF off, OFF end)
{
    while(off < end  &&  !ISBLANK(off))
        off++;
    return off;
}

/* Check whether the offset 'off' is at the end of a word, i.e. at a blank or
 * at the end 'end'. */
static int
org_is_word_end(ORG_CTX* ctx, OFF off, OFF end)
{
    return (off >= end  ||  ISBLANK(off));
}


/******************************
 ***  Attribute Management  ***
 ******************************/

typedef struct ORG_ATTRIBUTE_BUILD_tag ORG_ATTRIBUTE_BUILD;
struct ORG_ATTRIBUTE_BUILD_tag {
    ORG_TEXTTYPE trivial_types[1];
    OFF trivial_offsets[2];
};

/* Most attributes are trivial (i.e. there is only a single ORG_TEXT_NORMAL
 * substring). But each NUL forms an ORG_TEXT_NULLCHAR substring of its own,
 * as in the text callbacks (so the application may replace it). The attribute
 * text always points into the input; only the substring arrays of the
 * non-trivial attributes need an allocation. */
static void
org_build_attribute(ORG_CTX* ctx, const CHAR* text, SZ size, ORG_ATTRIBUTE* attr, ORG_ATTRIBUTE_BUILD* build)
{
    attr->text = (size > 0 ? text : NULL);
    attr->size = size;

    if(ctx->doc_has_nul  &&  size > 0) {
        SZ n_substrs = 0;
        int in_normal = false;
        SZ i;

        for(i = 0; i < size; i++) {
            if(text[i] == _T('\0')) {
                n_substrs++;
                in_normal = false;
            } else if(!in_normal) {
                n_substrs++;
                in_normal = true;
            }
        }

        if(n_substrs > 1  ||  text[0] == _T('\0')) {
            ORG_ATTRIBUTE_STORAGE* storage;
            OFF* offsets;
            ORG_TEXTTYPE* types;
            SZ k = 0;

            storage = (ORG_ATTRIBUTE_STORAGE*) malloc(sizeof(ORG_ATTRIBUTE_STORAGE) +
                        (n_substrs + 1) * sizeof(OFF) + n_substrs * sizeof(ORG_TEXTTYPE));
            if(storage != NULL) {
                storage->next = ctx->attribute_storages;
                ctx->attribute_storages = storage;
                offsets = (OFF*) (storage + 1);
                types = (ORG_TEXTTYPE*) (offsets + n_substrs + 1);

                for(i = 0; i < size; i++) {
                    if(text[i] == _T('\0')) {
                        types[k] = ORG_TEXT_NULLCHAR;
                        offsets[k++] = i;
                    } else if(i == 0  ||  text[i-1] == _T('\0')) {
                        types[k] = ORG_TEXT_NORMAL;
                        offsets[k++] = i;
                    }
                }
                offsets[k] = size;

                attr->substr_types = types;
                attr->substr_offsets = offsets;
                return;
            }

            /* Out of memory: Report the attribute as trivial, NULs included. */
            ORG_LOG("malloc() failed.");
        }
    }

    build->trivial_types[0] = ORG_TEXT_NORMAL;
    build->trivial_offsets[0] = 0;
    build->trivial_offsets[1] = size;

    attr->substr_types = build->trivial_types;
    attr->substr_offsets = build->trivial_offsets;
}


/************************
 ***  Document Index  ***
 ************************/

/* Check the whole document once for the characters whose absence allows
 * some faster scanning later. */
static void
org_check_doc_chars(ORG_CTX* ctx)
{
#if defined ORG4C_USE_UTF16
    /* Conservative (memchr() works only for 8-bit characters). */
    ctx->doc_has_nul = true;
    ctx->doc_has_radio_opener = true;
#else
    const CHAR* ptr = ctx->text;
    const CHAR* end = ctx->text + ctx->size;

    ctx->doc_has_nul = (ctx->size > 0  &&  memchr(ctx->text, '\0', ctx->size) != NULL);

    /* Look for "<<<" (the opener of a radio target). */
    ctx->doc_has_radio_opener = false;
    while(end - ptr >= 3) {
        ptr = (const CHAR*) memchr(ptr, '<', (size_t) (end - ptr - 2));
        if(ptr == NULL)
            break;
        if(ptr[1] == _T('<')  &&  ptr[2] == _T('<')) {
            ctx->doc_has_radio_opener = true;
            break;
        }
        ptr++;
    }
#endif
}

/* Get the end of the line starting at the given offset (i.e. the offset of
 * the new line character or the end of the document). */
static OFF
org_line_end(ORG_CTX* ctx, OFF off)
{
    /* NOTE: This is one of the hottest loops in our code. Hence we try to
     * optimize this (as md4c does), assuming memchr() is highly optimized and
     * uses SIMD if it's available on the platform: We remember how far there
     * is no '\r' and no '\n' (the horizons), so each of them is searched for
     * only once in each chunk of the text. (Unlike in md4c, the lines are not
     * always visited in their order, so a horizon is valid only after the
     * offset where the search for it started.) */
    while(off < ctx->size  &&  !ISNEWLINE(off)) {
        /* Don't look too much ahead to keep the text in CPU cache as we scan
         * over it twice here. */
        static const SZ max_lookahead = 8192;

        if(off < ctx->cr_scan_beg  ||  (ctx->cr_horizon <= off  &&  ctx->cr_horizon < ctx->size)) {
            SZ scan_len = MIN(max_lookahead, ctx->size - off);
            const CHAR* ptr = (const CHAR*) org_memchr(STR(off), _T('\r'), scan_len);
            ctx->cr_scan_beg = off;
            ctx->cr_horizon = (ptr != NULL ? (OFF) (ptr - ctx->text) : off + scan_len);
        }
        if(off < ctx->lf_scan_beg  ||  (ctx->lf_horizon <= off  &&  ctx->lf_horizon < ctx->size)) {
            SZ scan_len = MIN(max_lookahead, ctx->size - off);
            const CHAR* ptr = (const CHAR*) org_memchr(STR(off), _T('\n'), scan_len);
            ctx->lf_scan_beg = off;
            ctx->lf_horizon = (ptr != NULL ? (OFF) (ptr - ctx->text) : off + scan_len);
        }
        off = MIN(ctx->cr_horizon, ctx->lf_horizon);
    }
    return off;
}

/* Skip the new line (if any) at the given offset. */
static OFF
org_skip_newline(ORG_CTX* ctx, OFF off)
{
    if(off < ctx->size  &&  CH(off) == _T('\r'))
        off++;
    if(off < ctx->size  &&  CH(off) == _T('\n'))
        off++;
    return off;
}

/* Check whether the line [beg, end) is a headline. If yes, returns its level;
 * zero otherwise. */
static unsigned
org_is_headline_line(ORG_CTX* ctx, OFF beg, OFF end)
{
    OFF off = beg;

    while(off < end  &&  CH(off) == _T('*'))
        off++;
    if(off == beg  ||  off >= end  ||  CH(off) != _T(' '))
        return 0;

    return (unsigned) MIN(off - beg, HEADLINE_MAX_LEVEL);
}

/* Check whether [beg, end) (after the indentation) is "#+END_name". If yes,
 * provides the name. */
static int
org_is_block_end_line(ORG_CTX* ctx, OFF beg, OFF end, OFF* p_name_beg, OFF* p_name_end)
{
    OFF off;

    if(!org_is_prefix_i(ctx, beg, end, _T("#+END_")))
        return false;

    off = beg + 6;
    *p_name_beg = off;
    off = org_skip_word(ctx, off, end);
    *p_name_end = off;
    if(*p_name_end == *p_name_beg)
        return false;

    return (org_skip_blanks(ctx, off, end) == end);
}

/* Check whether [beg, end) (after the indentation) is ":END:". */
static int
org_is_drawer_end_line(ORG_CTX* ctx, OFF beg, OFF end)
{
    if(!org_is_prefix_i(ctx, beg, end, _T(":END:")))
        return false;

    return (org_skip_blanks(ctx, beg + 5, end) == end);
}

/* Check whether [beg, end) (after the indentation) is "\end{name}". If yes,
 * provides the name. */
static int
org_is_latex_end_line(ORG_CTX* ctx, OFF beg, OFF end, OFF* p_name_beg, OFF* p_name_end)
{
    OFF off;

    if(!org_is_prefix(ctx, beg, end, _T("\\end{")))
        return false;

    off = beg + 5;
    *p_name_beg = off;
    while(off < end  &&  (ISALNUM(off)  ||  CH(off) == _T('*')))
        off++;
    *p_name_end = off;
    if(off == *p_name_beg  ||  off >= end  ||  CH(off) != _T('}'))
        return false;

    return (org_skip_blanks(ctx, off + 1, end) == end);
}

/* Collect the TODO keywords from "#+TODO: TODO NEXT | DONE CANCELED" line.
 * The keywords after '|' are the "done" ones; if there is no '|', only the
 * last keyword is. The fast access keys (e.g. "TODO(t)") are ignored. */
static int
org_collect_todo_keywords(ORG_CTX* ctx, OFF beg, OFF end)
{
    int first = ctx->n_todo_keywords;
    int has_bar = false;
    OFF off = beg;
    int ret = 0;

    while(1) {
        OFF word_beg = org_skip_blanks(ctx, off, end);
        OFF word_end = org_skip_word(ctx, word_beg, end);
        OFF name_end = word_beg;
        ORG_TODO_KEYWORD* kw;

        if(word_beg >= end)
            break;
        off = word_end;

        if(word_end - word_beg == 1  &&  CH(word_beg) == _T('|')) {
            has_bar = true;
            continue;
        }

        while(name_end < word_end  &&  CH(name_end) != _T('('))
            name_end++;
        if(name_end == word_beg)
            continue;

        ORG_GROW_ARRAY(ctx->todo_keywords, ctx->alloc_todo_keywords, ctx->n_todo_keywords);
        kw = &ctx->todo_keywords[ctx->n_todo_keywords++];
        kw->name = STR(word_beg);
        kw->name_size = name_end - word_beg;
        kw->is_done = has_bar;
    }

    if(!has_bar  &&  ctx->n_todo_keywords > first)
        ctx->todo_keywords[ctx->n_todo_keywords - 1].is_done = true;

abort:
    return ret;
}

/* Add a record into an array of the terminating lines. */
static int
org_push_block_end(ORG_CTX* ctx, ORG_BLOCK_END** p_ends, int* p_n_ends, int* p_alloc_ends,
                   OFF line_beg, OFF name_beg, OFF name_end)
{
    ORG_BLOCK_END* block_end;
    int ret = 0;

    ORG_GROW_ARRAY(*p_ends, *p_alloc_ends, *p_n_ends);
    block_end = &(*p_ends)[(*p_n_ends)++];
    block_end->beg = line_beg;
    block_end->name = STR(name_beg);
    block_end->name_size = name_end - name_beg;

abort:
    return ret;
}

static int
org_block_end_cmp(const void* a, const void* b)
{
    const ORG_BLOCK_END* end_a = (const ORG_BLOCK_END*) a;
    const ORG_BLOCK_END* end_b = (const ORG_BLOCK_END*) b;
    int cmp;

    cmp = org_ascii_case_cmp(end_a->name, end_a->name_size, end_b->name, end_b->name_size);
    if(cmp != 0)
        return cmp;
    if(end_a->beg != end_b->beg)
        return (end_a->beg < end_b->beg) ? -1 : +1;
    return 0;
}

static int org_is_keyword_line(ORG_CTX* ctx, OFF beg, OFF end, OFF* p_key_end, OFF* p_value_beg);

static int
org_doc_keyword_cmp(const void* a, const void* b)
{
    const ORG_DOC_KEYWORD* kw_a = (const ORG_DOC_KEYWORD*) a;
    const ORG_DOC_KEYWORD* kw_b = (const ORG_DOC_KEYWORD*) b;
    int cmp;

    cmp = org_ascii_case_cmp(kw_a->key, kw_a->key_size, kw_b->key, kw_b->key_size);
    if(cmp != 0)
        return cmp;
    if(kw_a->beg != kw_b->beg)
        return (kw_a->beg < kw_b->beg) ? -1 : +1;
    return 0;
}

/* Case-folded first character of the radio target. (In the UTF-16 build, its
 * lower byte: The radio targets are indexed by it.) */
static unsigned char
org_radio_target_key(const ORG_RADIO_TARGET* rt)
{
    unsigned char ch = (unsigned char) rt->text[0];
    return ISLOWER_(ch) ? (unsigned char) (ch - 'a' + 'A') : ch;
}

static int
org_radio_target_cmp(const void* a, const void* b)
{
    const ORG_RADIO_TARGET* rt_a = (const ORG_RADIO_TARGET*) a;
    const ORG_RADIO_TARGET* rt_b = (const ORG_RADIO_TARGET*) b;

    if(org_radio_target_key(rt_a) != org_radio_target_key(rt_b))
        return (org_radio_target_key(rt_a) < org_radio_target_key(rt_b)) ? -1 : +1;

    /* Longest first; so the longest match wins. */
    if(rt_a->size != rt_b->size)
        return (rt_a->size > rt_b->size) ? -1 : +1;
    return (rt_a->text < rt_b->text) ? -1 : ((rt_a->text > rt_b->text) ? +1 : 0);
}

/* Check whether there is a radio target "<<<text>>>" at 'off'. If yes,
 * provides the range of the text. */
static int
org_is_radio_target(ORG_CTX* ctx, OFF off, OFF end, OFF* p_text_beg, OFF* p_text_end)
{
    OFF q = off + 3;

    if(!org_is_prefix(ctx, off, end, _T("<<<"))  ||  q >= end  ||  ISWHITESPACE(q))
        return false;
    while(q < end  &&  !ISANYOF(q, _T("<>\r\n")))
        q++;
    if(q + 2 >= end  ||  !org_is_prefix(ctx, q, end, _T(">>>"))  ||  ISWHITESPACE(q-1))
        return false;

    *p_text_beg = off + 3;
    *p_text_end = q;
    return true;
}

/* Add a record into an array of the keyword lines. */
static int
org_push_doc_keyword(ORG_CTX* ctx, ORG_DOC_KEYWORD** p_arr, int* p_n, int* p_alloc,
                     OFF beg, OFF key_beg, OFF key_end, OFF value_beg, OFF value_end)
{
    ORG_DOC_KEYWORD* kw;
    int ret = 0;

    ORG_GROW_ARRAY(*p_arr, *p_alloc, *p_n);
    kw = &(*p_arr)[(*p_n)++];
    kw->beg = beg;
    kw->key = STR(key_beg);
    kw->key_size = key_end - key_beg;
    kw->value = STR(value_beg);
    kw->value_size = value_end - value_beg;

abort:
    return ret;
}

/* Collect the radio targets "<<<text>>>" on the line [beg, end). */
static int
org_collect_radio_targets(ORG_CTX* ctx, OFF beg, OFF end)
{
    OFF off = beg;
    int ret = 0;

    while(off + 6 < end) {
        OFF text_beg, text_end;

        /* Optimization: Skip quickly to the next '<'. */
#if defined ORG4C_USE_UTF16
        while(off + 6 < end  &&  CH(off) != _T('<'))
            off++;
#else
        {
            const CHAR* ptr = (const CHAR*) memchr(STR(off), '<', end - off);
            if(ptr == NULL)
                break;
            off = (OFF) (ptr - ctx->text);
        }
#endif
        if(off + 6 >= end)
            break;

        if(CH(off) == _T('<')  &&  org_is_radio_target(ctx, off, end, &text_beg, &text_end)  &&
           ctx->n_radio_targets < RADIO_TARGETS_MAX)
        {
            ORG_RADIO_TARGET* rt;

            ORG_GROW_ARRAY(ctx->radio_targets, ctx->alloc_radio_targets, ctx->n_radio_targets);
            rt = &ctx->radio_targets[ctx->n_radio_targets++];
            rt->text = STR(text_beg);
            rt->size = text_end - text_beg;
            off = text_end + 3;
        } else {
            off++;
        }
    }

abort:
    return ret;
}

/* Build the document-wide indexes of the headlines, of the lines which may
 * terminate a block or a drawer, of the keywords and of the radio targets. */
static int
org_build_doc_index(ORG_CTX* ctx)
{
    OFF line_beg = 0;
    int i;
    int ret = 0;

    while(line_beg < ctx->size) {
        OFF line_end = org_line_end(ctx, line_beg);
        OFF beg = org_skip_blanks(ctx, line_beg, line_end);
        OFF end = org_skip_blanks_backward(ctx, beg, line_end);
        OFF name_beg, name_end;
        OFF key_end, value_beg;

        if(beg == line_beg  &&  (ctx->parser.flags & ORG_FLAG_INLINETASKS)  &&
           org_is_headline_line(ctx, line_beg, line_end) >= INLINETASK_MIN_LEVEL)
        {
            /* Inline tasks are not real headlines. */
            ORG_GROW_ARRAY(ctx->inlinetask_ends, ctx->alloc_inlinetask_ends, ctx->n_inlinetask_ends);
            ctx->inlinetask_ends[ctx->n_inlinetask_ends++] = line_beg;
        } else if(beg == line_beg  &&  org_is_headline_line(ctx, line_beg, line_end) > 0) {
            ORG_GROW_ARRAY(ctx->headlines, ctx->alloc_headlines, ctx->n_headlines);
            ctx->headlines[ctx->n_headlines++] = line_beg;
        } else if(beg < end  &&  CH(beg) == _T('#')) {
            if(org_is_block_end_line(ctx, beg, end, &name_beg, &name_end)) {
                ORG_CHECK(org_push_block_end(ctx, &ctx->block_ends, &ctx->n_block_ends, &ctx->alloc_block_ends,
                            line_beg, name_beg, name_end));
            } else if(org_is_prefix_i(ctx, beg, end, _T("#+END:"))  &&  org_skip_blanks(ctx, beg + 6, end) == end) {
                ORG_GROW_ARRAY(ctx->dynamic_ends, ctx->alloc_dynamic_ends, ctx->n_dynamic_ends);
                ctx->dynamic_ends[ctx->n_dynamic_ends++] = line_beg;
            } else if(org_is_keyword_line(ctx, beg, end, &key_end, &value_beg)) {
                ORG_CHECK(org_push_doc_keyword(ctx, &ctx->doc_keywords, &ctx->n_doc_keywords,
                            &ctx->alloc_doc_keywords, line_beg, beg + 2, key_end, value_beg, end));

                if(org_is_prefix_i(ctx, beg, end, _T("#+TODO:"))) {
                    ORG_CHECK(org_collect_todo_keywords(ctx, beg + 7, end));
                } else if(org_is_prefix_i(ctx, beg, end, _T("#+SEQ_TODO:"))  ||
                          org_is_prefix_i(ctx, beg, end, _T("#+TYP_TODO:")))
                {
                    ORG_CHECK(org_collect_todo_keywords(ctx, beg + 11, end));
                } else if(org_is_prefix_i(ctx, beg, end, _T("#+MACRO:"))) {
                    /* "#+MACRO: name template" */
                    OFF macro_name_beg = org_skip_blanks(ctx, beg + 8, end);
                    OFF macro_name_end = org_skip_word(ctx, macro_name_beg, end);

                    if(macro_name_end > macro_name_beg) {
                        ORG_CHECK(org_push_doc_keyword(ctx, &ctx->macros, &ctx->n_macros, &ctx->alloc_macros,
                                    line_beg, macro_name_beg, macro_name_end,
                                    org_skip_blanks(ctx, macro_name_end, end), end));
                    }
                }
            }
        } else if(beg < end  &&  CH(beg) == _T(':')) {
            if(org_is_drawer_end_line(ctx, beg, end)) {
                ORG_GROW_ARRAY(ctx->drawer_ends, ctx->alloc_drawer_ends, ctx->n_drawer_ends);
                ctx->drawer_ends[ctx->n_drawer_ends++] = line_beg;
            }
        } else if(beg < end  &&  CH(beg) == _T('\\')) {
            if(org_is_latex_end_line(ctx, beg, end, &name_beg, &name_end)) {
                ORG_CHECK(org_push_block_end(ctx, &ctx->latex_ends, &ctx->n_latex_ends, &ctx->alloc_latex_ends,
                            line_beg, name_beg, name_end));
            }
        }

        /* Optimization: Most documents contain no radio target at all. */
        if(ctx->doc_has_radio_opener)
            ORG_CHECK(org_collect_radio_targets(ctx, beg, end));

        line_beg = org_skip_newline(ctx, line_end);
    }

    /* Headlines, drawer ends, inline tasks and dynamic block ends are sorted
     * by the nature of their collecting. The block ends and the keywords are
     * looked up by their name, so sort them (stable with respect to the
     * offset) by the name; the radio targets by their first character. */
    if(ctx->n_block_ends > 1)
        qsort(ctx->block_ends, ctx->n_block_ends, sizeof(ORG_BLOCK_END), org_block_end_cmp);
    if(ctx->n_latex_ends > 1)
        qsort(ctx->latex_ends, ctx->n_latex_ends, sizeof(ORG_BLOCK_END), org_block_end_cmp);
    if(ctx->n_doc_keywords > 1)
        qsort(ctx->doc_keywords, ctx->n_doc_keywords, sizeof(ORG_DOC_KEYWORD), org_doc_keyword_cmp);
    if(ctx->n_macros > 1)
        qsort(ctx->macros, ctx->n_macros, sizeof(ORG_DOC_KEYWORD), org_doc_keyword_cmp);
    if(ctx->n_radio_targets > 1)
        qsort(ctx->radio_targets, ctx->n_radio_targets, sizeof(ORG_RADIO_TARGET), org_radio_target_cmp);

    /* Map of (case-folded) first characters of the radio targets. */
    for(i = ctx->n_radio_targets - 1; i >= 0; i--) {
        unsigned char ch = org_radio_target_key(&ctx->radio_targets[i]);
        ctx->radio_first_index[ch] = i;
        ctx->radio_first_char_map[ch] = 1;
        if(ISUPPER_(ch))
            ctx->radio_first_char_map[ch - 'A' + 'a'] = 1;
    }

abort:
    return ret;
}

/* Returns beginning of the first headline after the given offset, or the end
 * of the document if there is none. */
static OFF
org_next_headline(ORG_CTX* ctx, OFF off)
{
    int i = org_bsearch_offset_after(ctx->headlines, ctx->n_headlines, off);
    return (i < ctx->n_headlines) ? ctx->headlines[i] : ctx->size;
}

/* Find the line (e.g. "#+END_name") terminating block beginning at 'off',
 * which has to be before 'limit'. Returns beginning of that line or zero if
 * there is no such line. */
static OFF
org_find_block_end(const ORG_BLOCK_END* ends, int n_ends, OFF off, const CHAR* name, SZ name_size, OFF limit)
{
    int lo = 0;
    int hi = n_ends;

    /* Find the first record which is not less than (name, off+1). */
    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        const ORG_BLOCK_END* block_end = &ends[pivot];
        int cmp = org_ascii_case_cmp(block_end->name, block_end->name_size, name, name_size);

        if(cmp < 0  ||  (cmp == 0  &&  block_end->beg <= off))
            lo = pivot + 1;
        else
            hi = pivot;
    }

    if(lo < n_ends) {
        const ORG_BLOCK_END* block_end = &ends[lo];

        if(block_end->name_size == name_size  &&
           org_ascii_case_eq(block_end->name, name, name_size)  &&
           block_end->beg < limit)
            return block_end->beg;
    }

    return 0;
}

/* Find the first of the lines 'ends' (e.g. ":END:" terminating a drawer)
 * after 'off' and before 'limit'. Returns beginning of that line or zero if
 * there is no such line. */
static OFF
org_find_end_line(const OFF* ends, int n_ends, OFF off, OFF limit)
{
    int i = org_bsearch_offset_after(ends, n_ends, off);

    if(i < n_ends  &&  ends[i] < limit)
        return ends[i];
    return 0;
}


/******************************************************
 ***  Recognizing Headlines, List Items and Blocks  ***
 ******************************************************/

typedef struct ORG_HEADLINE_INFO_tag ORG_HEADLINE_INFO;
struct ORG_HEADLINE_INFO_tag {
    unsigned level;
    OFF todo_beg;
    OFF todo_end;
    int is_done;
    CHAR priority;
    int is_commented;
    OFF title_beg;
    OFF title_end;
    OFF tags_beg;           /* Without the outer colons. */
    OFF tags_end;
};

static int
org_is_tag_char(CHAR ch)
{
    return (ISALNUM_(ch)  ||  ISANYOF_(ch, _T("_@#%"))  ||  !ISASCII_(ch));
}

/* Analyze headline [beg, end). (The caller guarantees it is a headline.) */
static void
org_analyze_headline(ORG_CTX* ctx, OFF beg, OFF end, ORG_HEADLINE_INFO* info)
{
    OFF off = beg;
    OFF word_end;
    OFF tail;

    memset(info, 0, sizeof(ORG_HEADLINE_INFO));

    while(off < end  &&  CH(off) == _T('*'))
        off++;
    info->level = (unsigned) MIN(off - beg, HEADLINE_MAX_LEVEL);
    off = org_skip_blanks(ctx, off, end);

    /* TODO keyword. */
    word_end = org_skip_word(ctx, off, end);
    if(word_end > off) {
        if(ctx->n_todo_keywords > 0) {
            int i;

            for(i = 0; i < ctx->n_todo_keywords; i++) {
                const ORG_TODO_KEYWORD* kw = &ctx->todo_keywords[i];

                if(kw->name_size == word_end - off  &&  org_ascii_eq(kw->name, STR(off), kw->name_size)) {
                    info->todo_beg = off;
                    info->todo_end = word_end;
                    info->is_done = kw->is_done;
                    break;
                }
            }
        } else if(word_end - off == 4  &&  org_ascii_eq(STR(off), _T("TODO"), 4)) {
            info->todo_beg = off;
            info->todo_end = word_end;
        } else if(word_end - off == 4  &&  org_ascii_eq(STR(off), _T("DONE"), 4)) {
            info->todo_beg = off;
            info->todo_end = word_end;
            info->is_done = true;
        }

        if(info->todo_end > info->todo_beg)
            off = org_skip_blanks(ctx, word_end, end);
    }

    /* Priority cookie. */
    if(off + 4 <= end  &&  CH(off) == _T('[')  &&  CH(off+1) == _T('#')  &&
       ISALNUM(off+2)  &&  CH(off+3) == _T(']')  &&  org_is_word_end(ctx, off+4, end))
    {
        info->priority = CH(off+2);
        off = org_skip_blanks(ctx, off + 4, end);
    }

    /* COMMENT keyword. */
    if(org_is_prefix(ctx, off, end, _T("COMMENT"))  &&  org_is_word_end(ctx, off+7, end)) {
        info->is_commented = true;
        off = org_skip_blanks(ctx, off + 7, end);
    }

    info->title_beg = off;

    /* Tags. */
    tail = org_skip_blanks_backward(ctx, off, end);
    info->title_end = tail;
    if(tail - off >= 3  &&  CH(tail-1) == _T(':')) {
        OFF tags_beg = tail - 1;

        while(tags_beg > off  &&  (org_is_tag_char(CH(tags_beg-1))  ||  CH(tags_beg-1) == _T(':')))
            tags_beg--;

        if(CH(tags_beg) == _T(':')  &&  tail - tags_beg >= 3  &&
           (tags_beg == off  ||  ISBLANK(tags_beg-1)))
        {
            info->tags_beg = tags_beg + 1;
            info->tags_end = tail - 1;
            info->title_end = org_skip_blanks_backward(ctx, off, tags_beg);
        }
    }
}


typedef struct ORG_ITEM_INFO_tag ORG_ITEM_INFO;
struct ORG_ITEM_INFO_tag {
    ORG_BLOCKTYPE list_type;    /* ORG_BLOCK_UL, ORG_BLOCK_OL or ORG_BLOCK_DL */
    CHAR mark;                  /* Bullet char, or delimiter char for ordered items. */
    unsigned counter;
    CHAR checkbox;
    OFF tag_beg;
    OFF tag_end;
    OFF contents_beg;
};

/* Check whether [beg, end) (after the indentation 'indent') is a list item.
 * If yes, provide details about it. */
static int
org_is_list_item(ORG_CTX* ctx, OFF beg, OFF end, unsigned indent, ORG_ITEM_INFO* info)
{
    OFF off = beg;

    memset(info, 0, sizeof(ORG_ITEM_INFO));

    if(off >= end)
        return false;

    /* The bullet. */
    if(ISANYOF2(off, _T('-'), _T('+'))  ||  (CH(off) == _T('*')  &&  indent > 0)) {
        info->list_type = ORG_BLOCK_UL;
        info->mark = CH(off);
        off++;
    } else if(ISDIGIT(off)) {
        while(off < end  &&  ISDIGIT(off)  &&  off - beg < 10)
            off++;
        if(off >= end  ||  !ISANYOF2(off, _T('.'), _T(')')))
            return false;
        info->list_type = ORG_BLOCK_OL;
        info->mark = CH(off);
        off++;
    } else {
        return false;
    }

    /* The bullet must be followed by a whitespace or by the end of line. */
    if(off < end  &&  !ISBLANK(off))
        return false;
    off = org_skip_blanks(ctx, off, end);

    /* Counter "[@N]". */
    if(off + 3 < end  &&  CH(off) == _T('[')  &&  CH(off+1) == _T('@')  &&  ISDIGIT(off+2)) {
        OFF tmp = off + 2;
        unsigned counter = 0;

        while(tmp < end  &&  ISDIGIT(tmp)) {
            if(counter <= (LIST_COUNTER_MAX - 9) / 10)
                counter = counter * 10 + (CH(tmp) - _T('0'));
            else
                counter = LIST_COUNTER_MAX;
            tmp++;
        }
        if(tmp < end  &&  CH(tmp) == _T(']')  &&  org_is_word_end(ctx, tmp+1, end)) {
            info->counter = counter;
            off = org_skip_blanks(ctx, tmp + 1, end);
        }
    }

    /* Checkbox "[ ]", "[X]" or "[-]". */
    if(off + 3 <= end  &&  CH(off) == _T('[')  &&  ISANYOF3(off+1, _T(' '), _T('X'), _T('-'))  &&
       CH(off+2) == _T(']')  &&  org_is_word_end(ctx, off+3, end))
    {
        info->checkbox = CH(off+1);
        off = org_skip_blanks(ctx, off + 3, end);
    }

    /* Tag of a description item: "TAG :: CONTENTS". (The last "::" wins.) */
    if(info->list_type == ORG_BLOCK_UL) {
        OFF sep = 0;    /* Offset of the 2nd ':' of the last "::"; or zero. */
        OFF tmp;

#if !defined ORG4C_USE_UTF16
        /* Optimization: Most items have no ':' at all, and memchr() is much
         * faster than a plain loop. So look for all the ':' forward and just
         * remember the last "::" found. */
        tmp = off + 2;
        while(tmp < end) {
            const CHAR* ptr = (const CHAR*) memchr(STR(tmp), ':', end - tmp);

            if(ptr == NULL)
                break;
            tmp = (OFF) (ptr - ctx->text);
            if(CH(tmp-1) == _T(':')  &&  ISBLANK(tmp-2)  &&  org_is_word_end(ctx, tmp+1, end))
                sep = tmp;
            tmp++;
        }
#else
        tmp = end;
        while(tmp > off + 2) {
            tmp--;
            if(CH(tmp) == _T(':')  &&  CH(tmp-1) == _T(':')  &&  ISBLANK(tmp-2)  &&
               org_is_word_end(ctx, tmp+1, end))
            {
                sep = tmp;
                break;
            }
        }
#endif

        if(sep > 0) {
            OFF tag_end = org_skip_blanks_backward(ctx, off, sep-1);

            if(tag_end > off) {
                info->list_type = ORG_BLOCK_DL;
                info->tag_beg = off;
                info->tag_end = tag_end;
                off = org_skip_blanks(ctx, sep + 1, end);
            }
        }
    }

    info->contents_beg = off;
    return true;
}


/* Classify the name of a block (as in "#+BEGIN_name"). */
static ORG_BLOCKTYPE
org_block_type_from_name(const CHAR* name, SZ name_size)
{
    static const struct {
        const CHAR* name;
        SZ name_size;
        ORG_BLOCKTYPE type;
    } block_names[] = {
        { _T("SRC"),        3,  ORG_BLOCK_SRC },
        { _T("EXAMPLE"),    7,  ORG_BLOCK_EXAMPLE },
        { _T("EXPORT"),     6,  ORG_BLOCK_EXPORT },
        { _T("COMMENT"),    7,  ORG_BLOCK_COMMENT },
        { _T("VERSE"),      5,  ORG_BLOCK_VERSE },
        { _T("QUOTE"),      5,  ORG_BLOCK_QUOTE },
        { _T("CENTER"),     6,  ORG_BLOCK_CENTER }
    };
    int i;

    for(i = 0; i < (int) SIZEOF_ARRAY(block_names); i++) {
        if(name_size == block_names[i].name_size  &&
           org_ascii_case_eq(name, block_names[i].name, name_size))
            return block_names[i].type;
    }

    return ORG_BLOCK_SPECIAL;
}

/* Verbatim-like blocks do not contain any other elements. Instead, all their
 * lines are just collected (as ORG_VERBATIMLINE). */
static int
org_block_has_verbatim_lines(ORG_BLOCKTYPE type)
{
    switch(type) {
        case ORG_BLOCK_SRC:
        case ORG_BLOCK_EXAMPLE:
        case ORG_BLOCK_EXPORT:
        case ORG_BLOCK_COMMENT:
        case ORG_BLOCK_FIXED_WIDTH:
        case ORG_BLOCK_PROPERTY_DRAWER:
        case ORG_BLOCK_PLANNING:
        case ORG_BLOCK_LATEX_ENVIRONMENT:
            return true;

        default:
            return false;
    }
}

/* Analyze the line "#+BEGIN_name params" [beg, end). */
static void
org_analyze_block_begin_line(ORG_CTX* ctx, OFF beg, OFF end,
                             OFF* p_name_beg, OFF* p_name_end, OFF* p_params_beg)
{
    OFF off = beg + 8;      /* strlen("#+BEGIN_") */

    *p_name_beg = off;
    off = org_skip_word(ctx, off, end);
    *p_name_end = off;
    *p_params_beg = org_skip_blanks(ctx, off, end);
}

/* Check whether [beg, end) is a drawer beginning ":NAME:". If yes, provides
 * the name. */
static int
org_is_drawer_begin_line(ORG_CTX* ctx, OFF beg, OFF end, OFF* p_name_beg, OFF* p_name_end)
{
    OFF off = beg + 1;

    if(CH(beg) != _T(':'))
        return false;

    while(off < end  &&  (ISALNUM(off)  ||  ISANYOF2(off, _T('-'), _T('_'))))
        off++;
    if(off == beg + 1  ||  off >= end  ||  CH(off) != _T(':'))
        return false;
    if(org_skip_blanks(ctx, off + 1, end) != end)
        return false;

    *p_name_beg = beg + 1;
    *p_name_end = off;
    return true;
}

/* Check whether all the lines after the line 'line_beg' up to the line
 * 'end_line' are node properties ":NAME: VALUE" (or ":NAME:"), as in a
 * property drawer. */
static int
org_is_node_property_block(ORG_CTX* ctx, OFF line_beg, OFF end_line)
{
    OFF off = org_skip_newline(ctx, org_line_end(ctx, line_beg));

    while(off < end_line) {
        OFF end = org_line_end(ctx, off);
        OFF beg = org_skip_blanks(ctx, off, end);

        /* The name is followed by a blank (or by the end of the line). */
        off = beg;
        while(off < end  &&  !ISBLANK(off))
            off++;
        if(off - beg < 3  ||  CH(beg) != _T(':')  ||  CH(off-1) != _T(':'))
            return false;

        off = org_skip_newline(ctx, end);
    }

    return true;
}

/* Check whether [beg, end) is a keyword line "#+KEY: VALUE". If yes,
 * provides the end of the key and the beginning of the value. */
static int
org_is_keyword_line(ORG_CTX* ctx, OFF beg, OFF end, OFF* p_key_end, OFF* p_value_beg)
{
    OFF off = beg + 2;

    if(!org_is_prefix(ctx, beg, end, _T("#+")))
        return false;

    while(off < end  &&  !ISBLANK(off)  &&  CH(off) != _T(':'))
        off++;
    if(off == beg + 2  ||  off >= end  ||  CH(off) != _T(':'))
        return false;

    *p_key_end = off;
    *p_value_beg = org_skip_blanks(ctx, off + 1, end);
    return true;
}


/*******************
 ***  Footnotes  ***
 *******************/

/* Footnotes are identified by their label (except the anonymous ones, i.e.
 * "[fn::definition]"). The definition may be either a block (a footnote
 * definition "[fn:label] ..." somewhere in the document) or inline (in the
 * reference itself, "[fn:label:definition]").
 *
 * The footnotes are numbered in the order of their first reference, and all
 * the referenced ones are reported at the end of the document. */
struct ORG_FOOTNOTE_tag {
    const CHAR* label;
    SZ label_size;
    int def_open_off;       /* Block definition: offset of its opener and closer in ctx->block_bytes; or -1. */
    int def_close_off;
    OFF inline_beg;         /* Inline definition: its contents; or zero. */
    OFF inline_end;
    int has_inline_def;
    int nesting_level;      /* Nesting of the inline definition (in other inline definitions). */
    unsigned id;            /* Assigned on the first reference; zero if not referenced (yet). */
    unsigned ref_count;
};

#define ORG_FNV1A_BASE      2166136261U
#define ORG_FNV1A_PRIME     16777619U

static unsigned
org_footnote_hash(const CHAR* label, SZ label_size)
{
    unsigned hash = ORG_FNV1A_BASE;
    SZ i;

    for(i = 0; i < label_size; i++) {
        hash ^= (unsigned) label[i];
        hash *= ORG_FNV1A_PRIME;
    }
    return hash;
}

/* Returns index of the footnote with the given label; or -1. */
static int
org_lookup_footnote(ORG_CTX* ctx, const CHAR* label, SZ label_size)
{
    unsigned i;

    if(ctx->n_footnote_buckets == 0)
        return -1;

    i = org_footnote_hash(label, label_size) & (ctx->n_footnote_buckets - 1);
    while(ctx->footnote_buckets[i] >= 0) {
        const ORG_FOOTNOTE* fn = &ctx->footnotes[ctx->footnote_buckets[i]];

        if(fn->label_size == label_size  &&  org_ascii_eq(fn->label, label, label_size))
            return ctx->footnote_buckets[i];
        i = (i + 1) & (ctx->n_footnote_buckets - 1);
    }

    return -1;
}

static int
org_footnote_hash_insert(ORG_CTX* ctx, int index)
{
    const ORG_FOOTNOTE* fn = &ctx->footnotes[index];
    unsigned i;

    /* Keep the load factor below 1/2. */
    if(2 * (ctx->n_labeled_footnotes + 1) > ctx->n_footnote_buckets) {
        int new_n_buckets = (ctx->n_footnote_buckets > 0) ? 2 * ctx->n_footnote_buckets : 64;
        int* new_buckets = (int*) malloc(new_n_buckets * sizeof(int));
        int j;

        if(new_buckets == NULL) {
            ORG_LOG("malloc() failed.");
            return -1;
        }
        for(j = 0; j < new_n_buckets; j++)
            new_buckets[j] = -1;
        for(j = 0; j < ctx->n_footnote_buckets; j++) {
            int k = ctx->footnote_buckets[j];

            if(k >= 0) {
                i = org_footnote_hash(ctx->footnotes[k].label, ctx->footnotes[k].label_size) & (new_n_buckets - 1);
                while(new_buckets[i] >= 0)
                    i = (i + 1) & (new_n_buckets - 1);
                new_buckets[i] = k;
            }
        }

        free(ctx->footnote_buckets);
        ctx->footnote_buckets = new_buckets;
        ctx->n_footnote_buckets = new_n_buckets;
    }

    i = org_footnote_hash(fn->label, fn->label_size) & (ctx->n_footnote_buckets - 1);
    while(ctx->footnote_buckets[i] >= 0)
        i = (i + 1) & (ctx->n_footnote_buckets - 1);
    ctx->footnote_buckets[i] = index;
    ctx->n_labeled_footnotes++;
    return 0;
}

/* Add a new footnote. (The caller guarantees there is no footnote of the same
 * label yet.) */
static int
org_add_footnote(ORG_CTX* ctx, const CHAR* label, SZ label_size, int* p_index)
{
    ORG_FOOTNOTE* fn;
    int ret = 0;

    ORG_GROW_ARRAY(ctx->footnotes, ctx->alloc_footnotes, ctx->n_footnotes);
    fn = &ctx->footnotes[ctx->n_footnotes];
    memset(fn, 0, sizeof(ORG_FOOTNOTE));
    fn->label = label;
    fn->label_size = label_size;
    fn->def_open_off = -1;
    fn->def_close_off = -1;
    *p_index = ctx->n_footnotes++;

    if(label_size > 0)
        ORG_CHECK(org_footnote_hash_insert(ctx, *p_index));

abort:
    return ret;
}

/* Get (or create) the footnote with the given label. */
static int
org_get_footnote(ORG_CTX* ctx, const CHAR* label, SZ label_size, int* p_index)
{
    *p_index = org_lookup_footnote(ctx, label, label_size);
    if(*p_index >= 0)
        return 0;
    return org_add_footnote(ctx, label, label_size, p_index);
}

/* Assign the footnote its number (if it has none yet). */
static int
org_reference_footnote(ORG_CTX* ctx, int index)
{
    int ret = 0;

    if(ctx->footnotes[index].id == 0) {
        ORG_GROW_ARRAY(ctx->footnote_order, ctx->alloc_footnote_order, ctx->n_footnote_order);
        ctx->footnote_order[ctx->n_footnote_order++] = index;
        ctx->footnotes[index].id = (unsigned) ctx->n_footnote_order;
    }
    ctx->footnotes[index].ref_count++;

abort:
    return ret;
}


/***************************
 ***  Recognizing Links  ***
 ***************************/

/* Known link types which may be used in the form "TYPE:PATH" in a bracket
 * link. Any other "foo:bar" is taken as a fuzzy link. */
static const CHAR* const org_link_types[] = {
    _T("attachment"), _T("doi"), _T("elisp"), _T("file"), _T("ftp"), _T("help"),
    _T("http"), _T("https"), _T("id"), _T("info"), _T("irc"), _T("mailto"),
    _T("news"), _T("shell")
};

/* Link types recognized in a plain link (e.g. https://example.com). */
static const CHAR* const org_plain_link_types[] = {
    _T("file"), _T("ftp"), _T("http"), _T("https"), _T("mailto")
};

/* Length of the longest type in org_plain_link_types[]. */
#define ORG_PLAIN_LINK_TYPE_MAX_LEN     6

static int
org_is_link_type(ORG_CTX* ctx, OFF beg, OFF end, const CHAR* const* types, int n_types)
{
    int i;

    for(i = 0; i < n_types; i++) {
        SZ n = (SZ) org_strlen(types[i]);
        if(end - beg == n  &&  org_ascii_eq(STR(beg), types[i], n))
            return true;
    }

    return false;
}

/* Check whether there is a bracket link "[[TARGET]]" or "[[TARGET][DESC]]"
 * at 'beg' (and before 'max_end'). */
static int
org_is_bracket_link(ORG_CTX* ctx, OFF beg, OFF max_end, ORG_MARK* mark)
{
    OFF off = beg + 2;

    /* The target: No unescaped brackets allowed. */
    while(off < max_end) {
        if(CH(off) == _T('\\')  &&  off+1 < max_end) {
            off += 2;
            continue;
        }
        if(ISANYOF2(off, _T('['), _T(']')))
            break;
        off++;
    }
    if(off + 1 >= max_end  ||  CH(off) != _T(']')  ||  off == beg + 2)
        return false;

    mark->sub_beg = beg + 2;
    mark->sub_end = off;
    mark->desc_beg = 0;
    mark->desc_end = 0;

    if(CH(off+1) == _T(']')) {
        mark->end = off + 2;
        return true;
    }

    if(CH(off+1) == _T('[')) {
        OFF desc_beg = off + 2;

        /* The description: Anything up to the first "]]". */
        if(ctx->link_close_horizon > 0  &&  desc_beg >= ctx->link_close_horizon)
            return false;
        off = desc_beg;
        while(off + 1 < max_end) {
            if(CH(off) == _T(']')  &&  CH(off+1) == _T(']'))
                break;
            off++;
        }
        if(off + 1 >= max_end) {
            /* Remember there is no "]]" so we do not rescan it again and
             * again for any subsequent potential links. */
            ctx->link_close_horizon = desc_beg;
            return false;
        }
        if(off == desc_beg)
            return false;

        mark->desc_beg = desc_beg;
        mark->desc_end = off;
        mark->end = off + 2;
        return true;
    }

    return false;
}

/* Find the end of a parenthesized part of a plain link path at 'off', i.e.
 * of any of "([<", followed by any non-bracket characters and (at the top
 * level only) other parenthesized parts, and any of ")]>". Return zero if
 * there is none. */
static OFF
org_plain_link_group_end(ORG_CTX* ctx, OFF off, OFF max_end, int is_nested)
{
    off++;
    while(off < max_end  &&  !ISUNICODEWHITESPACE(off)) {
        if(ISANYOF3(off, _T(')'), _T(']'), _T('>')))
            return off + 1;

        if(ISANYOF3(off, _T('('), _T('['), _T('<'))) {
            if(is_nested)
                return 0;
            off = org_plain_link_group_end(ctx, off, max_end, true);
            if(off == 0)
                return 0;
        } else {
            off++;
        }
    }

    return 0;
}

/* Check whether there is a plain link (e.g. "https://example.com") at 'beg'.
 *
 * As in Org (see org-link-plain-re), the path consists of the non-bracket
 * characters and the parenthesized parts (nested at most two levels deep),
 * and it ends with a parenthesized part, '-', '/' or a character other than
 * a punctuation. */
static int
org_is_plain_link(ORG_CTX* ctx, OFF beg, OFF max_end, OFF* p_end)
{
    OFF off = beg;
    OFF end = 0;
    int is_first = true;

    while(off < max_end  &&  ISALPHA(off))
        off++;
    if(off >= max_end  ||  CH(off) != _T(':'))
        return false;
    if(!org_is_link_type(ctx, beg, off, org_plain_link_types, (int) SIZEOF_ARRAY(org_plain_link_types)))
        return false;

    off++;
    while(off < max_end  &&  !ISUNICODEWHITESPACE(off)  &&  !ISANYOF3(off, _T(')'), _T(']'), _T('>'))) {
        if(ISANYOF3(off, _T('('), _T('['), _T('<'))) {
            off = org_plain_link_group_end(ctx, off, max_end, false);
            if(off == 0)
                break;
            if(!is_first)
                end = off;
        } else {
            if(!is_first  &&  (!ISPUNCT(off)  ||  ISANYOF2(off, _T('-'), _T('/'))))
                end = off + 1;
            off++;
        }
        is_first = false;
    }

    /* (The path has at least two characters or parenthesized parts.) */
    if(end == 0)
        return false;

    *p_end = end;
    return true;
}


/***********************************
 ***  Recognizing Other Objects  ***
 ***********************************/

/* Prepare (but do not build yet) the table of matching brackets for the
 * region [beg, end). As the table is not needed in most blocks, it is built
 * only on the first use; see org_bracket_match(). */
static int
org_reset_bracket_matches(ORG_CTX* ctx, OFF beg, OFF end)
{
    int size = (int) (end - beg);

    if(size + 1 > ctx->alloc_bracket_matches) {
        OFF* new_matches;
        int new_alloc;

        new_alloc = (ctx->alloc_bracket_matches > 0
                ? ctx->alloc_bracket_matches + ctx->alloc_bracket_matches / 2
                : 64);
        if(new_alloc < size + 1)
            new_alloc = size + 1;

        new_matches = (OFF*) realloc(ctx->bracket_matches, new_alloc * sizeof(OFF));
        if(new_matches == NULL) {
            ORG_LOG("realloc() failed.");
            return -1;
        }
        ctx->bracket_matches = new_matches;
        ctx->alloc_bracket_matches = new_alloc;
    }

    ctx->bracket_region_beg = beg;
    ctx->bracket_region_end = end;
    ctx->bracket_matches_valid = false;
    return 0;
}

/* Build the table of matching brackets ("[]", "{}", "()") for the region set
 * by org_reset_bracket_matches(). Org counts only brackets of the same kind
 * (e.g. "{a[}" is a balanced brace pair).
 *
 * All the three kinds are matched in a single pass. The stack of unmatched
 * openers of each kind is kept as a linked list in the table itself: The
 * entry of an unmatched opener holds the offset of the previous one (or 'end'
 * as a terminator). Note only the entries of the openers are ever set. */
static void
org_build_bracket_matches(ORG_CTX* ctx)
{
    OFF beg = ctx->bracket_region_beg;
    OFF end = ctx->bracket_region_end;
    OFF top[3] = { end, end, end };
    int kind;
    OFF off;

    for(off = beg; off < end; off++) {
        switch(CH(off)) {
            case _T('['):   kind = 0; break;
            case _T('{'):   kind = 1; break;
            case _T('('):   kind = 2; break;
            case _T(']'):   kind = -1; break;
            case _T('}'):   kind = -2; break;
            case _T(')'):   kind = -3; break;
            default:        continue;
        }

        if(kind >= 0) {
            /* Push the opener. */
            ctx->bracket_matches[off - beg] = top[kind];
            top[kind] = off;
        } else {
            /* Pop the opener (if any) and match it with the closer. */
            kind = -kind - 1;
            if(top[kind] != end) {
                OFF opener = top[kind];
                top[kind] = ctx->bracket_matches[opener - beg];
                ctx->bracket_matches[opener - beg] = off;
            }
        }
    }

    /* The remaining openers are unmatched. */
    for(kind = 0; kind < 3; kind++) {
        while(top[kind] != end) {
            OFF opener = top[kind];
            top[kind] = ctx->bracket_matches[opener - beg];
            ctx->bracket_matches[opener - beg] = 0;
        }
    }

    ctx->bracket_matches_valid = true;
}

/* Returns offset of the bracket matching the opening one at 'off' (see
 * org_build_bracket_matches()); or zero if there is none. */
static OFF
org_bracket_match(ORG_CTX* ctx, OFF off)
{
    if(!ctx->bracket_matches_valid)
        org_build_bracket_matches(ctx);
    return ctx->bracket_matches[off - ctx->bracket_region_beg];
}

/* Find the string 'str' in [off, end). Returns its offset, or zero if not
 * found. The horizon remembers there is no such string (at or after it), so
 * the subsequent searches (e.g. from other unterminated openers) are cheap. */
static OFF
org_find_string(ORG_CTX* ctx, OFF off, OFF end, const CHAR* str, OFF* p_horizon)
{
    SZ n = (SZ) org_strlen(str);
    OFF beg = off;

    if(*p_horizon != 0  &&  off >= *p_horizon)
        return 0;

    for(; off + n <= end; off++) {
        if(CH(off) == str[0]  &&  org_ascii_eq(STR(off), str, n))
            return off;
    }

    if(*p_horizon == 0  ||  beg < *p_horizon)
        *p_horizon = beg;
    return 0;
}

/* Check whether the name (e.g. "alpha" of "\alpha") is a known entity. */
static int
org_is_entity_name(const CHAR* name, SZ size)
{
#if defined ORG4C_USE_UTF16
    /* The entity table has 8-bit names. As the name consists only of ASCII
     * letters and digits, we can simply convert it. */
    char buf[ENTITY_NAME_MAX];
    SZ i;

    if(size > (SZ) sizeof(buf))
        return false;
    for(i = 0; i < size; i++)
        buf[i] = (char) name[i];
    return (org_entity_lookup(buf, size) != NULL);
#else
    return (org_entity_lookup(name, size) != NULL);
#endif
}

/* Entity (e.g. "\alpha" or "\alpha{}") or a LaTeX fragment ("\(...\)",
 * "\[...\]" or "\command[...]{...}") at 'off'. */
static int
org_is_backslash_object(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF p = off + 1;
    int with_latex = !(ctx->parser.flags & ORG_FLAG_NOLATEX);

    if(p >= end)
        return false;

    if(with_latex  &&  ISANYOF2(p, _T('('), _T('['))) {
        OFF found;

        if(CH(p) == _T('('))
            found = org_find_string(ctx, p + 1, end, _T("\\)"), &ctx->latex_paren_horizon);
        else
            found = org_find_string(ctx, p + 1, end, _T("\\]"), &ctx->latex_bracket_horizon);
        if(found == 0)
            return false;

        mark->ch = ORG_MARK_LATEX_OBJECT;
        mark->end = found + 2;
        return true;
    }

    /* Entity "\_ " (followed by up to 20 spaces). */
    if(CH(p) == _T('_')) {
        OFF q = p + 1;

        while(q < end  &&  CH(q) == _T(' ')  &&  q - p <= 20)
            q++;
        if(q == p + 1  ||  q - p > 20)
            return false;

        mark->ch = ORG_MARK_ENTITY_OBJECT;
        mark->end = q;
        return true;
    }

    if(ISALPHA(p)) {
        static const CHAR* const special_names[] = {
            _T("there4"), _T("sup1"), _T("sup2"), _T("sup3"),
            _T("frac12"), _T("frac14"), _T("frac34"), _T("frac13")
        };
        OFF q = p;
        int i;

        while(q < end  &&  ISALPHA(q))
            q++;

        /* Few entity names contain also digits. */
        for(i = 0; i < (int) SIZEOF_ARRAY(special_names); i++) {
            if(org_is_prefix(ctx, p, end, special_names[i])) {
                q = p + (OFF) org_strlen(special_names[i]);
                break;
            }
        }

        if(org_is_entity_name(STR(p), q - p)) {
            if(q + 1 < end  &&  CH(q) == _T('{')  &&  CH(q+1) == _T('}'))
                q += 2;
            mark->ch = ORG_MARK_ENTITY_OBJECT;
            mark->end = q;
            return true;
        }
    }

    /* LaTeX command, e.g. "\cmd", "\cmd*[opt]{arg}". */
    if(with_latex  &&  ISALPHA(p)) {
        OFF q = p;

        while(q < end  &&  ISALPHA(q))
            q++;
        if(q < end  &&  CH(q) == _T('*'))
            q++;

        while(q < end  &&  ISANYOF2(q, _T('['), _T('{'))) {
            CHAR closer = (CH(q) == _T('[')) ? _T(']') : _T('}');
            OFF r = q + 1;

            while(r < end  &&  CH(r) != closer  &&  !ISANYOF(r, _T("[]{}\r\n")))
                r++;
            if(r >= end  ||  CH(r) != closer)
                break;
            q = r + 1;
        }

        mark->ch = ORG_MARK_LATEX_OBJECT;
        mark->end = q;
        return true;
    }

    return false;
}

/* LaTeX fragment "$...$" or "$$...$$" at 'off'. */
static int
org_is_dollar_latex(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q;

    if(ctx->parser.flags & ORG_FLAG_NOLATEX)
        return false;

    if(off + 1 < end  &&  CH(off+1) == _T('$')) {
        q = org_find_string(ctx, off + 2, end, _T("$$"), &ctx->latex_dollars_horizon);
        if(q == 0  ||  q == off + 2)
            return false;
        mark->end = q + 2;
    } else {
        /* The contents must not start (and end) with a whitespace or some
         * punctuation, and the fragment cannot be followed by an alphanumeric
         * character. (Org takes the nearest '$' as the closer.) */
        if(off > 0  &&  CH(off-1) == _T('$'))
            return false;
        if(off + 1 >= end  ||  ISANYOF(off+1, _T(" \t\r\n,.;$")))
            return false;

        q = off + 1;
        while(q < end  &&  CH(q) != _T('$'))
            q++;
        if(q >= end  ||  ISANYOF(q-1, _T(" \t\r\n,.")))
            return false;
        if(q + 1 < end  &&  (ISALNUM(q+1)  ||  CH(q+1) == _T('$')))
            return false;
        mark->end = q + 1;
    }

    mark->ch = ORG_MARK_LATEX_OBJECT;
    return true;
}

/* Timestamp at 'off' ('<' for active, '[' for inactive ones), including a
 * range "<...>--<...>". */
static int
org_is_timestamp_at(ORG_CTX* ctx, OFF off, OFF end, OFF* p_end)
{
    CHAR opener = CH(off);
    CHAR closer = (opener == _T('<')) ? _T('>') : _T(']');
    OFF q;

    /* YYYY-MM-DD */
    if(off + 12 > end)
        return false;
    for(q = off + 1; q < off + 11; q++) {
        if(q == off + 5  ||  q == off + 8) {
            if(CH(q) != _T('-'))
                return false;
        } else if(!ISDIGIT(q)) {
            return false;
        }
    }
    if(CH(q) != _T(' ')  &&  CH(q) != closer)
        return false;

    /* The rest (day name, time, repeater, ...) up to the closer. */
    while(q < end  &&  CH(q) != closer  &&  CH(q) != opener  &&  !ISNEWLINE(q))
        q++;
    if(q >= end  ||  CH(q) != closer)
        return false;

    *p_end = q + 1;
    return true;
}

static int
org_is_timestamp(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF ts_end;
    OFF range_end;

    if(!org_is_timestamp_at(ctx, off, end, &ts_end))
        return false;

    mark->ch = ORG_MARK_TIMESTAMP_OBJECT;
    mark->end = ts_end;
    if(CH(off) == _T('<'))
        mark->flags |= ORG_MARK_ACTIVE_TIMESTAMP;

    if(ts_end + 2 < end  &&  CH(ts_end) == _T('-')  &&  CH(ts_end+1) == _T('-')  &&
       CH(ts_end+2) == CH(off)  &&  org_is_timestamp_at(ctx, ts_end + 2, end, &range_end))
    {
        mark->end = range_end;
        mark->flags |= ORG_MARK_TIMESTAMP_RANGE;
    }

    return true;
}

/* Objects starting with '<': target "<<target>>", timestamp, angle link. */
static int
org_is_angle_object(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q;

    if(off + 1 >= end)
        return false;

    if(CH(off+1) == _T('<')) {
        /* Radio targets "<<<target>>>" are recognized in org_is_object(). */
        if(off + 2 >= end  ||  CH(off+2) == _T('<')  ||  ISWHITESPACE(off+2))
            return false;

        q = off + 2;
        while(q < end  &&  !ISANYOF(q, _T("<>\r\n")))
            q++;
        if(q + 1 >= end  ||  CH(q) != _T('>')  ||  CH(q+1) != _T('>')  ||  ISWHITESPACE(q-1))
            return false;

        mark->ch = ORG_MARK_TARGET_OBJECT;
        mark->sub_beg = off + 2;
        mark->sub_end = q;
        mark->end = q + 2;
        return true;
    }

    if(ISDIGIT(off+1))
        return org_is_timestamp(ctx, off, end, mark);

    /* Angle link "<https://example.com>". */
    q = off + 1;
    while(q < end  &&  ISALPHA(q))
        q++;
    if(q >= end  ||  CH(q) != _T(':')  ||
       !org_is_link_type(ctx, off + 1, q, org_link_types, (int) SIZEOF_ARRAY(org_link_types)))
        return false;
    while(q < end  &&  !ISANYOF(q, _T("<>\r\n")))
        q++;
    if(q >= end  ||  CH(q) != _T('>'))
        return false;

    mark->ch = ORG_MARK_PLAIN_LINK_OBJECT;
    mark->sub_beg = off + 1;
    mark->sub_end = q;
    mark->end = q + 1;
    return true;
}

static int
org_is_footnote_label_char(ORG_CTX* ctx, OFF off)
{
    return (ISALNUM(off)  ||  ISANYOF2(off, _T('-'), _T('_'))  ||  !ISASCII(off));
}

/* Objects starting with '[' (except bracket links): footnote reference,
 * inactive timestamp, statistics cookie. */
static int
org_is_bracket_object(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q;

    if(off + 2 >= end)
        return false;

    /* Footnote reference "[fn:label]", "[fn:label:def]" or "[fn::def]".
     * (Not in a detached text: The footnote labels and definitions have to
     * live until the end of the document.) */
    if(org_is_prefix(ctx, off, end, _T("[fn:"))  &&  !ctx->is_detached) {
        q = off + 4;
        while(q < end  &&  org_is_footnote_label_char(ctx, q))
            q++;
        if(q >= end)
            return false;

        if(CH(q) == _T(']')  &&  q > off + 4) {
            mark->ch = ORG_MARK_FOOTNOTE_REF_OBJECT;
            mark->sub_beg = off + 4;
            mark->sub_end = q;
            mark->end = q + 1;
            return true;
        }

        if(CH(q) == _T(':')) {
            OFF match;

            if(ctx->footnote_nesting_level >= FOOTNOTE_MAX_NESTING)
                return false;
            match = org_bracket_match(ctx, off);
            if(match == 0)
                return false;
            mark->ch = ORG_MARK_FOOTNOTE_REF_OBJECT;
            mark->sub_beg = off + 4;
            mark->sub_end = q;
            mark->desc_beg = q + 1;
            mark->desc_end = match;
            mark->flags |= ORG_MARK_INLINE_DEFINITION;
            mark->end = match + 1;
            return true;
        }

        return false;
    }

    if(ISDIGIT(off+1)  &&  org_is_timestamp(ctx, off, end, mark))
        return true;

    /* Statistics cookie "[N/M]" or "[N%]". */
    q = off + 1;
    while(q < end  &&  ISDIGIT(q))
        q++;
    if(q + 1 < end  &&  CH(q) == _T('%')  &&  CH(q+1) == _T(']')) {
        q += 2;
    } else if(q < end  &&  CH(q) == _T('/')) {
        q++;
        while(q < end  &&  ISDIGIT(q))
            q++;
        if(q >= end  ||  CH(q) != _T(']'))
            return false;
        q++;
    } else {
        return false;
    }

    mark->ch = ORG_MARK_STATISTICS_COOKIE_OBJECT;
    mark->end = q;
    return true;
}

/* Export snippet "@@backend:value@@". */
static int
org_is_export_snippet(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q = off + 2;
    OFF found;

    if(off + 1 >= end  ||  CH(off+1) != _T('@'))
        return false;

    while(q < end  &&  (ISALNUM(q)  ||  CH(q) == _T('-')))
        q++;
    if(q == off + 2  ||  q >= end  ||  CH(q) != _T(':'))
        return false;

    found = org_find_string(ctx, q + 1, end, _T("@@"), &ctx->snippet_horizon);
    if(found == 0)
        return false;

    mark->ch = ORG_MARK_EXPORT_SNIPPET_OBJECT;
    mark->sub_beg = off + 2;
    mark->sub_end = q;
    mark->desc_beg = q + 1;
    mark->desc_end = found;
    mark->end = found + 2;
    return true;
}

/* Inline source block "src_lang{body}" or "src_lang[params]{body}". */
static int
org_is_inline_src(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q = off + 4;
    OFF lang_end;
    OFF match;

    if(!org_is_prefix(ctx, off, end, _T("src_"))  ||  (off > 0  &&  ISALNUM(off-1)))
        return false;

    while(q < end  &&  !ISWHITESPACE(q)  &&  !ISNEWLINE(q)  &&  !ISANYOF2(q, _T('['), _T('{')))
        q++;
    if(q == off + 4  ||  q >= end)
        return false;
    lang_end = q;

    mark->aux_beg = mark->aux_end = 0;
    if(CH(q) == _T('[')) {
        match = org_bracket_match(ctx, q);
        if(match == 0)
            return false;
        mark->aux_beg = q + 1;
        mark->aux_end = match;
        q = match + 1;
    }

    if(q >= end  ||  CH(q) != _T('{'))
        return false;
    match = org_bracket_match(ctx, q);
    if(match == 0)
        return false;

    mark->ch = ORG_MARK_INLINE_SRC_OBJECT;
    mark->sub_beg = off + 4;
    mark->sub_end = lang_end;
    mark->desc_beg = q + 1;
    mark->desc_end = match;
    mark->end = match + 1;

    /* The code cannot span over multiple lines. */
    for(q = mark->beg; q < mark->end; q++) {
        if(ISNEWLINE(q))
            return false;
    }
    return true;
}

/* Subscript "a_b" or superscript "a^b" (at the '_' or '^'). */
static int
org_is_subsup(ORG_CTX* ctx, OFF off, OFF region_beg, OFF end, ORG_MARK* mark)
{
    OFF q = off + 1;

    if(off <= region_beg  ||  ISUNICODEWHITESPACEBEFORE(off)  ||  q >= end)
        return false;

    if(CH(q) == _T('{')) {
        OFF match = org_bracket_match(ctx, q);

        if(match == 0)
            return false;
        mark->desc_beg = q + 1;
        mark->desc_end = match;
        mark->end = match + 1;
    } else if(ctx->parser.flags & ORG_FLAG_SUBSUPERSCRIPTS_BRACED) {
        return false;
    } else if(CH(q) == _T('*')) {
        mark->desc_beg = q;
        mark->desc_end = q + 1;
        mark->end = q + 1;
    } else {
        OFF r;

        if(ISANYOF2(q, _T('+'), _T('-')))
            q++;
        r = q;
        while(r < end  &&  (ISALNUM(r)  ||  !ISASCII(r)  ||  ISANYOF(r, _T(",.\\"))))
            r++;
        /* It has to end with an alphanumeric character. */
        while(r > q  &&  !(ISALNUM(r-1)  ||  !ISASCII(r-1)))
            r--;
        if(r == q)
            return false;
        mark->desc_beg = off + 1;
        mark->desc_end = r;
        mark->end = r;
    }

    mark->ch = (CH(off) == _T('_')) ? ORG_MARK_SUBSCRIPT_OBJECT : ORG_MARK_SUPERSCRIPT_OBJECT;
    return true;
}

/* Macro "{{{name}}}" or "{{{name(arguments)}}}". */
static int
org_is_macro(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q = off + 3;
    OFF found;

    if(!org_is_prefix(ctx, off, end, _T("{{{"))  ||  q >= end  ||  !ISALPHA(q))
        return false;
    while(q < end  &&  (ISALNUM(q)  ||  ISANYOF2(q, _T('-'), _T('_'))))
        q++;
    if(q >= end)
        return false;

    mark->sub_beg = off + 3;
    mark->sub_end = q;

    if(org_is_prefix(ctx, q, end, _T("}}}"))) {
        mark->desc_beg = mark->desc_end = q;
        mark->end = q + 3;
    } else if(CH(q) == _T('(')) {
        /* The arguments: Anything up to the first ")}}}". */
        found = org_find_string(ctx, q + 1, end, _T(")}}}"), &ctx->macro_horizon);
        if(found == 0)
            return false;
        mark->desc_beg = q + 1;
        mark->desc_end = found;
        mark->end = found + 4;
    } else {
        return false;
    }

    mark->ch = ORG_MARK_MACRO_OBJECT;
    return true;
}

/* Citation "[cite:@key]" or "[cite/style:prefix @key suffix; ...]". */
static int
org_is_citation(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q = off + 5;
    OFF match;

    if(!org_is_prefix(ctx, off, end, _T("[cite")))
        return false;

    mark->sub_beg = mark->sub_end = 0;
    if(q < end  &&  CH(q) == _T('/')) {
        mark->sub_beg = q + 1;
        while(q < end  &&  (ISALNUM(q)  ||  ISANYOF(q, _T("/_-"))))
            q++;
        mark->sub_end = q;
    }
    if(q >= end  ||  CH(q) != _T(':'))
        return false;

    match = org_bracket_match(ctx, off);
    if(match == 0)
        return false;

    /* There has to be at least one key. */
    for(q = q + 1; q < match; q++) {
        if(CH(q) == _T('@')  &&  q + 1 < match  &&  !ISWHITESPACE(q+1)  &&  !ISANYOF(q+1, _T(";]")))
            break;
    }
    if(q >= match)
        return false;

    mark->ch = ORG_MARK_CITATION_OBJECT;
    mark->desc_beg = (mark->sub_end > 0 ? mark->sub_end : off + 5) + 1;
    mark->desc_end = match;
    mark->end = match + 1;
    return true;
}

/* Inline Babel call "call_name(arguments)", optionally with header arguments
 * "call_name[inside](arguments)[end]". */
static int
org_is_inline_babel_call(ORG_CTX* ctx, OFF off, OFF end, ORG_MARK* mark)
{
    OFF q = off + 5;
    OFF match;

    if(!org_is_prefix(ctx, off, end, _T("call_"))  ||  (off > 0  &&  ISALNUM(off-1)))
        return false;

    while(q < end  &&  !ISWHITESPACE(q)  &&  !ISNEWLINE(q)  &&  !ISANYOF(q, _T("[]()")))
        q++;
    if(q == off + 5  ||  q >= end)
        return false;
    mark->sub_beg = off + 5;
    mark->sub_end = q;

    if(CH(q) == _T('[')) {
        match = org_bracket_match(ctx, q);
        if(match == 0)
            return false;
        q = match + 1;
    }

    if(q >= end  ||  CH(q) != _T('('))
        return false;
    match = org_bracket_match(ctx, q);
    if(match == 0)
        return false;
    mark->desc_beg = q + 1;
    mark->desc_end = match;
    q = match + 1;

    if(q < end  &&  CH(q) == _T('[')) {
        match = org_bracket_match(ctx, q);
        if(match != 0)
            q = match + 1;
    }

    mark->ch = ORG_MARK_INLINE_BABEL_CALL_OBJECT;
    mark->end = q;
    return true;
}

/* Compare the text at 'off' with the radio target (case-insensitively, any
 * whitespace sequence in the target matches any whitespace sequence). Returns
 * the end of the match, or zero. */
static OFF
org_match_radio_target(ORG_CTX* ctx, OFF off, OFF end, const ORG_RADIO_TARGET* rt)
{
    SZ i = 0;

    while(i < rt->size) {
        CHAR ch = rt->text[i];

        if(off >= end)
            return 0;

        if(ISWHITESPACE_(ch)) {
            if(!ISWHITESPACE(off)  &&  !ISNEWLINE(off))
                return 0;
            while(i < rt->size  &&  ISWHITESPACE_(rt->text[i]))
                i++;
            while(off < end  &&  (ISWHITESPACE(off)  ||  ISNEWLINE(off)))
                off++;
            continue;
        }

        if(ISLOWER_(ch))
            ch += 'A' - 'a';
        if(ch != (ISLOWER(off) ? CH(off) + 'A' - 'a' : CH(off)))
            return 0;
        i++;
        off++;
    }

    return off;
}

/* Radio link, i.e. an occurrence of a text of some radio target (as a whole
 * word). */
static int
org_is_radio_link(ORG_CTX* ctx, OFF off, OFF region_beg, OFF end, ORG_MARK* mark)
{
    unsigned char key;
    int i;

    if(off > region_beg  &&  ISALNUM(off-1))
        return false;
    /* (In the UTF-16 build, the radio targets are indexed by the lower byte of
     * their first character; org_match_radio_target() checks the rest.) */
    if(!ctx->radio_first_char_map[(unsigned char) CH(off)])
        return false;

    /* (The radio targets of the same first character are sorted by their
     * length, so the longest match wins.) */
    key = (unsigned char) CH(off);
    if(ISLOWER_(key))
        key = (unsigned char) (key - 'a' + 'A');
    for(i = ctx->radio_first_index[key];
        i < ctx->n_radio_targets  &&  org_radio_target_key(&ctx->radio_targets[i]) == key;
        i++)
    {
        OFF match_end = org_match_radio_target(ctx, off, end, &ctx->radio_targets[i]);

        if(match_end != 0  &&  (match_end >= end  ||  !ISALNUM(match_end))) {
            mark->ch = ORG_MARK_RADIO_LINK_OBJECT;
            mark->sub_beg = off;
            mark->sub_end = match_end;
            mark->index = i;
            mark->end = match_end;
            return true;
        }
    }

    return false;
}


/******************************************
 ***  Processing Inlines (a.k.a Spans)  ***
 ******************************************/

/* Org resolves the inline objects in a simple way: It scans the text from the
 * left to the right; and whenever it sees a potential opener of an emphasis,
 * it finds the nearest potential closer of the same kind. If found, that forms
 * the span and its contents is (recursively) processed the same way, limited
 * only to the contents. If no closer is found, the character is just an
 * ordinary text.
 *
 * Note this is (unlike CommonMark) not affected by any other opener or
 * closer in between (e.g. "*a *b* c*" is "<b>a *b</b> c*").
 *
 * The contents of a span (or of a link description) is parsed as if it were
 * a stand-alone text: Its beginning and end act as a beginning and end of a
 * line. That allows e.g. "**a**" to be "<b><b>a</b></b>". Otherwise, the
 * contents of a span cannot contain another span of the same kind; so the
 * nesting is bounded.
 *
 * So, similarly to MD4C, we first collect all the potential marks within the
 * block into ctx->marks[], and link each of them to the nearest potential
 * closer of the same kind (ORG_MARK::next); see org_analyze_inlines(). Then
 * we walk the marks, resolve them and emit the text and the spans; see
 * org_process_inline_range().
 */

static const CHAR org_emph_chars[] = _T("*/_+=~");

static int
org_emph_index(CHAR ch)
{
    switch(ch) {
        case _T('*'):   return 0;
        case _T('/'):   return 1;
        case _T('_'):   return 2;
        case _T('+'):   return 3;
        case _T('='):   return 4;
        case _T('~'):   return 5;
        default:        return -1;
    }
}

static ORG_MARK*
org_push_mark(ORG_CTX* ctx)
{
    ORG_MARK* mark;

    if(ctx->n_marks >= ctx->alloc_marks) {
        ORG_MARK* new_marks;
        int new_alloc = (ctx->alloc_marks > 0 ? ctx->alloc_marks + ctx->alloc_marks / 2 : 64);

        new_marks = (ORG_MARK*) realloc(ctx->marks, new_alloc * sizeof(ORG_MARK));
        if(new_marks == NULL) {
            ORG_LOG("realloc() failed.");
            return NULL;
        }
        ctx->marks = new_marks;
        ctx->alloc_marks = new_alloc;
    }

    mark = &ctx->marks[ctx->n_marks++];
    memset(mark, 0, sizeof(ORG_MARK));
    mark->next = -1;
    return mark;
}

static void
org_build_mark_char_map(ORG_CTX* ctx)
{
    const CHAR* ch;

    memset(ctx->mark_char_map, 0, sizeof(ctx->mark_char_map));

    for(ch = org_emph_chars; *ch != _T('\0'); ch++)
        ctx->mark_char_map[(unsigned char) *ch] = 1;
    ctx->mark_char_map['['] = 1;
    ctx->mark_char_map['<'] = 1;
    ctx->mark_char_map['\\'] = 1;
    ctx->mark_char_map['@'] = 1;
    ctx->mark_char_map['{'] = 1;

    if(!(ctx->parser.flags & ORG_FLAG_NOLATEX))
        ctx->mark_char_map['$'] = 1;

    if(ctx->parser.flags & (ORG_FLAG_SUBSUPERSCRIPTS | ORG_FLAG_SUBSUPERSCRIPTS_BRACED))
        ctx->mark_char_map['^'] = 1;

    /* The colon after the type of a plain link. (We look back for the type
     * only when we see the colon: Any letter is too frequent a mark char.) */
    if(!(ctx->parser.flags & ORG_FLAG_NOPLAINLINKS))
        ctx->mark_char_map[':'] = 1;
}

/* Org's PRE and POST character sets for the emphasis (besides whitespace). */
#define ORG_EMPH_PRE_CHARS      _T("-({'\"")
#define ORG_EMPH_POST_CHARS     _T("-.,;:!?')}[\"\\")

/* Objects whose contents is not parsed for other objects. */
static int
org_is_raw_object(CHAR ch)
{
    switch(ch) {
        case ORG_MARK_BRACKET_LINK_OBJECT:
        case ORG_MARK_FOOTNOTE_REF_OBJECT:
        case ORG_MARK_SUBSCRIPT_OBJECT:
        case ORG_MARK_SUPERSCRIPT_OBJECT:
            return false;
        default:
            return true;
    }
}

/* Check whether there is an inline object at 'off'. Some objects are detected
 * only at a character after their beginning (e.g. a plain link at the colon
 * after its type); then 'mark->beg' is moved back to their beginning (which
 * is never before 'line_beg' nor 'raw_skip_until'). */
static int
org_is_object(ORG_CTX* ctx, OFF off, OFF region_beg, OFF region_end, OFF line_beg,
              OFF line_end, OFF link_skip_until, OFF raw_skip_until, ORG_MARK* mark)
{
    switch(CH(off)) {
        case _T('\\'):
            return org_is_backslash_object(ctx, off, region_end, mark);

        case _T('$'):
            return org_is_dollar_latex(ctx, off, region_end, mark);

        case _T('<'):
            if(org_is_prefix(ctx, off, region_end, _T("<<<"))) {
                /* Radio target. */
                if(!org_is_radio_target(ctx, off, region_end, &mark->sub_beg, &mark->sub_end))
                    return false;
                mark->ch = ORG_MARK_RADIO_TARGET_OBJECT;
                mark->end = mark->sub_end + 3;
                return true;
            }
            return org_is_angle_object(ctx, off, region_end, mark);

        case _T('{'):
            return org_is_macro(ctx, off, region_end, mark);

        case _T('@'):
            return org_is_export_snippet(ctx, off, region_end, mark);

        case _T('['):
            if(off + 1 < region_end  &&  CH(off+1) == _T('[')) {
                if(off < link_skip_until  ||  !org_is_bracket_link(ctx, off, region_end, mark))
                    return false;
                mark->ch = ORG_MARK_BRACKET_LINK_OBJECT;
                return true;
            }
            if(org_is_citation(ctx, off, region_end, mark))
                return true;
            return org_is_bracket_object(ctx, off, region_end, mark);

        case _T('_'):
            /* Inline source block "src_" or Babel call "call_". They take
             * precedence over anything at the underscore. */
            if(off >= line_beg + 3  &&  off - 3 >= raw_skip_until  &&
               org_is_inline_src(ctx, off - 3, region_end, mark))
            {
                mark->beg = off - 3;
                return true;
            }
            if(off >= line_beg + 4  &&  off - 4 >= raw_skip_until  &&
               org_is_inline_babel_call(ctx, off - 4, region_end, mark))
            {
                mark->beg = off - 4;
                return true;
            }
            ORG_FALLTHROUGH();

        case _T('^'):
            if(!(ctx->parser.flags & (ORG_FLAG_SUBSUPERSCRIPTS | ORG_FLAG_SUBSUPERSCRIPTS_BRACED)))
                return false;
            return org_is_subsup(ctx, off, region_beg, region_end, mark);

        case _T(':'):
            /* Plain link: Look back for its type, i.e. for a whole word
             * (not beginning before 'link_skip_until' nor 'raw_skip_until'). */
            if(!(ctx->parser.flags & ORG_FLAG_NOPLAINLINKS)) {
                OFF beg = off;
                OFF beg_min = line_beg;
                OFF end;

                if(beg_min < link_skip_until)
                    beg_min = link_skip_until;
                if(beg_min < raw_skip_until)
                    beg_min = raw_skip_until;
                while(beg > beg_min  &&  off - beg < ORG_PLAIN_LINK_TYPE_MAX_LEN  &&  ISALPHA(beg-1))
                    beg--;

                if(beg < off  &&  (beg == 0  ||  !ISALNUM(beg-1))  &&
                   org_is_plain_link(ctx, beg, line_end, &end))
                {
                    mark->ch = ORG_MARK_PLAIN_LINK_OBJECT;
                    mark->beg = beg;
                    mark->end = end;
                    mark->sub_beg = beg;
                    mark->sub_end = end;
                    return true;
                }
            }
            break;

        /* The letters are mark chars only if they start some radio target
         * and then these have to be tried before the radio link. (Otherwise,
         * they are detected at the underscore; see above.) */
        case _T('s'):
            if(org_is_inline_src(ctx, off, region_end, mark))
                return true;
            break;

        case _T('c'):
            if(org_is_inline_babel_call(ctx, off, region_end, mark))
                return true;
            break;

        default:
            break;
    }

    /* Radio link. */
    if(ctx->n_radio_targets > 0  &&  off >= link_skip_until  &&
       org_is_radio_link(ctx, off, region_beg, region_end, mark))
        return true;

    return false;
}

static int
org_collect_marks(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines)
{
    OFF region_beg = lines[0].beg;
    OFF region_end = lines[n_lines-1].end;
    OFF link_skip_until = 0;
    OFF raw_skip_until = 0;
    SZ line_index;
    int ret = 0;

    /* The objects with brackets (links, citations, ...) need them. */
    ORG_CHECK(org_reset_bracket_matches(ctx, region_beg, region_end));

    for(line_index = 0; line_index < n_lines; line_index++) {
        const ORG_LINE* line = &lines[line_index];
        OFF off;

        for(off = line->beg; off < line->end; off++) {
            CHAR ch;
            ORG_MARK tmp;
            ORG_MARK* mark;

#if defined ORG4C_USE_UTF16
    /* For UTF-16, mark_char_map[] covers only ASCII. (A non-ASCII character
     * may start only a radio link; see org_is_radio_link().) */
    #define IS_MARK_CHAR(off)   ((CH(off) < SIZEOF_ARRAY(ctx->mark_char_map))              \
                                    ? ctx->mark_char_map[(unsigned char) CH(off)]           \
                                    : ctx->radio_first_char_map[(unsigned char) CH(off)])
#else
    /* For 8-bit encodings, mark_char_map[] covers all 256 elements. */
    #define IS_MARK_CHAR(off)   (ctx->mark_char_map[(unsigned char) CH(off)])
#endif

            /* Optimization: Use some loop unrolling. */
            while(off + 3 < line->end  &&  !IS_MARK_CHAR(off+0)  &&  !IS_MARK_CHAR(off+1)
                                       &&  !IS_MARK_CHAR(off+2)  &&  !IS_MARK_CHAR(off+3))
                off += 4;
            while(off < line->end  &&  !IS_MARK_CHAR(off+0))
                off++;

            if(off >= line->end)
                break;

            ch = CH(off);

            /* Inline objects. Note we do not look for objects within the
             * contents of other objects whose contents is not parsed (e.g.
             * within a LaTeX fragment): That would be useless and it could
             * make us quadratic. */
            if(off >= raw_skip_until) {
                memset(&tmp, 0, sizeof(ORG_MARK));
                tmp.beg = off;
                tmp.next = -1;

                if(org_is_object(ctx, off, region_beg, region_end, line->beg, line->end,
                                 link_skip_until, raw_skip_until, &tmp)) {
                    mark = org_push_mark(ctx);
                    if(mark == NULL) {
                        ret = -1;
                        goto abort;
                    }
                    *mark = tmp;

                    if(tmp.ch == ORG_MARK_BRACKET_LINK_OBJECT  ||  tmp.ch == ORG_MARK_PLAIN_LINK_OBJECT  ||
                       tmp.ch == ORG_MARK_RADIO_LINK_OBJECT)
                        link_skip_until = tmp.end;
                    if(org_is_raw_object(tmp.ch))
                        raw_skip_until = tmp.end;

                    /* If the object begins before 'off' (see org_is_object()),
                     * 'off' is within its (raw) contents; so it may still be
                     * an emphasis mark (as any other mark char there). */
                    if(tmp.beg == off)
                        continue;
                }
            }

            if(org_emph_index(ch) >= 0) {
                unsigned flags = 0;

                if(off > region_beg  &&  !ISUNICODEWHITESPACEBEFORE(off))
                    flags |= ORG_MARK_NONSPACE_BEFORE;
                if(off + 1 < region_end  &&  !ISUNICODEWHITESPACE(off+1))
                    flags |= ORG_MARK_NONSPACE_AFTER;

                /* Potential opener: preceded by PRE (or the beginning), and
                 * followed by a non-whitespace. */
                if((flags & ORG_MARK_NONSPACE_AFTER)  &&
                   (off == 0  ||  ISUNICODEWHITESPACEBEFORE(off)  ||  ISANYOF(off-1, ORG_EMPH_PRE_CHARS)))
                    flags |= ORG_MARK_POTENTIAL_OPENER;

                /* Potential closer: preceded by a non-whitespace, and followed
                 * by POST (or the end). */
                if((flags & ORG_MARK_NONSPACE_BEFORE)  &&
                   (off + 1 >= region_end  ||  ISUNICODEWHITESPACE(off+1)  ||  ISANYOF(off+1, ORG_EMPH_POST_CHARS)))
                    flags |= ORG_MARK_POTENTIAL_CLOSER;

                if(flags == 0)
                    continue;

                mark = org_push_mark(ctx);
                if(mark == NULL) {
                    ret = -1;
                    goto abort;
                }
                mark->beg = off;
                mark->end = off + 1;
                mark->ch = ch;
                mark->flags = flags;
            }
        }
    }

abort:
    return ret;
}

#undef IS_MARK_CHAR

/* Link each emphasis mark to the nearest potential closer of the same kind
 * (see ORG_MARK::next). */
static void
org_analyze_marks(ORG_CTX* ctx)
{
    int last_closer[6] = { -1, -1, -1, -1, -1, -1 };
    int i;

    for(i = ctx->n_marks - 1; i >= 0; i--) {
        ORG_MARK* mark = &ctx->marks[i];
        int emph_index = org_emph_index(mark->ch);

        if(emph_index < 0)
            continue;
        mark->next = last_closer[emph_index];
        if(mark->flags & ORG_MARK_POTENTIAL_CLOSER)
            last_closer[emph_index] = i;
    }
}

/* Analyze the inline contents of a block, i.e. collect all the marks and
 * link them together. */
static int
org_analyze_inlines(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines)
{
    int ret = 0;

    /* Reset the previously collected stack of marks. */
    ctx->n_marks = 0;

    /* Reset all the horizons. */
    ctx->link_close_horizon = 0;
    ctx->latex_paren_horizon = 0;
    ctx->latex_bracket_horizon = 0;
    ctx->latex_dollars_horizon = 0;
    ctx->snippet_horizon = 0;
    ctx->macro_horizon = 0;

    ORG_CHECK(org_collect_marks(ctx, lines, n_lines));
    org_analyze_marks(ctx);

abort:
    return ret;
}

/* Find index of the mark at the given offset; or -1. */
static int
org_lookup_mark(ORG_CTX* ctx, OFF off)
{
    int lo = 0;
    int hi = ctx->n_marks;

    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        if(ctx->marks[pivot].beg < off)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    if(lo < ctx->n_marks  &&  ctx->marks[lo].beg == off)
        return lo;
    return -1;
}

/* Find the closer for the opener marks[mark_index] within a range ending at
 * 'end'; or -1. */
static int
org_find_closer(ORG_CTX* ctx, int mark_index, OFF end)
{
    ORG_MARK* opener = &ctx->marks[mark_index];
    int closer_index = opener->next;

    /* The contents of the span cannot be empty. */
    if(closer_index >= 0  &&  ctx->marks[closer_index].beg == opener->beg + 1)
        closer_index = ctx->marks[closer_index].next;

    if(closer_index >= 0  &&  ctx->marks[closer_index].beg < end)
        return closer_index;

    /* The end of the range acts as an end of line. */
    if(end >= opener->beg + 3) {
        closer_index = org_lookup_mark(ctx, end - 1);
        if(closer_index >= 0  &&  ctx->marks[closer_index].ch == opener->ch  &&
           (ctx->marks[closer_index].flags & ORG_MARK_NONSPACE_BEFORE))
            return closer_index;
    }

    return -1;
}

/* Emit the text [beg, end), which may span over multiple lines. */
static int
org_process_text(ORG_CTX* ctx, ORG_TEXTTYPE text_type, const ORG_LINE* lines, SZ n_lines,
                 OFF beg, OFF end, int hard_breaks)
{
    const ORG_LINE* line;
    SZ line_index;
    OFF off = beg;
    int ret = 0;

    if(beg >= end)
        return 0;

    /* Optimization: The texts are mostly emitted in their order, so the line
     * is usually the first one, the one where the previous text ended, or
     * the next one. (Same result as org_lookup_line(): The first line not
     * ending before beg.) */
    if(n_lines > 0  &&  lines[0].end >= beg) {
        line_index = 0;
    } else {
        line_index = ctx->text_line_hint;
        if(line_index < n_lines  &&  lines[line_index].end < beg)
            line_index++;
    }
    if(line_index < n_lines  &&  lines[line_index].end >= beg  &&
       (line_index == 0  ||  lines[line_index-1].end < beg))
    {
        line = &lines[line_index];
    } else {
        line = org_lookup_line(beg, lines, n_lines, &line_index);
        if(line == NULL)
            return 0;
    }
    if(off < line->beg)
        off = line->beg;

    while(1) {
        OFF line_end = line->end;
        int is_hard_break = hard_breaks;

        /* Line break "\\" at the end of line. (It must not be preceded by
         * another backslash.) */
        if(text_type == ORG_TEXT_NORMAL  &&  line_end >= line->beg + 2  &&
           CH(line_end-1) == _T('\\')  &&  CH(line_end-2) == _T('\\')  &&
           (line_end == line->beg + 2  ||  CH(line_end-3) != _T('\\')))
        {
            line_end -= 2;
            is_hard_break = true;
        }

        if(off < MIN(end, line_end))
            ORG_TEXT_INSECURE(text_type, STR(off), MIN(end, line_end) - off);

        if(end <= line->end  ||  line_index + 1 >= n_lines)
            break;

        if(is_hard_break)
            ORG_TEXT(ORG_TEXT_BR, _T("\n"), 1);
        else
            ORG_TEXT(ORG_TEXT_SOFTBR, _T("\n"), 1);

        line_index++;
        line++;
        off = line->beg;
        if(off >= end)
            break;
    }

    ctx->text_line_hint = line_index;

abort:
    return ret;
}

static int
org_process_inline_range(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines,
                         OFF beg, OFF end, int mark_index, int hard_breaks);

static const ORG_CODE_REF* org_lookup_code_ref(ORG_CTX* ctx, const CHAR* name, SZ name_size);

static int
org_process_link(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines, int mark_index, int hard_breaks)
{
    ORG_MARK* mark = &ctx->marks[mark_index];
    ORG_SPAN_LINK_DETAIL det;
    ORG_ATTRIBUTE_BUILD type_build;
    ORG_ATTRIBUTE_BUILD path_build;
    ORG_ATTRIBUTE_BUILD raw_build;
    OFF target_beg = mark->sub_beg;
    OFF target_end = mark->sub_end;
    OFF path_beg = target_beg;
    const CHAR* type = NULL;
    SZ type_size = 0;
    OFF off;
    int ret = 0;

    memset(&det, 0, sizeof(ORG_SPAN_LINK_DETAIL));

    /* Determine the link type. */
    off = target_beg;
    while(off < target_end  &&  (ISALNUM(off)  ||  ISANYOF(off, _T("+-."))))
        off++;
    if(off < target_end  &&  off > target_beg  &&  CH(off) == _T(':')  &&
       (mark->ch == ORG_MARK_PLAIN_LINK_OBJECT  ||
        org_is_link_type(ctx, target_beg, off, org_link_types, (int) SIZEOF_ARRAY(org_link_types))))
    {
        type = STR(target_beg);
        type_size = off - target_beg;
        path_beg = off + 1;
    } else if(CH(target_beg) == _T('#')) {
        type = _T("custom-id");
        path_beg = target_beg + 1;
    } else if(CH(target_beg) == _T('(')  &&  CH(target_end-1) == _T(')')) {
        type = _T("coderef");
        path_beg = target_beg + 1;
        target_end--;
    } else if(CH(target_beg) == _T('/')  ||
              org_is_prefix(ctx, target_beg, target_end, _T("./"))  ||
              org_is_prefix(ctx, target_beg, target_end, _T("../"))  ||
              org_is_prefix(ctx, target_beg, target_end, _T("~/")))
    {
        type = _T("file");
    } else {
        type = _T("fuzzy");
    }
    if(type_size == 0)
        type_size = (SZ) org_strlen(type);

    org_build_attribute(ctx, type, type_size, &det.type, &type_build);
    org_build_attribute(ctx, STR(path_beg), target_end - path_beg, &det.path, &path_build);
    org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.raw, &raw_build);
    det.is_plain = (mark->ch == ORG_MARK_PLAIN_LINK_OBJECT);
    det.has_description = (mark->desc_end > mark->desc_beg);

    ORG_ENTER_SPAN(ORG_SPAN_LINK, &det);

    /* Link to a code reference without a description shows the name of the
     * reference; or its line number if the label is not retained ("-r"). */
    if(!det.has_description  &&  CH(mark->sub_beg) == _T('(')  &&  target_end > path_beg  &&
       type_size == 7  &&  org_ascii_eq(type, _T("coderef"), 7))
    {
        const ORG_CODE_REF* ref = org_lookup_code_ref(ctx, STR(path_beg), target_end - path_beg);

        if(ref != NULL  &&  !ref->use_label) {
            char num[16];
            CHAR num_text[16];
            int i;

            snprintf(num, sizeof(num), "%u", ref->line_number);
            for(i = 0; num[i] != '\0'; i++)
                num_text[i] = (CHAR) num[i];
            ORG_TEXT(ORG_TEXT_NORMAL, num_text, (SZ) i);
        } else {
            ORG_TEXT_INSECURE(ORG_TEXT_NORMAL, STR(path_beg), target_end - path_beg);
        }
        ORG_LEAVE_SPAN(ORG_SPAN_LINK, &det);
        goto abort;
    }

    if(det.has_description) {
        ORG_CHECK(org_process_inline_range(ctx, lines, n_lines, mark->desc_beg, mark->desc_end,
                    mark_index + 1, hard_breaks));
    } else {
        ORG_CHECK(org_process_text(ctx, ORG_TEXT_NORMAL, lines, n_lines,
                    mark->sub_beg, mark->sub_end, hard_breaks));
    }
    ORG_LEAVE_SPAN(ORG_SPAN_LINK, &det);

abort:
    return ret;
}

static int
org_process_footnote_ref(ORG_CTX* ctx, int mark_index)
{
    const ORG_MARK* mark = &ctx->marks[mark_index];
    ORG_SPAN_FOOTNOTE_REF_DETAIL det;
    ORG_ATTRIBUTE_BUILD label_build;
    ORG_FOOTNOTE* fn;
    int index;
    int ret = 0;

    if(mark->sub_end == mark->sub_beg) {
        /* Anonymous footnote. */
        ORG_CHECK(org_add_footnote(ctx, NULL, 0, &index));
    } else {
        ORG_CHECK(org_get_footnote(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &index));
    }

    fn = &ctx->footnotes[index];
    if((mark->flags & ORG_MARK_INLINE_DEFINITION)  &&  fn->def_open_off < 0  &&  !fn->has_inline_def) {
        fn->has_inline_def = true;
        fn->inline_beg = mark->desc_beg;
        fn->inline_end = mark->desc_end;
        fn->nesting_level = ctx->footnote_nesting_level + 1;
    }

    ORG_CHECK(org_reference_footnote(ctx, index));
    fn = &ctx->footnotes[index];

    memset(&det, 0, sizeof(det));
    det.id = fn->id;
    det.ref_id = fn->ref_count;
    org_build_attribute(ctx, fn->label, fn->label_size, &det.label, &label_build);

    ORG_ENTER_SPAN(ORG_SPAN_FOOTNOTE_REF, &det);
    ORG_LEAVE_SPAN(ORG_SPAN_FOOTNOTE_REF, &det);

abort:
    return ret;
}

static int org_process_normal_block_contents(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines, int hard_breaks);

/* Growable string buffer (for the macro expansion). */
typedef struct ORG_STRBUF_tag ORG_STRBUF;
struct ORG_STRBUF_tag {
    CHAR* data;
    SZ size;
    SZ alloc;
};

static int
org_strbuf_append(ORG_CTX* ctx, ORG_STRBUF* buf, const CHAR* str, SZ size)
{
    /* (The buffer may still be NULL, and so may be 'str' when empty.) */
    if(size == 0)
        return 0;

    if(buf->size + size > buf->alloc) {
        SZ new_alloc = buf->alloc + buf->alloc / 2 + size + 64;
        CHAR* new_data = (CHAR*) realloc(buf->data, new_alloc * sizeof(CHAR));

        if(new_data == NULL) {
            ORG_LOG("realloc() failed.");
            return -1;
        }
        buf->data = new_data;
        buf->alloc = new_alloc;
    }

    memcpy(buf->data + buf->size, str, size * sizeof(CHAR));
    buf->size += size;
    return 0;
}

/* Split the macro arguments [beg, end) into the buffer 'buf'. The offset and
 * size of each argument within the buffer is stored in 'arg_offs' and
 * 'arg_sizes'. As Org does, whitespace sequences are collapsed into a single
 * space, and commas may be escaped with a backslash. */
static int
org_macro_arguments(ORG_CTX* ctx, OFF beg, OFF end, ORG_STRBUF* buf,
                    SZ* arg_offs, SZ* arg_sizes, int* p_n_args)
{
    OFF off = beg;
    SZ arg_beg = 0;
    int n_args = 0;
    int ret = 0;

    *p_n_args = 0;
    while(off < end  &&  (ISWHITESPACE(off)  ||  ISNEWLINE(off)))
        off++;
    if(off >= end  &&  beg == end)
        return 0;

    while(1) {
        if(off >= end  ||  CH(off) == _T(',')) {
            if(n_args < MACRO_MAX_ARGS) {
                arg_offs[n_args] = arg_beg;
                arg_sizes[n_args] = buf->size - arg_beg;
                n_args++;
            }
            if(off >= end)
                break;
            off++;
            arg_beg = buf->size;
            continue;
        }

        if(CH(off) == _T('\\')) {
            OFF bs_end = off;
            SZ n_bs;

            while(bs_end < end  &&  CH(bs_end) == _T('\\'))
                bs_end++;
            n_bs = bs_end - off;
            if(bs_end < end  &&  CH(bs_end) == _T(',')) {
                /* An odd count escapes the comma; the backslashes before it
                 * are escaped by each other. */
                SZ i;
                for(i = 0; i < n_bs / 2; i++)
                    ORG_CHECK(org_strbuf_append(ctx, buf, _T("\\"), 1));
                if(n_bs % 2 == 1) {
                    ORG_CHECK(org_strbuf_append(ctx, buf, _T(","), 1));
                    off = bs_end + 1;
                } else {
                    off = bs_end;
                }
            } else {
                ORG_CHECK(org_strbuf_append(ctx, buf, STR(off), n_bs));
                off = bs_end;
            }
            continue;
        }

        if(ISWHITESPACE(off)  ||  ISNEWLINE(off)) {
            while(off < end  &&  (ISWHITESPACE(off)  ||  ISNEWLINE(off)))
                off++;
            ORG_CHECK(org_strbuf_append(ctx, buf, _T(" "), 1));
            continue;
        }

        ORG_CHECK(org_strbuf_append(ctx, buf, STR(off), 1));
        off++;
    }

    *p_n_args = n_args;

abort:
    return ret;
}

/* Append values of all the keywords "#+KEY:" (separated by a space). */
static int
org_append_keyword_values(ORG_CTX* ctx, ORG_STRBUF* buf, const CHAR* key, SZ key_size)
{
    int lo = 0;
    int hi = ctx->n_doc_keywords;
    int is_first = true;
    int ret = 0;

    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        const ORG_DOC_KEYWORD* kw = &ctx->doc_keywords[pivot];

        if(org_ascii_case_cmp(kw->key, kw->key_size, key, key_size) < 0)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    while(lo < ctx->n_doc_keywords  &&  ctx->doc_keywords[lo].key_size == key_size  &&
          org_ascii_case_eq(ctx->doc_keywords[lo].key, key, key_size))
    {
        if(!is_first)
            ORG_CHECK(org_strbuf_append(ctx, buf, _T(" "), 1));
        ORG_CHECK(org_strbuf_append(ctx, buf, ctx->doc_keywords[lo].value, ctx->doc_keywords[lo].value_size));
        is_first = false;
        lo++;
    }

abort:
    return ret;
}

/* Find the (first) definition of the macro. */
static const ORG_DOC_KEYWORD*
org_lookup_macro(ORG_CTX* ctx, const CHAR* name, SZ name_size)
{
    int lo = 0;
    int hi = ctx->n_macros;

    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        const ORG_DOC_KEYWORD* macro = &ctx->macros[pivot];

        if(org_ascii_case_cmp(macro->key, macro->key_size, name, name_size) < 0)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    if(lo < ctx->n_macros  &&  ctx->macros[lo].key_size == name_size  &&
       org_ascii_case_eq(ctx->macros[lo].key, name, name_size))
        return &ctx->macros[lo];
    return NULL;
}

/* Macro "{{{n}}}", "{{{n(name)}}}", "{{{n(name,action)}}}". */
static int
org_expand_counter_macro(ORG_CTX* ctx, ORG_STRBUF* buf, const CHAR* args,
                         const SZ* arg_offs, const SZ* arg_sizes, int n_args)
{
    const CHAR* name = (n_args > 0) ? args + arg_offs[0] : _T("");
    SZ name_size = (n_args > 0) ? arg_sizes[0] : 0;
    const CHAR* action = (n_args > 1) ? args + arg_offs[1] : _T("");
    SZ action_size = (n_args > 1) ? arg_sizes[1] : 0;
    ORG_MACRO_COUNTER* counter = NULL;
    char num[16];
    int i;

    while(name_size > 0  &&  ISWHITESPACE_(name[name_size-1]))
        name_size--;
    while(action_size > 0  &&  ISWHITESPACE_(*action)) {
        action++;
        action_size--;
    }
    while(action_size > 0  &&  ISWHITESPACE_(action[action_size-1]))
        action_size--;

    if(name_size > MACRO_COUNTER_NAME_MAX)
        name_size = MACRO_COUNTER_NAME_MAX;

    for(i = 0; i < ctx->n_macro_counters; i++) {
        if(ctx->macro_counters[i].name_size == name_size  &&
           org_ascii_eq(ctx->macro_counters[i].name, name, name_size))
        {
            counter = &ctx->macro_counters[i];
            break;
        }
    }
    if(counter == NULL) {
        if(ctx->n_macro_counters >= MACRO_MAX_COUNTERS)
            return 0;
        counter = &ctx->macro_counters[ctx->n_macro_counters++];
        memcpy(counter->name, name, name_size * sizeof(CHAR));
        counter->name_size = name_size;
        counter->value = 0;
    }

    if(action_size == 1  &&  action[0] == _T('-')) {
        /* Just the current value. */
    } else if(action_size > 0  &&  ISDIGIT_(action[0])) {
        int value = 0;
        for(i = 0; i < (int) action_size  &&  ISDIGIT_(action[i])  &&  value < NUMBER_MAX; i++)
            value = value * 10 + (action[i] - _T('0'));
        counter->value = value;
    } else if(action_size > 0) {
        counter->value = 1;
    } else {
        counter->value++;
    }

    snprintf(num, sizeof(num), "%d", counter->value);
    for(i = 0; num[i] != '\0'; i++) {
        CHAR ch = (CHAR) num[i];
        if(org_strbuf_append(ctx, buf, &ch, 1) != 0)
            return -1;
    }
    return 0;
}

/* Expand the macro 'mark' into 'buf'. Returns 1 if the macro is
 * unknown (and not expanded). */
static int
org_expand_macro(ORG_CTX* ctx, const ORG_MARK* mark, ORG_STRBUF* buf)
{
    const CHAR* name = STR(mark->sub_beg);
    SZ name_size = mark->sub_end - mark->sub_beg;
    ORG_STRBUF args = { NULL, 0, 0 };
    const CHAR* args_text;
    SZ arg_offs[MACRO_MAX_ARGS];
    SZ arg_sizes[MACRO_MAX_ARGS];
    int n_args;
    const ORG_DOC_KEYWORD* macro;
    int ret = 0;

    ORG_CHECK(org_macro_arguments(ctx, mark->desc_beg, mark->desc_end, &args, arg_offs, arg_sizes, &n_args));
    /* If all the arguments are empty, the buffer is still NULL. */
    args_text = (args.data != NULL ? args.data : _T(""));

    macro = org_lookup_macro(ctx, name, name_size);
    if(macro != NULL) {
        const CHAR* tmpl = macro->value;
        SZ tmpl_size = macro->value_size;
        SZ i = 0;

        /* We cannot evaluate Emacs Lisp: Treat it as an unknown macro. */
        if(tmpl_size >= 6  &&  org_ascii_eq(tmpl, _T("(eval "), 6)) {
            ret = 1;
            goto abort;
        }

        while(i < tmpl_size) {
            if(tmpl[i] == _T('$')  &&  i + 1 < tmpl_size  &&  ISDIGIT_(tmpl[i+1])) {
                int n = 0;

                i++;
                while(i < tmpl_size  &&  ISDIGIT_(tmpl[i])  &&  n < 1000)
                    n = n * 10 + (tmpl[i++] - _T('0'));
                if(n >= 1  &&  n <= n_args)
                    ORG_CHECK(org_strbuf_append(ctx, buf, args_text + arg_offs[n-1], arg_sizes[n-1]));
            } else {
                ORG_CHECK(org_strbuf_append(ctx, buf, tmpl + i, 1));
                i++;
            }
        }
    } else if(name_size == 5  &&  org_ascii_case_eq(name, _T("title"), 5)) {
        ORG_CHECK(org_append_keyword_values(ctx, buf, _T("TITLE"), 5));
    } else if(name_size == 6  &&  org_ascii_case_eq(name, _T("author"), 6)) {
        ORG_CHECK(org_append_keyword_values(ctx, buf, _T("AUTHOR"), 6));
    } else if(name_size == 5  &&  org_ascii_case_eq(name, _T("email"), 5)) {
        ORG_CHECK(org_append_keyword_values(ctx, buf, _T("EMAIL"), 5));
    } else if(name_size == 4  &&  org_ascii_case_eq(name, _T("date"), 4)) {
        ORG_CHECK(org_append_keyword_values(ctx, buf, _T("DATE"), 4));
    } else if(name_size == 7  &&  org_ascii_case_eq(name, _T("keyword"), 7)) {
        if(n_args > 0)
            ORG_CHECK(org_append_keyword_values(ctx, buf, args_text + arg_offs[0], arg_sizes[0]));
    } else if(name_size == 1  &&  (name[0] == _T('n')  ||  name[0] == _T('N'))) {
        /* The arguments of the counter are not escaped. */
        SZ raw_offs[MACRO_MAX_ARGS];
        SZ raw_sizes[MACRO_MAX_ARGS];
        OFF off = mark->desc_beg;
        int n_raw = 0;

        while(off < mark->desc_end  &&  n_raw < 2) {
            OFF arg_end = off;

            while(arg_end < mark->desc_end  &&  CH(arg_end) != _T(','))
                arg_end++;
            while(off < arg_end  &&  ISWHITESPACE(off))
                off++;
            raw_offs[n_raw] = off - mark->desc_beg;
            raw_sizes[n_raw] = arg_end - off;
            n_raw++;
            off = arg_end + 1;
        }
        ORG_CHECK(org_expand_counter_macro(ctx, buf, STR(mark->desc_beg), raw_offs, raw_sizes, n_raw));
    } else if((name_size == 4  &&  org_ascii_case_eq(name, _T("time"), 4))  ||
              (name_size == 8  &&  org_ascii_case_eq(name, _T("property"), 8))  ||
              (name_size == 10  &&  org_ascii_case_eq(name, _T("input-file"), 10))  ||
              (name_size == 17  &&  org_ascii_case_eq(name, _T("modification-time"), 17)))
    {
        /* Known, but unsupported: They expand to nothing. */
    } else {
        ret = 1;
    }

abort:
    free(args.data);
    return ret;
}

/* Process an inline text which is not a part of the document (e.g. a macro
 * expansion). All the context of the inline processing is saved and then
 * restored, as we may be in the middle of processing another block. */
static int
org_process_detached_text(ORG_CTX* ctx, const CHAR* text, SZ size)
{
    ORG_CTX saved;
    ORG_LINE line;
    int ret;

    /* (Nothing to do; and 'text' may be NULL then.) */
    if(size == 0)
        return 0;

    saved = *ctx;

    ctx->text = text;
    ctx->size = size;
    ctx->marks = NULL;
    ctx->n_marks = 0;
    ctx->alloc_marks = 0;
    ctx->bracket_matches = NULL;
    ctx->alloc_bracket_matches = 0;
    ctx->bracket_matches_valid = false;
    ctx->cr_scan_beg = 0;
    ctx->cr_horizon = 0;
    ctx->lf_scan_beg = 0;
    ctx->lf_horizon = 0;
    ctx->is_detached = true;

    line.beg = 0;
    line.end = size;
    ret = org_process_normal_block_contents(ctx, &line, 1, false);

    free(ctx->marks);
    free(ctx->bracket_matches);

    /* Restore everything except the state which has to survive. */
    saved.span_nesting_level = ctx->span_nesting_level;
    saved.macro_output_budget = ctx->macro_output_budget;
    saved.n_macro_counters = ctx->n_macro_counters;
    memcpy(saved.macro_counters, ctx->macro_counters, sizeof(saved.macro_counters));
    saved.footnotes = ctx->footnotes;
    saved.n_footnotes = ctx->n_footnotes;
    saved.alloc_footnotes = ctx->alloc_footnotes;
    saved.footnote_buckets = ctx->footnote_buckets;
    saved.n_footnote_buckets = ctx->n_footnote_buckets;
    saved.n_labeled_footnotes = ctx->n_labeled_footnotes;
    saved.footnote_order = ctx->footnote_order;
    saved.n_footnote_order = ctx->n_footnote_order;
    saved.alloc_footnote_order = ctx->alloc_footnote_order;
    saved.attribute_storages = ctx->attribute_storages;
    *ctx = saved;

    return ret;
}

static int
org_process_macro(ORG_CTX* ctx, int mark_index)
{
    const ORG_MARK* mark = &ctx->marks[mark_index];
    ORG_SPAN_MACRO_DETAIL det;
    ORG_ATTRIBUTE_BUILD attr_build[2];
    ORG_STRBUF buf = { NULL, 0, 0 };
    OFF beg = mark->beg;
    OFF end = mark->end;
    int ret = 0;

    memset(&det, 0, sizeof(det));
    org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.name, &attr_build[0]);
    org_build_attribute(ctx, STR(mark->desc_beg), mark->desc_end - mark->desc_beg, &det.args, &attr_build[1]);

    ret = org_expand_macro(ctx, mark, &buf);
    if(ret < 0)
        goto abort;
    if(ret > 0) {
        /* Unknown macro: Keep it as it is. */
        ret = 0;
        ORG_TEXT_INSECURE(ORG_TEXT_NORMAL, STR(beg), end - beg);
        goto abort;
    }

    ORG_ENTER_SPAN(ORG_SPAN_MACRO, &det);
    if(ctx->macro_nesting_level < MACRO_MAX_NESTING  &&  buf.size <= ctx->macro_output_budget) {
        ctx->macro_output_budget -= buf.size;
        ctx->macro_nesting_level++;
        ret = org_process_detached_text(ctx, buf.data, buf.size);
        ctx->macro_nesting_level--;
        if(ret != 0)
            goto abort;
    } else {
        ORG_LOG("Macro expansion too big or too deeply nested.");
    }
    ORG_LEAVE_SPAN(ORG_SPAN_MACRO, &det);

abort:
    free(buf.data);
    return ret;
}

/* Citation: Split the body into the references (separated by ';'). The part
 * before the first reference (if it has no key) is the global prefix, the part
 * after the last one is the global suffix. */
static int
org_process_citation(ORG_CTX* ctx, int mark_index)
{
    const ORG_MARK* mark = &ctx->marks[mark_index];
    ORG_SPAN_CITATION_DETAIL det;
    ORG_ATTRIBUTE_BUILD attr_build[3];
    OFF parts[CITATION_MAX_PARTS][2];
    int n_parts = 0;
    int first_ref, last_ref;
    OFF off = mark->desc_beg;
    int i;
    int ret = 0;

    /* Split into the parts. (See CITATION_MAX_PARTS.) */
    while(n_parts < (int) SIZEOF_ARRAY(parts)) {
        OFF part_end = off;

        while(part_end < mark->desc_end  &&  CH(part_end) != _T(';'))
            part_end++;
        parts[n_parts][0] = off;
        parts[n_parts][1] = part_end;
        n_parts++;
        if(part_end >= mark->desc_end)
            break;
        off = part_end + 1;
    }

    /* Which parts are references (i.e. contain a key)? */
    first_ref = -1;
    last_ref = -1;
    for(i = 0; i < n_parts; i++) {
        OFF q;
        for(q = parts[i][0]; q < parts[i][1]; q++) {
            if(CH(q) == _T('@')  &&  q + 1 < parts[i][1]  &&  !ISWHITESPACE(q+1))
                break;
        }
        if(q < parts[i][1]) {
            if(first_ref < 0)
                first_ref = i;
            last_ref = i;
        }
    }

    memset(&det, 0, sizeof(det));
    org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.style, &attr_build[0]);
    if(first_ref > 0) {
        OFF beg = org_skip_blanks(ctx, parts[0][0], parts[0][1]);
        org_build_attribute(ctx, STR(beg), org_skip_blanks_backward(ctx, beg, parts[0][1]) - beg, &det.prefix, &attr_build[1]);
    } else {
        org_build_attribute(ctx, NULL, 0, &det.prefix, &attr_build[1]);
    }
    if(last_ref >= 0  &&  last_ref < n_parts - 1) {
        OFF beg = org_skip_blanks(ctx, parts[n_parts-1][0], parts[n_parts-1][1]);
        org_build_attribute(ctx, STR(beg), org_skip_blanks_backward(ctx, beg, parts[n_parts-1][1]) - beg, &det.suffix, &attr_build[2]);
    } else {
        org_build_attribute(ctx, NULL, 0, &det.suffix, &attr_build[2]);
    }

    ORG_ENTER_SPAN(ORG_SPAN_CITATION, &det);
    for(i = first_ref; i >= 0  &&  i <= last_ref; i++) {
        ORG_SPAN_CITATION_REFERENCE_DETAIL ref_det;
        ORG_ATTRIBUTE_BUILD ref_build[3];
        OFF at, key_end, beg, end;

        for(at = parts[i][0]; at < parts[i][1]; at++) {
            if(CH(at) == _T('@')  &&  at + 1 < parts[i][1]  &&  !ISWHITESPACE(at+1))
                break;
        }
        if(at >= parts[i][1])
            continue;
        key_end = at + 1;
        while(key_end < parts[i][1]  &&  !ISWHITESPACE(key_end)  &&  !ISNEWLINE(key_end))
            key_end++;

        memset(&ref_det, 0, sizeof(ref_det));
        org_build_attribute(ctx, STR(at + 1), key_end - (at + 1), &ref_det.key, &ref_build[0]);
        beg = org_skip_blanks(ctx, parts[i][0], at);
        end = org_skip_blanks_backward(ctx, beg, at);
        org_build_attribute(ctx, STR(beg), end - beg, &ref_det.prefix, &ref_build[1]);
        beg = org_skip_blanks(ctx, key_end, parts[i][1]);
        end = org_skip_blanks_backward(ctx, beg, parts[i][1]);
        org_build_attribute(ctx, STR(beg), end - beg, &ref_det.suffix, &ref_build[2]);

        ORG_ENTER_SPAN(ORG_SPAN_CITATION_REFERENCE, &ref_det);
        ORG_LEAVE_SPAN(ORG_SPAN_CITATION_REFERENCE, &ref_det);
    }
    ORG_LEAVE_SPAN(ORG_SPAN_CITATION, &det);

abort:
    return ret;
}

static int
org_process_object(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines, int mark_index, int hard_breaks)
{
    const ORG_MARK* mark = &ctx->marks[mark_index];
    union {
        ORG_SPAN_TIMESTAMP_DETAIL timestamp;
        ORG_SPAN_INLINE_SRC_DETAIL inline_src;
        ORG_SPAN_EXPORT_SNIPPET_DETAIL snippet;
        ORG_SPAN_TARGET_DETAIL target;
        ORG_SPAN_LINK_DETAIL link;
        ORG_SPAN_INLINE_BABEL_CALL_DETAIL call;
    } det;
    ORG_ATTRIBUTE_BUILD link_build[3];
    ORG_ATTRIBUTE_BUILD attr_build[2];
    ORG_SPANTYPE span_type;
    int ret = 0;

    memset(&det, 0, sizeof(det));

    switch(mark->ch) {
        case ORG_MARK_BRACKET_LINK_OBJECT:
        case ORG_MARK_PLAIN_LINK_OBJECT:
            return org_process_link(ctx, lines, n_lines, mark_index, hard_breaks);

        case ORG_MARK_FOOTNOTE_REF_OBJECT:
            return org_process_footnote_ref(ctx, mark_index);

        case ORG_MARK_MACRO_OBJECT:
            return org_process_macro(ctx, mark_index);

        case ORG_MARK_CITATION_OBJECT:
            return org_process_citation(ctx, mark_index);

        case ORG_MARK_RADIO_TARGET_OBJECT:
            org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.target.name, &attr_build[0]);
            det.target.is_radio = true;
            ORG_ENTER_SPAN(ORG_SPAN_TARGET, &det.target);
            ORG_TEXT_INSECURE(ORG_TEXT_NORMAL, STR(mark->sub_beg), mark->sub_end - mark->sub_beg);
            ORG_LEAVE_SPAN(ORG_SPAN_TARGET, &det.target);
            break;

        case ORG_MARK_RADIO_LINK_OBJECT:
        {
            const ORG_RADIO_TARGET* rt = &ctx->radio_targets[mark->index];

            org_build_attribute(ctx, _T("radio"), 5, &det.link.type, &link_build[0]);
            org_build_attribute(ctx, rt->text, rt->size, &det.link.path, &link_build[1]);
            org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.link.raw, &link_build[2]);
            det.link.has_description = true;
            ORG_ENTER_SPAN(ORG_SPAN_LINK, &det.link);
            ORG_TEXT_INSECURE(ORG_TEXT_NORMAL, STR(mark->sub_beg), mark->sub_end - mark->sub_beg);
            ORG_LEAVE_SPAN(ORG_SPAN_LINK, &det.link);
            break;
        }

        case ORG_MARK_INLINE_BABEL_CALL_OBJECT:
            org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.call.name, &attr_build[0]);
            org_build_attribute(ctx, STR(mark->desc_beg), mark->desc_end - mark->desc_beg, &det.call.args, &attr_build[1]);
            ORG_ENTER_SPAN(ORG_SPAN_INLINE_BABEL_CALL, &det.call);
            ORG_LEAVE_SPAN(ORG_SPAN_INLINE_BABEL_CALL, &det.call);
            break;

        case ORG_MARK_ENTITY_OBJECT:
            ORG_TEXT(ORG_TEXT_ENTITY, STR(mark->beg), mark->end - mark->beg);
            break;

        case ORG_MARK_LATEX_OBJECT:
            /* Report the raw source, including any new lines. */
            ORG_ENTER_SPAN(ORG_SPAN_LATEX, NULL);
            ORG_TEXT_INSECURE(ORG_TEXT_LATEX, STR(mark->beg), mark->end - mark->beg);
            ORG_LEAVE_SPAN(ORG_SPAN_LATEX, NULL);
            break;

        case ORG_MARK_TIMESTAMP_OBJECT:
            det.timestamp.is_active = ((mark->flags & ORG_MARK_ACTIVE_TIMESTAMP) != 0);
            det.timestamp.is_range = ((mark->flags & ORG_MARK_TIMESTAMP_RANGE) != 0);
            ORG_ENTER_SPAN(ORG_SPAN_TIMESTAMP, &det.timestamp);
            ORG_TEXT_INSECURE(ORG_TEXT_NORMAL, STR(mark->beg), mark->end - mark->beg);
            ORG_LEAVE_SPAN(ORG_SPAN_TIMESTAMP, &det.timestamp);
            break;

        case ORG_MARK_STATISTICS_COOKIE_OBJECT:
            ORG_ENTER_SPAN(ORG_SPAN_STATISTICS_COOKIE, NULL);
            ORG_TEXT(ORG_TEXT_NORMAL, STR(mark->beg), mark->end - mark->beg);
            ORG_LEAVE_SPAN(ORG_SPAN_STATISTICS_COOKIE, NULL);
            break;

        case ORG_MARK_TARGET_OBJECT:
            org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.target.name, &attr_build[0]);
            ORG_ENTER_SPAN(ORG_SPAN_TARGET, &det.target);
            ORG_LEAVE_SPAN(ORG_SPAN_TARGET, &det.target);
            break;

        case ORG_MARK_EXPORT_SNIPPET_OBJECT:
            org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.snippet.backend, &attr_build[0]);
            ORG_ENTER_SPAN(ORG_SPAN_EXPORT_SNIPPET, &det.snippet);
            ORG_TEXT_INSECURE(ORG_TEXT_EXPORT, STR(mark->desc_beg), mark->desc_end - mark->desc_beg);
            ORG_LEAVE_SPAN(ORG_SPAN_EXPORT_SNIPPET, &det.snippet);
            break;

        case ORG_MARK_INLINE_SRC_OBJECT:
            org_build_attribute(ctx, STR(mark->sub_beg), mark->sub_end - mark->sub_beg, &det.inline_src.lang, &attr_build[0]);
            org_build_attribute(ctx, STR(mark->aux_beg), mark->aux_end - mark->aux_beg, &det.inline_src.params, &attr_build[1]);
            ORG_ENTER_SPAN(ORG_SPAN_INLINE_SRC, &det.inline_src);
            ORG_TEXT_INSECURE(ORG_TEXT_CODE, STR(mark->desc_beg), mark->desc_end - mark->desc_beg);
            ORG_LEAVE_SPAN(ORG_SPAN_INLINE_SRC, &det.inline_src);
            break;

        case ORG_MARK_SUBSCRIPT_OBJECT:
        case ORG_MARK_SUPERSCRIPT_OBJECT:
            span_type = (mark->ch == ORG_MARK_SUBSCRIPT_OBJECT) ? ORG_SPAN_SUBSCRIPT : ORG_SPAN_SUPERSCRIPT;
            ORG_ENTER_SPAN(span_type, NULL);
            if(ctx->span_nesting_level < SPAN_MAX_NESTING) {
                ctx->span_nesting_level++;
                ret = org_process_inline_range(ctx, lines, n_lines,
                            mark->desc_beg, mark->desc_end, mark_index + 1, hard_breaks);
                ctx->span_nesting_level--;
                if(ret != 0)
                    goto abort;
            } else {
                ORG_CHECK(org_process_text(ctx, ORG_TEXT_NORMAL, lines, n_lines,
                            mark->desc_beg, mark->desc_end, hard_breaks));
            }
            ORG_LEAVE_SPAN(span_type, NULL);
            break;

        default:
            ORG_UNREACHABLE();
            break;
    }

abort:
    return ret;
}

static int
org_process_inline_range(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines,
                         OFF beg, OFF end, int mark_index, int hard_breaks)
{
    static const ORG_SPANTYPE span_types[6] = {
        ORG_SPAN_BOLD, ORG_SPAN_ITALIC, ORG_SPAN_UNDERLINE,
        ORG_SPAN_STRIKE, ORG_SPAN_VERBATIM, ORG_SPAN_CODE
    };
    OFF off = beg;
    int i = mark_index;
    int ret = 0;

    while(i < ctx->n_marks  &&  ctx->marks[i].beg < end) {
        ORG_MARK* mark = &ctx->marks[i];

        if(mark->beg < off) {
            /* Consumed by a preceding link or span. */
            i++;
            continue;
        }

        if(org_emph_index(mark->ch) < 0) {
            /* An inline object. */
            if(mark->end <= end) {
                ORG_CHECK(org_process_text(ctx, ORG_TEXT_NORMAL, lines, n_lines, off, mark->beg, hard_breaks));
                ORG_CHECK(org_process_object(ctx, lines, n_lines, i, hard_breaks));
                off = mark->end;
            }
        } else if(ctx->span_nesting_level < SPAN_MAX_NESTING  &&
                  ((mark->flags & ORG_MARK_POTENTIAL_OPENER)  ||
                   /* The beginning of the range acts as a beginning of line. */
                   (mark->beg == beg  &&  (mark->flags & ORG_MARK_NONSPACE_AFTER))))
        {
            int closer_index = org_find_closer(ctx, i, end);

            if(closer_index >= 0) {
                ORG_MARK* closer = &ctx->marks[closer_index];
                ORG_SPANTYPE span_type = span_types[org_emph_index(mark->ch)];

                ORG_CHECK(org_process_text(ctx, ORG_TEXT_NORMAL, lines, n_lines, off, mark->beg, hard_breaks));

                ORG_ENTER_SPAN(span_type, NULL);
                if(span_type == ORG_SPAN_VERBATIM  ||  span_type == ORG_SPAN_CODE) {
                    ORG_CHECK(org_process_text(ctx, ORG_TEXT_CODE, lines, n_lines,
                                mark->end, closer->beg, false));
                } else {
                    ctx->span_nesting_level++;
                    ret = org_process_inline_range(ctx, lines, n_lines,
                                mark->end, closer->beg, i + 1, hard_breaks);
                    ctx->span_nesting_level--;
                    if(ret != 0)
                        goto abort;
                }
                ORG_LEAVE_SPAN(span_type, NULL);

                off = closer->end;
                i = closer_index + 1;
                continue;
            }
        }

        i++;
    }

    ORG_CHECK(org_process_text(ctx, ORG_TEXT_NORMAL, lines, n_lines, off, end, hard_breaks));

abort:
    return ret;
}


/***************************
 ***  Processing Tables  ***
 ***************************/

/* Iterate over the cells of a table row. Initially, *p_off has to point just
 * behind the leading '|'. */
static int
org_next_table_cell(ORG_CTX* ctx, OFF* p_off, OFF end, OFF* p_cell_beg, OFF* p_cell_end)
{
    OFF off = *p_off;
    OFF cell_beg;

    if(off >= end)
        return false;

    cell_beg = off;
    while(off < end  &&  CH(off) != _T('|'))
        off++;

    *p_cell_end = org_skip_blanks_backward(ctx, cell_beg, off);
    *p_cell_beg = org_skip_blanks(ctx, cell_beg, *p_cell_end);
    *p_off = (off < end) ? off + 1 : off;
    return true;
}

/* Count the cells of a table row; i.e. how many times org_next_table_cell()
 * succeeds for it. */
static int
org_count_table_cells(ORG_CTX* ctx, const ORG_LINE* row)
{
    OFF off;
    int n = 0;

    if(row->beg + 1 >= row->end)
        return 0;

    /* Optimization: Only count the '|' delimiters; the cell contents and their
     * surrounding blanks do not matter here. Each '|' ends one cell, and the
     * text after the last one (if any) forms one more. */
    for(off = row->beg + 1; off < row->end; off++) {
        if(CH(off) == _T('|'))
            n++;
    }
    if(CH(row->end - 1) != _T('|'))
        n++;
    return n;
}

static int
org_is_table_rule_row(ORG_CTX* ctx, const ORG_LINE* row)
{
    return (row->beg + 1 < row->end  &&  CH(row->beg + 1) == _T('-'));
}

/* Alignment cookie, e.g. "<l>", "<c10>" or "<r>". */
static int
org_is_table_cookie(ORG_CTX* ctx, OFF beg, OFF end, ORG_ALIGN* p_align)
{
    OFF off = beg + 1;

    if(end - beg < 2  ||  CH(beg) != _T('<')  ||  CH(end-1) != _T('>'))
        return false;

    *p_align = ORG_ALIGN_DEFAULT;
    switch(CH(off)) {
        case _T('l'):   *p_align = ORG_ALIGN_LEFT; off++; break;
        case _T('c'):   *p_align = ORG_ALIGN_CENTER; off++; break;
        case _T('r'):   *p_align = ORG_ALIGN_RIGHT; off++; break;
    }
    while(off < end - 1  &&  ISDIGIT(off))
        off++;

    return (off == end - 1);
}

/* A row consisting only of alignment cookies (and empty cells) is not
 * exported, it only specifies the column alignment. */
static int
org_is_table_cookie_row(ORG_CTX* ctx, const ORG_LINE* row)
{
    OFF off = row->beg + 1;
    OFF cell_beg, cell_end;
    ORG_ALIGN align;
    int n_cookies = 0;

    while(org_next_table_cell(ctx, &off, row->end, &cell_beg, &cell_end)) {
        if(cell_beg == cell_end)
            continue;
        if(!org_is_table_cookie(ctx, cell_beg, cell_end, &align))
            return false;
        n_cookies++;
    }

    return (n_cookies > 0);
}

static int org_process_caption(ORG_CTX* ctx);

static int
org_process_table_row(ORG_CTX* ctx, ORG_BLOCKTYPE cell_type, const ORG_LINE* row,
                      const ORG_ALIGN* align, int col_count)
{
    ORG_BLOCK_TD_DETAIL det;
    OFF off = row->beg + 1;
    OFF cell_beg, cell_end;
    int col = 0;
    int ret = 0;

    ORG_ENTER_BLOCK(ORG_BLOCK_TR, NULL);
    while(col < col_count) {
        ORG_LINE cell;

        if(!org_next_table_cell(ctx, &off, row->end, &cell_beg, &cell_end))
            cell_beg = cell_end = row->end;

        det.align = align[col];
        cell.beg = cell_beg;
        cell.end = cell_end;

        ORG_ENTER_BLOCK(cell_type, &det);
        if(cell_beg < cell_end)
            ORG_CHECK(org_process_normal_block_contents(ctx, &cell, 1, false));
        ORG_LEAVE_BLOCK(cell_type, &det);
        col++;
    }
    ORG_LEAVE_BLOCK(ORG_BLOCK_TR, NULL);

abort:
    return ret;
}

static int
org_process_table_block_contents(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines)
{
    ORG_BLOCK_TABLE_DETAIL det;
    ORG_ALIGN* align = NULL;
    SZ first_rule = n_lines;
    SZ head_end = 0;
    int col_count = 0;
    SZ i;
    int ret = 0;

    memset(&det, 0, sizeof(ORG_BLOCK_TABLE_DETAIL));

    /* Count the columns and find the first rule. */
    for(i = 0; i < n_lines; i++) {
        if(org_is_table_rule_row(ctx, &lines[i])) {
            if(first_rule == n_lines)
                first_rule = i;
            continue;
        }

        col_count = MAX(col_count, org_count_table_cells(ctx, &lines[i]));
    }

    align = (ORG_ALIGN*) calloc(MAX(col_count, 1), sizeof(ORG_ALIGN));
    if(align == NULL) {
        ORG_LOG("calloc() failed.");
        ret = -1;
        goto abort;
    }

    /* Collect the alignment cookies; and count the header and body rows.
     * There is a header if there are some rows before the first rule and
     * some more after it. */
    for(i = 0; i < n_lines; i++) {
        if(org_is_table_rule_row(ctx, &lines[i]))
            continue;

        if(org_is_table_cookie_row(ctx, &lines[i])) {
            OFF off = lines[i].beg + 1;
            OFF cell_beg, cell_end;
            int col = 0;

            while(org_next_table_cell(ctx, &off, lines[i].end, &cell_beg, &cell_end)  &&  col < col_count) {
                ORG_ALIGN a;
                if(org_is_table_cookie(ctx, cell_beg, cell_end, &a))
                    align[col] = a;
                col++;
            }
            continue;
        }

        if(i < first_rule)
            det.head_row_count++;
        else
            det.body_row_count++;
    }
    if(det.body_row_count == 0) {
        det.body_row_count = det.head_row_count;
        det.head_row_count = 0;
    }
    head_end = (det.head_row_count > 0) ? first_rule : 0;
    det.col_count = col_count;

    if(col_count == 0)
        goto abort;

    ORG_ENTER_BLOCK(ORG_BLOCK_TABLE, &det);
    ORG_CHECK(org_process_caption(ctx));

    if(det.head_row_count > 0) {
        ORG_ENTER_BLOCK(ORG_BLOCK_THEAD, NULL);
        for(i = 0; i < head_end; i++) {
            if(org_is_table_rule_row(ctx, &lines[i])  ||  org_is_table_cookie_row(ctx, &lines[i]))
                continue;
            ORG_CHECK(org_process_table_row(ctx, ORG_BLOCK_TH, &lines[i], align, col_count));
        }
        ORG_LEAVE_BLOCK(ORG_BLOCK_THEAD, NULL);
    }

    /* Each group of rows separated by a rule forms its own body. */
    i = head_end;
    while(i < n_lines) {
        int in_body = false;

        while(i < n_lines  &&  !org_is_table_rule_row(ctx, &lines[i])) {
            if(!org_is_table_cookie_row(ctx, &lines[i])) {
                if(!in_body) {
                    ORG_ENTER_BLOCK(ORG_BLOCK_TBODY, NULL);
                    in_body = true;
                }
                ORG_CHECK(org_process_table_row(ctx, ORG_BLOCK_TD, &lines[i], align, col_count));
            }
            i++;
        }
        if(in_body)
            ORG_LEAVE_BLOCK(ORG_BLOCK_TBODY, NULL);

        /* Skip the rule(s). */
        while(i < n_lines  &&  org_is_table_rule_row(ctx, &lines[i]))
            i++;
    }

    ORG_LEAVE_BLOCK(ORG_BLOCK_TABLE, &det);

abort:
    free(align);
    return ret;
}


/**************************
 ***  Processing Block  ***
 **************************/

#define ORG_BLOCK_CONTAINER_OPENER   0x01
#define ORG_BLOCK_CONTAINER_CLOSER   0x02
#define ORG_BLOCK_CONTAINER          (ORG_BLOCK_CONTAINER_OPENER | ORG_BLOCK_CONTAINER_CLOSER)
#define ORG_BLOCK_TIGHT              0x04   /* A paragraph which is the only contents of a list item (ignoring its sub-list). */
#define ORG_BLOCK_AFFILIATED         0x08   /* An affiliated keyword. */

struct ORG_BLOCK_tag {
    ORG_BLOCKTYPE type  :  8;
    unsigned flags      :  8;

    /* ORG_BLOCK_UL, ORG_BLOCK_OL, ORG_BLOCK_DL:  Bullet (or delimiter) character.
     * ORG_BLOCK_SECTION:  Level.
     */
    unsigned data       : 16;

    /* The line which introduces the block (after the indentation), e.g. the
     * headline, the "#+BEGIN_name" line or the first line of a list item. */
    OFF beg;
    OFF end;

    /* Leaf blocks:     Count of lines following this block header.
     * Containers:      Unused.
     */
    SZ n_lines;
};

struct ORG_CONTAINER_tag {
    ORG_BLOCKTYPE type;
    unsigned data;
    OFF beg;
    OFF end;
    unsigned indent;        /* Lists and list items: indentation of the bullet. */
    unsigned level;         /* Sections: headline level. */
    OFF end_line;           /* Blocks, drawers and inline tasks: Beginning of the terminating line; zero otherwise. */

    /* Footnote definitions: Index into ctx->footnotes[]; or -1 (e.g. for a
     * duplicate definition which is ignored). */
    int footnote_index;

    /* List items: The lines up to item_scan_end do not end the item; the
     * line at item_scan_end does if item_scan_is_cut. (See
     * org_is_cut_by_item_end(); so that the lines are not scanned again for
     * another LaTeX environment in the item.) */
    OFF item_scan_end;
    int item_scan_is_cut;

    /* For detection of ORG_BLOCK_TIGHT paragraphs in list items. */
    int p_block_off;        /* Offset (in ctx->block_bytes) of the first child if it is a paragraph; -1 otherwise. */
    unsigned n_children;
    unsigned n_list_children;
};


static int
org_process_normal_block_contents(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines, int hard_breaks)
{
    int ret = 0;

    if(n_lines == 0)
        return 0;

    ORG_CHECK(org_analyze_inlines(ctx, lines, n_lines));
    ORG_CHECK(org_process_inline_range(ctx, lines, n_lines,
                lines[0].beg, lines[n_lines-1].end, 0, hard_breaks));

abort:
    return ret;
}

/* Switches of source and example blocks (e.g. "-n 10 -r -l "[%s]""), for
 * the line numbers and the code references. */
typedef struct ORG_CODE_OPTIONS_tag ORG_CODE_OPTIONS;
struct ORG_CODE_OPTIONS_tag {
    int numbering;              /* 0: none; 1: "-n"; 2: "+n" (continue the numbering of the previous block) */
    unsigned number;            /* The optional number following "-n" or "+n"; or zero. */
    int retain_labels;          /* Zero for "-r" (unless with "-n" and "-k"). */
    int use_labels;             /* Zero for "-r" or "-k" (the links show the line numbers). */
    int preserve_indent;        /* Non-zero for "-i". */
    const CHAR* label_pre;      /* Label format (split at its "%s"). */
    SZ label_pre_size;
    const CHAR* label_post;
    SZ label_post_size;
};

/* Parse the switches of a source (or example) block in [beg, end). In a
 * source block, they have to precede anything else (i.e. the header
 * arguments), so 'only_leading' stops at the first word which is not a
 * switch. */
static void
org_parse_code_switches(ORG_CTX* ctx, OFF beg, OFF end, int only_leading, ORG_CODE_OPTIONS* opts)
{
    OFF off = beg;
    int has_r = false;
    int has_k = false;

    memset(opts, 0, sizeof(ORG_CODE_OPTIONS));
    opts->label_pre = _T("(ref:");
    opts->label_pre_size = 5;
    opts->label_post = _T(")");
    opts->label_post_size = 1;

    while(off < end) {
        OFF word_beg = org_skip_blanks(ctx, off, end);
        OFF word_end = org_skip_word(ctx, word_beg, end);

        off = word_end;
        if(word_end - word_beg != 2  ||  !ISANYOF2(word_beg, _T('-'), _T('+'))) {
            if(only_leading)
                break;
            continue;
        }

        if(CH(word_beg+1) == _T('n')) {
            OFF num_beg = org_skip_blanks(ctx, word_end, end);
            OFF num_end = num_beg;

            opts->numbering = (CH(word_beg) == _T('-')) ? 1 : 2;
            while(num_end < end  &&  ISDIGIT(num_end))
                num_end++;
            if(num_end > num_beg  &&  org_is_word_end(ctx, num_end, end)) {
                unsigned n = 0;
                OFF q;
                for(q = num_beg; q < num_end  &&  n < NUMBER_MAX; q++)
                    n = n * 10 + (CH(q) - _T('0'));
                opts->number = n;
                off = num_end;
            }
        } else if(CH(word_beg) == _T('-')  &&  CH(word_beg+1) == _T('r')) {
            has_r = true;
        } else if(CH(word_beg) == _T('-')  &&  CH(word_beg+1) == _T('i')) {
            opts->preserve_indent = true;
        } else if(CH(word_beg) == _T('-')  &&  CH(word_beg+1) == _T('k')) {
            has_k = true;
        } else if(CH(word_beg) == _T('-')  &&  CH(word_beg+1) == _T('l')) {
            /* -l "format" */
            OFF fmt_beg = org_skip_blanks(ctx, word_end, end);
            OFF fmt_end;
            OFF q;

            if(fmt_beg >= end  ||  CH(fmt_beg) != _T('"')) {
                if(only_leading)
                    break;
                continue;
            }
            fmt_end = fmt_beg + 1;
            while(fmt_end < end  &&  CH(fmt_end) != _T('"'))
                fmt_end++;
            if(fmt_end >= end) {
                if(only_leading)
                    break;
                continue;
            }
            off = fmt_end + 1;

            for(q = fmt_beg + 1; q + 1 < fmt_end; q++) {
                if(CH(q) == _T('%')  &&  CH(q+1) == _T('s')) {
                    opts->label_pre = STR(fmt_beg + 1);
                    opts->label_pre_size = q - (fmt_beg + 1);
                    opts->label_post = STR(q + 2);
                    opts->label_post_size = fmt_end - (q + 2);
                    break;
                }
            }
        } else if(only_leading) {
            break;
        }
    }

    /* As in Org (see org-element-src-block-parser): "-k" keeps the labels
     * despite "-r" in numbered blocks, but the links always use the line
     * numbers with it. */
    opts->retain_labels = (!has_r  ||  (opts->numbering != 0  &&  has_k));
    opts->use_labels = (opts->retain_labels  &&  !has_k);
}

/* Find a code reference label at the end of the line [beg, end). */
static int
org_find_coderef_label(ORG_CTX* ctx, const ORG_CODE_OPTIONS* opts, OFF beg, OFF end,
                       OFF* p_label_beg, OFF* p_name_beg, OFF* p_name_end)
{
    OFF e = org_skip_blanks_backward(ctx, beg, end);
    OFF name_end, name_beg;

    if(e - beg < opts->label_pre_size + opts->label_post_size + 1)
        return false;
    if(!org_ascii_eq(STR(e - opts->label_post_size), opts->label_post, opts->label_post_size))
        return false;

    name_end = e - opts->label_post_size;
    name_beg = name_end;
    while(name_beg > beg  &&  (ISALNUM(name_beg-1)  ||  ISANYOF(name_beg-1, _T("-_ "))))
        name_beg--;
    while(name_beg < name_end  &&  CH(name_beg) == _T(' '))
        name_beg++;
    if(name_beg >= name_end  ||  name_beg - beg < opts->label_pre_size)
        return false;
    if(!org_ascii_eq(STR(name_beg - opts->label_pre_size), opts->label_pre, opts->label_pre_size))
        return false;

    *p_label_beg = org_skip_blanks_backward(ctx, beg, name_beg - opts->label_pre_size);
    *p_name_beg = name_beg;
    *p_name_end = name_end;
    return true;
}

/* Get the switches of the source (or example) block. */
static void
org_get_block_code_options(ORG_CTX* ctx, const ORG_BLOCK* block, ORG_CODE_OPTIONS* opts)
{
    OFF name_beg, name_end, params_beg;

    org_analyze_block_begin_line(ctx, block->beg, block->end, &name_beg, &name_end, &params_beg);
    if(block->type == ORG_BLOCK_SRC)
        params_beg = org_skip_blanks(ctx, org_skip_word(ctx, params_beg, block->end), block->end);
    org_parse_code_switches(ctx, params_beg, block->end, (block->type == ORG_BLOCK_SRC), opts);
}

static int
org_code_ref_cmp(const void* a, const void* b)
{
    const ORG_CODE_REF* ref_a = (const ORG_CODE_REF*) a;
    const ORG_CODE_REF* ref_b = (const ORG_CODE_REF*) b;
    int cmp;

    cmp = org_ascii_case_cmp(ref_a->name, ref_a->name_size, ref_b->name, ref_b->name_size);
    if(cmp != 0)
        return cmp;
    return (ref_a->name < ref_b->name) ? -1 : ((ref_a->name > ref_b->name) ? +1 : 0);
}

static int org_block_record_size(const ORG_BLOCK* block);

/* Walk all the source and example blocks: Compute their line numbering and
 * collect the code references (so that the links to them can be resolved,
 * even if they precede the block). */
static int
org_resolve_code_blocks(ORG_CTX* ctx)
{
    unsigned last_line = 0;
    int byte_off = 0;
    int ret = 0;

    while(byte_off < ctx->n_block_bytes) {
        const ORG_BLOCK* block = (const ORG_BLOCK*)((char*)ctx->block_bytes + byte_off);

        if(!(block->flags & ORG_BLOCK_CONTAINER)  &&
           (block->type == ORG_BLOCK_SRC  ||  block->type == ORG_BLOCK_EXAMPLE))
        {
            const ORG_VERBATIMLINE* lines = (const ORG_VERBATIMLINE*) (block + 1);
            ORG_CODE_OPTIONS opts;
            unsigned first = 0;
            SZ i;

            org_get_block_code_options(ctx, block, &opts);
            if(opts.numbering == 1)
                first = (opts.number > 0) ? opts.number : 1;
            else if(opts.numbering == 2)
                first = last_line + ((opts.number > 0) ? opts.number : 1);
            if(first > 0) {
                ORG_GROW_ARRAY(ctx->code_blocks, ctx->alloc_code_blocks, ctx->n_code_blocks);
                ctx->code_blocks[ctx->n_code_blocks].byte_off = byte_off;
                ctx->code_blocks[ctx->n_code_blocks].first_line = first;
                ctx->n_code_blocks++;
                last_line = first + block->n_lines - 1;
            }

            for(i = 0; i < block->n_lines; i++) {
                OFF label_beg, name_beg, name_end;

                if(org_find_coderef_label(ctx, &opts, lines[i].beg, lines[i].end, &label_beg, &name_beg, &name_end)) {
                    ORG_CODE_REF* ref;

                    ORG_GROW_ARRAY(ctx->code_refs, ctx->alloc_code_refs, ctx->n_code_refs);
                    ref = &ctx->code_refs[ctx->n_code_refs++];
                    ref->name = STR(name_beg);
                    ref->name_size = name_end - name_beg;
                    ref->line_number = (first > 0) ? first + (unsigned) i : (unsigned) i + 1;
                    ref->use_label = opts.use_labels;
                }
            }
        }

        byte_off += org_block_record_size(block);
    }

    if(ctx->n_code_refs > 1)
        qsort(ctx->code_refs, ctx->n_code_refs, sizeof(ORG_CODE_REF), org_code_ref_cmp);

abort:
    return ret;
}

/* Number of the first line of the source block; or zero if not numbered. */
static unsigned
org_code_block_first_line(ORG_CTX* ctx, const ORG_BLOCK* block)
{
    int byte_off = (int) ((const char*) block - (const char*) ctx->block_bytes);
    int lo = 0;
    int hi = ctx->n_code_blocks;

    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        if(ctx->code_blocks[pivot].byte_off < byte_off)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    if(lo < ctx->n_code_blocks  &&  ctx->code_blocks[lo].byte_off == byte_off)
        return ctx->code_blocks[lo].first_line;
    return 0;
}

static const ORG_CODE_REF*
org_lookup_code_ref(ORG_CTX* ctx, const CHAR* name, SZ name_size)
{
    int lo = 0;
    int hi = ctx->n_code_refs;

    while(lo < hi) {
        int pivot = lo + (hi - lo) / 2;
        const ORG_CODE_REF* ref = &ctx->code_refs[pivot];

        if(org_ascii_case_cmp(ref->name, ref->name_size, name, name_size) < 0)
            lo = pivot + 1;
        else
            hi = pivot;
    }

    if(lo < ctx->n_code_refs  &&  ctx->code_refs[lo].name_size == name_size  &&
       org_ascii_case_eq(ctx->code_refs[lo].name, name, name_size))
        return &ctx->code_refs[lo];
    return NULL;
}


static int
org_process_verbatim_block_contents(ORG_CTX* ctx, ORG_TEXTTYPE text_type, const ORG_VERBATIMLINE* lines,
                                    SZ n_lines, int strip_indent, int unescape,
                                    const ORG_CODE_OPTIONS* code_opts, unsigned first_line)
{
    unsigned min_indent = 0;
    SZ i;
    int ret = 0;

    /* Org removes the common indentation of all the (non-blank) lines
     * (unless "-i" is used). (If all of them are blank, they become empty.) */
    if(strip_indent  &&  !(code_opts != NULL  &&  code_opts->preserve_indent)) {
        min_indent = UINT_MAX;
        for(i = 0; i < n_lines; i++) {
            if(lines[i].beg < lines[i].end)
                min_indent = MIN(min_indent, org_indent_width(ctx, lines[i].indent_beg, lines[i].beg));
        }
    }

    for(i = 0; i < n_lines; i++) {
        const ORG_VERBATIMLINE* line = &lines[i];
        OFF beg = line->beg;
        OFF end = line->end;
        OFF indent_beg = line->indent_beg;
        unsigned col;
        OFF off;
        OFF label_beg, name_beg, name_end;
        int has_coderef = false;
        ORG_SPAN_CODEREF_DETAIL coderef_det;
        ORG_ATTRIBUTE_BUILD coderef_build;

        /* Code reference label: The line is reported within a span, without
         * the label. */
        if(code_opts != NULL  &&
           org_find_coderef_label(ctx, code_opts, line->beg, line->end, &label_beg, &name_beg, &name_end))
        {
            has_coderef = true;
            end = label_beg;
            memset(&coderef_det, 0, sizeof(coderef_det));
            org_build_attribute(ctx, STR(name_beg), name_end - name_beg, &coderef_det.name, &coderef_build);
            coderef_det.line_number = (first_line > 0) ? first_line + (unsigned) i : 0;
            coderef_det.is_label_retained = code_opts->retain_labels;
            ORG_ENTER_SPAN(ORG_SPAN_CODEREF, &coderef_det);
        }

        /* Output the indentation, except its first min_indent columns. As
         * Org does (see org-do-remove-indentation), the rest of it is kept
         * verbatim; a tab which spans the column min_indent is kept whole. */
        col = 0;
        while(indent_beg < beg) {
            unsigned next_col = ORG_NEXT_INDENT_COL(col, CH(indent_beg));

            if(next_col > min_indent)
                break;
            col = next_col;
            indent_beg++;
        }
        ORG_TEXT(text_type, STR(indent_beg), beg - indent_beg);

        /* Org escapes lines starting with '*' or "#+" by prepending a comma.
         * (Already escaped lines get just another comma.) So remove one. */
        off = beg;
        while(unescape  &&  off < end  &&  CH(off) == _T(','))
            off++;
        if(off > beg  &&  off < end  &&
           (CH(off) == _T('*')  ||  (CH(off) == _T('#')  &&  off+1 < end  &&  CH(off+1) == _T('+'))))
            beg++;

        /* Output the code line itself. */
        ORG_TEXT_INSECURE(text_type, STR(beg), end - beg);

        if(has_coderef)
            ORG_LEAVE_SPAN(ORG_SPAN_CODEREF, &coderef_det);

        /* Enforce end-of-line. */
        ORG_TEXT(text_type, _T("\n"), 1);
    }

abort:
    return ret;
}

/* Report the caption of the element (if any). It is called just after
 * entering the element, so the caption is its first child. */
static int
org_process_caption(ORG_CTX* ctx)
{
    ORG_LINE line;
    int ret = 0;

    if(ctx->caption_end <= ctx->caption_beg)
        return 0;

    line.beg = ctx->caption_beg;
    line.end = ctx->caption_end;
    ctx->caption_beg = ctx->caption_end = 0;

    ORG_ENTER_BLOCK(ORG_BLOCK_CAPTION, NULL);
    ORG_CHECK(org_process_normal_block_contents(ctx, &line, 1, false));
    ORG_LEAVE_BLOCK(ORG_BLOCK_CAPTION, NULL);

abort:
    return ret;
}

/* Check whether the paragraph consists only of a link without any
 * description. */
static int
org_is_standalone_link(ORG_CTX* ctx, const ORG_LINE* lines, SZ n_lines)
{
    ORG_MARK mark;
    OFF end;

    if(n_lines != 1  ||  lines[0].end - lines[0].beg < 4)
        return false;

    memset(&mark, 0, sizeof(mark));
    ctx->link_close_horizon = 0;
    if(org_is_prefix(ctx, lines[0].beg, lines[0].end, _T("[["))) {
        return (org_is_bracket_link(ctx, lines[0].beg, lines[0].end, &mark)  &&
                mark.end == lines[0].end  &&  mark.desc_end == mark.desc_beg);
    }

    return (!(ctx->parser.flags & ORG_FLAG_NOPLAINLINKS)  &&
            org_is_plain_link(ctx, lines[0].beg, lines[0].end, &end)  &&  end == lines[0].end);
}

/* Org keeps the indentation of verse lines relative to the least indented
 * (non-blank) line. The lines have been stored without any indentation, so
 * (re-)include the relative part of it. */
static void
org_adjust_verse_indentation(ORG_CTX* ctx, ORG_LINE* lines, SZ n_lines)
{
    OFF min_indent = (OFF) -1;
    SZ i;

    for(i = 0; i < n_lines; i++) {
        OFF off = lines[i].beg;

        if(lines[i].beg >= lines[i].end)
            continue;
        while(off > 0  &&  ISBLANK(off-1))
            off--;
        min_indent = MIN(min_indent, lines[i].beg - off);
    }

    for(i = 0; i < n_lines; i++) {
        OFF off = lines[i].beg;

        if(lines[i].beg >= lines[i].end)
            continue;
        while(off > 0  &&  ISBLANK(off-1))
            off--;
        lines[i].beg = off + min_indent;
    }
}

static int
org_process_leaf_block(ORG_CTX* ctx, const ORG_BLOCK* block)
{
    union {
        ORG_BLOCK_HEADLINE_DETAIL headline;
        ORG_BLOCK_SRC_DETAIL src;
        ORG_BLOCK_EXPORT_DETAIL export_;
        ORG_BLOCK_KEYWORD_DETAIL keyword;
        ORG_BLOCK_LATEX_ENVIRONMENT_DETAIL latex_env;
        ORG_BLOCK_P_DETAIL p;
        ORG_BLOCK_EXAMPLE_DETAIL example;
    } det;
    ORG_CODE_OPTIONS code_opts;
    int has_code_opts = false;
    unsigned first_line = 0;
    ORG_ATTRIBUTE_BUILD attr_build[2];
    void* detail = NULL;
    const void* lines = (const void*) (block + 1);
    ORG_TEXTTYPE text_type = ORG_TEXT_NORMAL;
    int strip_indent = false;
    int unescape = false;
    int ret = 0;

    memset(&det, 0, sizeof(det));

    switch(block->type) {
        case ORG_BLOCK_HEADLINE:
        {
            ORG_HEADLINE_INFO info;
            ORG_LINE title;

            org_analyze_headline(ctx, block->beg, block->end, &info);
            det.headline.level = info.level;
            org_build_attribute(ctx, STR(info.todo_beg), info.todo_end - info.todo_beg, &det.headline.todo, &attr_build[0]);
            det.headline.is_done = info.is_done;
            det.headline.priority = info.priority;
            det.headline.is_commented = info.is_commented;
            org_build_attribute(ctx, STR(info.tags_beg), info.tags_end - info.tags_beg, &det.headline.tags, &attr_build[1]);

            title.beg = info.title_beg;
            title.end = info.title_end;

            ORG_ENTER_BLOCK(ORG_BLOCK_HEADLINE, &det.headline);
            if(title.beg < title.end)
                ORG_CHECK(org_process_normal_block_contents(ctx, &title, 1, false));
            ORG_LEAVE_BLOCK(ORG_BLOCK_HEADLINE, &det.headline);
            goto abort;
        }

        case ORG_BLOCK_P:
            det.p.is_standalone_link = org_is_standalone_link(ctx, (const ORG_LINE*) lines, block->n_lines);
            if(!(block->flags & ORG_BLOCK_TIGHT)) {
                ORG_ENTER_BLOCK(ORG_BLOCK_P, &det.p);
                ORG_CHECK(org_process_caption(ctx));
            }
            ORG_CHECK(org_process_normal_block_contents(ctx, (const ORG_LINE*) lines, block->n_lines, false));
            if(!(block->flags & ORG_BLOCK_TIGHT))
                ORG_LEAVE_BLOCK(ORG_BLOCK_P, &det.p);
            goto abort;

        case ORG_BLOCK_VERSE:
            org_adjust_verse_indentation(ctx, (ORG_LINE*) lines, block->n_lines);
            ORG_ENTER_BLOCK(ORG_BLOCK_VERSE, NULL);
            ORG_CHECK(org_process_caption(ctx));
            ORG_CHECK(org_process_normal_block_contents(ctx, (const ORG_LINE*) lines, block->n_lines, true));
            ORG_LEAVE_BLOCK(ORG_BLOCK_VERSE, NULL);
            goto abort;

        case ORG_BLOCK_TABLE:
            ret = org_process_table_block_contents(ctx, (const ORG_LINE*) lines, block->n_lines);
            goto abort;

        case ORG_BLOCK_HR:
            ORG_ENTER_BLOCK(ORG_BLOCK_HR, NULL);
            ORG_LEAVE_BLOCK(ORG_BLOCK_HR, NULL);
            goto abort;

        case ORG_BLOCK_KEYWORD:
        {
            OFF key_end, value_beg;

            org_is_keyword_line(ctx, block->beg, block->end, &key_end, &value_beg);
            org_build_attribute(ctx, STR(block->beg + 2), key_end - (block->beg + 2), &det.keyword.key, &attr_build[0]);
            org_build_attribute(ctx, STR(value_beg), block->end - value_beg, &det.keyword.value, &attr_build[1]);
            det.keyword.is_affiliated = ((block->flags & ORG_BLOCK_AFFILIATED) != 0);
            ORG_ENTER_BLOCK(ORG_BLOCK_KEYWORD, &det.keyword);
            ORG_LEAVE_BLOCK(ORG_BLOCK_KEYWORD, &det.keyword);
            goto abort;
        }

        case ORG_BLOCK_SRC:
        {
            OFF name_beg, name_end, params_beg;
            OFF lang_end;

            org_analyze_block_begin_line(ctx, block->beg, block->end, &name_beg, &name_end, &params_beg);
            lang_end = org_skip_word(ctx, params_beg, block->end);
            org_build_attribute(ctx, STR(params_beg), lang_end - params_beg, &det.src.lang, &attr_build[0]);
            params_beg = org_skip_blanks(ctx, lang_end, block->end);
            org_build_attribute(ctx, STR(params_beg), block->end - params_beg, &det.src.params, &attr_build[1]);
            org_get_block_code_options(ctx, block, &code_opts);
            has_code_opts = true;
            first_line = org_code_block_first_line(ctx, block);
            det.src.first_line_number = first_line;
            det.src.line_count = block->n_lines;
            detail = &det.src;
            text_type = ORG_TEXT_CODE;
            strip_indent = true;
            unescape = true;
            break;
        }

        case ORG_BLOCK_EXPORT:
        {
            OFF name_beg, name_end, params_beg;

            if(ctx->parser.flags & ORG_FLAG_NOEXPORTBLOCKS)
                return 0;

            org_analyze_block_begin_line(ctx, block->beg, block->end, &name_beg, &name_end, &params_beg);
            org_build_attribute(ctx, STR(params_beg),
                        org_skip_word(ctx, params_beg, block->end) - params_beg, &det.export_.backend, &attr_build[0]);
            detail = &det.export_;
            text_type = ORG_TEXT_EXPORT;
            strip_indent = true;
            unescape = true;
            break;
        }

        case ORG_BLOCK_EXAMPLE:
        {
            OFF name_beg, name_end, params_beg;

            org_analyze_block_begin_line(ctx, block->beg, block->end, &name_beg, &name_end, &params_beg);
            org_build_attribute(ctx, STR(params_beg), block->end - params_beg, &det.example.switches, &attr_build[0]);
            org_get_block_code_options(ctx, block, &code_opts);
            has_code_opts = true;
            first_line = org_code_block_first_line(ctx, block);
            det.example.first_line_number = first_line;
            det.example.line_count = block->n_lines;
            detail = &det.example;
            text_type = ORG_TEXT_CODE;
            strip_indent = true;
            unescape = true;
            break;
        }

        case ORG_BLOCK_LATEX_ENVIRONMENT:
        {
            OFF name_beg = block->beg + 7;      /* strlen("\\begin{") */
            OFF name_end = name_beg;

            while(name_end < block->end  &&  CH(name_end) != _T('}'))
                name_end++;
            org_build_attribute(ctx, STR(name_beg), name_end - name_beg, &det.latex_env.name, &attr_build[0]);
            detail = &det.latex_env;
            text_type = ORG_TEXT_LATEX;
            strip_indent = true;
            break;
        }

        case ORG_BLOCK_FIXED_WIDTH:
            text_type = ORG_TEXT_CODE;
            strip_indent = true;
            break;

        case ORG_BLOCK_COMMENT:
            unescape = true;
            break;

        case ORG_BLOCK_PROPERTY_DRAWER:
        case ORG_BLOCK_PLANNING:
            break;

        default:
            ORG_UNREACHABLE();
            break;
    }

    /* All the remaining ones are the verbatim-like blocks. */
    ORG_ENTER_BLOCK(block->type, detail);
    ORG_CHECK(org_process_caption(ctx));
    ORG_CHECK(org_process_verbatim_block_contents(ctx, text_type,
                (const ORG_VERBATIMLINE*) lines, block->n_lines, strip_indent, unescape,
                (has_code_opts ? &code_opts : NULL), first_line));
    ORG_LEAVE_BLOCK(block->type, detail);

abort:
    return ret;
}

/* Size of the block record, including its lines. */
static int
org_block_record_size(const ORG_BLOCK* block)
{
    if(block->flags & ORG_BLOCK_CONTAINER)
        return sizeof(ORG_BLOCK);
    if(org_block_has_verbatim_lines(block->type))
        return sizeof(ORG_BLOCK) + block->n_lines * sizeof(ORG_VERBATIMLINE);
    return sizeof(ORG_BLOCK) + block->n_lines * sizeof(ORG_LINE);
}

/* Check whether the tag (e.g. "noexport") is among the colon-separated tags
 * [beg, end). */
static int
org_has_tag(ORG_CTX* ctx, OFF beg, OFF end, const CHAR* tag)
{
    SZ tag_size = (SZ) org_strlen(tag);

    while(beg < end) {
        OFF tag_end = beg;

        while(tag_end < end  &&  CH(tag_end) != _T(':'))
            tag_end++;
        if(tag_end - beg == tag_size  &&  org_ascii_eq(STR(beg), tag, tag_size))
            return true;
        beg = tag_end + 1;
    }

    return false;
}

/* Find the record closing the section opened by the record at byte_off. */
static int
org_find_section_closer(ORG_CTX* ctx, int byte_off, int byte_end)
{
    int depth = 0;

    while(byte_off < byte_end) {
        ORG_BLOCK* block = (ORG_BLOCK*)((char*)ctx->block_bytes + byte_off);

        if(block->type == ORG_BLOCK_SECTION  &&  (block->flags & ORG_BLOCK_CONTAINER_OPENER))
            depth++;
        else if(block->type == ORG_BLOCK_SECTION  &&  (block->flags & ORG_BLOCK_CONTAINER_CLOSER))
            depth--;
        if(depth == 0)
            break;
        byte_off += org_block_record_size(block);
    }

    return byte_off;
}

/* Process the block records in ctx->block_bytes within [byte_beg, byte_end). */
static int
org_process_blocks(ORG_CTX* ctx, int byte_beg, int byte_end)
{
    int byte_off = byte_beg;
    int ret = 0;

    while(byte_off < byte_end) {
        ORG_BLOCK* block = (ORG_BLOCK*)((char*)ctx->block_bytes + byte_off);
        union {
            ORG_BLOCK_SECTION_DETAIL section;
            ORG_BLOCK_LIST_DETAIL list;
            ORG_BLOCK_LI_DETAIL li;
            ORG_BLOCK_SPECIAL_DETAIL special;
            ORG_BLOCK_DRAWER_DETAIL drawer;
            ORG_BLOCK_DYNAMIC_DETAIL dynamic;
            ORG_BLOCK_HEADLINE_DETAIL inlinetask;
        } det;
        ORG_ATTRIBUTE_BUILD attr_build[2];
        void* detail = NULL;
        ORG_ITEM_INFO item_info;
        int skip_contents = false;

        memset(&det, 0, sizeof(det));

        /* An affiliated caption belongs to the next element. */
        if(block->type == ORG_BLOCK_KEYWORD) {
            if(block->flags & ORG_BLOCK_AFFILIATED) {
                OFF key_end, value_beg;
                OFF q = block->beg + 2;

                org_is_keyword_line(ctx, block->beg, block->end, &key_end, &value_beg);
                while(q < key_end  &&  CH(q) != _T('['))
                    q++;
                if(q - (block->beg + 2) == 7  &&  org_ascii_case_eq(STR(block->beg + 2), _T("CAPTION"), 7)) {
                    ctx->next_caption_beg = value_beg;
                    ctx->next_caption_end = block->end;
                }
            }
        } else if(!(block->flags & ORG_BLOCK_CONTAINER_CLOSER)) {
            ctx->caption_beg = ctx->next_caption_beg;
            ctx->caption_end = ctx->next_caption_end;
            ctx->next_caption_beg = ctx->next_caption_end = 0;
        }

        if(!(block->flags & ORG_BLOCK_CONTAINER)) {
            ORG_CHECK(org_process_leaf_block(ctx, block));
            byte_off += org_block_record_size(block);
            continue;
        }

        /* Footnote definitions are reported at the end of the document (see
         * org_process_footnote_defs()), so skip it (including its contents). */
        if(block->type == ORG_BLOCK_FOOTNOTE_DEF) {
            do {
                block = (ORG_BLOCK*)((char*)ctx->block_bytes + byte_off);
                byte_off += org_block_record_size(block);
            } while(byte_off < byte_end  &&
                    !(block->type == ORG_BLOCK_FOOTNOTE_DEF  &&  (block->flags & ORG_BLOCK_CONTAINER_CLOSER)));
            continue;
        }

        switch(block->type) {
            case ORG_BLOCK_SECTION:
            {
                ORG_HEADLINE_INFO info;

                int is_archived;

                org_analyze_headline(ctx, block->beg, block->end, &info);
                is_archived = org_has_tag(ctx, info.tags_beg, info.tags_end, _T("ARCHIVE"));

                /* Skip the whole subtree if requested. */
                if((block->flags & ORG_BLOCK_CONTAINER_OPENER)  &&
                   (((ctx->parser.flags & ORG_FLAG_SKIPCOMMENTED)  &&  info.is_commented)  ||
                    ((ctx->parser.flags & ORG_FLAG_SKIPNOEXPORT)  &&
                     org_has_tag(ctx, info.tags_beg, info.tags_end, _T("noexport")))  ||
                    ((ctx->parser.flags & ORG_FLAG_SKIPARCHIVED)  &&  is_archived)))
                {
                    byte_off = org_find_section_closer(ctx, byte_off, byte_end);
                    if(byte_off < byte_end)
                        byte_off += org_block_record_size((ORG_BLOCK*)((char*)ctx->block_bytes + byte_off));
                    continue;
                }

                /* Or all of it but the headline. */
                if((block->flags & ORG_BLOCK_CONTAINER_OPENER)  &&
                   (ctx->parser.flags & ORG_FLAG_SKIPARCHIVEDCONTENTS)  &&  is_archived)
                    skip_contents = true;

                det.section.level = info.level;
                det.section.is_commented = info.is_commented;
                org_build_attribute(ctx, STR(info.tags_beg), info.tags_end - info.tags_beg, &det.section.tags, &attr_build[0]);
                det.section.is_footnote_section = (info.title_end - info.title_beg == 9  &&
                            org_ascii_eq(STR(info.title_beg), _T("Footnotes"), 9));
                detail = &det.section;
                break;
            }

            case ORG_BLOCK_INLINETASK:
            {
                ORG_HEADLINE_INFO info;

                org_analyze_headline(ctx, block->beg, block->end, &info);
                det.inlinetask.level = info.level;
                org_build_attribute(ctx, STR(info.todo_beg), info.todo_end - info.todo_beg, &det.inlinetask.todo, &attr_build[0]);
                det.inlinetask.is_done = info.is_done;
                det.inlinetask.priority = info.priority;
                det.inlinetask.is_commented = info.is_commented;
                org_build_attribute(ctx, STR(info.tags_beg), info.tags_end - info.tags_beg, &det.inlinetask.tags, &attr_build[1]);
                detail = &det.inlinetask;
                break;
            }

            case ORG_BLOCK_UL:
            case ORG_BLOCK_OL:
            case ORG_BLOCK_DL:
                det.list.mark = (CHAR) block->data;
                detail = &det.list;
                break;

            case ORG_BLOCK_LI:
            case ORG_BLOCK_DD:
                org_is_list_item(ctx, block->beg, block->end, 1, &item_info);
                det.li.checkbox = item_info.checkbox;
                det.li.counter = item_info.counter;
                detail = &det.li;
                break;

            case ORG_BLOCK_SPECIAL:
            {
                OFF name_beg, name_end, params_beg;

                org_analyze_block_begin_line(ctx, block->beg, block->end, &name_beg, &name_end, &params_beg);
                org_build_attribute(ctx, STR(name_beg), name_end - name_beg, &det.special.name, &attr_build[0]);
                org_build_attribute(ctx, STR(params_beg), block->end - params_beg, &det.special.params, &attr_build[1]);
                detail = &det.special;
                break;
            }

            case ORG_BLOCK_DRAWER:
                org_build_attribute(ctx, STR(block->beg + 1), block->end - block->beg - 2, &det.drawer.name, &attr_build[0]);
                detail = &det.drawer;
                break;

            case ORG_BLOCK_DYNAMIC:
            {
                OFF name_beg = org_skip_blanks(ctx, block->beg + 8, block->end);
                OFF name_end = org_skip_word(ctx, name_beg, block->end);
                OFF params_beg = org_skip_blanks(ctx, name_end, block->end);

                org_build_attribute(ctx, STR(name_beg), name_end - name_beg, &det.dynamic.name, &attr_build[0]);
                org_build_attribute(ctx, STR(params_beg), block->end - params_beg, &det.dynamic.params, &attr_build[1]);
                detail = &det.dynamic;
                break;
            }

            default:
                break;
        }

        if(block->flags & ORG_BLOCK_CONTAINER_CLOSER)
            ORG_LEAVE_BLOCK(block->type, detail);

        if(block->flags & ORG_BLOCK_CONTAINER_OPENER) {
            if(block->type == ORG_BLOCK_DD) {
                ORG_LINE tag;

                /* Description list item: Report its tag as ORG_BLOCK_DT. */
                tag.beg = item_info.tag_beg;
                tag.end = item_info.tag_end;
                ORG_ENTER_BLOCK(ORG_BLOCK_DT, detail);
                if(tag.beg < tag.end)
                    ORG_CHECK(org_process_normal_block_contents(ctx, &tag, 1, false));
                ORG_LEAVE_BLOCK(ORG_BLOCK_DT, detail);
            }

            ORG_ENTER_BLOCK(block->type, detail);
            ORG_CHECK(org_process_caption(ctx));
        }

        if(skip_contents) {
            /* The headline is the first record of the section. */
            int closer_off = org_find_section_closer(ctx, byte_off, byte_end);

            byte_off += sizeof(ORG_BLOCK);
            block = (ORG_BLOCK*)((char*)ctx->block_bytes + byte_off);
            ORG_ASSERT(block->type == ORG_BLOCK_HEADLINE);
            ORG_CHECK(org_process_leaf_block(ctx, block));
            byte_off = closer_off;
            continue;
        }

        byte_off += sizeof(ORG_BLOCK);
    }

abort:
    return ret;
}


/************************************
 ***  Grouping Lines into Blocks  ***
 ************************************/

static void*
org_push_block_bytes(ORG_CTX* ctx, int n_bytes)
{
    void* ptr;

    if(ctx->n_block_bytes + n_bytes > ctx->alloc_block_bytes) {
        void* new_block_bytes;
        int new_alloc = (ctx->alloc_block_bytes > 0
                ? ctx->alloc_block_bytes + ctx->alloc_block_bytes / 2
                : 512);

        if(new_alloc < ctx->n_block_bytes + n_bytes)
            new_alloc = ctx->n_block_bytes + n_bytes;
        new_block_bytes = realloc(ctx->block_bytes, new_alloc);
        if(new_block_bytes == NULL) {
            ORG_LOG("realloc() failed.");
            return NULL;
        }

        /* Fix the ->current_block after the reallocation. */
        if(ctx->current_block != NULL) {
            OFF off_current_block = (OFF) ((char*) ctx->current_block - (char*) ctx->block_bytes);
            ctx->current_block = (ORG_BLOCK*) ((char*) new_block_bytes + off_current_block);
        }

        ctx->block_bytes = new_block_bytes;
        ctx->alloc_block_bytes = new_alloc;
    }

    ptr = (char*)ctx->block_bytes + ctx->n_block_bytes;
    ctx->n_block_bytes += n_bytes;
    return ptr;
}

/* Remember the new block (or container) as a child of the current container
 * (which is needed to detect ORG_BLOCK_TIGHT paragraphs). */
static void
org_note_child(ORG_CTX* ctx, ORG_BLOCKTYPE type)
{
    ORG_CONTAINER* container;

    if(ctx->n_containers == 0)
        return;

    container = &ctx->containers[ctx->n_containers - 1];
    if(container->n_children == 0  &&  type == ORG_BLOCK_P)
        container->p_block_off = ctx->n_block_bytes;
    container->n_children++;
    if(type == ORG_BLOCK_UL  ||  type == ORG_BLOCK_OL  ||  type == ORG_BLOCK_DL)
        container->n_list_children++;
}

static void
org_end_current_block(ORG_CTX* ctx)
{
    ctx->current_block = NULL;
}

static int
org_start_new_block(ORG_CTX* ctx, ORG_BLOCKTYPE type, OFF beg, OFF end)
{
    ORG_BLOCK* block;

    org_end_current_block(ctx);
    org_note_child(ctx, type);

    block = (ORG_BLOCK*) org_push_block_bytes(ctx, sizeof(ORG_BLOCK));
    if(block == NULL)
        return -1;

    block->type = type;
    block->flags = 0;
    block->data = 0;
    block->beg = beg;
    block->end = end;
    block->n_lines = 0;

    ctx->current_block = block;
    return 0;
}

static int
org_add_line_into_current_block(ORG_CTX* ctx, OFF beg, OFF end, OFF indent_beg)
{
    ORG_ASSERT(ctx->current_block != NULL);

    if(org_block_has_verbatim_lines(ctx->current_block->type)) {
        ORG_VERBATIMLINE* line;

        line = (ORG_VERBATIMLINE*) org_push_block_bytes(ctx, sizeof(ORG_VERBATIMLINE));
        if(line == NULL)
            return -1;

        line->beg = beg;
        line->end = end;
        line->indent_beg = indent_beg;
    } else {
        ORG_LINE* line;

        line = (ORG_LINE*) org_push_block_bytes(ctx, sizeof(ORG_LINE));
        if(line == NULL)
            return -1;

        line->beg = beg;
        line->end = org_skip_blanks_backward(ctx, beg, end);
    }

    ctx->current_block->n_lines++;
    return 0;
}

static int
org_push_container(ORG_CTX* ctx, ORG_BLOCKTYPE type, unsigned data, const ORG_LINE_ANALYSIS* line)
{
    ORG_CONTAINER* container;
    ORG_BLOCK* block;
    int ret = 0;

    org_end_current_block(ctx);
    org_note_child(ctx, type);

    ORG_GROW_ARRAY(ctx->containers, ctx->alloc_containers, ctx->n_containers);
    container = &ctx->containers[ctx->n_containers++];
    container->type = type;
    container->data = data;
    container->beg = line->beg;
    container->end = line->end;
    container->indent = line->indent;
    container->level = (type == ORG_BLOCK_SECTION) ? data : 0;
    container->end_line = line->end_line;
    container->footnote_index = -1;
    container->p_block_off = -1;
    container->item_scan_end = 0;
    container->item_scan_is_cut = false;
    container->n_children = 0;
    container->n_list_children = 0;

    /* Register the footnote definition (the first definition wins). */
    if(type == ORG_BLOCK_FOOTNOTE_DEF) {
        OFF label_beg = line->beg + 4;
        OFF label_end = label_beg;
        int index;

        while(label_end < line->end  &&  CH(label_end) != _T(']'))
            label_end++;
        ORG_CHECK(org_get_footnote(ctx, STR(label_beg), label_end - label_beg, &index));
        if(ctx->footnotes[index].def_open_off < 0  &&  !ctx->footnotes[index].has_inline_def) {
            ctx->footnotes[index].def_open_off = ctx->n_block_bytes;
            container->footnote_index = index;
        }
    }

    block = (ORG_BLOCK*) org_push_block_bytes(ctx, sizeof(ORG_BLOCK));
    if(block == NULL) {
        ret = -1;
        goto abort;
    }
    block->type = type;
    block->flags = ORG_BLOCK_CONTAINER_OPENER;
    block->data = data;
    block->beg = line->beg;
    block->end = line->end;
    block->n_lines = 0;

abort:
    return ret;
}

/* Close all the containers above the given count. */
static int
org_leave_child_containers(ORG_CTX* ctx, int n_keep)
{
    org_end_current_block(ctx);

    while(ctx->n_containers > n_keep) {
        ORG_CONTAINER* container = &ctx->containers[ctx->n_containers - 1];
        ORG_BLOCK* block;

        /* Org does not wrap the first paragraph of a list item into <p> if it
         * is alone in the item, or if it is followed only by a sub-list. */
        if((container->type == ORG_BLOCK_LI  ||  container->type == ORG_BLOCK_DD)  &&
           container->p_block_off >= 0  &&
           (container->n_children == 1  ||  (container->n_children == 2  &&  container->n_list_children == 1)))
        {
            ORG_BLOCK* p_block = (ORG_BLOCK*) ((char*) ctx->block_bytes + container->p_block_off);
            p_block->flags |= ORG_BLOCK_TIGHT;
        }

        if(container->type == ORG_BLOCK_FOOTNOTE_DEF  &&  container->footnote_index >= 0)
            ctx->footnotes[container->footnote_index].def_close_off = ctx->n_block_bytes;

        block = (ORG_BLOCK*) org_push_block_bytes(ctx, sizeof(ORG_BLOCK));
        if(block == NULL)
            return -1;
        block->type = container->type;
        block->flags = ORG_BLOCK_CONTAINER_CLOSER;
        block->data = container->data;
        block->beg = container->beg;
        block->end = container->end;
        block->n_lines = 0;

        ctx->n_containers--;
    }

    return 0;
}


/***********************
 ***  Line Analysis  ***
 ***********************/

static OFF org_block_limit(ORG_CTX* ctx, OFF line_beg);

/* Analyze the line at 'off' for org_is_cut_by_item_end(). As Org does (see
 * org-list-struct), a block or a drawer (terminated before 'limit') is
 * skipped as a whole. */
static void
org_scan_line(ORG_CTX* ctx, OFF off, OFF limit, ORG_SCAN_LINE* scan_line)
{
    OFF line_end = org_line_end(ctx, off);
    OFF p = org_skip_blanks(ctx, off, line_end);
    OFF name_beg, name_end, params_beg;
    OFF skip_to = 0;

    scan_line->beg = off;
    if(p >= line_end) {
        scan_line->indent = UINT_MAX;
    } else {
        scan_line->indent = org_indent_width(ctx, off, p);

        if(org_is_prefix_i(ctx, p, line_end, _T("#+BEGIN_"))) {
            org_analyze_block_begin_line(ctx, p, line_end, &name_beg, &name_end, &params_beg);
            skip_to = org_find_block_end(ctx->block_ends, ctx->n_block_ends, off,
                        STR(name_beg), name_end - name_beg, limit);
        } else if(org_is_drawer_begin_line(ctx, p, line_end, &name_beg, &name_end)) {
            skip_to = org_find_end_line(ctx->drawer_ends, ctx->n_drawer_ends, off, limit);
        }
        if(skip_to != 0)
            line_end = org_line_end(ctx, skip_to);
    }

    scan_line->next = (line_end < ctx->size) ? org_skip_newline(ctx, line_end) : ctx->size;
}

/* Check whether the list item, in which a LaTeX environment would begin on
 * the line 'line', ends before the line 'end_line' (which terminates the
 * environment) does.
 *
 * Unlike the blocks and drawers, Org (see org-list-struct) does not skip
 * the LaTeX environments when looking for the end of an item, so any line
 * not indented more than the bullet, or two blank lines, end it (and then
 * there is no environment). */
static int
org_is_cut_by_item_end(ORG_CTX* ctx, const ORG_LINE_ANALYSIS* line, OFF next_line_beg, OFF end_line)
{
    ORG_CONTAINER* item;
    OFF limit;
    OFF off = next_line_beg;
    int n_blank_lines = 0;
    int i, k;

    /* Find the item the environment would begin in (if the line ends some
     * items, see org_leave_lists_by_indent()). */
    for(i = ctx->n_containers - 1; i >= 0; i--) {
        const ORG_CONTAINER* container = &ctx->containers[i];

        if(container->type == ORG_BLOCK_LI  ||  container->type == ORG_BLOCK_DD) {
            if(line->indent > container->indent)
                break;
        } else if(container->type != ORG_BLOCK_UL  &&  container->type != ORG_BLOCK_OL  &&
                  container->type != ORG_BLOCK_DL)
        {
            return false;
        }
    }
    if(i < 0)
        return false;
    item = &ctx->containers[i];

    /* All the lines of the item before this one have not ended it (or it
     * would not be here), so continue the last scan (if it is still ahead). */
    if(off < item->item_scan_end  ||  (off == item->item_scan_end  &&  item->item_scan_is_cut)) {
        if(item->item_scan_is_cut  ||  item->item_scan_end > end_line)
            return (item->item_scan_is_cut  &&  item->item_scan_end <= end_line);
        off = item->item_scan_end;
    }

    /* Find 'off' among the lines scanned before (or start them over). */
    limit = org_block_limit(ctx, off);
    k = -1;
    if(ctx->n_scan_lines > 0  &&  limit == ctx->scan_limit) {
        int lo = 0;
        int hi = ctx->n_scan_lines;

        while(lo < hi) {
            int pivot = lo + (hi - lo) / 2;
            if(ctx->scan_lines[pivot].beg < off)
                lo = pivot + 1;
            else
                hi = pivot;
        }
        if((lo < ctx->n_scan_lines  &&  ctx->scan_lines[lo].beg == off)  ||
           (lo == ctx->n_scan_lines  &&  ctx->scan_lines[lo-1].next == off))
            k = lo;
    }
    if(k < 0) {
        k = 0;
        ctx->n_scan_lines = 0;
        ctx->scan_limit = limit;
    }

    while(off <= end_line) {
        ORG_SCAN_LINE scan_line;

        if(k < ctx->n_scan_lines) {
            scan_line = ctx->scan_lines[k];
        } else {
            org_scan_line(ctx, off, limit, &scan_line);
            if(ctx->n_scan_lines == k  &&  k < ctx->alloc_scan_lines) {
                ctx->scan_lines[ctx->n_scan_lines++] = scan_line;
            } else if(ctx->n_scan_lines == k) {
                int new_alloc = (ctx->alloc_scan_lines > 0 ? ctx->alloc_scan_lines * 2 : 64);
                ORG_SCAN_LINE* new_scan_lines = (ORG_SCAN_LINE*) realloc(ctx->scan_lines, new_alloc * sizeof(ORG_SCAN_LINE));

                /* (Without the memory, the lines are just not remembered.) */
                if(new_scan_lines != NULL) {
                    ctx->scan_lines = new_scan_lines;
                    ctx->alloc_scan_lines = new_alloc;
                    ctx->scan_lines[ctx->n_scan_lines++] = scan_line;
                }
            }
        }
        k++;

        if(scan_line.indent == UINT_MAX) {
            n_blank_lines++;
            if(n_blank_lines == 2)
                break;
        } else {
            n_blank_lines = 0;
            if(scan_line.indent <= item->indent)
                break;
        }

        off = scan_line.next;
        if(off >= ctx->size)
            break;
    }

    item->item_scan_is_cut = (off <= end_line);
    item->item_scan_end = off;
    return item->item_scan_is_cut;
}

/* Blocks and drawers cannot span over a headline, nor over the end of the
 * enclosing block or drawer. */
static OFF
org_block_limit(ORG_CTX* ctx, OFF line_beg)
{
    OFF limit = org_next_headline(ctx, line_beg);
    int i;

    for(i = ctx->n_containers - 1; i >= 0; i--) {
        if(ctx->containers[i].end_line != 0) {
            limit = MIN(limit, ctx->containers[i].end_line);
            break;
        }
    }

    return limit;
}

static void
org_analyze_line(ORG_CTX* ctx, OFF line_beg, OFF* p_next_line_beg, ORG_LINE_ANALYSIS* line)
{
    OFF line_end = org_line_end(ctx, line_beg);
    OFF off = line_beg;
    OFF end;
    unsigned indent = 0;
    int i;

    *p_next_line_beg = org_skip_newline(ctx, line_end);

    memset(line, 0, sizeof(ORG_LINE_ANALYSIS));
    line->line_beg = line_beg;

    /* Indentation. */
    while(off < line_end  &&  ISBLANK(off)) {
        if(CH(off) == _T('\t'))
            indent = (indent / TAB_WIDTH + 1) * TAB_WIDTH;
        else
            indent++;
        off++;
    }
    line->indent = indent;
    line->beg = off;
    line->contents_beg = off;

    /* Inside a verbatim-like block, we only look for its end. */
    if(ctx->verbatim_end != 0) {
        line->end = line_end;
        line->type = (line_beg == ctx->verbatim_end) ? ORG_LINE_BLOCKEND : ORG_LINE_VERBATIM;
        return;
    }

    end = org_skip_blanks_backward(ctx, off, line_end);
    line->end = end;

    if(off >= end) {
        line->type = ORG_LINE_BLANK;
        return;
    }

    /* The end of the innermost block or drawer? */
    for(i = ctx->n_containers - 1; i >= 0; i--) {
        if(ctx->containers[i].end_line != 0) {
            if(line_beg == ctx->containers[i].end_line) {
                line->type = ORG_LINE_BLOCKEND;
                return;
            }
            break;
        }
    }

    /* Headline. */
    if(off == line_beg) {
        unsigned level = org_is_headline_line(ctx, line_beg, line_end);

        if(level >= INLINETASK_MIN_LEVEL  &&  (ctx->parser.flags & ORG_FLAG_INLINETASKS)) {
            line->type = ORG_LINE_INLINETASK;
            line->data = level;
            /* It has contents only if the very next inline task line is "END". */
            line->end_line = org_find_end_line(ctx->inlinetask_ends, ctx->n_inlinetask_ends, line_beg,
                        org_block_limit(ctx, line_beg));
            if(line->end_line != 0) {
                OFF end_line_end = org_line_end(ctx, line->end_line);
                OFF q = org_skip_blanks(ctx, line->end_line + org_is_headline_line(ctx, line->end_line, end_line_end),
                            end_line_end);
                OFF e = org_skip_blanks_backward(ctx, q, end_line_end);

                if(e - q != 3  ||  !org_ascii_eq(STR(q), _T("END"), 3))
                    line->end_line = 0;
            }
            return;
        }
        if(level > 0) {
            line->type = ORG_LINE_HEADLINE;
            line->data = level;
            return;
        }
    }

    /* Planning line. */
    if(ctx->after_headline == 1  &&
       (org_is_prefix(ctx, off, end, _T("SCHEDULED:"))  ||
        org_is_prefix(ctx, off, end, _T("DEADLINE:"))  ||
        org_is_prefix(ctx, off, end, _T("CLOSED:"))))
    {
        line->type = ORG_LINE_PLANNING;
        return;
    }

    /* Footnote definition "[fn:label] ..." (at the beginning of the line). */
    if(off == line_beg  &&  org_is_prefix(ctx, off, end, _T("[fn:"))) {
        OFF q = off + 4;

        while(q < end  &&  org_is_footnote_label_char(ctx, q))
            q++;
        if(q > off + 4  &&  q < end  &&  CH(q) == _T(']')) {
            line->type = ORG_LINE_FOOTNOTEDEF;
            line->contents_beg = org_skip_blanks(ctx, q + 1, end);
            return;
        }
    }

    switch(CH(off)) {
        case _T('\\'):
            /* LaTeX environment "\begin{name}". */
            if(!(ctx->parser.flags & ORG_FLAG_NOLATEX)  &&  org_is_prefix(ctx, off, end, _T("\\begin{"))) {
                OFF name_beg = off + 7;
                OFF name_end = name_beg;

                while(name_end < end  &&  (ISALNUM(name_end)  ||  CH(name_end) == _T('*')))
                    name_end++;
                if(name_end > name_beg  &&  name_end < end  &&  CH(name_end) == _T('}')) {
                    OFF end_line = org_find_block_end(ctx->latex_ends, ctx->n_latex_ends, line_beg,
                                STR(name_beg), name_end - name_beg, org_block_limit(ctx, line_beg));
                    if(end_line != 0  &&  !org_is_cut_by_item_end(ctx, line, *p_next_line_beg, end_line)) {
                        line->type = ORG_LINE_BLOCKBEGIN;
                        line->data = ORG_BLOCK_LATEX_ENVIRONMENT;
                        line->end_line = end_line;
                        return;
                    }
                }
            }
            break;

        case _T('#'):
            /* (Not a tab, as in Emacs, unlike in the indentation.) */
            if(off + 1 == line_end  ||  CH(off+1) == _T(' ')) {
                line->type = ORG_LINE_COMMENT;
                line->contents_beg = (off + 1 < end) ? off + 2 : end;
                return;
            }

            if(org_is_prefix_i(ctx, off, end, _T("#+BEGIN_"))) {
                OFF name_beg, name_end, params_beg;

                org_analyze_block_begin_line(ctx, off, end, &name_beg, &name_end, &params_beg);
                if(name_end > name_beg) {
                    OFF end_line = org_find_block_end(ctx->block_ends, ctx->n_block_ends, line_beg,
                                STR(name_beg), name_end - name_beg, org_block_limit(ctx, line_beg));
                    if(end_line != 0) {
                        line->type = ORG_LINE_BLOCKBEGIN;
                        line->data = org_block_type_from_name(STR(name_beg), name_end - name_beg);
                        line->end_line = end_line;
                        return;
                    }
                }
            } else if(org_is_prefix_i(ctx, off, end, _T("#+BEGIN:"))) {
                /* Dynamic block "#+BEGIN: name params". */
                OFF end_line = org_find_end_line(ctx->dynamic_ends, ctx->n_dynamic_ends, line_beg,
                            org_block_limit(ctx, line_beg));

                if(end_line != 0  &&  org_skip_blanks(ctx, off + 8, end) < end) {
                    line->type = ORG_LINE_BLOCKBEGIN;
                    line->data = ORG_BLOCK_DYNAMIC;
                    line->end_line = end_line;
                    return;
                }
            } else {
                OFF key_end, value_beg;

                if(org_is_keyword_line(ctx, off, end, &key_end, &value_beg)) {
                    line->type = ORG_LINE_KEYWORD;
                    return;
                }
            }
            break;

        case _T(':'):
        {
            OFF name_beg, name_end;

            if(off + 1 == line_end  ||  CH(off+1) == _T(' ')) {
                /* (Unlike in other lines, the trailing whitespace is a part
                 * of the contents.) */
                line->type = ORG_LINE_FIXEDWIDTH;
                line->end = line_end;
                line->contents_beg = (off + 1 < line_end) ? off + 2 : line_end;
                return;
            }

            if(org_is_drawer_begin_line(ctx, off, end, &name_beg, &name_end)  &&
               !org_is_drawer_end_line(ctx, off, end))
            {
                OFF end_line = org_find_end_line(ctx->drawer_ends, ctx->n_drawer_ends, line_beg,
                            org_block_limit(ctx, line_beg));

                if(end_line != 0) {
                    line->end_line = end_line;
                    /* (A property drawer also may be the first element of
                     * the document, or the second one after a comment.) */
                    if((ctx->after_headline > 0  ||  ctx->at_doc_start > 0)  &&
                       name_end - name_beg == 10  &&
                       org_ascii_case_eq(STR(name_beg), _T("PROPERTIES"), 10)  &&
                       org_is_node_property_block(ctx, line_beg, end_line))
                        line->type = ORG_LINE_PROPERTYDRAWERBEGIN;
                    else
                        line->type = ORG_LINE_DRAWERBEGIN;
                    return;
                }
            }
            break;
        }

        case _T('|'):
            line->type = ORG_LINE_TABLE;
            return;

        case _T('-'):
        {
            OFF tmp = off;

            while(tmp < end  &&  CH(tmp) == _T('-'))
                tmp++;
            if(tmp == end  &&  tmp - off >= 5) {
                line->type = ORG_LINE_HR;
                return;
            }
            break;
        }

        default:
            break;
    }

    /* List item. */
    if(ISANYOF(off, _T("-+*"))  ||  ISDIGIT(off)) {
        ORG_ITEM_INFO info;

        if(org_is_list_item(ctx, off, end, indent, &info)) {
            line->type = ORG_LINE_ITEM;
            line->data = info.list_type;
            line->contents_beg = info.contents_beg;
            return;
        }
    }

    line->type = ORG_LINE_TEXT;
}

static int
org_is_list_container(ORG_BLOCKTYPE type)
{
    return (type == ORG_BLOCK_UL  ||  type == ORG_BLOCK_OL  ||  type == ORG_BLOCK_DL  ||
            type == ORG_BLOCK_LI  ||  type == ORG_BLOCK_DD);
}

/* Close all the (possibly nested) lists at the top of the container stack.
 * (Org ends all the lists on two consecutive blank lines.) */
static int
org_leave_all_lists(ORG_CTX* ctx)
{
    int n = ctx->n_containers;

    while(n > 0  &&  org_is_list_container(ctx->containers[n-1].type))
        n--;

    if(n < ctx->n_containers)
        return org_leave_child_containers(ctx, n);
    return 0;
}

/* Close the list items (and lists) as dictated by the indentation of the
 * given (non-blank) line. */
static int
org_leave_lists_by_indent(ORG_CTX* ctx, const ORG_LINE_ANALYSIS* line)
{
    int n = ctx->n_containers;

    while(n > 0) {
        const ORG_CONTAINER* container = &ctx->containers[n-1];

        if(container->type == ORG_BLOCK_LI  ||  container->type == ORG_BLOCK_DD) {
            /* Item contents has to be indented more than its bullet. */
            if(line->indent > container->indent)
                break;
        } else if(container->type == ORG_BLOCK_UL  ||  container->type == ORG_BLOCK_OL  ||
                  container->type == ORG_BLOCK_DL)
        {
            /* Another item of the same list? */
            if(line->type == ORG_LINE_ITEM  &&  line->indent == container->indent)
                break;
            if(line->indent > container->indent)
                break;
        } else {
            break;
        }

        n--;
    }

    if(n < ctx->n_containers)
        return org_leave_child_containers(ctx, n);
    return 0;
}

static int
org_process_item_line(ORG_CTX* ctx, const ORG_LINE_ANALYSIS* line)
{
    ORG_ITEM_INFO info;
    ORG_BLOCKTYPE list_type;
    ORG_CONTAINER* top;
    OFF contents_beg;
    int ret = 0;

    org_is_list_item(ctx, line->beg, line->end, line->indent, &info);

    top = (ctx->n_containers > 0) ? &ctx->containers[ctx->n_containers - 1] : NULL;
    if(top != NULL  &&  (top->type == ORG_BLOCK_UL  ||  top->type == ORG_BLOCK_OL  ||  top->type == ORG_BLOCK_DL)  &&
       top->indent == line->indent)
    {
        /* The list type is determined by its first item. */
        list_type = top->type;
    } else {
        list_type = info.list_type;
        ORG_CHECK(org_push_container(ctx, list_type, (unsigned) info.mark, line));
    }

    contents_beg = info.contents_beg;
    if(list_type == ORG_BLOCK_DL) {
        ORG_CHECK(org_push_container(ctx, ORG_BLOCK_DD, 0, line));
    } else {
        /* A tag in a non-description list is just an ordinary contents. */
        if(info.tag_end > info.tag_beg)
            contents_beg = info.tag_beg;
        ORG_CHECK(org_push_container(ctx, ORG_BLOCK_LI, 0, line));
    }

    if(contents_beg < line->end) {
        ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_P, contents_beg, line->end));
        ORG_CHECK(org_add_line_into_current_block(ctx, contents_beg, line->end, line->line_beg));
    }

abort:
    return ret;
}

/* Check whether the keyword [beg, end) (without "#+" and ':') is an affiliated
 * one. */
static int
org_is_affiliated_keyword(ORG_CTX* ctx, OFF beg, OFF end)
{
    static const CHAR* const affiliated_keys[] = {
        _T("CAPTION"), _T("DATA"), _T("HEADER"), _T("HEADERS"), _T("LABEL"), _T("NAME"),
        _T("PLOT"), _T("RESNAME"), _T("RESULT"), _T("RESULTS"), _T("SOURCE"), _T("SRCNAME"),
        _T("TBLNAME")
    };
    OFF off = beg;
    int i;

    if(org_is_prefix_i(ctx, beg, end, _T("ATTR_")))
        return (end > beg + 5);

    /* Ignore the optional part, e.g. "CAPTION[short]". */
    while(off < end  &&  CH(off) != _T('['))
        off++;

    for(i = 0; i < (int) SIZEOF_ARRAY(affiliated_keys); i++) {
        SZ n = (SZ) org_strlen(affiliated_keys[i]);
        if(off - beg == n  &&  org_ascii_case_eq(STR(beg), affiliated_keys[i], n))
            return true;
    }

    return false;
}

/* The element (starting at the current line) gets the preceding affiliated
 * keywords (if 'affiliate' is non-zero); or they are just ordinary keywords. */
static void
org_resolve_pending_keywords(ORG_CTX* ctx, int affiliate)
{
    int i;

    if(affiliate) {
        for(i = 0; i < ctx->n_pending_keywords; i++) {
            ORG_BLOCK* block = (ORG_BLOCK*) ((char*) ctx->block_bytes + ctx->pending_keywords[i]);
            block->flags |= ORG_BLOCK_AFFILIATED;
        }
    }
    ctx->n_pending_keywords = 0;
}

/* Start a new block of the given type, unless the line may just be appended
 * into the current one. */
static int
org_add_line_into_block_of_type(ORG_CTX* ctx, ORG_BLOCKTYPE type, const ORG_LINE_ANALYSIS* line)
{
    int ret = 0;

    if(ctx->current_block == NULL  ||  ctx->current_block->type != type)
        ORG_CHECK(org_start_new_block(ctx, type, line->beg, line->end));
    if(org_block_has_verbatim_lines(type)) {
        /* In a fixed-width area (or a comment), the indentation of the line
         * follows its ": " (or "# "). */
        ORG_CHECK(org_add_line_into_current_block(ctx, org_skip_blanks(ctx, line->contents_beg, line->end),
                    line->end, line->contents_beg));
    } else {
        ORG_CHECK(org_add_line_into_current_block(ctx, line->contents_beg, line->end, line->line_beg));
    }

abort:
    return ret;
}

static int
org_process_line(ORG_CTX* ctx, const ORG_LINE_ANALYSIS* line)
{
    int after_headline = 0;
    int i;
    int ret = 0;

    if(line->type == ORG_LINE_BLANK) {
        org_resolve_pending_keywords(ctx, false);
        org_end_current_block(ctx);
        ctx->n_blank_lines++;
        ctx->after_headline = 0;
        if(ctx->at_doc_start == 2)
            ctx->at_doc_start = 3;
        if(ctx->n_blank_lines == 2) {
            /* Two blank lines end all the lists, as well as any footnote
             * definition. */
            ORG_CHECK(org_leave_all_lists(ctx));
            if(ctx->n_containers > 0  &&  ctx->containers[ctx->n_containers - 1].type == ORG_BLOCK_FOOTNOTE_DEF)
                ORG_CHECK(org_leave_child_containers(ctx, ctx->n_containers - 1));
        }
        return 0;
    }

    ctx->n_blank_lines = 0;

    /* (The consecutive comment lines form a single comment.) */
    if(line->type == ORG_LINE_COMMENT  &&  (ctx->at_doc_start == 1  ||  ctx->at_doc_start == 2))
        ctx->at_doc_start = 2;
    else
        ctx->at_doc_start = 0;

    /* Affiliated keywords belong to the following element (if there is one,
     * without any blank line between). */
    if(ctx->n_pending_keywords > 0  &&  line->type != ORG_LINE_KEYWORD)
        org_resolve_pending_keywords(ctx, (line->type != ORG_LINE_HEADLINE));

    switch(line->type) {
        case ORG_LINE_VERBATIM:
            ORG_CHECK(org_add_line_into_current_block(ctx, line->beg, line->end, line->line_beg));
            goto out;

        case ORG_LINE_BLOCKEND:
            if(ctx->verbatim_end != 0) {
                /* LaTeX environment includes also its "\end{name}" line. */
                if(ctx->current_block != NULL  &&  ctx->current_block->type == ORG_BLOCK_LATEX_ENVIRONMENT)
                    ORG_CHECK(org_add_line_into_current_block(ctx, line->beg, line->end, line->line_beg));
                org_end_current_block(ctx);
                ctx->verbatim_end = 0;
            } else {
                for(i = ctx->n_containers - 1; i >= 0; i--) {
                    if(ctx->containers[i].end_line != 0)
                        break;
                }
                ORG_ASSERT(i >= 0);
                ORG_CHECK(org_leave_child_containers(ctx, i));
            }
            goto out;

        case ORG_LINE_HEADLINE:
            /* Close everything down to the parent section. */
            for(i = ctx->n_containers - 1; i >= 0; i--) {
                if(ctx->containers[i].type == ORG_BLOCK_SECTION  &&  ctx->containers[i].level < line->data)
                    break;
            }
            ORG_CHECK(org_leave_child_containers(ctx, i + 1));
            ORG_CHECK(org_push_container(ctx, ORG_BLOCK_SECTION, line->data, line));
            ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_HEADLINE, line->beg, line->end));
            org_end_current_block(ctx);
            after_headline = 1;
            goto out;

        default:
            break;
    }

    ORG_CHECK(org_leave_lists_by_indent(ctx, line));

    switch(line->type) {
        case ORG_LINE_BLOCKBEGIN:
            if(org_block_has_verbatim_lines((ORG_BLOCKTYPE) line->data)  ||  line->data == ORG_BLOCK_VERSE) {
                ORG_CHECK(org_start_new_block(ctx, (ORG_BLOCKTYPE) line->data, line->beg, line->end));
                ctx->verbatim_end = line->end_line;

                /* LaTeX environment includes also its "\begin{name}" line. */
                if(line->data == ORG_BLOCK_LATEX_ENVIRONMENT)
                    ORG_CHECK(org_add_line_into_current_block(ctx, line->beg, line->end, line->line_beg));
            } else {
                ORG_CHECK(org_push_container(ctx, (ORG_BLOCKTYPE) line->data, 0, line));
            }
            break;

        case ORG_LINE_DRAWERBEGIN:
            ORG_CHECK(org_push_container(ctx, ORG_BLOCK_DRAWER, 0, line));
            break;

        case ORG_LINE_PROPERTYDRAWERBEGIN:
            ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_PROPERTY_DRAWER, line->beg, line->end));
            ctx->verbatim_end = line->end_line;
            break;

        case ORG_LINE_PLANNING:
            ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_PLANNING, line->beg, line->end));
            ORG_CHECK(org_add_line_into_current_block(ctx, line->beg, line->end, line->line_beg));
            org_end_current_block(ctx);
            after_headline = 2;
            break;

        case ORG_LINE_KEYWORD:
        {
            OFF key_end, value_beg;
            int block_off = ctx->n_block_bytes;

            ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_KEYWORD, line->beg, line->end));
            org_end_current_block(ctx);

            org_is_keyword_line(ctx, line->beg, line->end, &key_end, &value_beg);
            if(org_is_affiliated_keyword(ctx, line->beg + 2, key_end)) {
                ORG_GROW_ARRAY(ctx->pending_keywords, ctx->alloc_pending_keywords, ctx->n_pending_keywords);
                ctx->pending_keywords[ctx->n_pending_keywords++] = block_off;
            } else {
                /* The keyword itself is the element the pending ones belong to. */
                org_resolve_pending_keywords(ctx, true);
            }
            break;
        }

        case ORG_LINE_HR:
            ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_HR, line->beg, line->end));
            org_end_current_block(ctx);
            break;

        case ORG_LINE_COMMENT:
            ORG_CHECK(org_add_line_into_block_of_type(ctx, ORG_BLOCK_COMMENT, line));
            break;

        case ORG_LINE_FIXEDWIDTH:
            ORG_CHECK(org_add_line_into_block_of_type(ctx, ORG_BLOCK_FIXED_WIDTH, line));
            break;

        case ORG_LINE_TABLE:
            ORG_CHECK(org_add_line_into_block_of_type(ctx, ORG_BLOCK_TABLE, line));
            break;

        case ORG_LINE_ITEM:
            ORG_CHECK(org_process_item_line(ctx, line));
            break;

        case ORG_LINE_INLINETASK:
            ORG_CHECK(org_push_container(ctx, ORG_BLOCK_INLINETASK, line->data, line));
            ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_HEADLINE, line->beg, line->end));
            org_end_current_block(ctx);
            /* Without the terminating line, it has no contents. */
            if(line->end_line == 0)
                ORG_CHECK(org_leave_child_containers(ctx, ctx->n_containers - 1));
            break;

        case ORG_LINE_FOOTNOTEDEF:
            /* A footnote definition ends the previous one. */
            if(ctx->n_containers > 0  &&  ctx->containers[ctx->n_containers - 1].type == ORG_BLOCK_FOOTNOTE_DEF)
                ORG_CHECK(org_leave_child_containers(ctx, ctx->n_containers - 1));
            ORG_CHECK(org_push_container(ctx, ORG_BLOCK_FOOTNOTE_DEF, 0, line));
            if(line->contents_beg < line->end) {
                ORG_CHECK(org_start_new_block(ctx, ORG_BLOCK_P, line->contents_beg, line->end));
                ORG_CHECK(org_add_line_into_current_block(ctx, line->contents_beg, line->end, line->line_beg));
            }
            break;

        case ORG_LINE_TEXT:
            ORG_CHECK(org_add_line_into_block_of_type(ctx, ORG_BLOCK_P, line));
            break;

        default:
            ORG_UNREACHABLE();
            break;
    }

out:
    ctx->after_headline = after_headline;
abort:
    return ret;
}

/* Process an inline footnote definition [beg, end) as a paragraph. */
static int
org_process_inline_footnote_def(ORG_CTX* ctx, OFF beg, OFF end)
{
    ORG_BLOCK_P_DETAIL p_det;
    ORG_LINE* lines;
    SZ n_lines = 0;
    OFF off;
    int ret = 0;

    beg = org_skip_blanks(ctx, beg, end);
    end = org_skip_blanks_backward(ctx, beg, end);
    if(beg >= end)
        return 0;

    /* The definition may span over multiple lines. (Count all the new line
     * characters; that is an upper bound even with "\r\n".) */
    for(off = beg; off < end; off++) {
        if(ISNEWLINE(off))
            n_lines++;
    }
    n_lines++;

    lines = (ORG_LINE*) malloc(n_lines * sizeof(ORG_LINE));
    if(lines == NULL) {
        ORG_LOG("malloc() failed.");
        return -1;
    }

    n_lines = 0;
    off = beg;
    while(off < end) {
        OFF line_end = off;

        while(line_end < end  &&  !ISNEWLINE(line_end))
            line_end++;
        lines[n_lines].beg = org_skip_blanks(ctx, off, line_end);
        lines[n_lines].end = org_skip_blanks_backward(ctx, lines[n_lines].beg, line_end);
        n_lines++;
        off = org_skip_newline(ctx, line_end);
    }

    memset(&p_det, 0, sizeof(p_det));
    ORG_ENTER_BLOCK(ORG_BLOCK_P, &p_det);
    ORG_CHECK(org_process_normal_block_contents(ctx, lines, n_lines, false));
    ORG_LEAVE_BLOCK(ORG_BLOCK_P, &p_det);

abort:
    free(lines);
    return ret;
}

static int
org_process_footnote_def(ORG_CTX* ctx, int index)
{
    /* Note we work with a copy: The processing of the footnote contents may
     * reference some other footnotes, and so ctx->footnotes may get
     * reallocated. */
    ORG_FOOTNOTE fn = ctx->footnotes[index];
    ORG_BLOCK_FOOTNOTE_DEF_DETAIL det;
    ORG_ATTRIBUTE_BUILD label_build;
    int ret = 0;

    memset(&det, 0, sizeof(ORG_BLOCK_FOOTNOTE_DEF_DETAIL));
    det.id = fn.id;
    det.ref_count = fn.ref_count;
    org_build_attribute(ctx, fn.label, fn.label_size, &det.label, &label_build);
    det.is_defined = (fn.def_open_off >= 0  ||  fn.has_inline_def);

    ORG_ENTER_BLOCK(ORG_BLOCK_FOOTNOTE_DEF, &det);
    if(fn.def_open_off >= 0  &&  fn.def_close_off >= 0) {
        ORG_CHECK(org_process_blocks(ctx, fn.def_open_off + (int) sizeof(ORG_BLOCK), fn.def_close_off));
    } else if(fn.has_inline_def) {
        ctx->footnote_nesting_level = fn.nesting_level;
        ret = org_process_inline_footnote_def(ctx, fn.inline_beg, fn.inline_end);
        ctx->footnote_nesting_level = 0;
        if(ret != 0)
            goto abort;
    }
    ORG_LEAVE_BLOCK(ORG_BLOCK_FOOTNOTE_DEF, &det);

abort:
    return ret;
}

/* Render footnote definitions that were actually referenced, in reference
 * order. Called from org_process_doc() after org_process_blocks(). */
static int
org_process_footnote_defs(ORG_CTX* ctx)
{
    int i;
    int ret = 0;

    if(ctx->n_footnote_order == 0)
        return 0;

    ORG_ENTER_BLOCK(ORG_BLOCK_FOOTNOTE_DEF_SECTION, NULL);

    /* Note the processing of a footnote may reference some other footnotes
     * so ctx->n_footnote_order may grow in the loop. */
    for(i = 0; i < ctx->n_footnote_order; i++)
        ORG_CHECK(org_process_footnote_def(ctx, ctx->footnote_order[i]));

    ORG_LEAVE_BLOCK(ORG_BLOCK_FOOTNOTE_DEF_SECTION, NULL);

abort:
    return ret;
}

static int
org_process_doc(ORG_CTX* ctx)
{
    OFF off = 0;
    int i;
    int ret = 0;

    ORG_ENTER_BLOCK(ORG_BLOCK_DOC, NULL);

    ORG_CHECK(org_build_doc_index(ctx));

    /* Radio links may start with any first character of the radio targets. */
    for(i = 0; i < (int) SIZEOF_ARRAY(ctx->mark_char_map); i++) {
        if(ctx->radio_first_char_map[i])
            ctx->mark_char_map[i] = 1;
    }

    while(off < ctx->size) {
        ORG_LINE_ANALYSIS line;

        org_analyze_line(ctx, off, &off, &line);
        ORG_CHECK(org_process_line(ctx, &line));
    }

    /* Close all the blocks. */
    org_end_current_block(ctx);
    ORG_CHECK(org_leave_child_containers(ctx, 0));

    /* Process all the blocks. */
    ORG_CHECK(org_resolve_code_blocks(ctx));
    ORG_CHECK(org_process_blocks(ctx, 0, ctx->n_block_bytes));
    ORG_CHECK(org_process_footnote_defs(ctx));
    ORG_LEAVE_BLOCK(ORG_BLOCK_DOC, NULL);

abort:

#if 0
    /* Output some memory consumption statistics. */
    {
        char buffer[256];
        sprintf(buffer, "Alloced %u bytes for block buffer.",
                    (unsigned)(ctx->alloc_block_bytes));
        ORG_LOG(buffer);

        sprintf(buffer, "Alloced %u bytes for containers buffer.",
                    (unsigned)(ctx->alloc_containers * sizeof(ORG_CONTAINER)));
        ORG_LOG(buffer);

        sprintf(buffer, "Alloced %u bytes for marks buffer.",
                    (unsigned)(ctx->alloc_marks * sizeof(ORG_MARK)));
        ORG_LOG(buffer);
    }
#endif

    return ret;
}


/********************
 ***  Public API  ***
 ********************/

int
org_parse(const ORG_CHAR* text, ORG_SIZE size, const ORG_PARSER* parser, void* userdata)
{
    ORG_CTX ctx;
    int ret;

    if(parser->abi_version != 0) {
        if(parser->debug_log != NULL)
            parser->debug_log("Unsupported abi_version.", userdata);
        return -1;
    }

    /* Setup context structure. */
    memset(&ctx, 0, sizeof(ORG_CTX));
    ctx.text = text;
    ctx.size = size;
    memcpy(&ctx.parser, parser, sizeof(ORG_PARSER));
    ctx.userdata = userdata;
    org_check_doc_chars(&ctx);
    org_build_mark_char_map(&ctx);
    ctx.macro_output_budget = MACRO_MAX_OUTPUT(size);
    ctx.at_doc_start = 1;

    /* All the work. */
    ret = org_process_doc(&ctx);

    /* Clean-up. */
    free(ctx.headlines);
    free(ctx.drawer_ends);
    free(ctx.dynamic_ends);
    free(ctx.inlinetask_ends);
    free(ctx.block_ends);
    free(ctx.latex_ends);
    free(ctx.scan_lines);
    free(ctx.todo_keywords);
    free(ctx.doc_keywords);
    free(ctx.macros);
    free(ctx.radio_targets);
    free(ctx.code_blocks);
    free(ctx.code_refs);
    free(ctx.pending_keywords);
    free(ctx.footnotes);
    free(ctx.footnote_buckets);
    free(ctx.footnote_order);
    free(ctx.bracket_matches);
    free(ctx.marks);
    free(ctx.block_bytes);
    free(ctx.containers);
    while(ctx.attribute_storages != NULL) {
        ORG_ATTRIBUTE_STORAGE* next = ctx.attribute_storages->next;
        free(ctx.attribute_storages);
        ctx.attribute_storages = next;
    }

    return ret;
}

const char*
org_entity_html(const char* name, ORG_SIZE name_size)
{
    const ORG_ENTITY* entity = org_entity_lookup(name, name_size);
    return (entity != NULL) ? entity->html : NULL;
}
