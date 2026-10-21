#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "domain/content_entry.h"
#include "domain/frontmatter.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Parses frontmatter into an entry with a fresh long-lived arena and a fresh scratch arena.
 *
 * Each call reserves its own two arenas, so the parsed values stay valid for the rest of the test
 * and never overlap the scratch the parse worked in.
 *
 * @param entry       Initialized entry that receives the metadata. Must not be `NULL`.
 * @param frontmatter Frontmatter TOML bytes.
 * @param source_path Terminated source file path. Must not be `NULL`.
 * @param log         Log that receives every error `frontmatter_parse` records. Must not be `NULL`.
 * @return The result of `frontmatter_parse`.
 */
static int parse_frontmatter(struct ContentEntry* entry,
                             struct Str frontmatter,
                             const char* source_path,
                             struct Log* log) {
  struct Arena perm = test_arena();
  return frontmatter_parse(entry, frontmatter, str_from_cstr(source_path), &perm, test_arena(),
                           log);
}

/**
 * @brief Reports whether a log holds exactly one error, `expected` attributed to `source_path`, and
 *        then empties the log for the next check.
 *
 * @param log         Log to check and reset. Must not be `NULL`.
 * @param expected    Terminated message without its attribution. Must not be `NULL`.
 * @param source_path Terminated source path the message must name. Must not be `NULL`.
 * @return `true` when the log held exactly that one error.
 */
static bool is_one_error_taken(struct Log* log, const char* expected, const char* source_path) {
  const bool is_match =
      test_log_is(log, "%s (in '%s')\n", expected, source_path) && TEST_CHECK(log->errors == 1);
  log->buf.len = 0;
  log->errors = 0;
  return is_match;
}

// A CRLF document splits without an off-by-one: the frontmatter slice keeps its own trailing `\r\n`
// and the body begins after the closing fence's line ending. `str_eq` compares lengths first, so a
// slice one byte too long or too short fails.
static void test_split_handles_crlf_and_body(void) {
  const char doc[] = "+++\r\ntitle = \"x\"\r\n+++\r\nbody\n";
  struct FrontmatterSplit split;
  struct Log log = test_log();
  TEST_CHECK(frontmatter_split(str_from_cstr(doc), S("content/a.md"), &split, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(split.frontmatter, S("title = \"x\"\r\n")));
  TEST_CHECK(str_eq(split.body, S("body\n")));
}

// `frontmatter_split` skips a leading UTF-8 BOM, so it still recognizes the fence on the same line.
// Several Windows editors write a BOM by default. Without the skip, such a file fails with a
// missing-opening-fence diagnostic that says nothing about the real cause. The BOM belongs to
// neither slice.
static void test_split_skips_utf8_bom(void) {
  const char doc[] = "\xEF\xBB\xBF+++\ntitle = \"x\"\n+++\nbody\n";
  struct FrontmatterSplit split;
  struct Log log = test_log();
  TEST_CHECK(frontmatter_split(str_from_cstr(doc), S("content/a.md"), &split, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(split.frontmatter, S("title = \"x\"\n")));
  TEST_CHECK(str_eq(split.body, S("body\n")));
}

// The body slice is exactly the bytes after the closing fence's line ending, with no assumption
// that the file ends in a newline. A body whose last byte is not a newline is where a length that
// ran one byte short changes which bytes are compared, not merely the asserted count.
static void test_split_reports_body_without_trailing_newline(void) {
  const char doc[] = "+++\ntitle = \"x\"\n+++\nbody";
  struct FrontmatterSplit split;
  struct Log log = test_log();
  TEST_CHECK(frontmatter_split(str_from_cstr(doc), S("content/a.md"), &split, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(split.body, S("body")));
}

// A closing fence that is the file's last line with no trailing newline is still a fence. The body
// is then empty rather than absent. Every other fence in this file ends in a line ending, so a
// split that required the `\n` after a fence would accept them all and reject only this one.
// Editors that omit the final newline produce it routinely. The bare-`\r` variant is the same
// boundary for the `\r` strip.
static void test_split_accepts_closing_fence_at_end_of_file(void) {
  struct FrontmatterSplit split;
  struct Log log = test_log();
  const char doc[] = "+++\ntitle = \"x\"\n+++";
  TEST_CHECK(frontmatter_split(str_from_cstr(doc), S("content/a.md"), &split, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(split.frontmatter, S("title = \"x\"\n")));
  TEST_CHECK(split.body.len == 0);

  const char doc_cr[] = "+++\ntitle = \"x\"\n+++\r";
  struct Log log_cr = test_log();
  TEST_CHECK(frontmatter_split(str_from_cstr(doc_cr), S("content/a.md"), &split, &log_cr) == 0);
  TEST_CHECK(test_log_is_empty(&log_cr));
  TEST_CHECK(split.body.len == 0);
}

// A `NUL` byte in the file is ordinary to the split, which measures lines by length rather than by
// terminator, so the bytes on both sides of it land in the slices. The file-level rejection of such
// a byte is `os_read`'s, covered in `test_os.c`.
static void test_split_keeps_nul_bytes(void) {
  const struct Str doc = S("+++\ntitle = \"x\"\n+++\nbo\0dy\n");
  struct FrontmatterSplit split;
  struct Log log = test_log();
  TEST_CHECK(frontmatter_split(doc, S("content/a.md"), &split, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(split.frontmatter, S("title = \"x\"\n")));
  TEST_CHECK(str_eq(split.body, S("bo\0dy\n")));
}

// Missing opening and closing fences produce distinct diagnostics.
static void test_split_rejects_missing_fences(void) {
  struct FrontmatterSplit split;
  struct Log log = test_log();
  const char missing_open[] = "title = \"x\"\n+++\nbody\n";
  TEST_CHECK(frontmatter_split(str_from_cstr(missing_open), S("content/a.md"), &split, &log) == -1);
  TEST_CHECK(is_one_error_taken(&log, "missing opening '+++' frontmatter fence", "content/a.md"));

  const char missing_close[] = "+++\ntitle = \"x\"\nbody\n";
  TEST_CHECK(frontmatter_split(str_from_cstr(missing_close), S("content/a.md"), &split, &log) ==
             -1);
  TEST_CHECK(is_one_error_taken(&log, "missing closing '+++' frontmatter fence", "content/a.md"));
}

// `frontmatter_split` rejects a document with no first line as a missing opening fence rather than
// reading past its end. Both shapes below hand `is_frontmatter_fence_line` an empty line, where a
// `\r` probe of the last byte would index before the line. `-fsanitize=address` in the debug build
// catches a regression. A BOM-only file is the reachable second shape, because the splitter
// consumes the BOM before it cuts the first line.
static void test_split_rejects_document_without_a_first_line(void) {
  struct FrontmatterSplit split;
  struct Log log = test_log();
  TEST_CHECK(frontmatter_split(S(""), S("content/a.md"), &split, &log) == -1);
  TEST_CHECK(is_one_error_taken(&log, "missing opening '+++' frontmatter fence", "content/a.md"));

  const char bom_only[] = "\xEF\xBB\xBF";
  struct Log log_bom = test_log();
  TEST_CHECK(frontmatter_split(str_from_cstr(bom_only), S("content/a.md"), &split, &log_bom) == -1);
  TEST_CHECK(
      is_one_error_taken(&log_bom, "missing opening '+++' frontmatter fence", "content/a.md"));
}

// Parsing fills required fields, defaults the description, derives the slug, and copies tags.
static void test_parse_fills_required_and_defaults(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "tags = [\"a\", \"b\"]\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(fm), "content/title.md", &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(entry.title, S("Title")));
  TEST_CHECK(str_eq(entry.description, S("")));
  TEST_CHECK(str_eq(entry.slug, S("title")));
  TEST_CHECK(entry.tag_count == 2);
  TEST_CHECK(str_eq(entry.tags[1], S("b")));
  TEST_CHECK(entry.template.len == 0);
}

// An explicit slug is normalized instead of trusted as a path.
static void test_sanitizes_explicit_slug(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "slug = \"../../Pwned Outside\"\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(fm), "content/title.md", &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(entry.slug, S("pwned-outside")));
}

// Optional template filename and draft metadata are parsed.
static void test_parse_fills_optional_template_and_draft(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "draft = true\n"
      "template = \"article_1.html\"\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(fm), "content/title.md", &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(entry.template, S("article_1.html")));
  TEST_CHECK(entry.is_draft);
}

// The offset is applied before comparison: `09:00-07:00` is 16:00Z, so the entry with the earlier
// wall-clock time sorts later and a timezone-blind parse would reverse this pair.
static void test_date_epoch_uses_timezone(void) {
  const char fm_later[] =
      "title = \"Later\"\n"
      "date = 2026-07-05T09:00:00-07:00\n"
      "description = \"Desc\"\n";
  const char fm_earlier[] =
      "title = \"Earlier\"\n"
      "date = 2026-07-05T10:00:00Z\n"
      "description = \"Desc\"\n";
  struct ContentEntry later = {0};
  struct ContentEntry earlier = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&later, str_from_cstr(fm_later), "content/later.md", &log) == 0);
  TEST_CHECK(parse_frontmatter(&earlier, str_from_cstr(fm_earlier), "content/earlier.md", &log) ==
             0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(later.date_epoch > earlier.date_epoch);
}

// A TOML syntax error is reported at its line in the *file*, not its line in the frontmatter slice
// the opening fence was stripped from. Laid out as a whole document, the bad value sits on file
// line 3:
//
//   1  +++
//   2  title = "Title"
//   3  date = bad
//   4  +++
//
// `frontmatter_parse` receives only lines 2-3, so without the synthesized leading line in
// `frontmatter_parse_toml` the parser would call the bad value line 2 and send the reader to the
// title.
static void test_rejects_invalid_toml_at_file_line(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = bad\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(fm), "content/bad.md", &log) == -1);

  // The line number is the claim and is pinned exactly, anchored at the start so neither the prefix
  // nor the number can drift. What follows is tomlc17's own wording, which is not ours to pin. It
  // is only required to be present, so a reworded upstream message does not fail this test.
  static const char expected_head[] = "failed to parse frontmatter: (line 3) ";
  TEST_CHECK(strncmp(buf_cstr(&log.buf), expected_head, sizeof(expected_head) - 1) == 0);
  TEST_CHECK(log.buf.len > (ptrdiff_t)sizeof(expected_head) - 1);
}

// A missing required frontmatter key is reported as absent, naming the key. The message must be
// distinguishable from the wrong-type message for the same key, so a user knows whether to add the
// key or fix its value.
static void test_rejects_missing_required_key(void) {
  const char missing_title[] =
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(missing_title), "content/title.md", &log) ==
             -1);
  TEST_CHECK(
      is_one_error_taken(&log, "missing required frontmatter key 'title'", "content/title.md"));

  // `date` is read by its own helper rather than the shared required-string path, so its absence is
  // checked separately.
  const char missing_date[] = "title = \"Title\"\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(missing_date), "content/title.md", &log) ==
             -1);
  TEST_CHECK(
      is_one_error_taken(&log, "missing required frontmatter key 'date'", "content/title.md"));
}

// A key the schema does not define is rejected, naming the key, so a misspelling cannot silently
// keep a default. A key that is a prefix of a known key, or extends one, is as unknown as any
// other, and so is a table the schema has no place for.
static void test_rejects_unknown_keys(void) {
  static const char* const cases[][2] = {
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\ndrfat = true\n",
       "unknown frontmatter key 'drfat'"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\ntag = [\"c\"]\n",
       "unknown frontmatter key 'tag'"},
      {"title = \"Title\"\ntitles = \"Title\"\ndate = 2026-07-01T12:00:00Z\n",
       "unknown frontmatter key 'titles'"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\n[extra]\nkey = 1\n",
       "unknown frontmatter key 'extra'"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct ContentEntry entry = {0};
    struct Log log = test_log();
    TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(cases[i][0]), "content/title.md", &log) ==
               -1);
    TEST_CHECK(is_one_error_taken(&log, cases[i][1], "content/title.md"));
  }
}

// No key in the schema holds a table, so a table under a known key fails that key's type check.
static void test_rejects_table_values(void) {
  static const char* const cases[][2] = {
      {"date = 2026-07-01T12:00:00Z\n[title]\nname = \"Title\"\n",
       "frontmatter key 'title' must be a string"},
      {"title = \"Title\"\n[date]\nyear = 2026\n",
       "frontmatter key 'date' must be a TOML date/datetime"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\n[tags]\nname = \"c\"\n",
       "frontmatter key 'tags' must be an array"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\n[draft]\nvalue = true\n",
       "frontmatter key 'draft' must be a boolean"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct ContentEntry entry = {0};
    struct Log log = test_log();
    TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(cases[i][0]), "content/title.md", &log) ==
               -1);
    TEST_CHECK(is_one_error_taken(&log, cases[i][1], "content/title.md"));
  }
}

// A template filename that would escape the template root is rejected as unsafe, which is a
// distinct message from the wrong-type rejection covered below.
static void test_rejects_unsafe_template(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "template = \"../content-entry.html\"\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(fm), "content/title.md", &log) == -1);
  TEST_CHECK(is_one_error_taken(&log,
                                "frontmatter key 'template' must be a safe relative template name: "
                                "'../content-entry.html'",
                                "content/title.md"));
}

// Wrong-typed metadata is rejected with the offending key named and the expected type stated. A
// quoted date is rejected as a type error even when it is a well-formed date string.
static void test_rejects_invalid_metadata(void) {
  const char bad_date[] =
      "title = \"Title\"\n"
      "date = \"2030-01-02\"\n"
      "description = \"Desc\"\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_date), "content/title.md", &log) == -1);
  TEST_CHECK(is_one_error_taken(&log, "frontmatter key 'date' must be a TOML date/datetime",
                                "content/title.md"));

  const char bad_description[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = 7\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_description), "content/title.md", &log) ==
             -1);
  TEST_CHECK(is_one_error_taken(&log, "frontmatter key 'description' must be a string",
                                "content/title.md"));

  // A non-array `tags` and an array holding a non-string are different failures.
  const char bad_tags_type[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "tags = \"one\"\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_tags_type), "content/title.md", &log) ==
             -1);
  TEST_CHECK(
      is_one_error_taken(&log, "frontmatter key 'tags' must be an array", "content/title.md"));

  const char bad_tags[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "tags = [\"ok\", 3]\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_tags), "content/title.md", &log) == -1);
  TEST_CHECK(is_one_error_taken(&log, "frontmatter key 'tags' must contain only strings",
                                "content/title.md"));

  const char bad_title[] =
      "title = 7\n"
      "date = 2026-07-01T12:00:00Z\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_title), "content/title.md", &log) == -1);
  TEST_CHECK(
      is_one_error_taken(&log, "frontmatter key 'title' must be a string", "content/title.md"));

  const char bad_slug[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "slug = 7\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_slug), "content/title.md", &log) == -1);
  TEST_CHECK(
      is_one_error_taken(&log, "frontmatter key 'slug' must be a string", "content/title.md"));

  const char bad_draft[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "draft = \"yes\"\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_draft), "content/title.md", &log) == -1);
  TEST_CHECK(
      is_one_error_taken(&log, "frontmatter key 'draft' must be a boolean", "content/title.md"));

  const char bad_template[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "template = 7\n";
  entry = (struct ContentEntry){0};
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(bad_template), "content/title.md", &log) ==
             -1);
  TEST_CHECK(
      is_one_error_taken(&log, "frontmatter key 'template' must be a string", "content/title.md"));
}

// Every bad key in one frontmatter is reported by the one parse, unknown keys first and then the
// fields in schema order, so a file with several mistakes is fixed in one pass.
static void test_parse_reports_every_bad_key(void) {
  const char fm[] =
      "title = 7\n"
      "date = 2026-07-01T12:00:00Z\n"
      "draft = \"yes\"\n"
      "colour = \"red\"\n"
      "tags = \"one\"\n";
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(fm), "content/title.md", &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown frontmatter key 'colour' (in 'content/title.md')\n"
                         "frontmatter key 'title' must be a string (in 'content/title.md')\n"
                         "frontmatter key 'tags' must be an array (in 'content/title.md')\n"
                         "frontmatter key 'draft' must be a boolean (in 'content/title.md')\n"));
  TEST_CHECK(log.errors == 4);
}

// A TOML escape decoding to `U+0000` is rejected per key, not copied. tomlc17 accepts the escape
// and hands back a string whose bytes carry an embedded `NUL`, which `os_read`'s check cannot see
// because the parser manufactured it. The byte has no meaning in any format `sosig` reads or
// writes, so the input policy refuses it here rather than passing it into a page.
static void test_rejects_nul_in_string_values(void) {
  static const char* const cases[][2] = {
      {"title = \"a\\u0000b\"\ndate = 2026-07-01T12:00:00Z\n",
       "frontmatter key 'title' must not contain a NUL byte"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\ndescription = \"a\\u0000b\"\n",
       "frontmatter key 'description' must not contain a NUL byte"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\nslug = \"a\\u0000b\"\n",
       "frontmatter key 'slug' must not contain a NUL byte"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\ntags = [\"a\\u0000b\"]\n",
       "frontmatter key 'tags' must not contain a NUL byte"},
      {"title = \"Title\"\ndate = 2026-07-01T12:00:00Z\ntemplate = \"a\\u0000b\"\n",
       "frontmatter key 'template' must not contain a NUL byte"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct ContentEntry entry = {0};
    struct Log log = test_log();
    TEST_CHECK(parse_frontmatter(&entry, str_from_cstr(cases[i][0]), "content/title.md", &log) ==
               -1);
    TEST_CHECK(is_one_error_taken(&log, cases[i][1], "content/title.md"));
  }
}

// A quoted key holding a `NUL` decoded from an escape is rejected without its name, since the name
// printed through `%.*s` would stop at the `NUL` and read as the known key `draft`.
static void test_rejects_nul_in_key(void) {
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(
                 &entry,
                 S("title = \"Title\"\ndate = 2026-07-01T12:00:00Z\n\"draft\\u0000\" = true\n"),
                 "content/title.md", &log) == -1);
  TEST_CHECK(
      is_one_error_taken(&log, "frontmatter key must not contain a NUL byte", "content/title.md"));
}

// A raw `NUL` byte inside the frontmatter reaches tomlc17 with the rest of the slice instead of
// ending it early, and tomlc17 rejects it as invalid TOML, at the file line it sits on.
static void test_parse_rejects_raw_nul_in_frontmatter(void) {
  struct ContentEntry entry = {0};
  struct Log log = test_log();
  TEST_CHECK(parse_frontmatter(&entry, S("title = \"Title\"\ndate = 2026-07-01T12:00:00Z\n\0\n"),
                               "content/title.md", &log) == -1);
  static const char expected_head[] = "failed to parse frontmatter: (line 4) ";
  TEST_CHECK(strncmp(buf_cstr(&log.buf), expected_head, sizeof(expected_head) - 1) == 0);
}

TEST_LIST = {
    {"split handles crlf and body", test_split_handles_crlf_and_body},
    {"split skips utf8 bom", test_split_skips_utf8_bom},
    {"split reports body without trailing newline",
     test_split_reports_body_without_trailing_newline},
    {"split accepts closing fence at end of file", test_split_accepts_closing_fence_at_end_of_file},
    {"split keeps nul bytes", test_split_keeps_nul_bytes},
    {"split rejects missing fences", test_split_rejects_missing_fences},
    {"split rejects document without a first line",
     test_split_rejects_document_without_a_first_line},
    {"parse fills required and defaults", test_parse_fills_required_and_defaults},
    {"sanitizes explicit slug", test_sanitizes_explicit_slug},
    {"parse fills optional template and draft", test_parse_fills_optional_template_and_draft},
    {"date epoch uses timezone", test_date_epoch_uses_timezone},
    {"rejects invalid toml at file line", test_rejects_invalid_toml_at_file_line},
    {"rejects missing required key", test_rejects_missing_required_key},
    {"rejects unknown keys", test_rejects_unknown_keys},
    {"rejects table values", test_rejects_table_values},
    {"rejects unsafe template", test_rejects_unsafe_template},
    {"rejects invalid metadata", test_rejects_invalid_metadata},
    {"parse reports every bad key", test_parse_reports_every_bad_key},
    {"rejects nul in string values", test_rejects_nul_in_string_values},
    {"rejects nul in key", test_rejects_nul_in_key},
    {"parse rejects raw nul in frontmatter", test_parse_rejects_raw_nul_in_frontmatter},
    {NULL, NULL},
};
