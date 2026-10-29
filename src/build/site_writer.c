#include "build/site_writer.h"

#include <stddef.h>

#include "build/output_writer.h"
#include "build/template.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/str.h"

// This module's two writers are unit-tested in `src/build/test_site_writer.c`, and CTest's golden
// test suite exercises them end to end by building every fixture site and comparing each result
// with its `tests/expected/<site>` tree. Collision checks live in `manifest_builder`, unit-tested
// in `src/build/test_manifest_builder.c`.

/**
 * @brief Renders one configured template and writes it to its matching output path, or keeps an
 *        existing output whose inputs are fresh.
 *
 * @param writer          Writer the output is written or kept through. Must not be `NULL`.
 * @param templates_dir   Template directory root.
 * @param output_dir      Output directory the template name is rooted under.
 * @param template_name   Safe relative template name to render. `manifest_builder_populate` already
 *                        checked it against the output-path limits, because every call site runs
 *                        the manifest phase over the same configured lists first. This function
 *                        does not re-check, so an overlong name reaching here fails as an opaque
 *                        write error from the OS rather than with a diagnostic naming the limit.
 * @param context         Template context for this render. Must not be `NULL`.
 * @param is_inputs_fresh Whether an existing output may be kept.
 * @param os              Host the template is read from and the output inspected on. Must not be
 *                        `NULL`.
 * @param output          Arena that holds the rendered output until it is written.
 * @param scratch         Arena for the render's temporaries and the output path.
 * @param log             Log that receives the first failure. Its arena must be none of the others.
 *                        Must not be `NULL`.
 * @return `0` on success, or `-1` on a render or write failure.
 */
static int write_rendered_template(const struct OutputWriter* writer,
                                   struct Str templates_dir,
                                   struct Str output_dir,
                                   struct Str template_name,
                                   const struct TemplateContext* context,
                                   bool is_inputs_fresh,
                                   struct Os* os,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 5, 7, 10)));

int site_writer_write_templates(const struct OutputWriter* writer,
                                const struct SiteConfig* site_config,
                                const struct Str* template_names,
                                ptrdiff_t template_count,
                                const struct ContentEntry* const* content_entries,
                                ptrdiff_t content_entry_count,
                                struct Str site_updated,
                                bool is_inputs_fresh,
                                struct Os* os,
                                struct Arena output,
                                struct Arena scratch,
                                struct Log* log) {
  const struct TemplateContext context = {
      .site_config = site_config,
      .content_entry_current = NULL,
      .content_entries = content_entries,
      .content_entry_count = content_entry_count,
      .site_updated = site_updated,
  };
  for (ptrdiff_t i = 0; i < template_count; i++) {
    if (write_rendered_template(writer, site_config->templates_dir, site_config->output_dir,
                                template_names[i], &context, is_inputs_fresh, os, output, scratch,
                                log) != 0) {
      return -1;
    }
  }
  return 0;
}

static int write_rendered_template(const struct OutputWriter* writer,
                                   struct Str templates_dir,
                                   struct Str output_dir,
                                   struct Str template_name,
                                   const struct TemplateContext* context,
                                   bool is_inputs_fresh,
                                   struct Os* os,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) {
  memory_oom_set_job(template_name, NULL);
  const struct Str output_path = path_join(output_dir, template_name, &scratch);
  struct OsStat output_stat;
  if (is_inputs_fresh && os_stat(os, output_path, &output_stat) == 0 && !output_stat.is_dir) {
    return output_writer_keep(writer, output_path, log);
  }
  struct Str rendered_html = {0};
  if (template_render_file(templates_dir, template_name, context, os, &output, scratch,
                           &rendered_html, NULL, log) != 0) {
    return -1;
  }
  return output_writer_write(writer, output_path, (struct OsSource){.bytes = rendered_html},
                             S("failed to write template output"), log);
}

int site_writer_copy_static_files(const struct OutputWriter* writer,
                                  const struct SiteConfig* site_config,
                                  const struct PathList* static_paths,
                                  struct Arena scratch,
                                  struct Log* log) {
  for (ptrdiff_t i = 0; i < static_paths->count; i++) {
    memory_oom_set_job(static_paths->items[i], NULL);
    // Each copy works in its own copy of `scratch`, so one file's path is dead before the next.
    struct Arena copy_scratch = scratch;
    struct Str relative = {0};
    (void)path_is_below(static_paths->items[i], site_config->static_dir, &relative);
    const struct Str output_path = path_join(site_config->output_dir, relative, &copy_scratch);
    // The copy names the path that failed, the static file or its destination, which mirrors it.
    const struct OsSource source = {.kind = OS_SOURCE_FILE, .file_path = static_paths->items[i]};
    if (output_writer_write(writer, output_path, source, S("failed to copy static file"), log) !=
        0) {
      return -1;
    }
  }
  return 0;
}
