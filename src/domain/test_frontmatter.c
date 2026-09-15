#include <acutest.h>
#include <stdio.h>
#include <string.h>

#include "core/error.h"
#include "domain/content_entry.h"
#include "domain/frontmatter.h"

// A CRLF document splits without an off-by-one: the frontmatter slice keeps its own trailing `\r\n`
// and the body begins after the closing fence's line ending. Both slices are compared by their
// paired lengths rather than as C strings, because `struct FrontmatterSplit` disclaims termination.
// A `strcmp` stops at the enclosing buffer's own terminator and cannot see a wrong length.
static void test_split_handles_crlf_and_body(void) {
  const char doc[] = "+++\r\ntitle = \"x\"\r\n+++\r\nbody\n";
  struct FrontmatterSplit split;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_split(doc, strlen(doc), &split, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(split.frontmatter_len == strlen("title = \"x\"\r\n"));
  TEST_CHECK(memcmp(split.frontmatter, "title = \"x\"\r\n", split.frontmatter_len) == 0);
  TEST_CHECK(split.body_len == strlen("body\n"));
  TEST_CHECK(memcmp(split.body, "body\n", split.body_len) == 0);
}

// `frontmatter_split` skips a leading UTF-8 BOM, so it still recognizes the fence on the same line.
// Several Windows editors write a BOM by default. Without the skip, such a file fails with a
// missing-opening-fence diagnostic that says nothing about the real cause. The BOM belongs to
// neither slice.
static void test_split_skips_utf8_bom(void) {
  const char doc[] = "\xEF\xBB\xBF+++\ntitle = \"x\"\n+++\nbody\n";
  struct FrontmatterSplit split;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_split(doc, strlen(doc), &split, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(split.frontmatter_len == strlen("title = \"x\"\n"));
  TEST_CHECK(memcmp(split.frontmatter, "title = \"x\"\n", split.frontmatter_len) == 0);
  TEST_CHECK(split.body_len == strlen("body\n"));
  TEST_CHECK(memcmp(split.body, "body\n", split.body_len) == 0);
}

// The body slice is exactly the bytes after the closing fence's line ending, with no assumption
// that the file ends in a newline. A body whose last byte is not a newline is where a length that
// ran one byte short changes which bytes are compared, not merely the asserted count.
static void test_split_reports_body_without_trailing_newline(void) {
  const char doc[] = "+++\ntitle = \"x\"\n+++\nbody";
  struct FrontmatterSplit split;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_split(doc, strlen(doc), &split, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(split.body_len == strlen("body"));
  TEST_CHECK(memcmp(split.body, "body", split.body_len) == 0);
}

// A closing fence that is the file's last line with no trailing newline is still a fence. The body
// is then empty rather than absent. This shape separates the newline strip in
// `is_frontmatter_fence_line` from an unconditional decrement. Every other fence in this file ends
// in a line ending, so a guard spelled `line_len > 0` alone, without the `== '\n'` test, matches
// them all and rejects only this one. Editors that omit the final newline produce it routinely. The
// bare-`\r` variant is the same boundary for the second strip.
static void test_split_accepts_closing_fence_at_end_of_file(void) {
  struct FrontmatterSplit split;
  char err[ERROR_MESSAGE_SIZE] = "";
  const char doc[] = "+++\ntitle = \"x\"\n+++";
  TEST_CHECK(frontmatter_split(doc, strlen(doc), &split, err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(split.frontmatter_len == strlen("title = \"x\"\n"));
  TEST_CHECK(memcmp(split.frontmatter, "title = \"x\"\n", split.frontmatter_len) == 0);
  TEST_CHECK(split.body_len == 0);

  const char doc_cr[] = "+++\ntitle = \"x\"\n+++\r";
  char err_cr[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_split(doc_cr, strlen(doc_cr), &split, err_cr, sizeof(err_cr)) == 0);
  TEST_CHECK(err_cr[0] == '\0');
  TEST_CHECK(split.body_len == 0);
}

// Missing opening and closing fences produce distinct diagnostics.
static void test_split_rejects_missing_fences(void) {
  struct FrontmatterSplit split;
  char err[ERROR_MESSAGE_SIZE];
  const char missing_open[] = "title = \"x\"\n+++\nbody\n";
  TEST_CHECK(frontmatter_split(missing_open, strlen(missing_open), &split, err, sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "missing opening '+++' frontmatter fence") == 0);

  const char missing_close[] = "+++\ntitle = \"x\"\nbody\n";
  TEST_CHECK(frontmatter_split(missing_close, strlen(missing_close), &split, err, sizeof(err)) !=
             0);
  TEST_CHECK(strcmp(err, "missing closing '+++' frontmatter fence") == 0);
}

// `frontmatter_split` rejects a document with no first line as a missing opening fence rather than
// reading past its end. Both shapes below make `line_length` return `0`, the only way
// `is_frontmatter_fence_line` is reached with nothing to inspect. Its `line_len > 0` guards stop
// the `line[line_len - 1]` probes from indexing at `SIZE_MAX`. `-fsanitize=address` in the debug
// build catches a regression. A BOM-only file is the reachable second shape, because the splitter
// consumes the BOM before it measures the line.
static void test_split_rejects_document_without_a_first_line(void) {
  struct FrontmatterSplit split;
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_split("", 0, &split, err, sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "missing opening '+++' frontmatter fence") == 0);

  const char bom_only[] = "\xEF\xBB\xBF";
  char err_bom[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_split(bom_only, strlen(bom_only), &split, err_bom, sizeof(err_bom)) != 0);
  TEST_CHECK(strcmp(err_bom, "missing opening '+++' frontmatter fence") == 0);
}

// Parsing fills required fields, defaults the description, derives the slug, and copies tags.
static void test_parse_fills_required_and_defaults(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "tags = [\"a\", \"b\"]\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/title.md", err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(entry.title, "Title") == 0);
  TEST_CHECK(strcmp(entry.description, "") == 0);
  TEST_CHECK(strcmp(entry.slug, "title") == 0);
  TEST_CHECK(entry.tag_count == 2);
  TEST_CHECK(strcmp(entry.tags[1], "b") == 0);
  TEST_CHECK(entry.template == NULL);
  content_entry_free(&entry);
}

// An explicit slug is normalized instead of trusted as a path.
static void test_sanitizes_explicit_slug(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "slug = \"../../Pwned Outside\"\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/title.md", err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(entry.slug, "pwned-outside") == 0);
  content_entry_free(&entry);
}

// A slug at exactly the length limit still builds.
static void test_accepts_max_length_slug(void) {
  char slug[SLUG_LEN_MAX + 1];
  memset(slug, 'a', SLUG_LEN_MAX);
  slug[SLUG_LEN_MAX] = '\0';
  char fm[SLUG_LEN_MAX + 128];
  const int n = snprintf(fm, sizeof(fm),
                         "title = \"Title\"\n"
                         "date = 2026-07-01T12:00:00Z\n"
                         "slug = \"%s\"\n",
                         slug);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(fm));
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/title.md", err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strlen(entry.slug) == SLUG_LEN_MAX);
  content_entry_free(&entry);
}

// Optional template filename and draft metadata are parsed.
static void test_parse_fills_optional_template_and_draft(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "draft = true\n"
      "template = \"article_1.html\"\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/title.md", err, sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(strcmp(entry.template, "article_1.html") == 0);
  TEST_CHECK(entry.is_draft);
  content_entry_free(&entry);
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
  struct ContentEntry later;
  struct ContentEntry earlier;
  content_entry_init(&later);
  content_entry_init(&earlier);
  char err[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(frontmatter_parse(&later, fm_later, strlen(fm_later), "content/later.md", err,
                               sizeof(err)) == 0);
  TEST_CHECK(frontmatter_parse(&earlier, fm_earlier, strlen(fm_earlier), "content/earlier.md", err,
                               sizeof(err)) == 0);
  TEST_CHECK(err[0] == '\0');
  TEST_CHECK(later.date_epoch > earlier.date_epoch);
  content_entry_free(&later);
  content_entry_free(&earlier);
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
//
// `frontmatter_parse_toml`'s other failure, the oversize rejection, is deliberately left uncovered:
// reaching it needs a frontmatter slice larger than `INT_MAX - 1`, so a test would have to allocate
// over 2 GiB. `core/error.h` records the same exemption for its `out of memory` family.
static void test_rejects_invalid_toml_at_file_line(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = bad\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/bad.md", err, sizeof(err)) != 0);

  // The line number is the claim and is pinned exactly, anchored at the start so neither the prefix
  // nor the number can drift. What follows is tomlc17's own wording, which is not ours to pin. It
  // is only required to be present, so a reworded upstream message does not fail this test.
  static const char expected_head[] = "failed to parse frontmatter: (line 3) ";
  TEST_CHECK(strncmp(err, expected_head, sizeof(expected_head) - 1) == 0);
  TEST_CHECK(strlen(err) > sizeof(expected_head) - 1);

  content_entry_free(&entry);
}

// A missing required frontmatter key is reported as absent, naming the key. The message must be
// distinguishable from the wrong-type message for the same key, so a user knows whether to add the
// key or fix its value.
static void test_rejects_missing_required_key(void) {
  const char missing_title[] =
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(frontmatter_parse(&entry, missing_title, strlen(missing_title), "content/title.md",
                               err, sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "missing required frontmatter key 'title'") == 0);
  content_entry_free(&entry);

  // `date` is read by its own helper rather than the shared required-string path, so its absence is
  // checked separately.
  const char missing_date[] = "title = \"Title\"\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, missing_date, strlen(missing_date), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "missing required frontmatter key 'date'") == 0);
  content_entry_free(&entry);
}

// A template filename that would escape the template root is rejected as unsafe, which is a
// distinct message from the wrong-type rejection covered below.
static void test_rejects_unsafe_template(void) {
  const char fm[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "template = \"../content-entry.html\"\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/title.md", err, sizeof(err)) != 0);
  TEST_CHECK(strcmp(err,
                    "frontmatter key 'template' must be a safe relative template name: "
                    "'../content-entry.html'") == 0);
  content_entry_free(&entry);
}

// Wrong-typed metadata is rejected with the offending key named and the expected type stated. A
// quoted date is rejected as a type error even when it is a well-formed date string.
static void test_rejects_invalid_metadata(void) {
  const char bad_date[] =
      "title = \"Title\"\n"
      "date = \"2030-01-02\"\n"
      "description = \"Desc\"\n";
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(frontmatter_parse(&entry, bad_date, strlen(bad_date), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'date' must be a TOML date/datetime") == 0);
  content_entry_free(&entry);

  const char bad_description[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = 7\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_description, strlen(bad_description), "content/title.md",
                               err, sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'description' must be a string") == 0);
  content_entry_free(&entry);

  // A non-array `tags` and an array holding a non-string are different failures.
  const char bad_tags_type[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "tags = \"one\"\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_tags_type, strlen(bad_tags_type), "content/title.md",
                               err, sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'tags' must be an array") == 0);
  content_entry_free(&entry);

  const char bad_tags[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "description = \"Desc\"\n"
      "tags = [\"ok\", 3]\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_tags, strlen(bad_tags), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'tags' must contain only strings") == 0);
  content_entry_free(&entry);

  const char bad_title[] =
      "title = 7\n"
      "date = 2026-07-01T12:00:00Z\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_title, strlen(bad_title), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'title' must be a string") == 0);
  content_entry_free(&entry);

  const char bad_slug[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "slug = 7\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_slug, strlen(bad_slug), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'slug' must be a string") == 0);
  content_entry_free(&entry);

  const char bad_draft[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "draft = \"yes\"\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_draft, strlen(bad_draft), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'draft' must be a boolean") == 0);
  content_entry_free(&entry);

  const char bad_template[] =
      "title = \"Title\"\n"
      "date = 2026-07-01T12:00:00Z\n"
      "template = 7\n";
  content_entry_init(&entry);
  TEST_CHECK(frontmatter_parse(&entry, bad_template, strlen(bad_template), "content/title.md", err,
                               sizeof(err)) != 0);
  TEST_CHECK(strcmp(err, "frontmatter key 'template' must be a string") == 0);
  content_entry_free(&entry);
}

// An overlong slug fails early. The diagnostic names the source, offending size, and limit. It
// omits the slug, which would fill the buffer and truncate the reason.
static void test_rejects_overlong_slug(void) {
  char slug[SLUG_LEN_MAX + 2];
  memset(slug, 'a', SLUG_LEN_MAX + 1);
  slug[SLUG_LEN_MAX + 1] = '\0';
  char fm[SLUG_LEN_MAX + 128];
  int n = snprintf(fm, sizeof(fm),
                   "title = \"Title\"\n"
                   "date = 2026-07-01T12:00:00Z\n"
                   "slug = \"%s\"\n",
                   slug);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(fm));
  struct ContentEntry entry;
  content_entry_init(&entry);
  char err[ERROR_MESSAGE_SIZE];
  TEST_CHECK(frontmatter_parse(&entry, fm, strlen(fm), "content/title.md", err, sizeof(err)) != 0);

  char expected[ERROR_MESSAGE_SIZE];
  // The message names every part the limits rule asks for: the limit, the measured length, and the
  // offending slug. The slug trails, so an arbitrarily long one truncates itself rather than the
  // reason. The source path is deliberately absent. The wrapping caller in `entry_renderer` appends
  // it, and naming it here too would both print it twice and push the limit clause out of the
  // buffer for a long path.
  n = snprintf(expected, sizeof(expected),
               "slug exceeds max slug length (%zu bytes) at %zu bytes: '%s'", (size_t)SLUG_LEN_MAX,
               strlen(slug), slug);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(err, expected) == 0);
  content_entry_free(&entry);
}

// A TOML escape decoding to `U+0000` is rejected per key, not copied. tomlc17 accepts the escape
// and hands back a string whose bytes carry an embedded `NUL`, which `fs_read_file`'s check cannot
// see because the parser manufactured it. Left through, `strlen` would truncate the value at the
// `NUL` and the entry would render with silently shortened metadata.
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

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct ContentEntry entry;
    content_entry_init(&entry);
    char err[ERROR_MESSAGE_SIZE] = "";
    TEST_CHECK(frontmatter_parse(&entry, cases[i][0], strlen(cases[i][0]), "content/title.md", err,
                                 sizeof(err)) != 0);
    TEST_CHECK(strcmp(err, cases[i][1]) == 0);
    content_entry_free(&entry);
  }
}

TEST_LIST = {
    {"split handles crlf and body", test_split_handles_crlf_and_body},
    {"split skips utf8 bom", test_split_skips_utf8_bom},
    {"split reports body without trailing newline",
     test_split_reports_body_without_trailing_newline},
    {"split accepts closing fence at end of file", test_split_accepts_closing_fence_at_end_of_file},
    {"split rejects missing fences", test_split_rejects_missing_fences},
    {"split rejects document without a first line",
     test_split_rejects_document_without_a_first_line},
    {"parse fills required and defaults", test_parse_fills_required_and_defaults},
    {"sanitizes explicit slug", test_sanitizes_explicit_slug},
    {"accepts max length slug", test_accepts_max_length_slug},
    {"parse fills optional template and draft", test_parse_fills_optional_template_and_draft},
    {"date epoch uses timezone", test_date_epoch_uses_timezone},
    {"rejects invalid toml at file line", test_rejects_invalid_toml_at_file_line},
    {"rejects missing required key", test_rejects_missing_required_key},
    {"rejects unsafe template", test_rejects_unsafe_template},
    {"rejects invalid metadata", test_rejects_invalid_metadata},
    {"rejects overlong slug", test_rejects_overlong_slug},
    {"rejects nul in string values", test_rejects_nul_in_string_values},
    {NULL, NULL}};
