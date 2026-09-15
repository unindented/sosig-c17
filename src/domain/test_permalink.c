#include <acutest.h>
#include <string.h>

#include "core/arena.h"
#include "domain/permalink.h"

// Expansion is exercised end-to-end by the golden test suite, whose `site_file_permalink` fixture
// site covers the default pattern and whose `site_index_permalink` fixture site covers one ending
// in `/`. The cases in this file pin the pattern grammar itself, including the shapes
// `site_config_load` relies on when it validates a configured pattern by expanding it over a sample
// grid.

// A pattern's `{section}` and `{slug}` tokens are substituted, a multi-segment section is
// substituted whole, and a pattern with no token still roots at `/`.
static void test_expand_substitutes_tokens(void) {
  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(strcmp(permalink_expand("/{section}/{slug}.html", "guides", "intro", &arena),
                    "/guides/intro.html") == 0);
  TEST_CHECK(strcmp(permalink_expand("/{slug}.html", "", "intro", &arena), "/intro.html") == 0);
  TEST_CHECK(strcmp(permalink_expand("/posts/{slug}.html", "", "intro", &arena),
                    "/posts/intro.html") == 0);

  // A multi-segment section is substituted whole.
  TEST_CHECK(strcmp(permalink_expand("/{section}/{slug}.html", "a/b", "intro", &arena),
                    "/a/b/intro.html") == 0);

  // A pattern with no token at all still roots at `/`.
  TEST_CHECK(strcmp(permalink_expand("about.html", "", "intro", &arena), "/about.html") == 0);

  arena_free(&arena);
}

// An empty section leaves no gap. A redundant separator in the pattern collapses, so the two cannot
// combine into an empty path segment that `path_is_safe_relative` would reject.
static void test_expand_collapses_redundant_separators(void) {
  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(
      strcmp(permalink_expand("/{section}/{slug}.html", "", "intro", &arena), "/intro.html") == 0);
  TEST_CHECK(strcmp(permalink_expand("/posts//{slug}.html", "", "intro", &arena),
                    "/posts/intro.html") == 0);
  TEST_CHECK(strcmp(permalink_expand("{slug}", "", "intro", &arena), "/intro") == 0);

  arena_free(&arena);
}

// A pattern whose *expansion* ends in `/` publishes the directory's index document, which is not
// the same rule as a pattern whose own last byte is `/`. All three cases below end in a separator
// in the pattern too, so on their own they cannot tell the implemented rule from the literal
// reading of it. The fourth pair is what separates them: `/{slug}/{section}` ends in a token, and
// only the empty section leaves the expansion ending in a separator. Taking the pattern's last byte
// instead would reject that permalink at config load, so every top-level entry would stop
// publishing.
static void test_expand_appends_directory_index(void) {
  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(strcmp(permalink_expand("/{slug}/", "", "intro", &arena), "/intro/index.html") == 0);
  TEST_CHECK(strcmp(permalink_expand("/{section}/{slug}/", "guides", "intro", &arena),
                    "/guides/intro/index.html") == 0);
  TEST_CHECK(strcmp(permalink_expand("/{section}/", "", "intro", &arena), "/index.html") == 0);

  // Pattern does not end in `/`. The empty section is what does.
  TEST_CHECK(
      strcmp(permalink_expand("/{slug}/{section}", "", "intro", &arena), "/intro/index.html") == 0);
  // And with the section populated the expansion does not end in a separator, so no index document
  // is appended.
  TEST_CHECK(strcmp(permalink_expand("/{slug}/{section}", "guides", "intro", &arena),
                    "/intro/guides") == 0);

  arena_free(&arena);
}

// An unrecognized token is left literal rather than dropped, so the caller's path-safety check is
// what rejects it. Dropping it would publish a different path than the pattern asked for.
static void test_expand_leaves_unknown_token_literal(void) {
  struct Arena arena;
  arena_init(&arena);

  TEST_CHECK(
      strcmp(permalink_expand("/{unknown}/{slug}", "", "intro", &arena), "/{unknown}/intro") == 0);

  arena_free(&arena);
}

TEST_LIST = {{"expand substitutes tokens", test_expand_substitutes_tokens},
             {"expand collapses redundant separators", test_expand_collapses_redundant_separators},
             {"expand appends directory index", test_expand_appends_directory_index},
             {"expand leaves unknown token literal", test_expand_leaves_unknown_token_literal},
             {NULL, NULL}};
