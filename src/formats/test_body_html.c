#include <acutest.h>
#include <stddef.h>

#include "formats/body_html.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

// A URL keeps its letters, digits and the punctuation a URL may hold as it is, so an ordinary link
// target is copied through unchanged.
static void test_url_escape_keeps_url_bytes(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_url_escape_append(&buf, S("https://example.com/a_b-c.d~e/f?g=h;i#j(k)*l!m,n+o$p@q%20"));
  TEST_CHECK(str_eq(buf_str(&buf), S("https://example.com/a_b-c.d~e/f?g=h;i#j(k)*l!m,n+o$p@q%20")));
}

// `&` becomes a character reference, so the attribute stays valid HTML, and every other byte a URL
// may not hold becomes `%XX` in uppercase hexadecimal, a byte above 0x7F included.
static void test_url_escape_encodes_other_bytes(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  body_html_url_escape_append(&buf, S("a b&c\"d<e'f\xC3\xA9"));
  TEST_CHECK(str_eq(buf_str(&buf), S("a%20b&amp;c%22d%3Ce%27f%C3%A9")));
}

TEST_LIST = {
    {"url escape keeps url bytes", test_url_escape_keeps_url_bytes},
    {"url escape encodes other bytes", test_url_escape_encodes_other_bytes},
    {NULL, NULL},
};
