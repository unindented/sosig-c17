#ifndef SOSIG_TREE_SITTER_WCTYPE_H
#define SOSIG_TREE_SITTER_WCTYPE_H

// `vendor/CMakeLists.txt` includes this header ahead of each tree-sitter grammar's own sources. The
// grammars' scanners classify characters with `iswalpha`, `iswalnum`, `iswdigit` and `iswspace`,
// whose answer for a character outside ASCII depends on the C library: in the C locale, which sosig
// never changes, macOS and glibc take none of them as a letter or a space, and musl takes many, so
// a block with `café` in it would be highlighted one way on macOS and another on the Linux release
// builds. The bash scanner also passes a code point to `isdigit`, which glibc answers from a table
// of 384 entries, so a character past it reads outside the table and can crash the build. The HTML
// scanner folds case with `towupper`, which musl applies to every cased letter whatever the locale
// and macOS, in the C locale, to ASCII alone, so `</ſcript>` would end a script on the Linux
// release builds and not on macOS. These definitions answer for ASCII alone, as macOS does in the C
// locale, on every host.

#include <ctype.h>
#include <stdbool.h>
#include <wctype.h>

/**
 * @brief Reports whether a character is an ASCII letter.
 *
 * @param c Character to check.
 * @return `true` for `A` to `Z` and `a` to `z`, `false` otherwise.
 */
static inline bool tree_sitter_wctype_is_alpha(wint_t c) {
  return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

/**
 * @brief Reports whether a character is an ASCII digit.
 *
 * @param c Character to check.
 * @return `true` for `0` to `9`, `false` otherwise.
 */
static inline bool tree_sitter_wctype_is_digit(wint_t c) {
  return c >= L'0' && c <= L'9';
}

/**
 * @brief Reports whether a character is an ASCII letter or digit.
 *
 * @param c Character to check.
 * @return `true` for a letter or a digit, `false` otherwise.
 */
static inline bool tree_sitter_wctype_is_alnum(wint_t c) {
  return tree_sitter_wctype_is_alpha(c) || tree_sitter_wctype_is_digit(c);
}

/**
 * @brief Reports whether a character is ASCII white space, as the C locale's `isspace` has it.
 *
 * @param c Character to check.
 * @return `true` for a space, a tab, a line feed, a vertical tab, a form feed or a carriage return,
 *         `false` otherwise.
 */
static inline bool tree_sitter_wctype_is_space(wint_t c) {
  return c == L' ' || (c >= L'\t' && c <= L'\r');
}

/**
 * @brief Returns a character in uppercase, if it is an ASCII letter.
 *
 * @param c Character to fold.
 * @return `A` to `Z` for `a` to `z`, `c` otherwise.
 */
static inline wint_t tree_sitter_wctype_to_upper(wint_t c) {
  return c >= L'a' && c <= L'z' ? c - L'a' + L'A' : c;
}

#undef isdigit
#define isdigit tree_sitter_wctype_is_digit
#define iswalpha tree_sitter_wctype_is_alpha
#define iswdigit tree_sitter_wctype_is_digit
#define iswalnum tree_sitter_wctype_is_alnum
#define iswspace tree_sitter_wctype_is_space
#define towupper tree_sitter_wctype_to_upper

#endif
