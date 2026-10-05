#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/site_writer.h"
#include "core/error.h"
#include "core/path.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few hundred bytes. */
enum { TEST_FILE_LEN_MAX = 1024 * 1024 };

/** Timestamp every write below passes as `site.updated`, which the writers require. */
static const char* const SITE_UPDATED = "2026-07-01T00:00:00Z";

/**
 * @brief Checks that one fixture file holds exactly the expected text.
 *
 * @param path     File to read. Must not be `NULL`.
 * @param expected Terminated text the file must hold byte for byte. Must not be `NULL`.
 */
static void check_file_text(const char* path, const char* expected) {
  char* bytes = NULL;
  size_t bytes_len = 0;
  char reason[FS_REASON_SIZE];
  if (!TEST_CHECK(
          fs_read_file(path, TEST_FILE_LEN_MAX, &bytes, &bytes_len, reason, sizeof(reason)) == 0)) {
    return;
  }
  TEST_CHECK(bytes_len == strlen(expected));
  TEST_CHECK(strcmp(bytes, expected) == 0);
  free(bytes);
}

// Aggregate and feed templates, nested ones included, land at their planned destinations, and a
// feed sees only the newest `feed_count` entries while an aggregate sees them all.
static void test_writes_complete_output_layer(void) {
  char root_dir_template[] = "/tmp/sosig-writer-output-layer.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena paths;
  arena_init(&paths);
  char* output_dir = path_join(root_dir, "public", &paths);
  char* templates_dir = path_join(root_dir, "templates", &paths);
  char* aggregate_output = path_join(output_dir, "index.html", &paths);
  char* nested_output = path_join(output_dir, "nested/links.html", &paths);
  char* feed_output = path_join(output_dir, "feeds/atom.xml", &paths);
  struct SiteConfig config;
  site_config_init(&config);
  const struct ContentEntry newer = {.title = "Newer", .url_path = "/newer.html"};
  const struct ContentEntry older = {.title = "Older", .url_path = "/older.html"};
  const struct ContentEntry* const entries[] = {&newer, &older};
  char err[ERROR_MESSAGE_SIZE];
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/index.html",
                                     "<h1>{{site.title}}</h1>{{#content_entries}}{{title}};"
                                     "{{/content_entries}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/nested/links.html",
                                     "{{#content_entries}}{{url}};{{/content_entries}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/feeds/atom.xml",
                                     "<updated>{{site.updated}}</updated>{{#content_entries}}"
                                     "{{title}};{{/content_entries}}") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(output_dir != NULL && templates_dir != NULL && aggregate_output != NULL &&
                  nested_output != NULL && feed_output != NULL)) {
    goto cleanup;
  }

  static const char* const aggregates[] = {"index.html", "nested/links.html"};
  static const char* const feeds[] = {"feeds/atom.xml"};
  config.title = "Site & Co";
  config.author = "Ada";
  config.base_url = "https://example.com";
  config.output_dir = output_dir;
  config.templates_dir = templates_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  config.feed_templates = feeds;
  config.feed_template_count = 1;
  config.feed_count = 1;

  TEST_CHECK(site_writer_write_aggregates(&config, entries, 2, SITE_UPDATED, err, sizeof(err)) ==
             0);
  TEST_CHECK(site_writer_write_feeds(&config, entries, 2, SITE_UPDATED, err, sizeof(err)) == 0);
  check_file_text(aggregate_output, "<h1>Site &amp; Co</h1>Newer;Older;");
  check_file_text(nested_output, "/newer.html;/older.html;");
  check_file_text(feed_output, "<updated>2026-07-01T00:00:00Z</updated>Newer;");

cleanup:
  site_config_free(&config);
  arena_free(&paths);
  remove_fixture_tree(root_dir);
}

// A failed aggregate write names the operation and the reason, then the destination. The
// destination is an existing directory, so opening it for writing fails.
static void test_write_aggregates_reports_write_failure(void) {
  char root_dir_template[] = "/tmp/sosig-writer-aggregate.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena paths;
  arena_init(&paths);
  char* output_dir = path_join(root_dir, "public", &paths);
  char* templates_dir = path_join(root_dir, "templates", &paths);
  char* aggregate_output = path_join(output_dir, "index.html", &paths);
  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/index.html", "<h1></h1>") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "public/index.html/keep", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(output_dir != NULL && templates_dir != NULL && aggregate_output != NULL)) {
    goto cleanup;
  }
  static const char* const aggregates[] = {"index.html"};
  config.output_dir = output_dir;
  config.templates_dir = templates_dir;
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 1;

  TEST_CHECK(site_writer_write_aggregates(&config, NULL, 0, SITE_UPDATED, err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected), "failed to write template output: %s ('%s')",
                          error_system_message(reason, sizeof(reason), EISDIR), aggregate_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("err: %s", err);

cleanup:
  site_config_free(&config);
  arena_free(&paths);
  remove_fixture_tree(root_dir);
}

// A failed feed write reports the same way as an aggregate, because both share one writer. The
// destination is an existing directory, so opening it for writing fails.
static void test_write_feeds_reports_write_failure(void) {
  char root_dir_template[] = "/tmp/sosig-writer-feed.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  struct Arena paths;
  arena_init(&paths);
  char* output_dir = path_join(root_dir, "public", &paths);
  char* templates_dir = path_join(root_dir, "templates", &paths);
  char* feed_output = path_join(output_dir, "atom.xml", &paths);
  struct SiteConfig config;
  site_config_init(&config);
  char err[ERROR_MESSAGE_SIZE] = "";
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  int expected_len = 0;
  if (!TEST_CHECK(write_fixture_file(root_dir, "templates/atom.xml", "<feed></feed>") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(write_fixture_file(root_dir, "public/atom.xml/keep", "") == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(output_dir != NULL && templates_dir != NULL && feed_output != NULL)) {
    goto cleanup;
  }
  static const char* const feeds[] = {"atom.xml"};
  config.output_dir = output_dir;
  config.templates_dir = templates_dir;
  config.feed_templates = feeds;
  config.feed_template_count = 1;

  TEST_CHECK(site_writer_write_feeds(&config, NULL, 0, SITE_UPDATED, err, sizeof(err)) == -1);
  expected_len = snprintf(expected, sizeof(expected), "failed to write template output: %s ('%s')",
                          error_system_message(reason, sizeof(reason), EISDIR), feed_output);
  if (!TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected))) {
    goto cleanup;
  }
  TEST_CHECK(strcmp(err, expected) == 0);
  TEST_MSG("err: %s", err);

cleanup:
  site_config_free(&config);
  arena_free(&paths);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"writes complete output layer", test_writes_complete_output_layer},
    {"write aggregates reports write failure", test_write_aggregates_reports_write_failure},
    {"write feeds reports write failure", test_write_feeds_reports_write_failure},
    {NULL, NULL},
};
