#ifndef SOSIG_CONTENT_ENTRY_H
#define SOSIG_CONTENT_ENTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/path.h"
#include "shared/arena.h"

/**
 * Upper bound on a slug, excluding the terminator. This bound is sized so that `<slug>.html`, the
 * filename the default permalink produces, fits inside `FILENAME_LEN_MAX` as a single path
 * component. That is the worst case rather than the only one. Under `permalink = "/{slug}/"` the
 * slug becomes a directory name instead. The `.html` allowance is then unused slack.
 *
 * This is not where the code enforces filename length, though it reads that way.
 * `path_check_output_limits` checks every generated output path against `FILENAME_LEN_MAX` and
 * `OUTPUT_PATH_RELATIVE_LEN_MAX`, for both producers. This constant only keeps the frontmatter slug
 * from being the component that overflows.
 */
enum { SLUG_LEN_MAX = FILENAME_LEN_MAX - (sizeof(".html") - 1) };

/**
 * Content entry metadata and generated paths for one Markdown source file.
 *
 * Every pointer field is either arena-owned or a string literal, so an entry stays valid until
 * `content_entry_free`. Nothing borrows from the source buffer, which the parse job frees before it
 * returns.
 *
 * The parse pass fills every field. The page, aggregate and feed passes only read. That is a
 * thread-safety invariant. During the page pass every worker holds pointers to every entry, so a
 * write would race, and allocating from `arena` would put one job's bytes in another job's arena.
 * Do not add a field that a later pass fills in place.
 */
struct ContentEntry {
  /** Owns copied metadata and generated strings for this content entry. */
  struct Arena arena;

  /** Source Markdown path, used for diagnostics and output messages. */
  const char* source_path;

  /** Required frontmatter title. */
  const char* title;

  /**
   * Normalized RFC 3339 timestamp. The Atom feed emits it verbatim into its `updated` element, so
   * the format is part of the feed's contract, not only a display choice.
   */
  const char* date;

  /**
   * Same instant as `date` in Unix epoch seconds, used to sort content entries. Both come from the
   * one frontmatter `date` key and are written together, so a change to either must keep them
   * agreeing or the feed order stops matching the shown dates.
   */
  int64_t date_epoch;

  /** Optional summary text for templates. Defaults to an empty string. */
  const char* description;

  /** URL-safe slug derived from frontmatter or source filename. */
  const char* slug;

  /** Array of arena-owned tag strings. */
  const char** tags;

  /**
   * Number of `tags` entries. `0` with `tags` as `NULL` when the frontmatter had no `tags` key, and
   * `0` with `tags` non-`NULL` for an empty array. Readers must not distinguish the two.
   */
  size_t tag_count;

  /** Optional safe relative template name override. `NULL` uses site config `content_template`. */
  const char* template;

  /**
   * Rendered HTML body, arena-copied from the Markdown conversion. Never `NULL` on an entry the
   * page pass sees. A draft, or an entry that failed any step, is freed before its result slot is
   * filled, so no half-built entry is ever published.
   */
  const char* body_html;

  /** Public URL path, always beginning with `/`, as expanded from the site `permalink`. */
  const char* url_path;

  /**
   * Filesystem output path: the site `output_dir` joined with `url_path` minus its leading `/`.
   * Checked against both output-path limits before it is stored here.
   */
  const char* output_path;

  /**
   * Whether the content entry should be parsed but not rendered. An independent status flag, so it
   * sits after the data it describes rather than between `tag_count` and `template`.
   */
  bool is_draft;
};

/**
 * @brief Initializes a content entry with defaults for optional frontmatter keys.
 *
 * Prepares the entry's arena and resets every field to its default so frontmatter parsing can fill
 * required fields and leave optional ones at their defaults.
 *
 * @param entry Entry handle to prepare. Must not be `NULL`, and must not already own arena
 *              allocations. This overwrites the arena handle without releasing it, so a populated
 *              entry passed here leaks every chunk it held. Reset a populated entry with
 *              `content_entry_free`, which leaves it initialized.
 */
void content_entry_init(struct ContentEntry* entry) __attribute__((nonnull(1)));

/**
 * @brief Releases arena-owned content entry data and resets it for reuse.
 *
 * Invalidates every arena-owned pointer on the entry. The entry stays initialized, so it may be
 * reused without calling `content_entry_init`.
 *
 * @param entry Entry to release. Must not be `NULL`.
 */
void content_entry_free(struct ContentEntry* entry) __attribute__((nonnull(1)));

/**
 * @brief Sorts content entries by newest date, then output path.
 *
 * Orders the array in place so templates see the newest entries first. Because output paths are
 * distinct in any build that gets past the manifest pass, the ordering is total and does not depend
 * on how `qsort` happens to arrange equal-comparing elements.
 *
 * @param content_entries     Array of content entry pointers to sort in place. Each entry must have
 *                            `date_epoch` and `output_path` set. May be `NULL` only when
 *                            `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 */
void content_entry_sort(struct ContentEntry** content_entries, size_t content_entry_count);

/**
 * @brief Returns the newest date among the entries, exposed to templates as `site.updated`.
 *
 * Resolved once from the sorted entry set and passed to every render, so a content template and an
 * aggregate template cannot disagree about it. An empty set falls back to the Unix epoch, so
 * `site.updated` always holds a valid RFC 3339 value even for a site with no posts yet.
 *
 * @param content_entries     Content entries sorted newest-first. May be `NULL` only when
 *                            `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @return The newest content entry's date, or the Unix epoch when there are no entries.
 */
const char* content_entry_latest_date(const struct ContentEntry* const* content_entries,
                                      size_t content_entry_count);

#endif
