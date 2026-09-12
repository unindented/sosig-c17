#ifndef SOSIG_FRONTMATTER_H
#define SOSIG_FRONTMATTER_H

#include <stddef.h>

struct ContentEntry;

/**
 * Borrowed slices that split a Markdown file around its frontmatter fences. Neither slice is
 * terminated. Both point into the caller's file buffer, and the caller must read them with their
 * paired length for as long as that buffer lives.
 */
struct FrontmatterSplit {
  /** TOML bytes between the opening and closing fences. */
  const char* frontmatter;

  /** Length of `frontmatter` in bytes. */
  size_t frontmatter_len;

  /** Body bytes after the closing fence. */
  const char* body;

  /** Length of `body` in bytes. */
  size_t body_len;
};

/**
 * @brief Splits a complete Markdown file into TOML frontmatter and body slices.
 *
 * Expects a leading fence and a matching closing fence, tolerating a UTF-8 BOM and CRLF line
 * endings. The returned slices borrow into `markdown`, and the function writes them only on
 * success.
 *
 * @param markdown     Full file bytes. Must hold at least `markdown_len` bytes. Must not be `NULL`.
 * @param markdown_len Number of file bytes.
 * @param split_out    Receives the frontmatter and body slices. Must not be `NULL`.
 * @param err          Buffer for a diagnostic message on failure.
 * @param err_len      Size of `err` in bytes.
 * @return `0` on success, or `-1` when a fence is missing (with a diagnostic in `err`).
 */
int frontmatter_split(const char* markdown,
                      size_t markdown_len,
                      struct FrontmatterSplit* split_out,
                      char* err,
                      size_t err_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Parses TOML frontmatter and applies it to an initialized content entry.
 *
 * Populates required and optional metadata in the entry's arena. It validates value types, the slug
 * length, and the template name.
 *
 * @param entry           Initialized entry that receives the parsed metadata. On failure the parse
 *                        may leave it partially populated, and the caller still owns it. Must not
 * be `NULL`.
 * @param frontmatter     Frontmatter TOML bytes. Must hold at least `frontmatter_len` bytes. Must
 *                        not be `NULL`.
 * @param frontmatter_len Number of frontmatter bytes.
 * @param source_path     Source file path, used for slug fallback and diagnostics. Must not be
 *                        `NULL`.
 * @param err             Buffer for a diagnostic message on failure.
 * @param err_len         Size of `err` in bytes.
 * @return `0` on success, or `-1` on a parse or validation error (with a diagnostic in `err`).
 */
int frontmatter_parse(struct ContentEntry* entry,
                      const char* frontmatter,
                      size_t frontmatter_len,
                      const char* source_path,
                      char* err,
                      size_t err_len) __attribute__((nonnull(1, 2, 4)));

#endif
