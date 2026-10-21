#ifndef SOSIG_FRONTMATTER_H
#define SOSIG_FRONTMATTER_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct ContentEntry;
struct Log;

/**
 * Borrowed slices that split a Markdown file around its frontmatter fences. Both point into the
 * caller's file buffer and live as long as it does.
 */
struct FrontmatterSplit {
  /** TOML bytes between the opening and closing fences. */
  struct Str frontmatter;

  /** Body bytes after the closing fence. */
  struct Str body;
};

/**
 * @brief Splits a complete Markdown file into TOML frontmatter and body slices.
 *
 * Expects a leading fence and a matching closing fence, tolerating a UTF-8 BOM and CRLF line
 * endings. The returned slices borrow into `markdown`, and the function writes them only on
 * success.
 *
 * @param markdown    Full file bytes.
 * @param source_path File the bytes came from, named in the error.
 * @param split_out   Receives the frontmatter and body slices. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` when a fence is missing.
 */
int frontmatter_split(struct Str markdown,
                      struct Str source_path,
                      struct FrontmatterSplit* split_out,
                      struct Log* log) __attribute__((nonnull(3, 4)));

/**
 * @brief Parses TOML frontmatter and applies it to a zeroed content entry.
 *
 * Populates required and optional metadata, copying every value it keeps into `perm`. It rejects a
 * key outside the schema and validates value types and the template name.
 *
 * @param entry       Zeroed entry that receives the parsed metadata. On failure the parse may leave
 *                    it partially populated, and the values it already copied stay in `perm`. Must
 *                    not be `NULL`.
 * @param frontmatter Frontmatter TOML bytes, shorter than `INT_MAX` bytes, which the content file
 *                    limit guarantees.
 * @param source_path Source file path, used for slug fallback and named in every error.
 * @param perm        Arena that owns the values copied onto the entry. Must not be `NULL`.
 * @param scratch     Arena for the parse's temporaries.
 * @param log         Log that receives every error. Every key is checked even after one fails, so
 *                    all of a file's mistakes are reported at once. Must not be `NULL`.
 * @return `0` on success, or `-1` on a parse or any validation error.
 */
int frontmatter_parse(struct ContentEntry* entry,
                      struct Str frontmatter,
                      struct Str source_path,
                      struct Arena* perm,
                      struct Arena scratch,
                      struct Log* log) __attribute__((nonnull(1, 4, 6)));

#endif
