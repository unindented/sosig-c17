#ifndef SOSIG_TEMPLATE_H
#define SOSIG_TEMPLATE_H

#include <stddef.h>

struct ContentEntry;
struct SiteConfig;

/**
 * Borrowed data visible to templates during one render call. Every pointer is `const`, and that is
 * a thread-safety invariant. During the page pass every worker holds pointers to every entry, and
 * the `const` stops one job allocating into another job's arena. Do not relax it to add a mutable
 * field.
 */
struct TemplateContext {
  /** Site configuration available to site-level template variables. */
  const struct SiteConfig* site_config;

  /** Content entries available to `{{#content_entries}}` section blocks. */
  const struct ContentEntry* const* content_entries;

  /** Number of entries in `content_entries`. */
  size_t content_entry_count;

  /** Current entry for content templates, or `NULL` for aggregate and feed templates. */
  const struct ContentEntry* content_entry_current;

  /** Site last-updated timestamp exposed as `site.updated`, or `NULL` when unavailable. */
  const char* site_updated;
};

/**
 * @brief Renders a template file into HTML using the given context.
 *
 * Loads `template_name` from `templates_dir`, renders it with mustache4c against `context`, and
 * transfers ownership of the result to the caller. The template name must be a safe relative path.
 *
 * @param templates_dir Template directory root. Must not be `NULL`.
 * @param template_name Safe relative template name within `templates_dir`. Must not be `NULL`.
 * @param context       Borrowed data visible to the template during this render. Must not be
 *                      `NULL`.
 * @param err           Buffer receiving a diagnostic that names the specific failure: an unsafe
 *                      name, an unreadable template or partial, or an exceeded limit. May be `NULL`
 *                      only when `err_len` is 0.
 * @param err_len       Size of `err` in bytes.
 * @return Terminated HTML the caller must `free`, or `NULL` on failure, with a diagnostic in `err`.
 */
char* template_render_file(const char* templates_dir,
                           const char* template_name,
                           const struct TemplateContext* context,
                           char* err,
                           size_t err_len) __attribute__((nonnull(1, 2, 3)));

#endif
