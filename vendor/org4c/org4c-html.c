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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "org4c-html.h"


#ifdef _WIN32
    #define snprintf _snprintf
#endif

/* For falling through case labels in switch statements. */
#if defined __clang__ && __clang_major__ >= 12
    #define ORG_FALLTHROUGH()       ;__attribute__((fallthrough))
#elif defined __GNUC__ && __GNUC__ >= 7
    #define ORG_FALLTHROUGH()       ;__attribute__((fallthrough))
#else
    #define ORG_FALLTHROUGH()       ((void)0)
#endif



/* Maximal count of the attributes (from all the "#+ATTR_HTML:" keywords) of
 * an element. Any more of them are ignored. */
#define ATTR_HTML_MAX           16

/* Attribute ":key value" of "#+ATTR_HTML:". */
typedef struct ORG_ATTR_HTML_tag ORG_ATTR_HTML;
struct ORG_ATTR_HTML_tag {
    const ORG_CHAR* key;        /* Without the leading ':'. */
    ORG_SIZE key_size;
    const ORG_CHAR* value;      /* NULL if there is no value. */
    ORG_SIZE value_size;
};

typedef struct ORG_HTML_tag ORG_HTML;
struct ORG_HTML_tag {
    void (*process_output)(const ORG_CHAR*, ORG_SIZE, void*);
    void* userdata;
    unsigned flags;
    int raw_link_nesting_level;     /* Inside a link whose raw contents is not rendered. */
    int raw_link_is_image;
    int suppress_level;         /* Non-zero inside contents which is not exported. */
    int verse_level;
    int at_verse_line_start;
    int in_hidden_snippet;      /* Inside an export snippet for another backend. */
    int in_latex_env;
    int in_timestamp;
    unsigned n_tables;
    unsigned n_listings;
    unsigned n_figures;
    int in_inlinetask_title;

    /* Some output has to be delayed:
     *  -- <pre><code> of a source block may be preceded by the <label> with
     *     its caption (which is reported as the first child of the block).
     *  -- A paragraph consisting only of an image is rendered as a figure
     *     (we know it only when we see the link), and its caption goes after
     *     the image. So the caption is captured into a buffer. */
    ORG_BLOCKTYPE last_block_type;
    int last_block_is_standalone_link;
    int src_pending;
    const ORG_CHAR* src_lang;
    ORG_SIZE src_lang_size;
    int p_pending;
    int p_is_figure;
    int in_caption;
    int out_of_memory;          /* When set, the callbacks abort the parsing. */

    /* Line numbers in source and example blocks. */
    int in_code_block;
    unsigned code_line;         /* Number of the current line; zero if not numbered. */
    int code_line_width;
    int at_code_line_start;

    /* Buffer for the captured caption of a figure (see above). */
    int capturing;
    char* capture;
    ORG_SIZE capture_size;
    ORG_SIZE capture_alloc;
    ORG_SIZE figure_caption_size;   /* Size of the captured caption. */

    unsigned n_citation_refs;   /* Count of references in the current citation. */
    int pending_dashes;         /* "--" or "---" ending the last text (see render_text()). */

    /* Affiliated keywords "#+NAME:" and "#+ATTR_HTML:" of the element being
     * rendered. (They point into the document, which is valid during all the
     * parsing.) */
    int aff_collecting;         /* Set while the keywords preceding the element are reported. */
    int aff_first_link;         /* Set until the first link of a paragraph (which gets the attributes too). */
    const ORG_CHAR* aff_name;
    ORG_SIZE aff_name_size;
    ORG_ATTR_HTML aff_attrs[ATTR_HTML_MAX];
    int n_aff_attrs;

    char escape_map[256];
};

#define NEED_HTML_ESC_FLAG   0x1
#define NEED_URL_ESC_FLAG    0x2
#define NEED_SPECIAL_FLAG    0x4    /* May start a special string (see render_text()). */


/*****************************************
 ***  HTML rendering helper functions  ***
 *****************************************/

#define ISDIGIT(ch)     ('0' <= (ch) && (ch) <= '9')
#define ISLOWER(ch)     ('a' <= (ch) && (ch) <= 'z')
#define ISUPPER(ch)     ('A' <= (ch) && (ch) <= 'Z')
#define ISALNUM(ch)     (ISLOWER(ch) || ISUPPER(ch) || ISDIGIT(ch))
#define ISBLANK(ch)     ((ch) == ' ' || (ch) == '\t')


static void
render_captured(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    if(r->capture_size + size > r->capture_alloc) {
        ORG_SIZE new_alloc = r->capture_alloc + r->capture_alloc / 2 + size + 256;
        char* new_capture = (char*) realloc(r->capture, new_alloc);

        if(new_capture == NULL) {
            r->out_of_memory = 1;
            return;
        }
        r->capture = new_capture;
        r->capture_alloc = new_alloc;
    }
    memcpy(r->capture + r->capture_size, text, size);
    r->capture_size += size;
}

static inline void
render_verbatim(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    if(r->capturing) {
        render_captured(r, text, size);
        return;
    }

    r->process_output(text, size, r->userdata);
}

/* Keep this as a macro. Most compiler should then be smart enough to replace
 * the strlen() call with a compile-time constant if the string is a C literal. */
#define RENDER_VERBATIM(r, verbatim)                                    \
        render_verbatim((r), (verbatim), (ORG_SIZE) (strlen(verbatim)))


static void
render_html_escaped(ORG_HTML* r, const ORG_CHAR* data, ORG_SIZE size)
{
    ORG_OFFSET beg = 0;
    ORG_OFFSET off = 0;

    /* Some characters need to be escaped in normal HTML text. */
    #define NEED_HTML_ESC(ch)   (r->escape_map[(unsigned char)(ch)] & NEED_HTML_ESC_FLAG)

    while(1) {
        /* Optimization: Use some loop unrolling. */
        while(off + 3 < size  &&  !NEED_HTML_ESC(data[off+0])  &&  !NEED_HTML_ESC(data[off+1])
                              &&  !NEED_HTML_ESC(data[off+2])  &&  !NEED_HTML_ESC(data[off+3]))
            off += 4;
        while(off < size  &&  !NEED_HTML_ESC(data[off]))
            off++;

        if(off > beg)
            render_verbatim(r, data + beg, off - beg);

        if(off < size) {
            switch(data[off]) {
                case '"':   RENDER_VERBATIM(r, "&quot;"); break;
                case '&':   RENDER_VERBATIM(r, "&amp;"); break;
                case '\'':  RENDER_VERBATIM(r, "&#x27;"); break;
                case '<':   RENDER_VERBATIM(r, "&lt;"); break;
                case '>':   RENDER_VERBATIM(r, "&gt;"); break;
            }
            off++;
        } else {
            break;
        }
        beg = off;
    }
}

static void
render_url_escaped(ORG_HTML* r, const ORG_CHAR* data, ORG_SIZE size)
{
    static const ORG_CHAR hex_chars[] = "0123456789ABCDEF";
    ORG_OFFSET beg = 0;
    ORG_OFFSET off = 0;

    /* Some characters need to be escaped in URL attributes. */
    #define NEED_URL_ESC(ch)    (r->escape_map[(unsigned char)(ch)] & NEED_URL_ESC_FLAG)

    while(1) {
        while(off < size  &&  !NEED_URL_ESC(data[off]))
            off++;
        if(off > beg)
            render_verbatim(r, data + beg, off - beg);

        if(off < size) {
            char hex[3];

            switch(data[off]) {
                case '&':   RENDER_VERBATIM(r, "&amp;"); break;
                default:
                    hex[0] = '%';
                    hex[1] = hex_chars[((unsigned)data[off] >> 4) & 0xf];
                    hex[2] = hex_chars[((unsigned)data[off] >> 0) & 0xf];
                    render_verbatim(r, hex, 3);
                    break;
            }
            off++;
        } else {
            break;
        }

        beg = off;
    }
}

static void
render_utf8_codepoint(ORG_HTML* r, unsigned codepoint,
                      void (*fn_append)(ORG_HTML*, const ORG_CHAR*, ORG_SIZE))
{
    static const ORG_CHAR utf8_replacement_char[] = { (char)0xef, (char)0xbf, (char)0xbd };

    unsigned char utf8[4];
    size_t n;

    if(codepoint <= 0x7f) {
        n = 1;
        utf8[0] = codepoint;
    } else if(codepoint <= 0x7ff) {
        n = 2;
        utf8[0] = 0xc0 | ((codepoint >>  6) & 0x1f);
        utf8[1] = 0x80 + ((codepoint >>  0) & 0x3f);
    } else if(codepoint <= 0xffff) {
        n = 3;
        utf8[0] = 0xe0 | ((codepoint >> 12) & 0xf);
        utf8[1] = 0x80 + ((codepoint >>  6) & 0x3f);
        utf8[2] = 0x80 + ((codepoint >>  0) & 0x3f);
    } else {
        n = 4;
        utf8[0] = 0xf0 | ((codepoint >> 18) & 0x7);
        utf8[1] = 0x80 + ((codepoint >> 12) & 0x3f);
        utf8[2] = 0x80 + ((codepoint >>  6) & 0x3f);
        utf8[3] = 0x80 + ((codepoint >>  0) & 0x3f);
    }

    if(0 < codepoint  &&  codepoint <= 0x10ffff
            &&  (codepoint < 0xd800 || codepoint > 0xdfff))
        fn_append(r, (char*)utf8, (ORG_SIZE)n);
    else
        fn_append(r, utf8_replacement_char, 3);
}

static void
render_attribute_part(ORG_HTML* r, const ORG_ATTRIBUTE* attr, ORG_OFFSET beg, ORG_OFFSET end,
                      void (*fn_append)(ORG_HTML*, const ORG_CHAR*, ORG_SIZE))
{
    int i;

    for(i = 0; attr->substr_offsets[i] < attr->size; i++) {
        ORG_TEXTTYPE type = attr->substr_types[i];
        ORG_OFFSET off = attr->substr_offsets[i];
        ORG_OFFSET off_end = attr->substr_offsets[i+1];

        /* Clip the substring to [beg, end). */
        if(off < beg)
            off = beg;
        if(off_end > end)
            off_end = end;
        if(off >= off_end)
            continue;

        switch(type) {
            case ORG_TEXT_NULLCHAR:  render_utf8_codepoint(r, 0x0000, render_verbatim); break;
            default:                 fn_append(r, attr->text + off, off_end - off); break;
        }
    }
}

static void
render_attribute(ORG_HTML* r, const ORG_ATTRIBUTE* attr,
                 void (*fn_append)(ORG_HTML*, const ORG_CHAR*, ORG_SIZE))
{
    render_attribute_part(r, attr, 0, attr->size, fn_append);
}

/* Case insensitive comparison of the attribute with the given (lower-case)
 * literal. */
static int
attribute_eq(const ORG_ATTRIBUTE* attr, const char* literal)
{
    ORG_SIZE i;

    if(attr->size != strlen(literal))
        return 0;

    for(i = 0; i < attr->size; i++) {
        ORG_CHAR ch = attr->text[i];
        if(ISUPPER(ch))
            ch += 'a' - 'A';
        if(ch != literal[i])
            return 0;
    }

    return 1;
}

/* Get the value of the header argument (e.g. ":exports") of a source block.
 * Returns zero if not present. */
static int
get_header_arg(const ORG_ATTRIBUTE* params, const char* name, ORG_ATTRIBUTE* value)
{
    size_t name_size = strlen(name);
    ORG_SIZE off = 0;

    while(off < params->size) {
        ORG_SIZE word_end = off;

        while(word_end < params->size  &&  params->text[word_end] != ' '  &&  params->text[word_end] != '\t')
            word_end++;

        if(word_end - off == name_size  &&  memcmp(params->text + off, name, name_size) == 0) {
            ORG_SIZE value_beg = word_end;
            ORG_SIZE value_end;

            while(value_beg < params->size  &&  ISBLANK(params->text[value_beg]))
                value_beg++;
            value_end = value_beg;
            while(value_end < params->size  &&  params->text[value_end] != ' '  &&  params->text[value_end] != '\t')
                value_end++;

            *value = *params;
            value->text = params->text + value_beg;
            value->size = value_end - value_beg;
            return 1;
        }

        off = word_end;
        while(off < params->size  &&  ISBLANK(params->text[off]))
            off++;
    }

    return 0;
}

/* Check whether the path looks like an image. */
static int
is_image_path(const ORG_ATTRIBUTE* path)
{
    static const char* const extensions[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp", ".tif", ".tiff", ".xpm", ".pbm", ".pgm", ".ppm"
    };
    int i;

    for(i = 0; i < (int) (sizeof(extensions) / sizeof(extensions[0])); i++) {
        ORG_SIZE n = (ORG_SIZE) strlen(extensions[i]);
        ORG_SIZE j;

        if(path->size <= n)
            continue;
        for(j = 0; j < n; j++) {
            ORG_CHAR ch = path->text[path->size - n + j];
            if(ISUPPER(ch))
                ch += 'a' - 'A';
            if(ch != extensions[i][j])
                break;
        }
        if(j == n)
            return 1;
    }

    return 0;
}

static void
clear_affiliated(ORG_HTML* r)
{
    r->aff_name = NULL;
    r->aff_name_size = 0;
    r->n_aff_attrs = 0;
}

/* Check whether the key of the attribute is the given one. */
static int
attr_html_key_eq(const ORG_ATTR_HTML* attr, const ORG_CHAR* key, ORG_SIZE key_size)
{
    return (attr->key_size == key_size  &&  memcmp(attr->key, key, key_size) == 0);
}

static ORG_ATTR_HTML*
find_attr_html(ORG_HTML* r, const ORG_CHAR* key, ORG_SIZE key_size)
{
    int i;

    for(i = 0; i < r->n_aff_attrs; i++) {
        if(attr_html_key_eq(&r->aff_attrs[i], key, key_size))
            return &r->aff_attrs[i];
    }
    return NULL;
}

/* Keep these as macros, for the same reason as RENDER_VERBATIM(). */
#define ATTR_HTML_KEY_EQ(attr, literal)                                 \
        attr_html_key_eq((attr), (literal), (ORG_SIZE) (strlen(literal)))
#define FIND_ATTR_HTML(r, literal)                                      \
        find_attr_html((r), (literal), (ORG_SIZE) (strlen(literal)))

/* An attribute with no value, or with the value "nil", is not output. */
static int
is_attr_html_output(const ORG_ATTR_HTML* attr)
{
    return (attr != NULL  &&  attr->value != NULL  &&
            !(attr->value_size == 3  &&  memcmp(attr->value, "nil", 3) == 0));
}

/* Check whether the word is a key of "#+ATTR_HTML:", i.e. ":[-a-zA-Z0-9_]+". */
static int
is_attr_html_key(const ORG_CHAR* word, ORG_SIZE size)
{
    ORG_SIZE i;

    if(size < 2  ||  word[0] != ':')
        return 0;
    for(i = 1; i < size; i++) {
        if(!ISALNUM(word[i])  &&  word[i] != '-'  &&  word[i] != '_')
            return 0;
    }
    return 1;
}

/* Parse the value of "#+ATTR_HTML:", i.e. ":key1 value1 :key2 value2 ...".
 * (Any text before the first key is ignored.) As in ox-html, a later value of
 * the same key replaces the former one. */
static void
collect_attr_html(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    ORG_ATTR_HTML* attr = NULL;
    ORG_SIZE off = 0;

    while(1) {
        ORG_SIZE beg;

        while(off < size  &&  ISBLANK(text[off]))
            off++;
        if(off >= size)
            break;
        beg = off;
        while(off < size  &&  !ISBLANK(text[off]))
            off++;

        if(is_attr_html_key(text + beg, off - beg)) {
            attr = find_attr_html(r, text + beg + 1, off - beg - 1);
            if(attr == NULL  &&  r->n_aff_attrs < ATTR_HTML_MAX) {
                attr = &r->aff_attrs[r->n_aff_attrs++];
                attr->key = text + beg + 1;
                attr->key_size = off - beg - 1;
            }
            if(attr != NULL)
                attr->value = NULL;
        } else if(attr != NULL) {
            /* The value spans all the words up to the next key. */
            if(attr->value == NULL)
                attr->value = text + beg;
            attr->value_size = (ORG_SIZE) (text + off - attr->value);
        }
    }
}

/* Remember the affiliated keywords of the element which follows. */
static void
collect_affiliated_keyword(ORG_HTML* r, const ORG_BLOCK_KEYWORD_DETAIL* det)
{
    if(!r->aff_collecting) {
        clear_affiliated(r);
        r->aff_collecting = 1;
    }

    if(attribute_eq(&det->key, "name")) {
        /* If there are more of them, the last one wins (as in Emacs). */
        r->aff_name = det->value.text;
        r->aff_name_size = det->value.size;
    } else if(attribute_eq(&det->key, "attr_html")) {
        collect_attr_html(r, det->value.text, det->value.size);
    }
}

/* What render_affiliated_attrs() outputs. */
#define AFF_ID          0x1     /* The "id" from "#+NAME:" (unless AFF_ATTRS and there is ":id"). */
#define AFF_ATTRS       0x2     /* The attributes of "#+ATTR_HTML:". */
#define AFF_IMG         0x4     /* ... except ":src" and ":alt" (the caller handles them). */
#define AFF_LINK        0x8     /* ... except ":href". */

/* Output the attributes of the element given by its affiliated keywords. Its
 * class is 'base_class' (may be empty), extended with the ":class" of
 * "#+ATTR_HTML:" (with AFF_ATTRS). */
static void
render_affiliated_attrs(ORG_HTML* r, const ORG_CHAR* base_class, ORG_SIZE base_class_size, unsigned what)
{
    const ORG_ATTR_HTML* class_attr = NULL;
    int i;

    if(what & AFF_ATTRS)
        class_attr = FIND_ATTR_HTML(r, "class");
    if(!is_attr_html_output(class_attr))
        class_attr = NULL;
    if(base_class_size > 0  ||  class_attr != NULL) {
        RENDER_VERBATIM(r, " class=\"");
        render_html_escaped(r, base_class, base_class_size);
        if(base_class_size > 0  &&  class_attr != NULL)
            RENDER_VERBATIM(r, " ");
        if(class_attr != NULL)
            render_html_escaped(r, class_attr->value, class_attr->value_size);
        RENDER_VERBATIM(r, "\"");
    }

    /* The ":id" of "#+ATTR_HTML:" (if any) takes precedence over "#+NAME:".
     * (Links to the named element point to "#NAME" and, unlike ox-html, we
     * give it the id also where ox-html does not, e.g. for a paragraph.) */
    if((what & AFF_ID)  &&  ((what & AFF_ATTRS) == 0  ||  FIND_ATTR_HTML(r, "id") == NULL)  &&
       r->aff_name_size > 0)
    {
        RENDER_VERBATIM(r, " id=\"");
        render_html_escaped(r, r->aff_name, r->aff_name_size);
        RENDER_VERBATIM(r, "\"");
    }

    if(!(what & AFF_ATTRS))
        return;

    for(i = 0; i < r->n_aff_attrs; i++) {
        const ORG_ATTR_HTML* attr = &r->aff_attrs[i];

        if(!is_attr_html_output(attr))
            continue;
        if(ATTR_HTML_KEY_EQ(attr, "class"))
            continue;
        if((what & AFF_IMG)  &&  (ATTR_HTML_KEY_EQ(attr, "src")  ||  ATTR_HTML_KEY_EQ(attr, "alt")))
            continue;
        if((what & AFF_LINK)  &&  ATTR_HTML_KEY_EQ(attr, "href"))
            continue;

        RENDER_VERBATIM(r, " ");
        render_verbatim(r, attr->key, attr->key_size);
        RENDER_VERBATIM(r, "=\"");
        render_html_escaped(r, attr->value, attr->value_size);
        RENDER_VERBATIM(r, "\"");
    }
}

/* Output the opening tag "<tag ...>" of an element with the attributes given
 * by its affiliated keywords. */
static void
render_open_tag(ORG_HTML* r, const ORG_CHAR* tag, const ORG_CHAR* base_class, unsigned what)
{
    RENDER_VERBATIM(r, "<");
    RENDER_VERBATIM(r, tag);
    render_affiliated_attrs(r, base_class, (ORG_SIZE) strlen(base_class), what);
    RENDER_VERBATIM(r, ">");
}


static void
render_headline_tags(ORG_HTML* r, const ORG_ATTRIBUTE* tags)
{
    ORG_SIZE beg = 0;
    int is_first = 1;

    RENDER_VERBATIM(r, "&#xa0;&#xa0;&#xa0;<span class=\"tag\">");
    while(beg < tags->size) {
        ORG_SIZE end = beg;

        while(end < tags->size  &&  tags->text[end] != ':')
            end++;
        if(end > beg) {
            if(!is_first)
                RENDER_VERBATIM(r, "&#xa0;");
            RENDER_VERBATIM(r, "<span class=\"");
            render_html_escaped(r, tags->text + beg, end - beg);
            RENDER_VERBATIM(r, "\">");
            render_html_escaped(r, tags->text + beg, end - beg);
            RENDER_VERBATIM(r, "</span>");
            is_first = 0;
        }
        beg = end + 1;
    }
    RENDER_VERBATIM(r, "</span>");
}

/* The level (1 to 6) of the <hN> tag of a headline. */
static int
headline_hlevel(ORG_HTML* r, const ORG_BLOCK_HEADLINE_DETAIL* det)
{
    int level = (int) det->level;

    if(r->flags & ORG_HTML_FLAG_TOPLEVEL_H2)
        level++;
    return (level < 6 ? level : 6);
}

static void
render_open_headline_block(ORG_HTML* r, const ORG_BLOCK_HEADLINE_DETAIL* det)
{
    static const ORG_CHAR* head[6] = { "<h1>", "<h2>", "<h3>", "<h4>", "<h5>", "<h6>" };

    /* The title of an inline task. */
    if(r->in_inlinetask_title)
        RENDER_VERBATIM(r, "<b>");
    else
        RENDER_VERBATIM(r, head[headline_hlevel(r, det) - 1]);

    if(det->todo.size > 0  &&  !(r->flags & ORG_HTML_FLAG_NO_TODO)) {
        RENDER_VERBATIM(r, det->is_done ? "<span class=\"done " : "<span class=\"todo ");
        render_attribute(r, &det->todo, render_html_escaped);
        RENDER_VERBATIM(r, "\">");
        render_attribute(r, &det->todo, render_html_escaped);
        RENDER_VERBATIM(r, "</span> ");
    }

    if(det->priority != 0  &&  !(r->flags & ORG_HTML_FLAG_NO_PRIORITY)) {
        RENDER_VERBATIM(r, "<span class=\"priority\">[");
        render_html_escaped(r, &det->priority, 1);
        RENDER_VERBATIM(r, "]</span> ");
    }
}

static void
render_close_headline_block(ORG_HTML* r, const ORG_BLOCK_HEADLINE_DETAIL* det)
{
    static const ORG_CHAR* head[6] = { "</h1>\n", "</h2>\n", "</h3>\n", "</h4>\n", "</h5>\n", "</h6>\n" };

    if(det->tags.size > 0  &&  !(r->flags & ORG_HTML_FLAG_NO_TAGS))
        render_headline_tags(r, &det->tags);

    if(r->in_inlinetask_title) {
        RENDER_VERBATIM(r, (r->flags & ORG_HTML_FLAG_XHTML) ? "</b><br />\n" : "</b><br>\n");
        r->in_inlinetask_title = 0;
    } else {
        RENDER_VERBATIM(r, head[headline_hlevel(r, det) - 1]);
    }
}

static void
render_checkbox(ORG_HTML* r, ORG_CHAR checkbox)
{
    switch(checkbox) {
        case 'X':   RENDER_VERBATIM(r, "<code>[X]</code> "); break;
        case '-':   RENDER_VERBATIM(r, "<code>[-]</code> "); break;
        default:    RENDER_VERBATIM(r, "<code>[&#xa0;]</code> "); break;
    }
}

static void
render_open_li_block(ORG_HTML* r, const ORG_CHAR* tag, const ORG_BLOCK_LI_DETAIL* det)
{
    char buf[64];

    RENDER_VERBATIM(r, "<");
    RENDER_VERBATIM(r, tag);
    switch(det->checkbox) {
        case 'X':   RENDER_VERBATIM(r, " class=\"on\""); break;
        case '-':   RENDER_VERBATIM(r, " class=\"trans\""); break;
        case ' ':   RENDER_VERBATIM(r, " class=\"off\""); break;
        default:    break;
    }
    if(det->counter > 0) {
        snprintf(buf, sizeof(buf), " value=\"%u\"", det->counter);
        RENDER_VERBATIM(r, buf);
    }
    RENDER_VERBATIM(r, ">");

    if(det->checkbox != 0)
        render_checkbox(r, det->checkbox);
}

static void
render_open_src_block(ORG_HTML* r, const ORG_BLOCK_SRC_DETAIL* det)
{
    /* Delay it: There may be a caption. (Note we cannot keep the attribute
     * itself; the source text is valid during all the parsing, though.) */
    r->src_pending = 1;
    r->src_lang = det->lang.text;
    r->src_lang_size = det->lang.size;
}

static void
flush_pending_src(ORG_HTML* r)
{
    if(!r->src_pending  ||  r->in_caption)
        return;
    r->src_pending = 0;

    render_open_tag(r, "pre", "", AFF_ID);
    RENDER_VERBATIM(r, "<code");

    /* If known, output the HTML 5 attribute class="language-LANGNAME". */
    if(r->src_lang != NULL) {
        RENDER_VERBATIM(r, " class=\"language-");
        render_html_escaped(r, r->src_lang, r->src_lang_size);
        RENDER_VERBATIM(r, "\"");
    }

    RENDER_VERBATIM(r, ">");
}

static void
flush_pending_p(ORG_HTML* r, int is_figure)
{
    if(!r->p_pending  ||  r->capturing)
        return;
    r->p_pending = 0;
    r->p_is_figure = is_figure;
    if(is_figure) {
        /* The attributes of "#+ATTR_HTML:" go to the image. */
        render_open_tag(r, "div", "figure", AFF_ID);
        RENDER_VERBATIM(r, "\n<p>");
    } else {
        render_open_tag(r, "p", "", AFF_ID | AFF_ATTRS);
    }
}

static void
start_code_block(ORG_HTML* r, unsigned first_line, unsigned line_count)
{
    char buf[16];

    r->in_code_block = 1;
    r->code_line = first_line;
    r->at_code_line_start = 1;
    if(first_line > 0) {
        snprintf(buf, sizeof(buf), "%u", first_line + (line_count > 0 ? line_count - 1 : 0));
        r->code_line_width = (int) strlen(buf);
    }
}

static void
render_code_line_number(ORG_HTML* r)
{
    char buf[64];

    if(!r->in_code_block  ||  !r->at_code_line_start)
        return;
    r->at_code_line_start = 0;
    if(r->code_line == 0)
        return;

    snprintf(buf, sizeof(buf), "<span class=\"linenr\">%*u: </span>", r->code_line_width, r->code_line);
    RENDER_VERBATIM(r, buf);
}

static void
render_open_coderef_span(ORG_HTML* r, const ORG_SPAN_CODEREF_DETAIL* det)
{
    RENDER_VERBATIM(r, "<span id=\"coderef-");
    render_attribute(r, &det->name, render_html_escaped);
    RENDER_VERBATIM(r, "\" class=\"coderef-off\">");
    render_code_line_number(r);
}

static void
render_close_coderef_span(ORG_HTML* r, const ORG_SPAN_CODEREF_DETAIL* det)
{
    if(det->is_label_retained) {
        RENDER_VERBATIM(r, " (");
        render_attribute(r, &det->name, render_html_escaped);
        RENDER_VERBATIM(r, ")");
    }
    RENDER_VERBATIM(r, "</span>");
}

static void
render_open_caption_block(ORG_HTML* r)
{
    char buf[80];

    r->in_caption = 1;
    switch(r->last_block_type) {
        case ORG_BLOCK_TABLE:
            snprintf(buf, sizeof(buf), "<caption class=\"t-above\"><span class=\"table-number\">Table %u:</span> ",
                     ++r->n_tables);
            RENDER_VERBATIM(r, buf);
            break;

        case ORG_BLOCK_SRC:
            snprintf(buf, sizeof(buf), "<label class=\"org-src-name\"><span class=\"listing-number\">Listing %u: </span>",
                     ++r->n_listings);
            RENDER_VERBATIM(r, buf);
            break;

        case ORG_BLOCK_P:
            /* Capture it; it is rendered after the image (if it is one). */
            r->capture_size = 0;
            r->capturing = 1;
            break;

        default:
            break;
    }
}

static void
render_close_caption_block(ORG_HTML* r)
{
    r->in_caption = 0;
    switch(r->last_block_type) {
        case ORG_BLOCK_TABLE:   RENDER_VERBATIM(r, "</caption>\n"); break;
        case ORG_BLOCK_SRC:     RENDER_VERBATIM(r, "</label>"); flush_pending_src(r); break;
        case ORG_BLOCK_P:       r->capturing = 0; r->figure_caption_size = r->capture_size; break;
        default:                break;
    }
}

static void
render_close_p_block(ORG_HTML* r)
{
    char buf[64];

    flush_pending_p(r, 0);

    if(r->p_is_figure) {
        RENDER_VERBATIM(r, "\n</p>\n");
        if(r->figure_caption_size > 0) {
            snprintf(buf, sizeof(buf), "<p><span class=\"figure-number\">Figure %u: </span>", ++r->n_figures);
            RENDER_VERBATIM(r, buf);
            render_verbatim(r, r->capture, r->figure_caption_size);
            RENDER_VERBATIM(r, "</p>\n");
        }
        RENDER_VERBATIM(r, "</div>\n");
    } else {
        RENDER_VERBATIM(r, "</p>\n");
    }

    r->p_is_figure = 0;
    r->figure_caption_size = 0;
    r->aff_first_link = 0;
}

static void
render_open_special_block(ORG_HTML* r, const ORG_BLOCK_SPECIAL_DETAIL* det)
{
    RENDER_VERBATIM(r, "<div");
    render_affiliated_attrs(r, det->name.text, det->name.size, AFF_ID | AFF_ATTRS);
    RENDER_VERBATIM(r, ">\n");
}

static void
render_open_td_block(ORG_HTML* r, const ORG_CHAR* cell_type, const ORG_BLOCK_TD_DETAIL* det)
{
    RENDER_VERBATIM(r, "<");
    RENDER_VERBATIM(r, cell_type);

    switch(det->align) {
        case ORG_ALIGN_LEFT:    RENDER_VERBATIM(r, " align=\"left\">"); break;
        case ORG_ALIGN_CENTER:  RENDER_VERBATIM(r, " align=\"center\">"); break;
        case ORG_ALIGN_RIGHT:   RENDER_VERBATIM(r, " align=\"right\">"); break;
        default:                RENDER_VERBATIM(r, ">"); break;
    }
}

/* Output the path of a "file" link. Links to Org files are made to point to
 * the respective exported HTML files. Of the search options, only a custom ID
 * ("file:foo.org::#id") is kept, as the fragment "#id". (The others refer to
 * the contents of the other file, which we do not see.) */
static void
render_file_link_path(ORG_HTML* r, const ORG_ATTRIBUTE* path,
                      void (*fn_append)(ORG_HTML*, const ORG_CHAR*, ORG_SIZE))
{
    ORG_SIZE size = path->size;
    ORG_SIZE i;

    /* Find the search option (e.g. "file:foo.org::*Heading"). */
    for(i = 0; i + 1 < size; i++) {
        if(path->text[i] == ':'  &&  path->text[i+1] == ':') {
            size = i;
            break;
        }
    }

    if(size > 4  &&  memcmp(path->text + size - 4, ".org", 4) == 0) {
        render_attribute_part(r, path, 0, size - 4, fn_append);
        RENDER_VERBATIM(r, ".html");
    } else {
        render_attribute_part(r, path, 0, size, fn_append);
    }

    if(size + 2 < path->size  &&  path->text[size + 2] == '#')
        render_attribute_part(r, path, size + 2, path->size, fn_append);
}

static void
render_link_href(ORG_HTML* r, const ORG_SPAN_LINK_DETAIL* det)
{
    if(attribute_eq(&det->type, "file")) {
        render_file_link_path(r, &det->path, render_url_escaped);
    } else if(attribute_eq(&det->type, "coderef")) {
        RENDER_VERBATIM(r, "#coderef-");
        render_attribute(r, &det->path, render_url_escaped);
    } else if(attribute_eq(&det->type, "custom-id")) {
        RENDER_VERBATIM(r, "#");
        render_attribute(r, &det->path, render_url_escaped);
    } else if(attribute_eq(&det->type, "radio")) {
        RENDER_VERBATIM(r, "#");
        render_attribute(r, &det->path, render_url_escaped);
    } else if(attribute_eq(&det->type, "id")) {
        RENDER_VERBATIM(r, "#ID-");
        render_attribute(r, &det->path, render_url_escaped);
    } else if(attribute_eq(&det->type, "fuzzy")) {
        /* "*Heading" is a link to a headline. */
        ORG_OFFSET beg = (det->path.size > 0  &&  det->path.text[0] == '*') ? 1 : 0;

        RENDER_VERBATIM(r, "#");
        render_attribute_part(r, &det->path, beg, det->path.size, render_url_escaped);
    } else if(attribute_eq(&det->type, "doi")) {
        RENDER_VERBATIM(r, "https://doi.org/");
        render_attribute(r, &det->path, render_url_escaped);
    } else {
        render_attribute(r, &det->type, render_url_escaped);
        RENDER_VERBATIM(r, ":");
        render_attribute(r, &det->path, render_url_escaped);
    }
}

/* Check whether the text of the link is its rendered path instead of the raw
 * link (as ox-html does for "file" links without a description, e.g.
 * "[[file:notes.org]]" has the text "notes.html"). */
static int
is_path_text_link(const ORG_SPAN_LINK_DETAIL* det)
{
    return (!det->has_description  &&  attribute_eq(&det->type, "file"));
}

static int
is_image_link(const ORG_SPAN_LINK_DETAIL* det)
{
    if(det->has_description)
        return 0;
    if(!attribute_eq(&det->type, "file")  &&  !attribute_eq(&det->type, "http")  &&
       !attribute_eq(&det->type, "https"))
        return 0;
    return is_image_path(&det->path);
}

/* With 'with_attrs', the link gets the attributes of "#+ATTR_HTML:" of its
 * paragraph. (As in ox-html, only the first link of the paragraph gets them;
 * this is how the attributes of an inline image are specified.) */
static void
render_open_link_span(ORG_HTML* r, const ORG_SPAN_LINK_DETAIL* det, int with_attrs)
{
    const char* base_class = (attribute_eq(&det->type, "coderef") ? "coderef" : "");

    if(is_image_link(det)) {
        const ORG_ATTR_HTML* src = (with_attrs ? FIND_ATTR_HTML(r, "src") : NULL);
        const ORG_ATTR_HTML* alt = (with_attrs ? FIND_ATTR_HTML(r, "alt") : NULL);

        RENDER_VERBATIM(r, "<img");
        if(src == NULL) {
            RENDER_VERBATIM(r, " src=\"");
            render_link_href(r, det);
            RENDER_VERBATIM(r, "\"");
        } else if(is_attr_html_output(src)) {
            RENDER_VERBATIM(r, " src=\"");
            render_html_escaped(r, src->value, src->value_size);
            RENDER_VERBATIM(r, "\"");
        }
        if(alt == NULL) {
            ORG_SIZE basename = det->path.size;

            while(basename > 0  &&  det->path.text[basename-1] != '/')
                basename--;
            RENDER_VERBATIM(r, " alt=\"");
            render_attribute_part(r, &det->path, basename, det->path.size, render_html_escaped);
            RENDER_VERBATIM(r, "\"");
        } else if(is_attr_html_output(alt)) {
            RENDER_VERBATIM(r, " alt=\"");
            render_html_escaped(r, alt->value, alt->value_size);
            RENDER_VERBATIM(r, "\"");
        }
        render_affiliated_attrs(r, "", 0, with_attrs ? (AFF_ATTRS | AFF_IMG) : 0);
        RENDER_VERBATIM(r, (r->flags & ORG_HTML_FLAG_XHTML) ? " />" : ">");
        return;
    }

    RENDER_VERBATIM(r, "<a href=\"");
    render_link_href(r, det);
    RENDER_VERBATIM(r, "\"");
    render_affiliated_attrs(r, base_class, (ORG_SIZE) strlen(base_class),
                            with_attrs ? (AFF_ATTRS | AFF_LINK) : 0);
    RENDER_VERBATIM(r, ">");
    if(is_path_text_link(det))
        render_file_link_path(r, &det->path, render_html_escaped);
}

/* Output the identifier of the footnote: Its label, or its number for
 * anonymous footnotes and footnotes with a numeric label (as ox-html does). */
static void
render_footnote_id(ORG_HTML* r, unsigned id, const ORG_ATTRIBUTE* label)
{
    char buf[32];
    int is_numeric = 1;
    ORG_SIZE i;

    for(i = 0; i < label->size; i++) {
        if(!ISDIGIT(label->text[i])) {
            is_numeric = 0;
            break;
        }
    }

    if(label->size > 0  &&  !is_numeric) {
        render_attribute(r, label, render_html_escaped);
    } else {
        snprintf(buf, sizeof(buf), "%u", id);
        RENDER_VERBATIM(r, buf);
    }
}

static void
render_open_footnote_ref_span(ORG_HTML* r, const ORG_SPAN_FOOTNOTE_REF_DETAIL* det)
{
    char buf[32];

    RENDER_VERBATIM(r, "<sup><a id=\"fnr.");
    render_footnote_id(r, det->id, &det->label);
    if(det->ref_id > 1) {
        snprintf(buf, sizeof(buf), ".%u", det->ref_id - 1);
        RENDER_VERBATIM(r, buf);
    }
    RENDER_VERBATIM(r, "\" class=\"footref\" href=\"#fn.");
    render_footnote_id(r, det->id, &det->label);
    snprintf(buf, sizeof(buf), "\">%u</a></sup>", det->id);
    RENDER_VERBATIM(r, buf);
}

static void
render_open_footnote_def_block(ORG_HTML* r, const ORG_BLOCK_FOOTNOTE_DEF_DETAIL* det)
{
    char buf[32];

    RENDER_VERBATIM(r, "<div class=\"footdef\"><sup><a id=\"fn.");
    render_footnote_id(r, det->id, &det->label);
    RENDER_VERBATIM(r, "\" class=\"footnum\" href=\"#fnr.");
    render_footnote_id(r, det->id, &det->label);
    snprintf(buf, sizeof(buf), "\">%u</a></sup> ", det->id);
    RENDER_VERBATIM(r, buf);
    RENDER_VERBATIM(r, "<div class=\"footpara\">");
}

static void
render_open_target_span(ORG_HTML* r, const ORG_SPAN_TARGET_DETAIL* det)
{
    RENDER_VERBATIM(r, "<a id=\"");
    render_attribute(r, &det->name, render_html_escaped);
    /* A radio target has the target text as its contents. */
    RENDER_VERBATIM(r, det->is_radio ? "\">" : "\"></a>");
}

static void
render_open_citation_span(ORG_HTML* r, const ORG_SPAN_CITATION_DETAIL* det)
{
    RENDER_VERBATIM(r, "<span class=\"citation\">(");
    if(det->prefix.size > 0) {
        render_attribute(r, &det->prefix, render_html_escaped);
        RENDER_VERBATIM(r, " ");
    }
    r->n_citation_refs = 0;
}

static void
render_close_citation_span(ORG_HTML* r, const ORG_SPAN_CITATION_DETAIL* det)
{
    if(det->suffix.size > 0) {
        RENDER_VERBATIM(r, " ");
        render_attribute(r, &det->suffix, render_html_escaped);
    }
    RENDER_VERBATIM(r, ")</span>");
}

static void
render_open_citation_reference_span(ORG_HTML* r, const ORG_SPAN_CITATION_REFERENCE_DETAIL* det)
{
    if(r->n_citation_refs++ > 0)
        RENDER_VERBATIM(r, "; ");
    if(det->prefix.size > 0) {
        render_attribute(r, &det->prefix, render_html_escaped);
        RENDER_VERBATIM(r, " ");
    }
    RENDER_VERBATIM(r, "<cite>");
    render_attribute(r, &det->key, render_html_escaped);
    RENDER_VERBATIM(r, "</cite>");
    if(det->suffix.size > 0) {
        RENDER_VERBATIM(r, " ");
        render_attribute(r, &det->suffix, render_html_escaped);
    }
}

static void
render_open_inline_src_span(ORG_HTML* r, const ORG_SPAN_INLINE_SRC_DETAIL* det)
{
    RENDER_VERBATIM(r, "<code class=\"src src-");
    render_attribute(r, &det->lang, render_html_escaped);
    RENDER_VERBATIM(r, "\">");
}

/* Output the dashes delayed by render_text(): As a special string if they are
 * followed by the end of line; otherwise as they are. */
static void
flush_pending_dashes(ORG_HTML* r, int at_line_end)
{
    int n = r->pending_dashes;

    if(n == 0)
        return;
    r->pending_dashes = 0;
    if(at_line_end)
        RENDER_VERBATIM(r, (n == 3) ? "&mdash;" : "&ndash;");
    else
        RENDER_VERBATIM(r, (n == 3) ? "---" : "--");
}

/* Output a normal text, translating the special strings (see
 * ORG_HTML_FLAG_VERBATIM_SPECIAL_STRINGS). */
static void
render_text(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    ORG_OFFSET beg = 0;
    ORG_OFFSET off = 0;

    if(r->flags & ORG_HTML_FLAG_VERBATIM_SPECIAL_STRINGS) {
        render_html_escaped(r, text, size);
        return;
    }

    /* Optimization: As in render_html_escaped(), but stop also on the
     * characters which may start a special string, so that the text is
     * scanned only once. */
    #define NEED_TEXT_ESC(ch)   (r->escape_map[(unsigned char)(ch)] & (NEED_HTML_ESC_FLAG | NEED_SPECIAL_FLAG))

    while(1) {
        const char* special = NULL;
        ORG_SIZE n = 0;

        /* Optimization: Use some loop unrolling. */
        while(off + 3 < size  &&  !NEED_TEXT_ESC(text[off+0])  &&  !NEED_TEXT_ESC(text[off+1])
                              &&  !NEED_TEXT_ESC(text[off+2])  &&  !NEED_TEXT_ESC(text[off+3]))
            off += 4;
        while(off < size  &&  !NEED_TEXT_ESC(text[off]))
            off++;

        if(off >= size)
            break;

        if(r->escape_map[(unsigned char) text[off]] & NEED_HTML_ESC_FLAG) {
            if(off > beg)
                render_verbatim(r, text + beg, off - beg);
            switch(text[off]) {
                case '"':   RENDER_VERBATIM(r, "&quot;"); break;
                case '&':   RENDER_VERBATIM(r, "&amp;"); break;
                case '\'':  RENDER_VERBATIM(r, "&#x27;"); break;
                case '<':   RENDER_VERBATIM(r, "&lt;"); break;
                case '>':   RENDER_VERBATIM(r, "&gt;"); break;
            }
            off++;
            beg = off;
            continue;
        }

        if(text[off] == '\\'  &&  off + 1 < size  &&  text[off+1] == '-') {
            special = "&shy;";
            n = 2;
        } else if(text[off] == '-'  &&  off + 2 < size  &&  text[off+1] == '-'  &&
                  text[off+2] == '-'  &&  (off + 3 == size  ||  text[off+3] != '-'))
        {
            special = "&mdash;";
            n = 3;
        } else if(text[off] == '-'  &&  off + 1 < size  &&  text[off+1] == '-'  &&
                  (off + 2 == size  ||  text[off+2] != '-'))
        {
            special = "&ndash;";
            n = 2;
        } else if(text[off] == '.'  &&  off + 2 < size  &&  text[off+1] == '.'  &&  text[off+2] == '.') {
            special = "&hellip;";
            n = 3;
        }

        if(special != NULL) {
            if(off > beg)
                render_verbatim(r, text + beg, off - beg);
            /* As in ox-html, the dashes at the end of the text are translated
             * only if the end of line follows. We know that only from the next
             * callback, so delay them (see flush_pending_dashes()). */
            if(text[off] == '-'  &&  off + n == size)
                r->pending_dashes = (int) n;
            else
                RENDER_VERBATIM(r, special);
            off += n;
            beg = off;
        } else {
            /* Not a special string: Keep the character in the current run. */
            off++;
        }
    }

    if(off > beg)
        render_verbatim(r, text + beg, off - beg);
}

/* Output a timestamp. Ranges ("<...>--<...>") use an en dash. */
static void
render_timestamp(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    ORG_SIZE i;

    for(i = 0; i + 1 < size; i++) {
        if(text[i] == '-'  &&  text[i+1] == '-'  &&  i > 0  &&  (text[i-1] == '>'  ||  text[i-1] == ']')) {
            render_html_escaped(r, text, i);
            RENDER_VERBATIM(r, "&ndash;");
            render_html_escaped(r, text + i + 2, size - i - 2);
            return;
        }
    }
    render_html_escaped(r, text, size);
}

/* Output the entity (e.g. "\alpha" or "\alpha{}"), as its HTML equivalent. */
static void
render_entity(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    const char* html = NULL;
    ORG_SIZE name_size = size - 1;

    if(name_size >= 2  &&  text[size-2] == '{'  &&  text[size-1] == '}')
        name_size -= 2;
    if(size > 1)
        html = org_entity_html(text + 1, name_size);

    if(html != NULL)
        RENDER_VERBATIM(r, html);
    else
        render_html_escaped(r, text, size);
}

/* Output the LaTeX fragment (to be processed by e.g. MathJax). Following
 * ox-html, "$...$" is converted into "\(...\)" and "$$...$$" into
 * "\[...\]". */
static void
render_latex(ORG_HTML* r, const ORG_CHAR* text, ORG_SIZE size)
{
    if(size >= 4  &&  text[0] == '$'  &&  text[1] == '$') {
        RENDER_VERBATIM(r, "\\[");
        render_html_escaped(r, text + 2, size - 4);
        RENDER_VERBATIM(r, "\\]");
    } else if(size >= 2  &&  text[0] == '$') {
        RENDER_VERBATIM(r, "\\(");
        render_html_escaped(r, text + 1, size - 2);
        RENDER_VERBATIM(r, "\\)");
    } else {
        render_html_escaped(r, text, size);
    }
}


/**************************************
 ***  HTML renderer implementation  ***
 **************************************/

/* Some Org contents is not meant to be exported. */
static int
is_suppressed_block(ORG_HTML* r, ORG_BLOCKTYPE type, void* detail)
{
    switch(type) {
        case ORG_BLOCK_EXPORT:
            return !attribute_eq(&((const ORG_BLOCK_EXPORT_DETAIL*) detail)->backend, "html");

        case ORG_BLOCK_SRC:
        {
            /* Org Babel header argument ":exports". (We never evaluate the
             * code so "results" means nothing is exported.) */
            ORG_ATTRIBUTE exports;

            if(get_header_arg(&((const ORG_BLOCK_SRC_DETAIL*) detail)->params, ":exports", &exports))
                return (attribute_eq(&exports, "none")  ||  attribute_eq(&exports, "results"));
            return 0;
        }

        case ORG_BLOCK_DRAWER:
            /* Org does not export the LOGBOOK drawers by default. */
            return ((r->flags & ORG_HTML_FLAG_NO_DRAWERS)  ||
                    attribute_eq(&((const ORG_BLOCK_DRAWER_DETAIL*) detail)->name, "logbook"));

        case ORG_BLOCK_FOOTNOTE_DEF_SECTION:
            return (r->flags & ORG_HTML_FLAG_NO_FOOTNOTES);

        case ORG_BLOCK_SECTION:
            /* Like ox-html, do not export the footnote section (the footnote
             * definitions in it are output in ORG_BLOCK_FOOTNOTE_DEF_SECTION
             * anyway). */
            return ((const ORG_BLOCK_SECTION_DETAIL*) detail)->is_footnote_section;

        case ORG_BLOCK_COMMENT:
        case ORG_BLOCK_PROPERTY_DRAWER:
        case ORG_BLOCK_PLANNING:
        case ORG_BLOCK_KEYWORD:
            return 1;

        default:
            return 0;
    }
}

static int
enter_block_callback(ORG_BLOCKTYPE type, void* detail, void* userdata)
{
    ORG_HTML* r = (ORG_HTML*) userdata;

    /* E.g. a sub-list after the text of a list item. */
    flush_pending_dashes(r, 1);

    /* The affiliated keywords are reported just before the element they belong
     * to. (The caption is reported as the first child of the element.) */
    if(type == ORG_BLOCK_KEYWORD  &&  ((const ORG_BLOCK_KEYWORD_DETAIL*) detail)->is_affiliated) {
        if(r->suppress_level == 0)
            collect_affiliated_keyword(r, (const ORG_BLOCK_KEYWORD_DETAIL*) detail);
    } else if(type != ORG_BLOCK_CAPTION) {
        if(!r->aff_collecting)
            clear_affiliated(r);
        r->aff_collecting = 0;
    }

    if(r->suppress_level > 0  ||  is_suppressed_block(r, type, detail)) {
        r->suppress_level++;
        return 0;
    }

    /* We render the captions only of tables, source blocks and figures. */
    if(type == ORG_BLOCK_CAPTION) {
        if(r->last_block_type != ORG_BLOCK_TABLE  &&  r->last_block_type != ORG_BLOCK_SRC  &&
           !(r->last_block_type == ORG_BLOCK_P  &&  r->last_block_is_standalone_link))
        {
            r->suppress_level++;
            return 0;
        }
        render_open_caption_block(r);
        return 0;
    }

    r->last_block_type = type;
    r->last_block_is_standalone_link = (type == ORG_BLOCK_P  &&  detail != NULL  &&
                ((const ORG_BLOCK_P_DETAIL*) detail)->is_standalone_link);

    switch(type) {
        case ORG_BLOCK_DOC:             /* noop */ break;
        case ORG_BLOCK_SECTION:         /* noop */ break;
        case ORG_BLOCK_HEADLINE:        render_open_headline_block(r, (const ORG_BLOCK_HEADLINE_DETAIL*) detail); break;
        case ORG_BLOCK_INLINETASK:      RENDER_VERBATIM(r, "<div class=\"inlinetask\">\n"); r->in_inlinetask_title = 1; break;
        case ORG_BLOCK_P:
            r->aff_first_link = (r->n_aff_attrs > 0);
            if(r->last_block_is_standalone_link)
                r->p_pending = 1;       /* It might be a figure. */
            else
                render_open_tag(r, "p", "", AFF_ID | AFF_ATTRS);
            break;
        case ORG_BLOCK_UL:              render_open_tag(r, "ul", "", AFF_ID | AFF_ATTRS); RENDER_VERBATIM(r, "\n"); break;
        case ORG_BLOCK_OL:              render_open_tag(r, "ol", "", AFF_ID | AFF_ATTRS); RENDER_VERBATIM(r, "\n"); break;
        case ORG_BLOCK_DL:              render_open_tag(r, "dl", "", AFF_ID | AFF_ATTRS); RENDER_VERBATIM(r, "\n"); break;
        case ORG_BLOCK_LI:              render_open_li_block(r, "li", (const ORG_BLOCK_LI_DETAIL*) detail); break;
        case ORG_BLOCK_DT:              render_open_li_block(r, "dt", (const ORG_BLOCK_LI_DETAIL*) detail); break;
        case ORG_BLOCK_DD:              RENDER_VERBATIM(r, "<dd>"); break;
        case ORG_BLOCK_HR:              RENDER_VERBATIM(r, (r->flags & ORG_HTML_FLAG_XHTML) ? "<hr />\n" : "<hr>\n"); break;
        case ORG_BLOCK_TABLE:           render_open_tag(r, "table", "", AFF_ID | AFF_ATTRS); RENDER_VERBATIM(r, "\n"); break;
        case ORG_BLOCK_THEAD:           RENDER_VERBATIM(r, "<thead>\n"); break;
        case ORG_BLOCK_TBODY:           RENDER_VERBATIM(r, "<tbody>\n"); break;
        case ORG_BLOCK_TR:              RENDER_VERBATIM(r, "<tr>\n"); break;
        case ORG_BLOCK_TH:              render_open_td_block(r, "th", (ORG_BLOCK_TD_DETAIL*)detail); break;
        case ORG_BLOCK_TD:              render_open_td_block(r, "td", (ORG_BLOCK_TD_DETAIL*)detail); break;
        case ORG_BLOCK_SRC:
            render_open_src_block(r, (const ORG_BLOCK_SRC_DETAIL*) detail);
            start_code_block(r, ((const ORG_BLOCK_SRC_DETAIL*) detail)->first_line_number,
                             ((const ORG_BLOCK_SRC_DETAIL*) detail)->line_count);
            break;
        case ORG_BLOCK_EXAMPLE:
            render_open_tag(r, "pre", "example", AFF_ID | AFF_ATTRS);
            start_code_block(r, ((const ORG_BLOCK_EXAMPLE_DETAIL*) detail)->first_line_number,
                             ((const ORG_BLOCK_EXAMPLE_DETAIL*) detail)->line_count);
            break;
        case ORG_BLOCK_FIXED_WIDTH:     render_open_tag(r, "pre", "example", AFF_ID); break;
        case ORG_BLOCK_EXPORT:          /* noop */ break;
        case ORG_BLOCK_VERSE:           render_open_tag(r, "p", "verse", AFF_ID); r->verse_level++; r->at_verse_line_start = 1; break;
        case ORG_BLOCK_QUOTE:           render_open_tag(r, "blockquote", "", AFF_ID | AFF_ATTRS); RENDER_VERBATIM(r, "\n"); break;
        case ORG_BLOCK_CENTER:          render_open_tag(r, "div", "org-center", AFF_ID); RENDER_VERBATIM(r, "\n"); break;
        case ORG_BLOCK_SPECIAL:         render_open_special_block(r, (const ORG_BLOCK_SPECIAL_DETAIL*) detail); break;
        case ORG_BLOCK_DRAWER:          /* noop */ break;
        case ORG_BLOCK_COMMENT:         ORG_FALLTHROUGH();
        case ORG_BLOCK_PROPERTY_DRAWER: ORG_FALLTHROUGH();
        case ORG_BLOCK_PLANNING:        ORG_FALLTHROUGH();
        case ORG_BLOCK_KEYWORD:         /* noop (suppressed) */ break;
        case ORG_BLOCK_LATEX_ENVIRONMENT:   r->in_latex_env = 1; break;
        case ORG_BLOCK_FOOTNOTE_DEF_SECTION:
            RENDER_VERBATIM(r, "<div id=\"footnotes\">\n<h2 class=\"footnotes\">Footnotes:</h2>\n<div id=\"text-footnotes\">\n");
            break;
        case ORG_BLOCK_FOOTNOTE_DEF:    render_open_footnote_def_block(r, (const ORG_BLOCK_FOOTNOTE_DEF_DETAIL*) detail); break;
        case ORG_BLOCK_DYNAMIC:         /* noop */ break;
        case ORG_BLOCK_CAPTION:         /* handled above */ break;
    }

    /* An allocation of the buffer for delayed output has failed. */
    return (r->out_of_memory ? -1 : 0);
}

static int
leave_block_callback(ORG_BLOCKTYPE type, void* detail, void* userdata)
{
    ORG_HTML* r = (ORG_HTML*) userdata;

    /* The end of a paragraph (or of a list item without it) is the end of
     * line, unlike the end of a headline, a term or a table cell. */
    flush_pending_dashes(r, (type != ORG_BLOCK_HEADLINE  &&  type != ORG_BLOCK_DT  &&
                             type != ORG_BLOCK_TH  &&  type != ORG_BLOCK_TD  &&
                             type != ORG_BLOCK_CAPTION));

    if(r->suppress_level > 0) {
        r->suppress_level--;
        return 0;
    }

    switch(type) {
        case ORG_BLOCK_DOC:             /* noop */ break;
        case ORG_BLOCK_SECTION:         /* noop */ break;
        case ORG_BLOCK_HEADLINE:        render_close_headline_block(r, (const ORG_BLOCK_HEADLINE_DETAIL*) detail); break;
        case ORG_BLOCK_INLINETASK:      RENDER_VERBATIM(r, "</div>\n"); break;
        case ORG_BLOCK_P:               render_close_p_block(r); break;
        case ORG_BLOCK_UL:              RENDER_VERBATIM(r, "</ul>\n"); break;
        case ORG_BLOCK_OL:              RENDER_VERBATIM(r, "</ol>\n"); break;
        case ORG_BLOCK_DL:              RENDER_VERBATIM(r, "</dl>\n"); break;
        case ORG_BLOCK_LI:              RENDER_VERBATIM(r, "</li>\n"); break;
        case ORG_BLOCK_DT:              RENDER_VERBATIM(r, "</dt>\n"); break;
        case ORG_BLOCK_DD:              RENDER_VERBATIM(r, "</dd>\n"); break;
        case ORG_BLOCK_HR:              /* noop */ break;
        case ORG_BLOCK_TABLE:           RENDER_VERBATIM(r, "</table>\n"); break;
        case ORG_BLOCK_THEAD:           RENDER_VERBATIM(r, "</thead>\n"); break;
        case ORG_BLOCK_TBODY:           RENDER_VERBATIM(r, "</tbody>\n"); break;
        case ORG_BLOCK_TR:              RENDER_VERBATIM(r, "</tr>\n"); break;
        case ORG_BLOCK_TH:              RENDER_VERBATIM(r, "</th>\n"); break;
        case ORG_BLOCK_TD:              RENDER_VERBATIM(r, "</td>\n"); break;
        case ORG_BLOCK_SRC:             flush_pending_src(r); RENDER_VERBATIM(r, "</code></pre>\n"); r->in_code_block = 0; break;
        case ORG_BLOCK_EXAMPLE:         r->in_code_block = 0; ORG_FALLTHROUGH();
        case ORG_BLOCK_FIXED_WIDTH:     RENDER_VERBATIM(r, "</pre>\n"); break;
        case ORG_BLOCK_EXPORT:          /* noop */ break;
        case ORG_BLOCK_VERSE:           RENDER_VERBATIM(r, "</p>\n"); r->verse_level--; break;
        case ORG_BLOCK_QUOTE:           RENDER_VERBATIM(r, "</blockquote>\n"); break;
        case ORG_BLOCK_CENTER:          ORG_FALLTHROUGH();
        case ORG_BLOCK_SPECIAL:         RENDER_VERBATIM(r, "</div>\n"); break;
        case ORG_BLOCK_DRAWER:          /* noop */ break;
        case ORG_BLOCK_COMMENT:         ORG_FALLTHROUGH();
        case ORG_BLOCK_PROPERTY_DRAWER: ORG_FALLTHROUGH();
        case ORG_BLOCK_PLANNING:        ORG_FALLTHROUGH();
        case ORG_BLOCK_KEYWORD:         /* noop (suppressed) */ break;
        case ORG_BLOCK_LATEX_ENVIRONMENT:   r->in_latex_env = 0; break;
        case ORG_BLOCK_FOOTNOTE_DEF_SECTION:    RENDER_VERBATIM(r, "</div>\n</div>\n"); break;
        case ORG_BLOCK_FOOTNOTE_DEF:    RENDER_VERBATIM(r, "</div></div>\n"); break;
        case ORG_BLOCK_DYNAMIC:         /* noop */ break;
        case ORG_BLOCK_CAPTION:         render_close_caption_block(r); break;
    }

    /* An allocation of the buffer for delayed output has failed. */
    return (r->out_of_memory ? -1 : 0);
}

static int
enter_span_callback(ORG_SPANTYPE type, void* detail, void* userdata)
{
    ORG_HTML* r = (ORG_HTML*) userdata;
    int inside_raw_link = (r->raw_link_nesting_level > 0);

    flush_pending_dashes(r, 0);

    if(r->suppress_level > 0)
        return 0;

    /* We are inside an image, or a link showing its path. Its contents (the
     * raw link) is not rendered as render_open_link_span() has rendered the
     * image, or the path, already. */
    if(inside_raw_link) {
        if(type == ORG_SPAN_LINK)
            r->raw_link_nesting_level++;
        return 0;
    }

    r->at_verse_line_start = 0;
    flush_pending_src(r);
    flush_pending_p(r, type == ORG_SPAN_LINK  &&  is_image_link((const ORG_SPAN_LINK_DETAIL*) detail));

    if(type == ORG_SPAN_EXPORT_SNIPPET) {
        if(!attribute_eq(&((const ORG_SPAN_EXPORT_SNIPPET_DETAIL*) detail)->backend, "html"))
            r->in_hidden_snippet = 1;
        return 0;
    }

    switch(type) {
        case ORG_SPAN_BOLD:             RENDER_VERBATIM(r, "<b>"); break;
        case ORG_SPAN_ITALIC:           RENDER_VERBATIM(r, "<i>"); break;
        case ORG_SPAN_UNDERLINE:        RENDER_VERBATIM(r, "<span class=\"underline\">"); break;
        case ORG_SPAN_STRIKE:           RENDER_VERBATIM(r, "<del>"); break;
        case ORG_SPAN_VERBATIM:         ORG_FALLTHROUGH();
        case ORG_SPAN_CODE:             RENDER_VERBATIM(r, "<code>"); break;
        case ORG_SPAN_LINK:
        {
            /* (A link in the caption is not a part of the paragraph.) */
            int with_attrs = (r->aff_first_link  &&  !r->in_caption);

            if(with_attrs)
                r->aff_first_link = 0;
            render_open_link_span(r, (const ORG_SPAN_LINK_DETAIL*) detail, with_attrs);
            if(is_image_link((const ORG_SPAN_LINK_DETAIL*) detail)) {
                r->raw_link_is_image = 1;
                r->raw_link_nesting_level++;
            } else if(is_path_text_link((const ORG_SPAN_LINK_DETAIL*) detail)) {
                r->raw_link_is_image = 0;
                r->raw_link_nesting_level++;
            }
            break;
        }
        case ORG_SPAN_FOOTNOTE_REF:     if(!(r->flags & ORG_HTML_FLAG_NO_FOOTNOTES))
                                            render_open_footnote_ref_span(r, (const ORG_SPAN_FOOTNOTE_REF_DETAIL*) detail);
                                        break;
        case ORG_SPAN_TIMESTAMP:        RENDER_VERBATIM(r, "<span class=\"timestamp-wrapper\"><span class=\"timestamp\">"); r->in_timestamp = 1; break;
        case ORG_SPAN_LATEX:            /* noop */ break;
        case ORG_SPAN_SUBSCRIPT:        RENDER_VERBATIM(r, "<sub>"); break;
        case ORG_SPAN_SUPERSCRIPT:      RENDER_VERBATIM(r, "<sup>"); break;
        case ORG_SPAN_INLINE_SRC:       render_open_inline_src_span(r, (const ORG_SPAN_INLINE_SRC_DETAIL*) detail); break;
        case ORG_SPAN_EXPORT_SNIPPET:   /* handled above */ break;
        case ORG_SPAN_STATISTICS_COOKIE:    RENDER_VERBATIM(r, "<code>"); break;
        case ORG_SPAN_TARGET:           render_open_target_span(r, (const ORG_SPAN_TARGET_DETAIL*) detail); break;
        case ORG_SPAN_MACRO:            /* noop */ break;
        case ORG_SPAN_CITATION:         render_open_citation_span(r, (const ORG_SPAN_CITATION_DETAIL*) detail); break;
        case ORG_SPAN_CITATION_REFERENCE:   render_open_citation_reference_span(r, (const ORG_SPAN_CITATION_REFERENCE_DETAIL*) detail); break;
        case ORG_SPAN_INLINE_BABEL_CALL:    /* noop (we do not evaluate any code) */ break;
        case ORG_SPAN_CODEREF:          render_open_coderef_span(r, (const ORG_SPAN_CODEREF_DETAIL*) detail); break;
    }

    /* An allocation of the buffer for delayed output has failed. */
    return (r->out_of_memory ? -1 : 0);
}

static int
leave_span_callback(ORG_SPANTYPE type, void* detail, void* userdata)
{
    ORG_HTML* r = (ORG_HTML*) userdata;

    flush_pending_dashes(r, 0);

    if(r->suppress_level > 0)
        return 0;

    if(type == ORG_SPAN_EXPORT_SNIPPET) {
        r->in_hidden_snippet = 0;
        return 0;
    }

    if(r->raw_link_nesting_level > 0) {
        if(type == ORG_SPAN_LINK) {
            r->raw_link_nesting_level--;
            if(r->raw_link_nesting_level == 0  &&  !r->raw_link_is_image)
                RENDER_VERBATIM(r, "</a>");
        }
        return 0;
    }

    switch(type) {
        case ORG_SPAN_BOLD:             RENDER_VERBATIM(r, "</b>"); break;
        case ORG_SPAN_ITALIC:           RENDER_VERBATIM(r, "</i>"); break;
        case ORG_SPAN_UNDERLINE:        RENDER_VERBATIM(r, "</span>"); break;
        case ORG_SPAN_STRIKE:           RENDER_VERBATIM(r, "</del>"); break;
        case ORG_SPAN_VERBATIM:         ORG_FALLTHROUGH();
        case ORG_SPAN_CODE:             RENDER_VERBATIM(r, "</code>"); break;
        case ORG_SPAN_LINK:             RENDER_VERBATIM(r, "</a>"); break;
        case ORG_SPAN_FOOTNOTE_REF:     /* noop (fully rendered on enter) */ break;
        case ORG_SPAN_TIMESTAMP:        RENDER_VERBATIM(r, "</span></span>"); r->in_timestamp = 0; break;
        case ORG_SPAN_LATEX:            /* noop */ break;
        case ORG_SPAN_SUBSCRIPT:        RENDER_VERBATIM(r, "</sub>"); break;
        case ORG_SPAN_SUPERSCRIPT:      RENDER_VERBATIM(r, "</sup>"); break;
        case ORG_SPAN_INLINE_SRC:       RENDER_VERBATIM(r, "</code>"); break;
        case ORG_SPAN_EXPORT_SNIPPET:   /* handled above */ break;
        case ORG_SPAN_STATISTICS_COOKIE:    RENDER_VERBATIM(r, "</code>"); break;
        case ORG_SPAN_TARGET:           if(((const ORG_SPAN_TARGET_DETAIL*) detail)->is_radio)
                                            RENDER_VERBATIM(r, "</a>");
                                        break;
        case ORG_SPAN_MACRO:            /* noop */ break;
        case ORG_SPAN_CITATION:         render_close_citation_span(r, (const ORG_SPAN_CITATION_DETAIL*) detail); break;
        case ORG_SPAN_CITATION_REFERENCE:   /* noop (fully rendered on enter) */ break;
        case ORG_SPAN_INLINE_BABEL_CALL:    /* noop */ break;
        case ORG_SPAN_CODEREF:          render_close_coderef_span(r, (const ORG_SPAN_CODEREF_DETAIL*) detail); break;
    }

    /* An allocation of the buffer for delayed output has failed. */
    return (r->out_of_memory ? -1 : 0);
}

static int
text_callback(ORG_TEXTTYPE type, const ORG_CHAR* text, ORG_SIZE size, void* userdata)
{
    ORG_HTML* r = (ORG_HTML*) userdata;

    /* (Outside verse, ORG_TEXT_BR is the line break "\\", i.e. an object
     * following the text. In verse, it may be also "\\"; we cannot tell.) */
    flush_pending_dashes(r, (type == ORG_TEXT_SOFTBR  ||  (type == ORG_TEXT_BR  &&  r->verse_level > 0)  ||
                             (type == ORG_TEXT_NORMAL  &&  size > 0  &&  text[0] != '-')));

    if(r->suppress_level > 0  ||  r->raw_link_nesting_level > 0  ||  r->in_hidden_snippet)
        return 0;

    flush_pending_src(r);
    flush_pending_p(r, 0);

    /* Line numbers of a code block. */
    if(r->in_code_block  &&  type == ORG_TEXT_CODE) {
        render_code_line_number(r);
        if(size == 1  &&  text[0] == '\n') {
            if(r->code_line > 0)
                r->code_line++;
            r->at_code_line_start = 1;
        }
    }

    /* Preserve indentation of verse lines. */
    if(r->at_verse_line_start  &&  type == ORG_TEXT_NORMAL) {
        while(size > 0  &&  *text == ' ') {
            RENDER_VERBATIM(r, "&#xa0;");
            text++;
            size--;
        }
    }
    r->at_verse_line_start = 0;

    switch(type) {
        case ORG_TEXT_NULLCHAR:  render_utf8_codepoint(r, 0x0000, render_verbatim); break;
        case ORG_TEXT_BR:        RENDER_VERBATIM(r, (r->flags & ORG_HTML_FLAG_XHTML) ? "<br />\n" : "<br>\n");
                                 if(r->verse_level > 0)
                                     r->at_verse_line_start = 1;
                                 break;
        case ORG_TEXT_SOFTBR:    if(r->flags & ORG_HTML_FLAG_HARD_SOFT_BREAKS)
                                     RENDER_VERBATIM(r, (r->flags & ORG_HTML_FLAG_XHTML) ? "<br />\n" : "<br>\n");
                                 else
                                     RENDER_VERBATIM(r, "\n");
                                 break;
        case ORG_TEXT_EXPORT:    render_verbatim(r, text, size); break;
        case ORG_TEXT_ENTITY:    if(r->flags & ORG_HTML_FLAG_VERBATIM_ENTITIES)
                                     render_html_escaped(r, text, size);
                                 else
                                     render_entity(r, text, size);
                                 break;
        case ORG_TEXT_NORMAL:    if(r->in_timestamp)
                                     render_timestamp(r, text, size);
                                 else
                                     render_text(r, text, size);
                                 break;
        case ORG_TEXT_LATEX:     if(r->in_latex_env)
                                     render_html_escaped(r, text, size);
                                 else
                                     render_latex(r, text, size);
                                 break;
        default:                 render_html_escaped(r, text, size); break;
    }

    /* An allocation of the buffer for delayed output has failed. */
    return (r->out_of_memory ? -1 : 0);
}

static void
debug_log_callback(const char* msg, void* userdata)
{
    ORG_HTML* r = (ORG_HTML*) userdata;
    if(r->flags & ORG_HTML_FLAG_DEBUG)
        fprintf(stderr, "ORG4C: %s\n", msg);
}

/* Apply a single option "key:value" of "#+OPTIONS:". */
static void
apply_option(const ORG_CHAR* opt, ORG_SIZE size, unsigned* p_parser_flags, unsigned* p_renderer_flags)
{
    static const struct {
        const char* key;
        int is_renderer_flag;
        unsigned flag;
        int flag_means_nil;     /* The flag is set when the value is "nil". */
    } options[] = {
        { "tex",    0, ORG_FLAG_NOLATEX,                            1 },
        { "-",      1, ORG_HTML_FLAG_VERBATIM_SPECIAL_STRINGS,      1 },
        { "e",      1, ORG_HTML_FLAG_VERBATIM_ENTITIES,             1 },
        { "todo",   1, ORG_HTML_FLAG_NO_TODO,                       1 },
        { "pri",    1, ORG_HTML_FLAG_NO_PRIORITY,                   1 },
        { "tags",   1, ORG_HTML_FLAG_NO_TAGS,                       1 },
        { "f",      1, ORG_HTML_FLAG_NO_FOOTNOTES,                  1 },
        { "d",      1, ORG_HTML_FLAG_NO_DRAWERS,                    1 },
        { "\\n",    1, ORG_HTML_FLAG_HARD_SOFT_BREAKS,              0 }
    };
    const ORG_CHAR* value;
    ORG_SIZE key_size = 0;
    ORG_SIZE value_size;
    int is_nil;
    int i;

    while(key_size < size  &&  opt[key_size] != ':')
        key_size++;
    if(key_size >= size)
        return;
    value = opt + key_size + 1;
    value_size = size - key_size - 1;
    is_nil = (value_size == 3  &&  memcmp(value, "nil", 3) == 0);

    /* Archived subtrees: "headline", "nil" or "t" (any other value). */
    if(key_size == 4  &&  memcmp(opt, "arch", 4) == 0) {
        *p_parser_flags &= ~ORG_FLAG_SKIPARCHIVED;
        *p_renderer_flags &= ~ORG_HTML_FLAG_ARCHIVED_TREES;
        if(is_nil)
            *p_parser_flags |= ORG_FLAG_SKIPARCHIVED;
        else if(!(value_size == 8  &&  memcmp(value, "headline", 8) == 0))
            *p_renderer_flags |= ORG_HTML_FLAG_ARCHIVED_TREES;
        return;
    }

    /* Sub/superscripts: "t", "nil" or "{}". */
    if(key_size == 1  &&  opt[0] == '^') {
        *p_parser_flags &= ~(ORG_FLAG_SUBSUPERSCRIPTS | ORG_FLAG_SUBSUPERSCRIPTS_BRACED);
        if(value_size == 2  &&  memcmp(value, "{}", 2) == 0)
            *p_parser_flags |= ORG_FLAG_SUBSUPERSCRIPTS_BRACED;
        else if(!is_nil)
            *p_parser_flags |= ORG_FLAG_SUBSUPERSCRIPTS;
        return;
    }

    for(i = 0; i < (int) (sizeof(options) / sizeof(options[0])); i++) {
        unsigned* p_flags = options[i].is_renderer_flag ? p_renderer_flags : p_parser_flags;
        int set;

        if(key_size != strlen(options[i].key)  ||  memcmp(opt, options[i].key, key_size) != 0)
            continue;

        /* "tex:verbatim" is (for us) the same as "tex:nil". */
        if(options[i].flag == ORG_FLAG_NOLATEX  &&  !options[i].is_renderer_flag  &&
           value_size == 8  &&  memcmp(value, "verbatim", 8) == 0)
            is_nil = 1;

        set = options[i].flag_means_nil ? is_nil : !is_nil;
        if(set)
            *p_flags |= options[i].flag;
        else
            *p_flags &= ~options[i].flag;
        return;
    }
}

/* Honor the "#+OPTIONS:" lines of the document. */
static void
apply_in_buffer_options(const ORG_CHAR* input, ORG_SIZE size,
                        unsigned* p_parser_flags, unsigned* p_renderer_flags)
{
    ORG_SIZE off = 0;

    while(off < size) {
        ORG_SIZE line_end = off;

        while(line_end < size  &&  input[line_end] != '\n'  &&  input[line_end] != '\r')
            line_end++;
        while(off < line_end  &&  ISBLANK(input[off]))
            off++;

        if(line_end - off >= 10  &&  input[off] == '#'  &&  input[off+1] == '+') {
            static const char key[] = "OPTIONS:";
            ORG_SIZE i;

            for(i = 0; i < 8; i++) {
                ORG_CHAR ch = input[off + 2 + i];
                if(ISLOWER(ch))
                    ch += 'A' - 'a';
                if(ch != key[i])
                    break;
            }

            if(i == 8) {
                off += 10;
                while(off < line_end) {
                    ORG_SIZE opt_end;

                    while(off < line_end  &&  ISBLANK(input[off]))
                        off++;
                    opt_end = off;
                    while(opt_end < line_end  &&  input[opt_end] != ' '  &&  input[opt_end] != '\t')
                        opt_end++;
                    if(opt_end > off)
                        apply_option(input + off, opt_end - off, p_parser_flags, p_renderer_flags);
                    off = opt_end;
                }
            }
        }

        off = line_end + 1;
    }
}

int
org_html(const ORG_CHAR* input, ORG_SIZE input_size,
         void (*process_output)(const ORG_CHAR*, ORG_SIZE, void*),
         void* userdata, unsigned parser_flags, unsigned renderer_flags)
{
    ORG_HTML render;
    int ret;
    int i;

    ORG_PARSER parser = {
        0,
        parser_flags,
        enter_block_callback,
        leave_block_callback,
        enter_span_callback,
        leave_span_callback,
        text_callback,
        debug_log_callback,
        NULL
    };

    /* Consider skipping UTF-8 byte order mark (BOM). (Before looking for the
     * in-buffer options, so that they are found also on the first line.) */
    if(renderer_flags & ORG_HTML_FLAG_SKIP_UTF8_BOM  &&  sizeof(ORG_CHAR) == 1) {
        static const ORG_CHAR bom[3] = { (char)0xef, (char)0xbb, (char)0xbf };
        if(input_size >= sizeof(bom)  &&  memcmp(input, bom, sizeof(bom)) == 0) {
            input += sizeof(bom);
            input_size -= sizeof(bom);
        }
    }

    if(renderer_flags & ORG_HTML_FLAG_IN_BUFFER_OPTIONS)
        apply_in_buffer_options(input, input_size, &parser_flags, &renderer_flags);
    /* The renderer never outputs the commented subtrees and the ones tagged
     * "noexport", so the parser does not need to report them. (Otherwise, it
     * would count also the footnotes referenced only from them.) The same
     * holds for the contents of the archived subtrees, by default. */
    parser.flags = parser_flags | ORG_FLAG_SKIPCOMMENTED | ORG_FLAG_SKIPNOEXPORT;
    if(!(renderer_flags & ORG_HTML_FLAG_ARCHIVED_TREES))
        parser.flags |= ORG_FLAG_SKIPARCHIVEDCONTENTS;

    memset(&render, 0, sizeof(render));
    render.process_output = process_output;
    render.userdata = userdata;
    render.flags = renderer_flags;

    /* Build map of characters which need escaping. */
    for(i = 0; i < 256; i++) {
        unsigned char ch = (unsigned char) i;

        if(strchr("\"&'<>", ch) != NULL)
            render.escape_map[i] |= NEED_HTML_ESC_FLAG;

        if(!ISALNUM(ch)  &&  strchr("~-_.+!*(),%#@?=;:/$", ch) == NULL)
            render.escape_map[i] |= NEED_URL_ESC_FLAG;

        if(ch == '\\'  ||  ch == '-'  ||  ch == '.')
            render.escape_map[i] |= NEED_SPECIAL_FLAG;
    }

    ret = org_parse(input, input_size, &parser, (void*) &render);
    free(render.capture);
    return ret;
}
