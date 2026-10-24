#include <acutest.h>
#include <errno.h>
#include <string.h>

#include "build/site_writer.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every test here runs against the in-memory host: the writers read templates and static files and
// write their outputs only through `runtime/os.h`.

/** Timestamp every write below passes as `site.updated`, which the writers require. */
#define SITE_UPDATED S("2026-07-01T00:00:00Z")

/**
 * @brief Initializes a configuration rooted at `templates` and `public`, with no outputs.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 */
static void init_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->output_dir = S("public");
  site_config->templates_dir = S("templates");
}

// Templates and static files, nested ones included, land at their planned destinations, and the
// templates see the entries passed.
static void test_writes_complete_output_layer(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html",
                  "<h1>{{site.title}}</h1>{{#content_entries}}{{title}};{{/content_entries}}");
  os_memory_write(os, "templates/nested/links.html",
                  "{{#content_entries}}{{url}};{{/content_entries}}");
  os_memory_write(os, "templates/feeds/atom.xml",
                  "<updated>{{site.updated}}</updated>{{#content_entries}}{{title}};"
                  "{{/content_entries}}");
  os_memory_write(os, "static/icons/a.txt", "static");
  const struct ContentEntry newer = {.title = S("Newer"), .url_path = S("/newer.html")};
  const struct ContentEntry older = {.title = S("Older"), .url_path = S("/older.html")};
  const struct ContentEntry* const entries[] = {&newer, &older};
  static const struct Str aggregates[] = {STR_INIT("index.html"), STR_INIT("nested/links.html")};
  static const struct Str feeds[] = {STR_INIT("feeds/atom.xml")};
  struct SiteConfig site_config;
  init_config(&site_config);
  site_config.title = S("Site & Co");
  site_config.author = S("Ada");
  site_config.base_url = S("https://example.com");
  site_config.static_dir = S("static");
  struct Arena paths = test_arena();
  struct PathList static_paths = {0};
  path_list_push(&static_paths, S("static/icons/a.txt"), &paths);

  struct Arena output = test_arena();
  struct Log log = test_log();
  TEST_CHECK(site_writer_write_templates(&site_config, aggregates, 2, entries, 2, SITE_UPDATED, os,
                                         output, test_arena(), &log) == 0);
  TEST_CHECK(site_writer_write_templates(&site_config, feeds, 1, entries, 1, SITE_UPDATED, os,
                                         output, test_arena(), &log) == 0);
  TEST_CHECK(site_writer_copy_static_files(&site_config, &static_paths, os, test_arena(), &log) ==
             0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "<h1>Site &amp; Co</h1>Newer;Older;"));
  TEST_CHECK(os_memory_is_file(os, "public/nested/links.html", "/newer.html;/older.html;"));
  TEST_CHECK(os_memory_is_file(os, "public/feeds/atom.xml",
                               "<updated>2026-07-01T00:00:00Z</updated>Newer;"));
  TEST_CHECK(os_memory_is_file(os, "public/icons/a.txt", "static"));
}

// A failed write names the operation and the cause, then the destination. The destination is an
// existing directory, so opening it for writing fails.
static void test_write_templates_reports_write_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html", "<h1></h1>");
  os_memory_mkdir(os, "public/index.html");
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  struct SiteConfig site_config;
  init_config(&site_config);

  struct Arena output = test_arena();
  struct Log log = test_log();
  TEST_CHECK(site_writer_write_templates(&site_config, aggregates, 1, NULL, 0, SITE_UPDATED, os,
                                         output, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write template output: %s ('public/index.html')\n",
                         strerror(EISDIR)));
}

// A failed static copy names the operation and the cause, then the path that failed: the
// destination when it is an existing directory, and the static file when it is gone.
static void test_copy_static_files_reports_copy_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "static/a.css", "a {}");
  os_memory_mkdir(os, "public/a.css");
  struct SiteConfig site_config;
  init_config(&site_config);
  site_config.static_dir = S("static");
  struct Arena paths = test_arena();
  struct PathList static_paths = {0};
  path_list_push(&static_paths, S("static/a.css"), &paths);
  struct Log log = test_log();

  TEST_CHECK(site_writer_copy_static_files(&site_config, &static_paths, os, test_arena(), &log) ==
             -1);
  TEST_CHECK(
      test_log_is(&log, "failed to copy static file: %s ('public/a.css')\n", strerror(EISDIR)));

  static_paths = (struct PathList){0};
  path_list_push(&static_paths, S("static/gone.css"), &paths);
  log = test_log();
  TEST_CHECK(site_writer_copy_static_files(&site_config, &static_paths, os, test_arena(), &log) ==
             -1);
  TEST_CHECK(
      test_log_is(&log, "failed to copy static file: %s ('static/gone.css')\n", strerror(ENOENT)));
}

TEST_LIST = {
    {"writes complete output layer", test_writes_complete_output_layer},
    {"write templates reports write failure", test_write_templates_reports_write_failure},
    {"copy static files reports copy failure", test_copy_static_files_reports_copy_failure},
    {NULL, NULL},
};
