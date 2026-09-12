#include "domain/content_entry.h"

#include <stdlib.h>
#include <string.h>

/**
 * `content_entry_latest_date` returns the Unix epoch when there are no entries. This gives
 * `site.updated` a valid RFC 3339 value before the first post.
 */
static const char* const LATEST_DATE_FALLBACK = "1970-01-01T00:00:00Z";

/**
 * @brief Orders two content entries newest-first, breaking ties by output path.
 *
 * @param a First element, a `struct ContentEntry* const*`. Must not be `NULL`.
 * @param b Second element, a `struct ContentEntry* const*`. Must not be `NULL`.
 * @return Positive, zero, or negative so `qsort` orders the newer entry first.
 */
static int compare_content_entries(const void* a, const void* b) __attribute__((nonnull(1, 2)));

void content_entry_init(struct ContentEntry* entry) {
  // This is one literal naming only the non-zero defaults, so a field added to `ContentEntry`
  // cannot be left out here. A zeroed arena is a valid initialized arena, which is what
  // `arena_init` writes.
  *entry = (struct ContentEntry){.description = ""};
}

void content_entry_free(struct ContentEntry* entry) {
  arena_free(&entry->arena);
  content_entry_init(entry);
}

void content_entry_sort(struct ContentEntry** content_entries, size_t content_entry_count) {
  if (content_entry_count > 0) {
    qsort(content_entries, content_entry_count, sizeof(*content_entries), compare_content_entries);
  }
}

const char* content_entry_latest_date(const struct ContentEntry* const* content_entries,
                                      size_t content_entry_count) {
  return content_entry_count > 0 ? content_entries[0]->date : LATEST_DATE_FALLBACK;
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
  // The manifest pass later proves that output paths are distinct. A duplicate can compare equal
  // here, but that build fails before rendering a template.
  return strcmp(pa->output_path, pb->output_path);
}
