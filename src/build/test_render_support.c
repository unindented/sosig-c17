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
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"
#include "test_support.h"

/**
 * @brief Runs the page phase over entry slots the parse phase filled.
 *
 * Collects, sorts and dates the published entries the way the build does before its page phase.
 *
 * @param site_config        Configuration used for rendering. Must not be `NULL`.
 * @param source_entries     Entry slots filled by the parse phase. Must not be `NULL`.
 * @param source_entry_count Number of slots in `source_entries`.
 * @param worker_count       Worker threads the page phase runs on.
 * @param error_out          Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
static int render_pages(const struct SiteConfig* site_config,
                        struct ContentEntry* const* source_entries,
                        size_t source_entry_count,
                        size_t worker_count,
                        struct StringBuffer* error_out) {
  struct ContentEntry** entries = calloc(source_entry_count, sizeof(*entries));
  int rc = TEST_PLUMBING_FAILED;
  size_t entry_count = 0;
  const char* site_updated = NULL;
  if (!TEST_CHECK(entries != NULL || source_entry_count == 0)) {
    goto cleanup;
  }
  for (size_t i = 0; i < source_entry_count; i++) {
    if (source_entries[i] != NULL) {
      entries[entry_count++] = source_entries[i];
    }
  }
  content_entry_sort(entries, entry_count);
  site_updated = content_entry_latest_date((const struct ContentEntry* const*)entries, entry_count);
  rc = page_renderer_render_pages(site_config, (const struct ContentEntry* const*)source_entries,
                                  source_entry_count, (const struct ContentEntry* const*)entries,
                                  entry_count, site_updated, worker_count, false, error_out);

cleanup:
  free(entries);
  return rc;
}

int render_sources(const char* root_dir,
                   const char* const* relative_paths,
                   size_t relative_path_count,
                   const char* permalink_override,
                   struct SiteConfig* site_config,
                   struct PathList* source_paths,
                   struct ContentEntry** source_entries,
                   struct StringBuffer* error_out) {
  int saved_dir_fd = -1;
  if (working_dir_enter(root_dir, &saved_dir_fd) != 0) {
    return TEST_PLUMBING_FAILED;
  }

  char config_err[ERROR_MESSAGE_SIZE];
  config_err[0] = '\0';
  int rc = TEST_PLUMBING_FAILED;
  const bool is_config_loaded =
      site_config_load(site_config, "sosig.toml", config_err, sizeof(config_err)) == 0;
  TEST_CHECK(is_config_loaded);
  TEST_MSG("config: %s", config_err);
  if (is_config_loaded) {
    if (permalink_override != NULL) {
      site_config->permalink = permalink_override;
    }
    bool is_pushed = true;
    for (size_t i = 0; is_pushed && i < relative_path_count; i++) {
      is_pushed = path_list_push(source_paths, relative_paths[i]) == 0;
    }
    TEST_CHECK(is_pushed);
    if (is_pushed) {
      rc = entry_renderer_render_entries(site_config, source_paths, 1, false, source_entries,
                                         error_out);
      if (rc == 0) {
        rc = render_pages(site_config, source_entries, relative_path_count, 1, error_out);
      }
    }
  }

  return working_dir_leave(saved_dir_fd) == 0 ? rc : TEST_PLUMBING_FAILED;
}

int render_single_source(const char* root_dir,
                         const char* source_relative_path,
                         const char* permalink_override,
                         struct SiteConfig* site_config,
                         struct PathList* source_paths,
                         struct ContentEntry** source_entries,
                         struct StringBuffer* error_out) {
  return render_sources(root_dir, &source_relative_path, 1, permalink_override, site_config,
                        source_paths, source_entries, error_out);
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
