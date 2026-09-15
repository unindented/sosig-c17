#include "build/site_writer.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "build/render_job.h"
#include "build/template.h"
#include "core/arena.h"
#include "core/error.h"
#include "core/path.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"

// This module's three exported writers are exercised end-to-end by CTest's golden test suite, which
// builds every fixture site and compares each result with its `tests/expected/<site>` tree, so
// there is no `src/build/test_site_writer.c`. Collision checks live in `manifest_builder`,
// unit-tested in `src/build/test_manifest_builder.c`.

/**
 * @brief Renders one configured template and writes it to its matching output path.
 *
 * @param templates_dir Template directory root. Must not be `NULL`.
 * @param output_dir    Output directory the template name is rooted under. Must not be `NULL`.
 * @param template_name Safe relative template name to render. `manifest_builder_populate` already
 *                      checked it against the output-path limits, because every call site runs the
 *                      manifest pass over the same configured lists first. This pass does not
 *                      re-check, so an overlong name reaching here fails as an opaque
 * write error from the OS rather than with a diagnostic naming the limit. Must not be `NULL`.
 * @param context       Template context for this render. Must not be `NULL`.
 * @param err           Destination buffer for a failure diagnostic.
 * @param err_len       Size of `err` in bytes.
 * @return `0` on success, or `-1` on a render, path, or write failure.
 */
static int write_rendered_template(const char* templates_dir,
                                   const char* output_dir,
                                   const char* template_name,
                                   const struct TemplateContext* context,
                                   char* err,
                                   size_t err_len) __attribute__((nonnull(1, 2, 3, 4)));

int site_writer_write_content_entries(const struct RenderJobSet* render_jobs,
                                      char* err,
                                      size_t err_len) {
  for (size_t i = 0; i < render_jobs->count; i++) {
    const struct ContentEntry* entry = render_jobs->items[i].entry;
    // A `NULL` entry is a draft or an unparsed slot, and it is the only slot this pass skips. This
    // deliberately does not check `rendered_html` alongside it. The header's precondition is that
    // `page_renderer_render_pages` returned `0`, which guarantees every slot holding an entry also
    // holds its rendered HTML. Making the two checks symmetric would turn a broken invariant into a
    // silently missing page, which is the one outcome the header says must not happen.
    if (entry == NULL) {
      continue;
    }
    char reason[FS_REASON_SIZE];
    if (fs_write_file(entry->output_path, render_jobs->items[i].rendered_html,
                      strlen(render_jobs->items[i].rendered_html), reason, sizeof(reason)) != 0) {
      return error_report(err, err_len, "failed to write output: %s (for '%s', to '%s')", reason,
                          entry->source_path, entry->output_path);
    }
  }
  return 0;
}

int site_writer_write_aggregates(const struct SiteConfig* site_config,
                                 const struct ContentEntry* const* content_entries,
                                 size_t content_entry_count,
                                 const char* site_updated,
                                 char* err,
                                 size_t err_len) {
  const struct TemplateContext context = {
      .site_config = site_config,
      .content_entry_current = NULL,
      .content_entries = content_entries,
      .content_entry_count = content_entry_count,
      .site_updated = site_updated,
  };

  for (size_t i = 0; i < site_config->aggregate_template_count; i++) {
    if (write_rendered_template(site_config->templates_dir, site_config->output_dir,
                                site_config->aggregate_templates[i], &context, err, err_len) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_write_feeds(const struct SiteConfig* site_config,
                            const struct ContentEntry* const* content_entries,
                            size_t content_entry_count,
                            const char* site_updated,
                            char* err,
                            size_t err_len) {
  const size_t feed_content_entry_count =
      content_entry_count < site_config->feed_count ? content_entry_count : site_config->feed_count;
  const struct TemplateContext context = {
      .site_config = site_config,
      .content_entry_current = NULL,
      .content_entries = content_entries,
      .content_entry_count = feed_content_entry_count,
      .site_updated = site_updated,
  };

  for (size_t i = 0; i < site_config->feed_template_count; i++) {
    if (write_rendered_template(site_config->templates_dir, site_config->output_dir,
                                site_config->feed_templates[i], &context, err, err_len) != 0) {
      return -1;
    }
  }
  return 0;
}

static int write_rendered_template(const char* templates_dir,
                                   const char* output_dir,
                                   const char* template_name,
                                   const struct TemplateContext* context,
                                   char* err,
                                   size_t err_len) {
  struct Arena scratch;
  arena_init(&scratch);

  int rc = -1;
  char* output_path = NULL;
  char* rendered_html = template_render_file(templates_dir, template_name, context, err, err_len);
  if (rendered_html == NULL) {
    goto cleanup;
  }

  output_path = path_join(output_dir, template_name, &scratch);
  if (output_path == NULL) {
    (void)error_report(err, err_len, "out of memory building output path for '%s'", template_name);
    goto cleanup;
  }
  char reason[FS_REASON_SIZE];
  if (fs_write_file(output_path, rendered_html, strlen(rendered_html), reason, sizeof(reason)) !=
      0) {
    (void)error_report(err, err_len, "failed to write template output: %s ('%s')", reason,
                       output_path);
    goto cleanup;
  }
  rc = 0;

cleanup:
  arena_free(&scratch);
  free(rendered_html);
  return rc;
}
