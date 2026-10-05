#include "build/page_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "build/job.h"
#include "build/template.h"
#include "core/error.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"

/**
 * Worker context shared by all content entry page render jobs. Every field is read-only to a job.
 */
struct ContentEntryPageContext {
  /**
   * Template context every page render starts from, fully populated here so a job varies only
   * `content_entry_current`. Leaving a field zeroed would make `site.updated` render empty. That
   * every pointer in `TemplateContext` is `const` is a thread-safety invariant. During this phase
   * every worker holds pointers to every entry. The `const` prevents one job from allocating into
   * another job's arena, because `template.c` can only allocate into its own per-call scratch
   * arena. Do not relax it to add a mutable field.
   */
  struct TemplateContext base_context;

  /** Template directory root shared by every render job. */
  const char* templates_dir;

  /** One entry slot per source path, or `NULL` where no entry was parsed. No entry is written. */
  const struct ContentEntry* const* source_entries;
};

/**
 * @brief Renders one parsed content entry's content template and writes its page.
 *
 * This is the `JobFn` of the page phase, so it runs concurrently with other indexes and writes only
 * its own page and its own error slot. Frees the rendered HTML once it is written. A slot left
 * `NULL` by a draft or by a source that was never parsed is skipped.
 *
 * @param jobs     Running job set, whose error slot for `index` the job may fill through
 *                 `job_set_error`. Must not be `NULL`.
 * @param index    Entry slot index this job renders.
 * @param userdata Pointer to the shared `struct ContentEntryPageContext`. Must not be `NULL`.
 * @return `0` on success, or a skipped slot, or `-1` after recording the failure through
 *         `job_set_error`.
 */
static int render_content_page_job(struct JobSet* jobs, size_t index, void* userdata)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Renders one content entry through its configured content template.
 *
 * @param templates_dir Template directory root. Must not be `NULL`.
 * @param context       Template context whose `content_entry_current` is the entry to render. Must
 *                      not be `NULL`.
 * @param html_len_out  Receives the length of the returned HTML in bytes on success. Must not be
 *                      `NULL`.
 * @param jobs          Running job set receiving the failure. Must not be `NULL`.
 * @param index         Job index whose error slot receives the failure.
 * @return Terminated HTML the caller must `free`, or `NULL` on render failure.
 */
static char* render_content_page_template(const char* templates_dir,
                                          const struct TemplateContext* context,
                                          size_t* html_len_out,
                                          struct JobSet* jobs,
                                          size_t index) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Writes one content entry's rendered HTML to its output path.
 *
 * Runs on a worker thread. `fs_write_file` creates missing parent directories, and a sibling job
 * creating the same parent at the same moment is not a failure, because it treats `EEXIST` as
 * success.
 *
 * @param entry             Entry whose `output_path` receives the page. Must not be `NULL`.
 * @param rendered_html     Page HTML to write. Must not be `NULL`.
 * @param rendered_html_len Number of bytes in `rendered_html`.
 * @param jobs              Running job set receiving the failure. Must not be `NULL`.
 * @param index             Job index whose error slot receives the failure.
 * @return `0` on success, or `-1` on a write failure.
 */
static int render_content_page_write(const struct ContentEntry* entry,
                                     const char* rendered_html,
                                     size_t rendered_html_len,
                                     struct JobSet* jobs,
                                     size_t index) __attribute__((nonnull(1, 2, 4)));

int page_renderer_render_pages(const struct SiteConfig* site_config,
                               const struct ContentEntry* const* source_entries,
                               size_t source_entry_count,
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
      .source_entries = source_entries,
  };
  return job_run(source_entry_count, worker_count, render_content_page_job, &page_context,
                 "rendering content", is_verbose, error_out);
}

static int render_content_page_job(struct JobSet* jobs, size_t index, void* userdata) {
  struct ContentEntryPageContext* page_context = userdata;
  const struct ContentEntry* entry = page_context->source_entries[index];

  int rc = 0;
  if (entry != NULL) {
    // A worker of the parse phase wrote `entry`. Reading it from a different thread here is not a
    // race. That phase's `pool_run` joined its workers before returning, and this phase's
    // `pthread_create` happens after, so the write is already published to this thread.

    // Copy the shared context before setting `content_entry_current`. Every page job reads
    // `base_context` concurrently, so mutating it in place would be a data race, which is undefined
    // behavior. The copy lives on this job's stack, so no other job can observe a change to it.
    struct TemplateContext context = page_context->base_context;
    context.content_entry_current = entry;
    size_t rendered_html_len = 0;
    char* rendered_html = render_content_page_template(page_context->templates_dir, &context,
                                                       &rendered_html_len, jobs, index);
    rc = rendered_html != NULL
             ? render_content_page_write(entry, rendered_html, rendered_html_len, jobs, index)
             : -1;
    free(rendered_html);
  }
  return rc;
}

static char* render_content_page_template(const char* templates_dir,
                                          const struct TemplateContext* context,
                                          size_t* html_len_out,
                                          struct JobSet* jobs,
                                          size_t index) {
  const struct ContentEntry* entry = context->content_entry_current;
  const char* template_name =
      entry->template != NULL ? entry->template : context->site_config->content_template;
  char error_message[ERROR_MESSAGE_SIZE];
  error_message[0] = '\0';
  char* rendered_html = template_render_file(templates_dir, template_name, context, html_len_out,
                                             error_message, sizeof(error_message));
  if (rendered_html == NULL) {
    // Parenthesize the attribution. The callee's message may itself end in a `: <reason>` clause. A
    // bare `for '%s'` suffix would read as part of that reason rather than as the entry the render
    // was for.
    job_set_error(jobs, index, "%s (while rendering '%s')", error_message, entry->source_path);
  }
  return rendered_html;
}

static int render_content_page_write(const struct ContentEntry* entry,
                                     const char* rendered_html,
                                     size_t rendered_html_len,
                                     struct JobSet* jobs,
                                     size_t index) {
  char reason[FS_REASON_SIZE];
  if (fs_write_file(entry->output_path, rendered_html, rendered_html_len, reason, sizeof(reason)) !=
      0) {
    job_set_error(jobs, index, "failed to write output: %s (for '%s', to '%s')", reason,
                  entry->source_path, entry->output_path);
    return -1;
  }
  return 0;
}
