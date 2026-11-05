#ifndef SOSIG_CONTENT_ENTRY_H
#define SOSIG_CONTENT_ENTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/os.h"
#include "shared/str.h"

/**
 * Content entry metadata and generated paths for one content source file.
 *
 * The entry, its source and every string it points at live in the long-lived arena of the worker
 * that parsed it, or are string literals, so an entry stays valid for the rest of the build. The
 * HTML of its body lives in the long-lived arena of the worker that converted it.
 *
 * The parse phase fills every field except `body_html`, `has_entry_link` and `has_asset_link`,
 * which the convert phase fills. A convert job writes only those fields of its own entry, and reads
 * the URL of other entries, so no two jobs write one field, and no job reads a field that another
 * job writes. The page, aggregate and feed phases only read. That is a thread-safety invariant.
 * During the page phase every worker holds pointers to every entry, so a write would race. Do not
 * add a field that a phase after the convert phase fills in place.
 */
struct ContentEntry {
  /** Content source path, used for diagnostics and output messages. */
  struct Str source_path;

  /**
   * What `os_stat` reported for the source just before the parse read it. The build cache records
   * it, so the next build can tell whether the source changed since.
   */
  struct OsStat source_stat;

  /** Required title, from the frontmatter or the Org keywords. */
  struct Str title;

  /**
   * Normalized RFC 3339 timestamp. A feed template can write it verbatim into an Atom `updated`
   * element when `modified` is empty, so the format is part of the feed's contract, not only a
   * display choice.
   */
  struct Str date;

  /**
   * Same instant as `date` in Unix epoch seconds, used to sort content entries. Both come from the
   * one `date` key or keyword and are written together, so a change to either must keep them
   * agreeing or the feed order stops matching the shown dates.
   */
  int64_t date_epoch;

  /**
   * Normalized RFC 3339 timestamp of the last change, from the `modified` key or the
   * `last_modified` keyword, in the same format as `date`. Empty when the source sets none, so a
   * template shows it only for an entry that set it.
   */
  struct Str modified;

  /**
   * Same instant as `modified` in Unix epoch seconds, or `date_epoch` when `modified` is empty, so
   * it always holds the entry's last change. `content_entry_modified_latest` compares it.
   */
  int64_t modified_epoch;

  /** Optional summary text for templates. Empty when the source omits it. */
  struct Str description;

  /** URL-safe slug derived from the `slug` key or keyword, or from the source filename. */
  struct Str slug;

  /** Array of arena-owned tag strings. */
  struct Str* tags;

  /**
   * Number of `tags` entries. `0` with `tags` as `NULL` when the source had no `tags` key or
   * `filetags` keyword, and `0` with `tags` non-`NULL` for an empty one. Readers must not
   * distinguish the two.
   */
  ptrdiff_t tag_count;

  /** Optional safe relative template name override. Empty uses site config `content_template`. */
  struct Str template;

  /**
   * Markdown or Org body that the convert phase reads: the source after its frontmatter, or the
   * whole Org source.
   */
  struct Str body_source;

  /**
   * Rendered HTML body. A draft, or an entry that failed to parse, never fills its entry slot, and
   * a link that fails in the body stops the build before the first write, so no half-built entry is
   * ever published.
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

  /**
   * Whether the body links another content source, whose URL the page then depends on, so the build
   * cache treats the page as one that read the entry list.
   */
  bool has_entry_link;

  /**
   * Whether the body links an asset through a denote identifier. The build cache has no row for an
   * asset, so every build makes the page again.
   */
  bool has_asset_link;
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
 * @brief Returns the newest change among the entries, exposed to templates as `site.modified`.
 *
 * An entry's change is its `modified`, or its `date` when `modified` is empty. Resolved once from
 * the sorted entry set and passed to every render, so a content template and an aggregate template
 * cannot disagree about it. An empty set falls back to the Unix epoch, so `site.modified` always
 * holds a valid RFC 3339 value even for a site with no posts yet.
 *
 * @param content_entries     Content entries sorted newest-first. Each entry must have
 *                            `modified_epoch` set. May be `NULL` only when `content_entry_count` is
 *                            0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @return The newest change, from the first entry in sorted order that has it, or the Unix epoch
 *         when there are no entries.
 */
struct Str content_entry_modified_latest(const struct ContentEntry* const* content_entries,
                                         ptrdiff_t content_entry_count);

#endif
