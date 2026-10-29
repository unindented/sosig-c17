#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/cmd_build.h"
#include "app/exit_code.h"
#include "build/entry_renderer.h"
#include "build/job.h"
#include "core/path_list.h"
#include "formats/toml.h"
#include "os_memory.h"
#include "runtime/os.h"
#include "runtime/pool.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// Every build here runs against the in-memory host, whose root is the working directory the build
// resolves `sosig.toml` from, and which keeps what the build prints. The golden sites build the
// real executable on the real filesystem.

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest text a test spells out.
 */
enum { EXPECTED_SIZE = 512 };

/**
 * Bytes of the arenas `build_in_tiny_arena` gives a build, a quarter for each of the main thread's
 * and the worker's perm and scratch. Each is too small for the configuration the build loads.
 */
enum { TINY_ARENA_SIZE = 128 };

/** Config body every fixture shares. It configures no aggregate or feed templates. */
static const char* const SITE_CONFIG =
    "base_url = \"https://example.com\"\n"
    "title = \"Site\"\n"
    "author = \"Author\"\n"
    "aggregate_templates = []\n"
    "feed_templates = []\n";

/** Minimal content template, which renders a page as its body alone. */
static const char* const CONTENT_TEMPLATE = "{{{body}}}\n";

/** One published content entry whose body renders as `<p>Body</p>`. */
static const char* const HELLO_ENTRY =
    "+++\n"
    "title = \"Hello\"\n"
    "date = 2026-07-01T00:00:00Z\n"
    "+++\n"
    "Body\n";

/** Page `HELLO_ENTRY` renders to through `CONTENT_TEMPLATE`. */
static const char* const HELLO_PAGE = "<p>Body</p>\n\n";

/** Files `every_output_kind_fixture` builds to, in byte order. */
static const char* const EVERY_OUTPUT_KIND_FILES[] = {
    "public/alpha.html", "public/beta.html", "public/feed.xml",
    "public/index.html", "public/sosig.css",
};

/** Number of paths in `EVERY_OUTPUT_KIND_FILES`. */
enum {
  EVERY_OUTPUT_KIND_FILE_COUNT = sizeof(EVERY_OUTPUT_KIND_FILES) /
  sizeof(EVERY_OUTPUT_KIND_FILES[0])
};

/**
 * @brief Returns a host holding `sosig.toml` and the content template.
 *
 * @param config_extra Config lines placed before the shared body, or `NULL` for the body alone. It
 *                     must not repeat a key the body sets, because TOML rejects a duplicate key.
 * @return The host.
 */
static struct Os* site_fixture(const char* config_extra) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  struct Arena arena = test_arena();
  struct Buf config = {.arena = &arena};
  if (config_extra != NULL) {
    buf_append(&config, str_from_cstr(config_extra));
  }
  buf_append(&config, str_from_cstr(SITE_CONFIG));
  os_memory_write(os, "sosig.toml", buf_cstr(&config));
  return os;
}

/**
 * @brief Returns a host holding a site with one output of every kind: two content pages beside a
 *        draft, an aggregate, a feed, and a static file, which build to `EVERY_OUTPUT_KIND_FILES`.
 *
 * @return The host.
 */
static struct Os* every_output_kind_fixture(void) {
  // The shared config turns off aggregates and feeds, so this fixture writes its own.
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "base_url = \"https://example.com\"\n"
                  "title = \"Site\"\n"
                  "author = \"Author\"\n"
                  "aggregate_templates = [\"index.html\"]\n"
                  "feed_templates = [\"feed.xml\"]\n");
  os_memory_write(os, "content/alpha.md",
                  "+++\ntitle = \"Alpha\"\ndate = 2026-07-02T00:00:00Z\n+++\nAlpha body\n");
  os_memory_write(os, "content/beta.md",
                  "+++\ntitle = \"Beta\"\ndate = 2026-07-01T00:00:00Z\n+++\nBeta body\n");
  os_memory_write(os, "content/draft.md",
                  "+++\ntitle = \"Draft\"\ndate = 2026-07-03T00:00:00Z\ndraft = true\n+++\n"
                  "Draft body\n");
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/index.html", "index\n");
  os_memory_write(os, "templates/feed.xml", "feed\n");
  os_memory_write(os, "static/sosig.css", "body{}\n");
  return os;
}

/**
 * @brief Executes a build with `options` in test arenas and collects its errors.
 *
 * @param options Build options passed to `cmd_build_execute`, whose worker count `0` stands for the
 *                CPU count, as on the command line. Must not be `NULL`.
 * @param os      Host the build runs against. Must not be `NULL`.
 * @param log     Log that receives every build error. Its arena must be none of the arenas the
 *                build allocates from, as `cmd_build_execute` requires. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build_with(const struct BuildOptions* options, struct Os* os, struct Log* log) {
  struct BuildOptions resolved = *options;
  if (resolved.worker_count == 0) {
    resolved.worker_count = pool_resolve_worker_count();
  }
  struct Arena perm = test_arena();
  struct JobArenas* worker_arenas = arena_new(&perm, resolved.worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < resolved.worker_count; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  return cmd_build_execute(&resolved, worker_arenas, os, perm, test_arena(), log);
}

/**
 * @brief Executes a build with default options and collects its errors.
 *
 * @param os  Host the build runs against. Must not be `NULL`.
 * @param log Log that receives every build error. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build(struct Os* os, struct Log* log) {
  const struct BuildOptions options = {0};
  return execute_build_with(&options, os, log);
}

/**
 * @brief Executes a dry run with otherwise default options and collects its errors.
 *
 * @param os  Host the dry run runs against. Must not be `NULL`.
 * @param log Log that receives every build error. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_dry_run(struct Os* os, struct Log* log) {
  const struct BuildOptions options = {.is_dry_run = true};
  return execute_build_with(&options, os, log);
}

/**
 * @brief Reports whether the regular files below `dir_path` are exactly `expected`, printing them
 *        when they are not.
 *
 * @param os             Host to inspect. Must not be `NULL`.
 * @param dir_path       Terminated directory to list. Must not be `NULL`.
 * @param expected       Array of `expected_count` terminated paths in byte order. Must not be
 *                       `NULL`.
 * @param expected_count Number of expected paths.
 * @return `true` when the files match.
 */
static bool is_file_list(struct Os* os,
                         const char* dir_path,
                         const char* const* expected,
                         ptrdiff_t expected_count) {
  const struct PathList files = os_memory_files(os, dir_path);
  bool is_equal = files.count == expected_count;
  for (ptrdiff_t i = 0; is_equal && i < expected_count; i++) {
    is_equal = str_eq(files.items[i], str_from_cstr(expected[i]));
  }
  TEST_CHECK_(is_equal, "'%s' holds exactly the expected files", dir_path);
  for (ptrdiff_t i = 0; !is_equal && i < files.count; i++) {
    TEST_MSG("found: '%.*s'", STR_FMT(files.items[i]));
  }
  return is_equal;
}

/**
 * @brief Reports whether `os_print` has written `line` as a whole line to `fd`, printing the stream
 *        when it has not.
 *
 * @param os   Host to inspect. Must not be `NULL`.
 * @param fd   `1` for standard output or `2` for standard error.
 * @param line Terminated line, without its newline. Must not be `NULL`.
 * @return `true` when one of the printed lines is `line`.
 */
static bool has_printed_line(struct Os* os, int fd, const char* line) {
  const struct Str printed = os_memory_printed(os, fd);
  bool is_printed = false;
  struct StrCut cut = {.tail = printed, .is_found = true};
  while (!is_printed && cut.is_found) {
    cut = str_cut(cut.tail, '\n');
    is_printed = str_eq(cut.head, str_from_cstr(line));
  }
  TEST_CHECK_(is_printed, "'%s' is a printed line", line);
  if (!is_printed) {
    TEST_MSG("printed: '%.*s'", STR_FMT(printed));
  }
  return is_printed;
}

/**
 * @brief Reports whether the `write` lines a dry run printed name exactly the files below `public`
 *        on another host, printing the listing when they do not.
 *
 * Every line must be a `write` line, and the manifest makes the listed paths distinct, so a count
 * that matches and a path for each line that exists there make the two sets equal.
 *
 * @param dry_run_os Host the dry run printed its listing on. Must not be `NULL`.
 * @param real_os    Host a real build of the same site wrote. Must not be `NULL`.
 * @return `true` when the sets are equal.
 */
static bool is_write_set_of(struct Os* dry_run_os, struct Os* real_os) {
  const struct Str printed = os_memory_printed(dry_run_os, 1);
  const struct PathList files = os_memory_files(real_os, "public");
  struct Arena arena = test_arena();
  ptrdiff_t line_count = 0;
  bool is_equal = true;
  for (struct StrCut cut = str_cut(printed, '\n'); cut.is_found; cut = str_cut(cut.tail, '\n')) {
    const struct StrCut fields = str_cut(cut.head, '\t');
    is_equal = is_equal && str_eq(fields.head, S("write")) &&
               os_memory_has_path(real_os, str_cstr(fields.tail, &arena));
    line_count++;
  }
  is_equal = is_equal && line_count == files.count;
  TEST_CHECK_(is_equal, "the dry run lists exactly the files a real build writes");
  if (!is_equal) {
    TEST_MSG("printed: '%.*s'", STR_FMT(printed));
  }
  return is_equal;
}

/**
 * @brief Runs a one-worker build in `TINY_ARENA_SIZE` bytes of arenas. Run through `test_is_oom`,
 *        which calls it in a child process.
 *
 * @param userdata Host the build runs against, as a `struct Os*`. Must not be `NULL`.
 */
static void build_in_tiny_arena(void* userdata) {
  static _Alignas(max_align_t) char memory[TINY_ARENA_SIZE];
  struct Arena rest = {.beg = memory, .end = memory + sizeof(memory)};
  const struct Arena perm = arena_split(&rest, sizeof(memory) / 4);
  const struct Arena scratch = arena_split(&rest, sizeof(memory) / 4);
  struct JobArenas worker_arenas[1] = {
      {.perm = arena_split(&rest, sizeof(memory) / 4), .scratch = rest}};
  struct Log log = test_log();
  const struct BuildOptions options = {.worker_count = 1};
  (void)cmd_build_execute(&options, worker_arenas, userdata, perm, scratch, &log);
}

// A configured content template name is used instead of the default.
static void test_honors_configured_content_template(void) {
  struct Os* os = site_fixture("content_template = \"content-entry.html\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates/content-entry.html", "<main>{{title}} {{{body}}}</main>\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello.html", "<main>Hello <p>Body</p>\n</main>\n"));
}

// A build writes exactly the manifest's intended outputs: content pages, aggregate and feed
// outputs, static files, and nothing for drafts.
static void test_writes_exactly_manifest_outputs(void) {
  struct Os* os = every_output_kind_fixture();

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
}

// A trailing slash on `content_dir` does not leak the directory name into the output tree. The
// prefix strip that derives an entry's section compares `<content_dir>/`, so an unnormalized
// `"content/"` would miss and publish `public/content/post.html`.
static void test_tolerates_trailing_slash_on_content_dir(void) {
  struct Os* os = site_fixture("content_dir = \"content/\"\n");
  os_memory_write(os, "content/post.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/post.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 1));
}

// A site that ships no `static` directory builds. `static_dir` is optional with a default, so an
// absent one is skipped rather than reported.
static void test_builds_with_no_static_dir(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  TEST_CHECK(!os_memory_has_path(os, "static"));

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello.html", HELLO_PAGE));
}

// A `static` directory is mirrored into `output_dir` as-is, so a top-level file lands at the output
// root and a nested one keeps its directories.
static void test_copies_static_files_when_present(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static/robots.txt", "User-agent: *\n");
  os_memory_write(os, "static/assets/sosig.css", "body{}\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/robots.txt", "User-agent: *\n"));
  TEST_CHECK(os_memory_is_file(os, "public/assets/sosig.css", "body{}\n"));
}

// A custom pretty permalink publishes `<slug>/index.html` instead of `<slug>.html`. The golden test
// compares that output shape end to end with `tests/expected/site_index_permalink`. What this adds
// is the assertion at the command boundary.
static void test_honors_custom_permalink(void) {
  struct Os* os = site_fixture("permalink = \"/{slug}/\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello/index.html", HELLO_PAGE));
}

// Same-named sources in different directories publish to distinct, non-colliding outputs under the
// default permalink.
static void test_distinguishes_same_name_in_different_dirs(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a/post.md", HELLO_ENTRY);
  os_memory_write(os, "content/b/post.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/a/post.html", "public/b/post.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 2));
}

// A second build over a finished output tree succeeds and produces the same set of outputs.
static void test_rebuild_succeeds_and_repeats_its_outputs(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The one content page: the second build adds nothing and removes nothing.
  static const char* const expected[] = {"public/hello.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 1));
}

// A rebuild removes what the current inputs no longer produce: the page of a deleted entry, an old
// section tree, and a file added to `output_dir` by hand. Every current output stays, and a third
// build finds nothing more to remove.
static void test_rebuild_removes_stale_outputs(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "content/goodbye.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_memory_has_path(os, "public/goodbye.html"));

  TEST_CHECK(os_remove(os, S("content/goodbye.md"), S("failed to remove fixture"), &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_write(os, "public/old-section/deeper/page.html", "old");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/hello.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 1));
  TEST_CHECK(!os_memory_has_path(os, "public/old-section"));

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", expected, 1));
}

// An `output_dir` that a symlink inside `content_dir` reaches is left out of the content walk, so a
// rebuild does not read the first build's outputs back as sources. The aggregate here publishes a
// `.md` file, which the walk would otherwise parse as a content entry and claim as an input.
static void test_rebuild_skips_output_dir_linked_from_content_dir(void) {
  // The shared config turns off aggregates, so this fixture writes its own.
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "base_url = \"https://example.com\"\n"
                  "title = \"Site\"\n"
                  "author = \"Author\"\n"
                  "aggregate_templates = [\"notes.md\"]\n"
                  "feed_templates = []\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/notes.md", "notes\n");
  os_memory_symlink(os, "../public", "content/published");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The page and the aggregate, and no page rendered from the aggregate's own output.
  static const char* const expected[] = {"public/hello.html", "public/notes.md"};
  TEST_CHECK(is_file_list(os, "public", expected, 2));
}

// A dry run of a site with every kind of output succeeds and creates nothing, not even
// `output_dir`, which shows `prepare_output_dir` was withheld. Its `write` lines name exactly the
// files a real build of the same site writes, so no site left a path pending, and they come in
// manifest order: content pages newest first, then the aggregate, the feed and the static file.
static void test_dry_run_lists_every_output_and_writes_nothing(void) {
  struct Os* real_os = every_output_kind_fixture();
  struct Os* dry_run_os = every_output_kind_fixture();

  struct Log log = test_log();
  TEST_CHECK(execute_build(real_os, &log) == 0);
  TEST_CHECK(execute_dry_run(dry_run_os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(dry_run_os, "public"));
  TEST_CHECK(is_write_set_of(dry_run_os, real_os));
  TEST_CHECK(os_memory_is_printed(dry_run_os, 1,
                                  "write\tpublic/alpha.html\n"
                                  "write\tpublic/beta.html\n"
                                  "write\tpublic/index.html\n"
                                  "write\tpublic/feed.xml\n"
                                  "write\tpublic/sosig.css\n"));
}

// After a real build, a dry run lists the one output, then what the prune would remove, sorted: a
// deleted entry's page, a hand-added file, and a stale directory with its page. It leaves each of
// them in place. Its verbose status counts the `remove` lines, on `stderr`, and adds nothing to the
// listing.
static void test_dry_run_lists_stale_outputs_and_keeps_them(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "content/goodbye.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_remove(os, S("content/goodbye.md"), S("failed to remove fixture"), &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_write(os, "public/old/page.html", "old");

  const struct BuildOptions options = {.is_verbose = true, .is_dry_run = true};
  TEST_CHECK(execute_build_with(&options, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "write\tpublic/hello.html\n"
                                  "remove\tpublic/goodbye.html\n"
                                  "remove\tpublic/notes.txt\n"
                                  "remove\tpublic/old\n"
                                  "remove\tpublic/old/page.html\n"));
  TEST_CHECK(has_printed_line(os, 2, "removed stale outputs: 4"));
  TEST_CHECK(os_memory_is_file(os, "public/goodbye.html", HELLO_PAGE));
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
  TEST_CHECK(os_memory_is_file(os, "public/old/page.html", "old"));
}

// A dry run over an existing tree, where every write and every removal below `output_dir` would
// fail, still succeeds, so it attempts neither.
static void test_dry_run_attempts_no_change(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public", EACCES);
  os_memory_fail(os, OS_MEMORY_OP_REMOVE, "public", EACCES);

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
}

// A dry run leaves nothing behind that a later build sees: a real build after it writes the same
// files as a real build alone.
static void test_dry_run_leaves_no_state(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "index\n"));
}

// A caller-supplied `worker_count` reaches every pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose phase lines, which are the only
// place the module reports it. Workers finish their jobs in any order, so the progress lines are
// not compared.
static void test_honors_requested_worker_count(void) {
  // One worker, requested explicitly, must be honored rather than read as unset. A guard spelled
  // `worker_count != 1` instead of `!= 0` swallows that request and falls back to the detected core
  // count, which asking for `2` cannot see. Asking for one is also the only way to ask for a serial
  // build. A request the build dropped would show the detected count. Both cases assume the host
  // reports at least three cores.
  static const ptrdiff_t worker_counts[] = {2, 1};
  static const char* const source_paths[] = {"content/a.md", "content/b.md", "content/c.md"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(worker_counts) / sizeof(worker_counts[0])); i++) {
    struct Os* os = site_fixture(NULL);
    for (ptrdiff_t j = 0; j < 3; j++) {
      os_memory_write(os, source_paths[j], HELLO_ENTRY);
    }
    const struct BuildOptions options = {.worker_count = worker_counts[i], .is_verbose = true};
    TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
    TEST_CHECK(os_memory_is_printed(os, 1, ""));
    struct Arena arena = test_arena();
    struct Buf parse_line = {.arena = &arena};
    struct Buf render_line = {.arena = &arena};
    buf_printf(&parse_line, "parsing content, workers: %td", worker_counts[i]);
    buf_printf(&render_line, "rendering content, workers: %td", worker_counts[i]);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&parse_line)));
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&render_line)));
    // The requested count changes only how the work is scheduled, never the output.
    TEST_CHECK(os_memory_is_file(os, "public/a.html", HELLO_PAGE));
    TEST_CHECK(os_memory_is_file(os, "public/c.html", HELLO_PAGE));
  }
}

// A build runs at most four workers per online CPU, however many it is asked for, and the verbose
// phase lines name that count. `BUILD_WORKER_PER_CPU_MAX` is file-local to `cmd_build.c`, so the
// four is spelled here, and changing it must change this test.
static void test_caps_the_worker_count_at_four_per_cpu(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  const struct BuildOptions options = {.worker_count = 100000, .is_verbose = true};
  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
  struct Arena arena = test_arena();
  struct Buf parse_line = {.arena = &arena};
  struct Buf render_line = {.arena = &arena};
  buf_printf(&parse_line, "parsing content, workers: %td", 4 * pool_resolve_worker_count());
  buf_printf(&render_line, "rendering content, workers: %td", 4 * pool_resolve_worker_count());
  TEST_CHECK(has_printed_line(os, 2, buf_cstr(&parse_line)));
  TEST_CHECK(has_printed_line(os, 2, buf_cstr(&render_line)));
  TEST_CHECK(os_memory_is_file(os, "public/hello.html", HELLO_PAGE));
}

// Under an address-space limit the build halves its worker count, down to one, before it halves any
// arena, and the verbose phase lines name the count it kept. The limits follow the sizes in
// `cmd_build.c`: 30 GiB lies between the 22 GiB two workers need and the 36.5 GiB four need, and
// 10 GiB is under the 14.75 GiB of one, which then runs on arenas halved twice.
static void test_cuts_workers_before_arenas_under_limit(void) {
  static const struct {
    ptrdiff_t len_max;
    ptrdiff_t worker_count;
  } cases[] = {
      {(ptrdiff_t)30 << 30, 2},
      {(ptrdiff_t)10 << 30, 1},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = site_fixture(NULL);
    os_memory_write(os, "content/hello.md", HELLO_ENTRY);
    const struct BuildOptions options = {.worker_count = 4, .is_verbose = true};
    arena_reserve_len_max = cases[i].len_max;
    TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
    arena_reserve_len_max = PTRDIFF_MAX;
    struct Arena arena = test_arena();
    struct Buf parse_line = {.arena = &arena};
    buf_printf(&parse_line, "parsing content, workers: %td", cases[i].worker_count);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&parse_line)));
    TEST_CHECK(os_memory_is_file(os, "public/hello.html", HELLO_PAGE));
    TEST_MSG("case %td", i);
  }
}

// A site with no content sources skips the parse and render phases, so the verbose log carries no
// status or progress line for either. A phase with no jobs returns before it prints, so an empty
// site does not report a worker count for work it never starts.
static void test_skips_empty_phases_in_verbose_output(void) {
  struct Os* os = site_fixture(NULL);
  // A file that is not a source keeps `content_dir` present while the site holds no sources.
  os_memory_write(os, "content/notes.txt", "not a source\n");
  const struct BuildOptions options = {.worker_count = 3, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "discovering content\n"
                                  "discovering static files\n"
                                  "collecting content entries\n"
                                  "building output manifest\n"
                                  "rendering aggregate templates\n"
                                  "rendering feed templates\n"
                                  "copying static files\n"
                                  "removing stale outputs\n"
                                  "removed stale outputs: 0\n"
                                  "build complete\n"));
}

// A build that succeeds leaves its log empty, which is the other half of `cmd_build_execute`'s
// `log` contract. Without this a phase that logged on the success path would go unnoticed, since
// every other test that inspects the log is a failure test.
static void test_leaves_log_empty_on_success(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// standard error exactly once, which no other test asserts. Every other test routes failures
// through `cmd_build_execute` to inspect the message, so without this the boundary itself is
// unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  // No `sosig.toml`, the earliest failure, so the diagnostic is a single known line.
  struct Os* os = os_memory();
  const struct BuildOptions options = {0};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "failed to read config: %s ('sosig.toml')\n", strerror(ENOENT));
  TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
}

// A build whose arena is too small for its data ends in `os_oom` rather than failing with a
// diagnostic or overrunning the arena. Under the test build's `os_oom` that is the process exiting
// with `TEST_OOM_EXIT_STATUS`. `main.c`'s `os_oom` exits with `EXIT_CODE_FAILURE` instead, so this
// test does not pin the production exit status.
static void test_tiny_arena_ends_in_oom(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  TEST_CHECK(test_is_oom(build_in_tiny_arena, os));
}

// A missing `sosig.toml` is logged with its cause, and nothing prints it before the command
// boundary. The cause is part of the claim: a missing config must not read like an unreadable one.
static void test_reports_missing_config(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log, "failed to read config: %s ('sosig.toml')\n", strerror(ENOENT)));
}

// `content_dir` is required, so an absent one is a failure, reported as a resolution failure
// carrying the system cause. `ENOENT` and `EACCES` need different fixes and nothing later would
// report either, so the cause has to appear here. Compared exactly, so a caller that appended the
// path a second time would fail.
static void test_reports_absent_content_dir(void) {
  struct Os* os = site_fixture(NULL);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'content_dir': %s ('content')\n",
                         strerror(ENOENT)));
}

// `templates_dir` is required the same way, and reports the key the user has to fix rather than the
// one that happened to be checked first. A root that cannot be inspected reports its own cause.
static void test_reports_absent_templates_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", SITE_CONFIG);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(ENOENT)));

  os_memory_fail(os, OS_MEMORY_OP_STAT, "content", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'content_dir': %s ('content')\nfailed "
                         "to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(EACCES), strerror(ENOENT)));
}

// A directory key the config load rejects is reported once. Its default names a directory the user
// never chose, so the build does not go on to report that default missing as well. A `/` is
// rejected as empty once its trailing `/` is trimmed, the same way.
static void test_reports_a_rejected_directory_key_once(void) {
  static const char* const cases[][4] = {
      {"content_dir = 5\n", "posts/hello.md", "templates/content.html",
       "config key 'content_dir' must be a string"},
      {"content_dir = \"/\"\n", "posts/hello.md", "templates/content.html",
       "config key 'content_dir' must not be empty"},
      {"templates_dir = [\"layouts\"]\n", "content/hello.md", "layouts/content.html",
       "config key 'templates_dir' must be a string"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i][0]);
    struct Os* os = os_memory();
    struct Arena config_arena = test_arena();
    struct Buf config = {.arena = &config_arena};
    buf_append(&config, str_from_cstr(cases[i][0]));
    buf_append(&config, str_from_cstr(SITE_CONFIG));
    os_memory_write(os, "sosig.toml", buf_cstr(&config));
    os_memory_write(os, cases[i][1], HELLO_ENTRY);
    os_memory_write(os, cases[i][2], CONTENT_TEMPLATE);
    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][3]));
  }
  TEST_CASE_(NULL);
}

// A rejected `static_dir` is reported once too, even when its default names something that would
// fail on its own: here a regular file, which the static walk would report as not a directory.
static void test_reports_a_rejected_static_dir_once(void) {
  struct Os* os = site_fixture("static_dir = \"\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static", "not a directory\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "config key 'static_dir' must not be empty"));
}

// A `templates_dir` that exists but is a regular file is reported as the wrong type, which is its
// own cause rather than an `errno`, so the message carries no system text.
static void test_reports_templates_dir_that_is_a_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", SITE_CONFIG);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates", "not a directory\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': not a directory "
                         "('templates')\n"));
}

// A configured `output_dir` that already exists as a regular file is reported once the manifest has
// passed, where the build creates `output_dir`, before any page is written. The reason names the
// component that failed and the caller does not repeat the configured root, so the message is
// compared whole.
static void test_reports_unusable_output_dir(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  // `public` is the default `output_dir`, so a file there is what makes `os_mkdirs` fail.
  os_memory_write(os, "public", "not a directory");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: exists and is not a directory ('public')\n"));
}

// An `output_dir` that is a dangling symlink is reported once the manifest has passed, where the
// build creates `output_dir`, before any page is written. A dangling symlink has no identity, so
// the manifest sees nothing there, and `os_mkdirs` then fails the `stat` that follows `mkdir`'s
// `EEXIST`. The message is compared whole.
static void test_reports_dangling_output_dir_symlink(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_symlink(os, "missing", "public");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: cannot inspect directory: %s ('public')\n",
      strerror(ENOENT)));
}

// An `output_dir` below `content_dir` is refused before content is discovered, so no earlier
// build's output can be read back as a source, and it is refused although it does not exist yet.
// The verbose phase lines show that the build stopped before the content walk, and the refused
// build creates none of the nested directories.
static void test_rejects_output_dir_inside_content_dir(void) {
  struct Os* os = site_fixture("output_dir = \"content/generated/site\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  const struct BuildOptions options = {.is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  // The status line comes first and the log once the build has stopped.
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would write inside 'content_dir': "
                                  "'content/generated/site'\n"));
  TEST_CHECK(!os_memory_has_path(os, "content/generated"));
}

// An `output_dir` that holds `content_dir`, such as `output_dir = "."`, is refused before content
// is discovered, so the project's own files never share a tree with its outputs, and the refused
// build writes nothing beside them.
static void test_rejects_output_dir_holding_content_dir(void) {
  struct Os* os = site_fixture("output_dir = \".\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  const struct BuildOptions options = {.is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would hold 'content_dir': '.'\n"));
  // The config, the template, and the source.
  static const char* const expected[] = {"./content/hello.md", "./sosig.toml",
                                         "./templates/content.html"};
  TEST_CHECK(is_file_list(os, ".", expected, 3));
}

// Slugging is lossy, so two different source names can claim one output path. The manifest reports
// the collision and names both sources instead of letting the second write win. A non-ASCII byte
// folds to hex, so `caf\xC3\xA9` produces the slug a literal `cafc3a9` also produces. The refused
// build creates no `output_dir`, because nothing is written before the manifest passes.
static void test_rejects_colliding_source_names(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/caf\xC3\xA9.md", HELLO_ENTRY);
  os_memory_write(os, "content/cafc3a9.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/cafc3a9.md' and "
                         "'content/caf\xC3\xA9.md': 'public/cafc3a9.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A static file copied to the output root shares its paths with the generated pages, so one named
// after a content page is rejected as a duplicate of it rather than overwriting it or being
// overwritten. The refused build creates no `output_dir`.
static void test_rejects_static_file_colliding_with_content_page(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static/hello.html", "static\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/hello.md' and "
                         "'static/hello.html': 'public/hello.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A content template that fails to parse fails the build at the page phase. The diagnostic carries
// the parser's cause, its position and the template name, followed by the entry that was being
// rendered, so a template error reads apart from a template that cannot be read.
static void test_reports_bad_template(void) {
  struct Os* os = site_fixture("content_template = \"broken.html\"\n");
  // The section never closes, so the template is read but fails to compile.
  os_memory_write(os, "templates/broken.html", "{{#title}}{{title}}\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "section-opening tag has no closer at line 1, column 1 (in 'broken.html') "
                         "(while rendering 'content/hello.md')\n"));
}

// A dry run renders every template, so a content template that does not compile fails it with the
// diagnostic a real build gives, and no listing is printed.
static void test_dry_run_reports_bad_template(void) {
  struct Os* os = site_fixture("content_template = \"broken.html\"\n");
  os_memory_write(os, "templates/broken.html", "{{#title}}{{title}}\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_dry_run(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "section-opening tag has no closer at line 1, column 1 (in 'broken.html') "
                         "(while rendering 'content/hello.md')\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A dry run of two entries on one output path fails with the duplicate-path diagnostic a real build
// gives, and no listing is printed.
static void test_dry_run_rejects_duplicate_output(void) {
  struct Os* os = site_fixture("permalink = \"/{slug}.html\"\n");
  os_memory_write(os, "content/a/post.md", HELLO_ENTRY);
  os_memory_write(os, "content/b/post.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_dry_run(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/a/post.md' and "
                         "'content/b/post.md': 'public/post.html'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A build that fails removes nothing, so a stale file from an earlier build is still there after a
// render failure.
static void test_failed_build_removes_no_stale_output(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "templates/content.html", "{{#title}}{{title}}\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "public/notes.txt", "hand");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
}

// A stale entry that cannot be removed fails the build after every output is written, and the
// failure names the entry.
static void test_reports_stale_output_that_cannot_be_removed(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_fail(os, OS_MEMORY_OP_REMOVE, "public/notes.txt", EACCES);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to remove stale output: %s ('public/notes.txt')\n",
                         strerror(EACCES)));
  TEST_CHECK(os_memory_has_path(os, "public/hello.html"));
  TEST_CHECK(os_memory_has_path(os, "public/notes.txt"));
}

// Two content entries that fail to render through one missing template fail for one cause. The
// report prints the first entry's line whole and counts the second, and both count as errors.
// `test_reports_unwritable_output` covers two entries that fail for different causes.
static void test_reports_a_shared_render_failure_once(void) {
  // No `templates/missing.html`, so the page render fails for both entries.
  struct Os* os = site_fixture("content_template = \"missing.html\"\n");
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.md",
                  "+++\ntitle = \"Second\"\ndate = 2026-07-02T00:00:00Z\n+++\nBody\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Sources are walked in sorted order, so `content/a.md` is the one printed.
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: %s ('templates/missing.html') (while rendering "
                         "'content/a.md')\n\xE2\x80\xA6 and 1 more failures\n",
                         strerror(ENOENT)));
  TEST_CHECK(log.errors == 2);
}

// An output path that cannot be written fails the build at the page phase, where each job writes
// the page it rendered. The failure is that entry's own diagnostic, so the other entries' pages are
// still written. A write failure reported as success would otherwise pass the suite with the page
// silently missing. One page path is an existing directory, and the other fails with an injected
// `EACCES`, as an unwritable directory would refuse it.
static void test_reports_unwritable_output(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.md", HELLO_ENTRY);
  os_memory_write(os, "content/c.md", HELLO_ENTRY);
  os_memory_mkdir(os, "public/a.html");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/c.html", EACCES);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to write output for 'content/a.md': %s ('public/a.html')\nfailed "
                         "to write output for 'content/c.md': %s ('public/c.html')\n",
                         strerror(EISDIR), strerror(EACCES)));
  TEST_CHECK(os_memory_is_file(os, "public/b.html", HELLO_PAGE));
}

// A permalink that would expand to a path escaping the output directory is rejected at config load,
// so the build reports it once rather than once per content entry.
static void test_rejects_unsafe_permalink(void) {
  struct Os* os = site_fixture("permalink = \"/{slug}/../evil.html\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // The rejected permalink keeps its default, so the entries that follow are checked against a
  // valid one and add nothing. Exact, not by substring: a substring check would also pass for this
  // message with something appended to it.
  TEST_CHECK(test_log_is(&log,
                         "config key 'permalink' must expand to a safe relative path using only "
                         "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                         "with no empty, '.' or '..' path segment: '/{slug}/../evil.html'\n"));
  TEST_CHECK(log.errors == 1);
}

// A rejected permalink or template list keeps a default the user never chose, so the build stops
// after the config load rather than reporting a collision with an output the default names. Without
// the stop, `content/index.md` collides with the default `aggregate_templates[0]` in the first
// case, and the default permalink's output in the second.
static void test_stops_after_rejected_output_key(void) {
  static const struct {
    const char* config;
    const char* expected;
  } cases[] = {
      {"aggregate_templates = [5]\n",
       "config key 'aggregate_templates' must contain only strings\n"},
      {"feed_templates = [\"../x\"]\npermalink = \"/{slug}.htm#l\"\n",
       "config key 'permalink' must expand to a safe relative path using only letters, digits, "
       "'_', '-', '.', '/' and the '{slug}'/'{section}' tokens, with no empty, '.' or '..' path "
       "segment: '/{slug}.htm#l'\n"
       "config key 'feed_templates' must contain only safe relative template names: '../x'\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].config);
    struct Os* os = os_memory();
    char config[512];
    const int config_len = snprintf(config, sizeof(config),
                                    "base_url = \"https://example.com\"\n"
                                    "title = \"Site\"\n"
                                    "author = \"Author\"\n"
                                    "%s",
                                    cases[i].config);
    TEST_ASSERT(config_len > 0 && config_len < (int)sizeof(config));
    os_memory_write(os, "sosig.toml", config);
    os_memory_write(os, "content/index.md", HELLO_ENTRY);
    os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
    os_memory_write(os, "templates/index.html", "index\n");

    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i].expected));
    TEST_CHECK(!os_memory_has_path(os, "public"));
  }
}

// A content entry that cannot be parsed fails the build at the parse phase and its diagnostic
// reaches the command boundary. This is a different wiring from
// `test_reports_a_shared_render_failure_once`, which fails at the *page* phase: the two failures
// travel through separate entry points, `entry_renderer_render_entries` and
// `page_renderer_render_pages`, so neither test covers the other's propagation. Without this, no
// test shows a parse-phase error reaching the log that stops the build before its first write. An
// unreadable source fails the same phase.
static void test_reports_unparsable_content(void) {
  struct Os* os = site_fixture(NULL);
  // No closing fence, so `frontmatter_split` rejects the file before any key is read.
  os_memory_write(os, "content/a.md", "+++\ntitle = \"X\"\ndate = 2026-07-01T00:00:00Z\nBody\n");
  os_memory_write(os, "content/b.md", HELLO_ENTRY);
  os_memory_fail(os, OS_MEMORY_OP_READ, "content/b.md", EACCES);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "missing closing '+++' frontmatter fence (in 'content/a.md')\nfailed to "
                         "read content: %s ('content/b.md')\n",
                         strerror(EACCES)));
}

// A source over the content limit is rejected by its read. The file is extended as `truncate`
// extends a sparse file, so the fixture costs no memory.
static void test_rejects_oversize_source_at_read(void) {
  enum { BIG_SOURCE_LEN = 1024 * 1024 * 1024 };
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read content: exceeds max file size (%d bytes) at %d bytes ('content/big.md')",
      CONTENT_FILE_LEN_MAX, BIG_SOURCE_LEN);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/big.md", HELLO_ENTRY);
  os_memory_truncate(os, "content/big.md", BIG_SOURCE_LEN);
  const struct BuildOptions options = {.worker_count = 1};

  struct Log log = test_log();
  TEST_CHECK(execute_build_with(&options, os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s\n", expected));
}

// A configured aggregate output that collides with a content entry output is rejected, with the
// collision described in the returned diagnostic. The refused build creates no `output_dir`,
// because nothing is written before the manifest passes.
static void test_rejects_duplicate_output(void) {
  // The shared config turns off aggregates, so this fixture writes its own.
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "base_url = \"https://example.com\"\n"
                  "title = \"Site\"\n"
                  "author = \"Author\"\n"
                  "aggregate_templates = [\"index.html\"]\n"
                  "feed_templates = []\n");
  os_memory_write(os, "content/index.md", HELLO_ENTRY);
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/index.html", "index\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/index.md' and "
                         "'aggregate_templates[0]': 'public/index.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// Several independent mistakes in one site are all reported by one build, and the build writes
// nothing: an unknown config key, two sources whose frontmatter fails in different ways, two
// sources that claim one output path, and a static file that claims a content page. Each kind comes
// from a different phase, so the order is the phase order, and within a phase it is the sorted walk
// order the slots follow.
static void test_reports_every_independent_error(void) {
  const char same_slug[] =
      "+++\n"
      "title = \"Same\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "slug = \"same\"\n"
      "+++\n"
      "Body\n";
  struct Os* os = site_fixture("colour = \"blue\"\n");
  os_memory_write(os, "content/a.md", same_slug);
  os_memory_write(os, "content/b.md", same_slug);
  os_memory_write(os, "content/c.md", "+++\ntitle = \"C\"\n");
  os_memory_write(os, "content/d.md",
                  "+++\ntitle = \"Draft\"\ndate = 2026-07-01T00:00:00Z\ndraft = \"yes\"\n+++\n"
                  "Body\n");
  os_memory_write(os, "static/same.html", "static\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown config key 'colour'\n"
                         "missing closing '+++' frontmatter fence (in 'content/c.md')\n"
                         "frontmatter key 'draft' must be a boolean (in 'content/d.md')\n"
                         "duplicate output path for 'content/a.md' and 'content/b.md': "
                         "'public/same.html'\n"
                         "duplicate output path for 'content/a.md' and 'static/same.html': "
                         "'public/same.html'\n"));
  TEST_CHECK(log.errors == 5);
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A `static_dir` that exists but is a regular file is reported. Skipping an absent optional root
// must not also skip a misconfigured one, which is why the skip asks whether anything is there
// rather than whether a directory is there: the wrong type survives the skip and reaches the type
// check, which names it.
static void test_reports_static_dir_that_is_a_file(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static", "not a directory\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'static_dir': not a directory "
                         "('static')\n"));
}

// A `static_dir` that cannot be inspected is reported with its cause rather than skipped as absent,
// because it may well be there. A symlink that resolves to itself stands in for any such cause.
static void test_reports_static_dir_that_cannot_be_inspected(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_symlink(os, "static", "static");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'static_dir': %s ('static')\n",
                         strerror(ELOOP)));
}

TEST_LIST = {
    {"honors configured content template", test_honors_configured_content_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"tolerates trailing slash on content_dir", test_tolerates_trailing_slash_on_content_dir},
    {"builds with no static dir", test_builds_with_no_static_dir},
    {"copies static files when present", test_copies_static_files_when_present},
    {"honors custom permalink", test_honors_custom_permalink},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"rebuild succeeds and repeats its outputs", test_rebuild_succeeds_and_repeats_its_outputs},
    {"rebuild removes stale outputs", test_rebuild_removes_stale_outputs},
    {"rebuild skips output dir linked from content dir",
     test_rebuild_skips_output_dir_linked_from_content_dir},
    {"dry run lists every output and writes nothing",
     test_dry_run_lists_every_output_and_writes_nothing},
    {"dry run lists stale outputs and keeps them", test_dry_run_lists_stale_outputs_and_keeps_them},
    {"dry run attempts no change", test_dry_run_attempts_no_change},
    {"dry run leaves no state", test_dry_run_leaves_no_state},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"caps the worker count at four per CPU", test_caps_the_worker_count_at_four_per_cpu},
    {"cuts workers before arenas under limit", test_cuts_workers_before_arenas_under_limit},
    {"skips empty phases in verbose output", test_skips_empty_phases_in_verbose_output},
    {"leaves log empty on success", test_leaves_log_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"tiny arena ends in oom", test_tiny_arena_ends_in_oom},
    {"reports missing config", test_reports_missing_config},
    {"reports absent content dir", test_reports_absent_content_dir},
    {"reports absent templates dir", test_reports_absent_templates_dir},
    {"reports a rejected directory key once", test_reports_a_rejected_directory_key_once},
    {"reports a rejected static dir once", test_reports_a_rejected_static_dir_once},
    {"reports templates dir that is a file", test_reports_templates_dir_that_is_a_file},
    {"reports unusable output dir", test_reports_unusable_output_dir},
    {"reports dangling output dir symlink", test_reports_dangling_output_dir_symlink},
    {"rejects output dir inside content dir", test_rejects_output_dir_inside_content_dir},
    {"rejects output dir holding content dir", test_rejects_output_dir_holding_content_dir},
    {"rejects colliding source names", test_rejects_colliding_source_names},
    {"rejects static file colliding with content page",
     test_rejects_static_file_colliding_with_content_page},
    {"reports bad template", test_reports_bad_template},
    {"dry run reports bad template", test_dry_run_reports_bad_template},
    {"dry run rejects duplicate output", test_dry_run_rejects_duplicate_output},
    {"failed build removes no stale output", test_failed_build_removes_no_stale_output},
    {"reports stale output that cannot be removed",
     test_reports_stale_output_that_cannot_be_removed},
    {"reports a shared render failure once", test_reports_a_shared_render_failure_once},
    {"reports unwritable output", test_reports_unwritable_output},
    {"rejects unsafe permalink", test_rejects_unsafe_permalink},
    {"stops after rejected output key", test_stops_after_rejected_output_key},
    {"reports unparsable content", test_reports_unparsable_content},
    {"rejects oversize source at read", test_rejects_oversize_source_at_read},
    {"rejects duplicate output", test_rejects_duplicate_output},
    {"reports every independent error", test_reports_every_independent_error},
    {"reports static dir that is a file", test_reports_static_dir_that_is_a_file},
    {"reports static dir that cannot be inspected",
     test_reports_static_dir_that_cannot_be_inspected},
    {NULL, NULL},
};
