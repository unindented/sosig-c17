// `TEST_NO_MAIN` keeps acutest's `main` and run state in each test executable, as in
// `tests/test_support.c`, so a helper here can still record a failure with `TEST_CHECK`.
#define TEST_NO_MAIN

#include "test_render_support.h"

#include <acutest.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "build/entry_renderer.h"
#include "build/page_renderer.h"
#include "build/render_job.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

int render_pages(const struct SiteConfig* site_config,
                 struct RenderJobSet* render_jobs,
                 size_t worker_count,
                 struct StringBuffer* error_out) {
  const size_t count = render_jobs->count;
  struct ContentEntry** entries = calloc(count > 0 ? count : 1, sizeof(*entries));
  TEST_ASSERT(entries != NULL);
  if (entries == NULL) {
    return 1;
  }
  size_t entry_count = 0;
  for (size_t i = 0; i < count; i++) {
    if (render_jobs->items[i].entry != NULL) {
      entries[entry_count++] = render_jobs->items[i].entry;
    }
  }
  content_entry_sort(entries, entry_count);
  const char* site_updated =
      content_entry_latest_date((const struct ContentEntry* const*)entries, entry_count);
  const int rc = page_renderer_render_pages(render_jobs, site_config,
                                            (const struct ContentEntry* const*)entries, entry_count,
                                            site_updated, worker_count, false, error_out);
  free(entries);
  return rc;
}

int render_sources(const char* root_dir,
                   const char* const* relative_paths,
                   size_t relative_path_count,
                   const char* permalink_override,
                   struct SiteConfig* site_config,
                   struct PathList* source_paths,
                   struct RenderJobSet* render_jobs_out,
                   struct StringBuffer* error_out) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return 1;
  }

  char config_err[ERROR_MESSAGE_SIZE];
  config_err[0] = '\0';
  int rc = 1;
  if (site_config_load(site_config, "sosig.toml", config_err, sizeof(config_err)) == 0) {
    if (permalink_override != NULL) {
      site_config->permalink = permalink_override;
    }
    rc = 0;
    for (size_t i = 0; rc == 0 && i < relative_path_count; i++) {
      rc = path_list_push(source_paths, relative_paths[i]);
    }
    if (rc == 0) {
      rc = entry_renderer_render_entries(site_config, source_paths, 1, false, render_jobs_out,
                                         error_out);
    }
    if (rc == 0) {
      rc = render_pages(site_config, render_jobs_out, 1, error_out);
    }
  }

  return working_dir_leave(saved_dir_fd) == 0 ? rc : 1;
}

int render_single_source(const char* root_dir,
                         const char* source_relative_path,
                         const char* permalink_override,
                         struct SiteConfig* site_config,
                         struct PathList* source_paths,
                         struct RenderJobSet* render_jobs_out,
                         struct StringBuffer* error_out) {
  return render_sources(root_dir, &source_relative_path, 1, permalink_override, site_config,
                        source_paths, render_jobs_out, error_out);
}

char* read_output(const char* root_dir, const char* relative_path) {
  enum { OUTPUT_LEN_MAX = 4096 };
  struct Arena arena;
  arena_init(&arena);
  const char* path = path_join(root_dir, relative_path, &arena);
  FILE* fp = path != NULL ? fopen(path, "rb") : NULL;
  arena_free(&arena);
  if (fp == NULL) {
    return NULL;
  }
  char* data = calloc(OUTPUT_LEN_MAX + 1, 1);
  const size_t data_len = data != NULL ? fread(data, 1, OUTPUT_LEN_MAX, fp) : 0;
  if (data != NULL && (ferror(fp) != 0 || data_len == OUTPUT_LEN_MAX)) {
    free(data);
    data = NULL;
  }
  (void)fclose(fp);
  return data;
}
