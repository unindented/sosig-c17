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

#ifndef ORG4C_ENTITY_H
#define ORG4C_ENTITY_H

#include <stdlib.h>


/* Org entity (e.g. "\alpha") and its HTML representation (e.g. "&alpha;"). */
typedef struct ORG_ENTITY_tag ORG_ENTITY;
struct ORG_ENTITY_tag {
    const char* name;
    const char* html;
};

const ORG_ENTITY* org_entity_lookup(const char* name, size_t name_size);


#endif  /* ORG4C_ENTITY_H */
