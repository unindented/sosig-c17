#ifndef SOSIG_CONTENT_ENTRY_H
#define SOSIG_CONTENT_ENTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/os.h"
#include "shared/str.h"

/**
 * Content entry metadata and generated paths for one Markdown source file.
 *
 * The entry and every string it points at live in the long-lived arena of the worker that parsed
 * it, or are string literals, so an entry stays valid for the rest of the build. Nothing borrows
 * from the source buffer, which lives in the parse job's scratch.
 *
 * The parse phase fills every field. The page, aggregate and feed phases only read. That is a
 * thread-safety invariant. During the page phase every worker holds pointers to every entry, so a
 * write would race. Do not add a field that a later phase fills in place.
 */
struct ContentEntry {
  /** Source Markdown path, used for diagnostics and output messages. */
  struct Str source_path;

  /**
   * What `os_stat` reported for the source just before the parse read it. The build cache records
   * it, so the next build can tell whether the source changed since.
   */
  struct OsStat source_stat;

  /** Required frontmatter title. */
  struct Str title;

  /**
   * Normalized RFC 3339 timestamp. The Atom feed emits it verbatim into its `updated` element, so
   * the format is part of the feed's contract, not only a display choice.
   */
  struct Str date;

  /**
   * Same instant as `date` in Unix epoch seconds, used to sort content entries. Both come from the
   * one frontmatter `date` key and are written together, so a change to either must keep them
   * agreeing or the feed order stops matching the shown dates.
   */
  int64_t date_epoch;

  /** Optional summary text for templates. Empty when the frontmatter omits it. */
  struct Str description;

  /** URL-safe slug derived from frontmatter or source filename. */
  struct Str slug;

  /** Array of arena-owned tag strings. */
  struct Str* tags;

  /**
   * Number of `tags` entries. `0` with `tags` as `NULL` when the frontmatter had no `tags` key, and
   * `0` with `tags` non-`NULL` for an empty array. Readers must not distinguish the two.
   */
  ptrdiff_t tag_count;

  /** Optional safe relative template name override. Empty uses site config `content_template`. */
  struct Str template;

  /**
   * Rendered HTML body. A draft, or an entry that failed any step, never fills its entry slot, so
   * no half-built entry is ever published.
   */
  struct Str body_html;

  /** Public URL path, always beginning with `/`, as expanded from the site `permalink`. */
  struct Str url_path;

  /**
   * Filesystem output path: the site `output_dir` joined with `url_path` minus its leading `/`.
   * Checked against both output-path limits before it is stored here.
   */
  struct Str output_path;

  /**
   * Whether the content entry should be parsed but not rendered. An independent status flag, so it
   * sits after the data it describes rather than between `tag_count` and `template`.
   */
  bool is_draft;
};

/**
 * @brief Sorts content entries by newest date, then output path.
 *
 * Orders the array in place so templates see the newest entries first. Because output paths are
 * distinct in any build that gets past the manifest phase, the ordering is total and does not
 * depend on how `qsort` happens to arrange equal-comparing elements.
 *
 * @param content_entries     Array of content entry pointers to sort in place. Each entry must have
 *                            `date_epoch` and `output_path` set. May be `NULL` only when
 *                            `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 */
void content_entry_sort(struct ContentEntry** content_entries, ptrdiff_t content_entry_count);

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
struct Str content_entry_date_latest(const struct ContentEntry* const* content_entries,
                                     ptrdiff_t content_entry_count);

#endif
