#include "build/page_renderer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/render_job.h"
#include "build/template.h"
#include "core/error.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"

/**
 * Worker context shared by all content entry page render jobs. Every field is read-only to a job
 * except `render_jobs`, where each job writes its own slot.
 */
struct ContentEntryPageContext {
  /**
   * Template context every page render starts from, fully populated here so a job varies only
   * `content_entry_current`. Leaving a field zeroed would make `site.updated` render empty. That
   * every pointer in `TemplateContext` is `const` is a thread-safety invariant. During this pass
   * every worker holds pointers to every entry. The `const` prevents one job from allocating into
   * another job's arena, because `template.c` can only allocate into its own per-call scratch
   * arena. Do not relax it to add a mutable field.
   */
  struct TemplateContext base_context;

  /** Template directory root shared by every render job. */
  const char* templates_dir;

  /** One result slot per source path. Each render job writes only its index. */
  struct RenderJob* render_jobs;

  /** Whether each finished job prints a progress dot to `stderr`. */
  bool is_verbose;
};

/**
 * @brief Renders one parsed content entry's content template as a worker-pool job.
 *
 * Transfers the rendered HTML to the matching result slot on success. A slot left empty by a draft
 * or by a source that was never parsed is skipped.
 *
 * @param index    Result slot index this job renders.
 * @param userdata Pointer to the shared `struct ContentEntryPageContext`. Must not be `NULL`.
 * @return `0` on success or a skipped slot, or `-1` on a render failure.
 */
static int render_content_page_job(size_t index, void* userdata) __attribute__((nonnull(2)));

/**
 * @brief Renders one content entry through its configured content template.
 *
 * @param templates_dir Template directory root. Must not be `NULL`.
 * @param context       Template context whose `content_entry_current` is the entry to render. Must
 *                      not be `NULL`.
 * @param result        Result slot that receives an error message on failure. Must not be `NULL`.
 * @return Terminated HTML the caller must `free`, or `NULL` on render failure.
 */
static char* render_content_page_template(const char* templates_dir,
                                          const struct TemplateContext* context,
                                          struct RenderJob* result)
    __attribute__((nonnull(1, 2, 3)));

int page_renderer_render_pages(struct RenderJobSet* render_jobs,
                               const struct SiteConfig* site_config,
                               const struct ContentEntry* const* content_entries,
                               size_t content_entry_count,
                               const char* site_updated,
                               size_t worker_count,
                               bool is_verbose,
                               struct StringBuffer* error_out) {
  struct ContentEntryPageContext page_context = {
      .base_context =
          {
              .site_config = site_config,
              .content_entry_current = NULL,
              .content_entries = content_entries,
              .content_entry_count = content_entry_count,
              .site_updated = site_updated,
          },
      .templates_dir = site_config->templates_dir,
      .render_jobs = render_jobs->items,
      .is_verbose = is_verbose,
  };

  return render_job_run(render_jobs, worker_count, render_content_page_job, &page_context,
                        is_verbose, error_out);
}

static int render_content_page_job(size_t index, void* userdata) {
  struct ContentEntryPageContext* page_context = userdata;
  struct RenderJob* result = &page_context->render_jobs[index];

  int rc = 0;
  if (result->entry != NULL) {
    // A worker of the entry pass wrote `result->entry`. Reading it from a different thread here is
    // not a race. That pass's `pool_run` joined its workers before returning, and this pass's
    // `pthread_create` happens after, so the write is already published to this thread.

    // Copy the shared context before setting `content_entry_current`. Every page job reads
    // `base_context` concurrently, so mutating it in place would be a data race, which is undefined
    // behavior. The copy lives on this job's stack, so no other job can observe a change to it.
    struct TemplateContext context = page_context->base_context;
    context.content_entry_current = result->entry;
    result->rendered_html =
        render_content_page_template(page_context->templates_dir, &context, result);
    rc = result->rendered_html != NULL ? 0 : -1;
  }
  render_job_progress_dot(page_context->is_verbose);
  return rc;
}

static char* render_content_page_template(const char* templates_dir,
                                          const struct TemplateContext* context,
                                          struct RenderJob* result) {
  const struct ContentEntry* entry = context->content_entry_current;
  const char* template_name =
      entry->template != NULL ? entry->template : context->site_config->content_template;
  char error_message[ERROR_MESSAGE_SIZE];
  error_message[0] = '\0';
  char* rendered_html = template_render_file(templates_dir, template_name, context, error_message,
                                             sizeof(error_message));
  if (rendered_html == NULL) {
    // Parenthesize the attribution. The callee's message may itself end in a `: <reason>` clause. A
    // bare `for '%s'` suffix would read as part of that reason rather than as the entry the render
    // was for.
    render_job_set_error(result, "%s (while rendering '%s')", error_message, entry->source_path);
  }
  return rendered_html;
}
