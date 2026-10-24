#include "build/page_renderer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/job.h"
#include "build/template.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

/**
 * Worker context shared by all content entry page render jobs. Every field is read-only to a job.
 */
struct PageRenderContext {
  /**
   * Template context every page render starts from, fully populated here so a job varies only
   * `content_entry_current`. Leaving a field zeroed would make `site.updated` render empty. That
   * every pointer in `TemplateContext` is `const` is a thread-safety invariant. During this phase
   * every worker holds pointers to every entry, and the `const` keeps any job from writing what
   * another job reads. Do not relax it to add a mutable field.
   */
  struct TemplateContext base_context;

  /** Template directory root shared by every render job. */
  struct Str templates_dir;

  /** Host every job reads its templates from and writes its page to. */
  struct Os* os;

  /** One entry slot per source path, or `NULL` where no entry was parsed. No entry is written. */
  const struct ContentEntry* const* source_entries;
};

/**
 * @brief Renders one parsed content entry's content template and writes its page.
 *
 * This is the `JobFn` of the page phase, so it runs concurrently with other indexes and writes only
 * its own page and its own log. The HTML goes into a copy of the worker's `perm`, past the entries
 * it holds, and is dropped once it is written. A slot left `NULL` by a draft or by a source that
 * was never parsed is skipped.
 *
 * @param index    Entry slot index this job handles. Must be below the entry slot count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives the failure. Must not be `NULL`.
 * @param userdata `struct PageRenderContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or a skipped slot, or `-1` after recording the failure.
 */
static int render_content_page_job(ptrdiff_t index,
                                   struct JobArenas* arenas,
                                   struct Log* log,
                                   void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Renders one content entry through its configured content template.
 *
 * @param templates_dir Template directory root.
 * @param context       Template context whose `content_entry_current` is the entry to render. Must
 *                      not be `NULL`.
 * @param os            Host the templates are read from. Must not be `NULL`.
 * @param perm          Arena that owns the HTML. Must not be `NULL`.
 * @param scratch       Arena for the render's temporaries.
 * @param html_out      Receives the HTML owned by `perm` on success. Must not be `NULL`.
 * @param log           Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on render failure.
 */
static int render_content_page_template(struct Str templates_dir,
                                        const struct TemplateContext* context,
                                        struct Os* os,
                                        struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* html_out,
                                        struct Log* log) __attribute__((nonnull(2, 3, 4, 6, 7)));

/**
 * @brief Writes one content entry's rendered HTML to its output path.
 *
 * Runs on a worker thread. `os_write` creates missing parent directories, and a sibling job
 * creating the same parent at the same moment is not a failure.
 *
 * @param entry         Entry whose `output_path` receives the page. Must not be `NULL`.
 * @param rendered_html Page HTML to write.
 * @param os            Host the page is written to. Must not be `NULL`.
 * @param scratch       Arena for the failure message's operation.
 * @param log           Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a write failure.
 */
static int render_content_page_write(const struct ContentEntry* entry,
                                     struct Str rendered_html,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) __attribute__((nonnull(1, 3, 5)));

int page_renderer_render_pages(const struct SiteConfig* site_config,
                               const struct ContentEntry* const* source_entries,
                               ptrdiff_t source_entry_count,
                               const struct ContentEntry* const* content_entries,
                               ptrdiff_t content_entry_count,
                               struct Str site_updated,
                               ptrdiff_t worker_count,
                               struct JobArenas* worker_arenas,
                               bool is_verbose,
                               struct Os* os,
                               struct Arena scratch,
                               struct Log* log) {
  struct PageRenderContext context = {
      .base_context =
          {
              .site_config = site_config,
              .content_entry_current = NULL,
              .content_entries = content_entries,
              .content_entry_count = content_entry_count,
              .site_updated = site_updated,
          },
      .templates_dir = site_config->templates_dir,
      .os = os,
      .source_entries = source_entries,
  };
  return job_run(source_entry_count, worker_count, worker_arenas, render_content_page_job, &context,
                 S("rendering content"), S(TEMPLATE_RENDER_ATTRIBUTION), is_verbose, os, scratch,
                 log);
}

static int render_content_page_job(ptrdiff_t index,
                                   struct JobArenas* arenas,
                                   struct Log* log,
                                   void* userdata) {
  const struct PageRenderContext* context = userdata;
  const struct ContentEntry* entry = context->source_entries[index];

  int rc = 0;
  if (entry != NULL) {
    // A worker of the parse phase wrote `entry`. Reading it from a different thread here is not a
    // race. That phase's `pool_run` joined its workers before returning, and this phase's
    // `pthread_create` happens after, so the write is already published to this thread.
    memory_oom_set_job(entry->source_path, log);

    // Copy the shared context before setting `content_entry_current`. Every page job reads
    // `base_context` concurrently, so mutating it in place would be a data race, which is undefined
    // behavior. The copy lives on this job's stack, so no other job can observe a change to it.
    struct TemplateContext template_context = context->base_context;
    template_context.content_entry_current = entry;
    // Both arenas are copies, so the page and the render's temporaries are dropped when this job
    // returns. The page lands past the entries this worker parsed, which the copy never touches.
    struct Arena perm = arenas->perm;
    struct Str rendered_html = {0};
    rc = render_content_page_template(context->templates_dir, &template_context, context->os, &perm,
                                      arenas->scratch, &rendered_html, log);
    if (rc == 0) {
      // The render has returned, so its temporaries are dead and the write can reuse the region.
      rc = render_content_page_write(entry, rendered_html, context->os, arenas->scratch, log);
      // The page is written, so its pages are given back rather than kept resident in the worker's
      // region. A large page costs far more to render than to fault in again.
      arena_discard(&perm, rendered_html.data, rendered_html.len);
    }
  }
  return rc;
}

static int render_content_page_template(struct Str templates_dir,
                                        const struct TemplateContext* context,
                                        struct Os* os,
                                        struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* html_out,
                                        struct Log* log) {
  const struct ContentEntry* entry = context->content_entry_current;
  const struct Str template_name =
      entry->template.len > 0 ? entry->template : context->site_config->content_template;
  // The render names the entry in its failure, because `content_entry_current` is set.
  return template_render_file(templates_dir, template_name, context, os, perm, scratch, html_out,
                              log);
}

static int render_content_page_write(const struct ContentEntry* entry,
                                     struct Str rendered_html,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) {
  // The operation names the source, and the write names the output path or the directory it could
  // not create, so a failure reads `failed to write output for '<source>': <cause> ('<path>')`.
  struct Buf operation = {.arena = &scratch};
  buf_printf(&operation, "failed to write output for '%.*s'", STR_FMT(entry->source_path));
  return os_write(os, entry->output_path, (struct OsSource){.bytes = rendered_html},
                  buf_str(&operation), log);
}
