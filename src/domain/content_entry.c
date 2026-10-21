#include "domain/content_entry.h"

#include <stddef.h>
#include <stdlib.h>

#include "shared/str.h"

/**
 * `content_entry_date_latest` returns the Unix epoch when there are no entries. This gives
 * `site.updated` a valid RFC 3339 value before the first post.
 */
#define DATE_LATEST_FALLBACK S("1970-01-01T00:00:00Z")

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

struct Str content_entry_date_latest(const struct ContentEntry* const* content_entries,
                                     ptrdiff_t content_entry_count) {
  return content_entry_count > 0 ? content_entries[0]->date : DATE_LATEST_FALLBACK;
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
