#include "domain/content_entry.h"

#include <stddef.h>
#include <stdlib.h>

#include "shared/str.h"

/**
 * `content_entry_modified_latest` returns the Unix epoch when there are no entries. This gives
 * `site.modified` a valid RFC 3339 value before the first post.
 */
#define MODIFIED_LATEST_FALLBACK S("1970-01-01T00:00:00Z")

/**
 * @brief Orders two content entries newest-first, breaking ties by output path.
 *
 * @param a First element, a `struct ContentEntry* const*`. Must not be `NULL`.
 * @param b Second element, a `struct ContentEntry* const*`. Must not be `NULL`.
 * @return Positive, zero, or negative so `qsort` orders the newer entry first.
 */
static int compare_content_entries(const void* a, const void* b) __attribute__((nonnull(1, 2)));

void content_entry_sort(struct ContentEntry** content_entries, ptrdiff_t content_entry_count) {
  if (content_entry_count > 0) {
    qsort(content_entries, (size_t)content_entry_count, sizeof(*content_entries),
          compare_content_entries);
  }
}

struct Str content_entry_modified_latest(const struct ContentEntry* const* content_entries,
                                         ptrdiff_t content_entry_count) {
  if (content_entry_count == 0) {
    return MODIFIED_LATEST_FALLBACK;
  }
  // The entries sort by `date`, so the newest change can be any of them. A tie keeps the earlier
  // entry, which makes a site without `modified` values return the first entry's `date`.
  const struct ContentEntry* latest = content_entries[0];
  for (ptrdiff_t i = 1; i < content_entry_count; i++) {
    if (content_entries[i]->modified_epoch > latest->modified_epoch) {
      latest = content_entries[i];
    }
  }
  return latest->modified.len > 0 ? latest->modified : latest->date;
}

static int compare_content_entries(const void* a, const void* b) {
  const struct ContentEntry* pa = *(struct ContentEntry* const*)a;
  const struct ContentEntry* pb = *(struct ContentEntry* const*)b;
  if (pb->date_epoch > pa->date_epoch) {
    return 1;
  }
  if (pb->date_epoch < pa->date_epoch) {
    return -1;
  }
  // The manifest phase later proves that output paths are distinct. A duplicate can compare equal
  // here, but that build fails before rendering a template.
  return str_compare(pa->output_path, pb->output_path);
}
