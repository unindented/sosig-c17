#ifndef SOSIG_TEMPLATE_H
#define SOSIG_TEMPLATE_H

#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct ContentEntry;
struct Log;
struct Os;
struct SiteConfig;

/**
 * Largest rendered output accepted from one template render, in bytes. `template.c` explains why
 * the bound sits where it does. The build also sizes its perms from it, because a render's output
 * lives in the caller's perm.
 */
enum { RENDER_OUTPUT_LEN_MAX = 256 * 1024 * 1024 };

/**
 * Largest template or partial file read, in bytes.
 *
 * A render reads its template and compiles up to `RENDER_PARTIAL_COUNT_MAX` (in `template.c`)
 * distinct partials, and the compiled form keeps a copy of every literal run, so 4 MiB each bounds
 * one render's compiled sources at 260 MiB, the same order as `RENDER_OUTPUT_LEN_MAX`. A
 * hand-written template with inline styles, scripts and SVG is tens of kilobytes, so this sits two
 * orders of magnitude above one. The read checks it before loading the file, so an oversize
 * template is never resident.
 */
enum { TEMPLATE_FILE_LEN_MAX = 4 * 1024 * 1024 };

/**
 * Text that starts a page render's attribution, which ends its failure message as
 * `(while rendering '<source>')`. The page phase passes it to `job_run`, so one template failure
 * that every page repeats is reported once.
 */
#define TEMPLATE_RENDER_ATTRIBUTION " (while rendering '"

/**
 * Borrowed data visible to templates during one render call. Every pointer is `const`, and that is
 * a thread-safety invariant. During the page phase every worker holds pointers to every entry, and
 * the `const` stops one job writing an entry another job is reading. Do not relax it to add a
 * mutable field.
 */
struct TemplateContext {
  /** Site configuration available to site-level template variables. Must not be `NULL`. */
  const struct SiteConfig* site_config;

  /** Content entries available to `{{#content_entries}}` section blocks. */
  const struct ContentEntry* const* content_entries;

  /** Number of entries in `content_entries`. */
  ptrdiff_t content_entry_count;

  /** Current entry for content templates, or `NULL` for aggregate and feed templates. */
  const struct ContentEntry* content_entry_current;

  /** Site last-updated timestamp exposed as `site.updated`. */
  struct Str site_updated;
};

/**
 * @brief Renders a template file into HTML using the given context.
 *
 * Loads `template_name` from `templates_dir`, renders it with mustache4c against `context`, and
 * builds the result in `perm`. The template name must be a safe relative path. Everything else the
 * render allocates goes into `scratch`, except mustache4c's compiled templates, which it releases
 * before returning.
 *
 * Templates are trusted input. The render limits terminate a partial that includes itself, bound
 * the output buffer, and bound the compiled-partial cache. They do not bound the render's total
 * memory: a template's own nested sections allocate in proportion to the iterations they ask for.
 *
 * @param templates_dir Template directory root.
 * @param template_name Safe relative template name within `templates_dir`.
 * @param context       Borrowed data visible to the template during this render. Must not be
 *                      `NULL`.
 * @param os            Host the template and its partials are read from. Must not be `NULL`.
 * @param perm          Arena that owns the returned HTML. Nothing else in the render allocates
 *                      there, so the output grows in place. A failed render may leave its partial
 *                      output there, so a caller that must not keep it passes a copy. Must not be
 *                      `NULL`.
 * @param scratch       Arena for the render's temporaries, released when it returns. Must not
 *                      overlap `perm`.
 * @param html_out      Receives the HTML, owned by `perm`, on success. Must not be `NULL`.
 * @param log           Log that receives the render's first failure: an unreadable template or
 *                      partial, a syntax error, or an exceeded limit. A page render, one with
 *                      `content_entry_current` set, ends the message with
 *                      `(while rendering '<source>')`, because the template is shared by every
 *                      page. The log's arena must not overlap `perm` or `scratch`. Must not be
 *                      `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
int template_render_file(struct Str templates_dir,
                         struct Str template_name,
                         const struct TemplateContext* context,
                         struct Os* os,
                         struct Arena* perm,
                         struct Arena scratch,
                         struct Str* html_out,
                         struct Log* log) __attribute__((nonnull(3, 4, 5, 7, 8)));

/**
 * @brief Renders a template file into HTML under a caller-chosen output bound.
 *
 * Behaves as `template_render_file`, which calls it with the production bound. Reaching a byte
 * bound costs that many bytes, so this lets a test assert the output-limit diagnostic cheaply.
 *
 * @param templates_dir  Template directory root.
 * @param template_name  Safe relative template name within `templates_dir`.
 * @param context        Borrowed data visible to the template during this render. Must not be
 *                       `NULL`.
 * @param output_len_max Largest rendered output accepted, in bytes. A render whose accumulated
 *                       output passes it fails with a diagnostic naming it.
 * @param os             Host the template is read from, as for `template_render_file`. Must not be
 *                       `NULL`.
 * @param perm           Arena that owns the returned HTML, as for `template_render_file`, which may
 *                       hold partial output after a failed render. Must not be `NULL`.
 * @param scratch        Arena for the render's temporaries, as for `template_render_file`.
 * @param html_out       Receives the HTML, as for `template_render_file`. Must not be `NULL`.
 * @param log            Log that receives the render's first failure, as for
 *                       `template_render_file`. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
int template_render_file_limited(struct Str templates_dir,
                                 struct Str template_name,
                                 const struct TemplateContext* context,
                                 ptrdiff_t output_len_max,
                                 struct Os* os,
                                 struct Arena* perm,
                                 struct Arena scratch,
                                 struct Str* html_out,
                                 struct Log* log) __attribute__((nonnull(3, 5, 6, 8, 9)));

#endif
