#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "build/output_path.h"
#include "core/path.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// A path within both limits is accepted and records nothing.
static void test_check_limits_accepts_bounded_path(void) {
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(S("posts/index.html"), S("content/a.md"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A path over the whole-path limit names the limit, the measured length, the producer, and the
// whole path.
static void test_check_limits_rejects_overlong_path(void) {
  enum { SEGMENT_LEN = 200, SEGMENT_COUNT = 6 };
  char path[SEGMENT_COUNT * (SEGMENT_LEN + 1)];
  ptrdiff_t path_len = 0;
  for (ptrdiff_t i = 0; i < SEGMENT_COUNT; i++) {
    if (i > 0) {
      path[path_len++] = '/';
    }
    memset(path + path_len, 'a', SEGMENT_LEN);
    path_len += SEGMENT_LEN;
  }
  const struct Str overlong = {path, path_len};
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(overlong, S("content/a.md"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'content/a.md' exceeds max output path length (%d bytes) "
                         "at %td bytes: '%.*s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, path_len, STR_FMT(overlong)));
  TEST_CHECK(log.errors == 1);
}

// A path within the whole-path limit but with one overlong segment names only that segment as the
// offending value.
static void test_check_limits_rejects_overlong_segment(void) {
  enum { SEGMENT_LEN = FILENAME_LEN_MAX + 1 };
  char path[sizeof("posts/") - 1 + SEGMENT_LEN];
  memcpy(path, "posts/", sizeof("posts/") - 1);
  memset(path + sizeof("posts/") - 1, 'b', SEGMENT_LEN);
  const struct Str segment = {path + sizeof("posts/") - 1, SEGMENT_LEN};
  const struct Str whole = {path, (ptrdiff_t)sizeof(path)};
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(whole, S("content/a.md"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'content/a.md' exceeds max filename length (%d "
                         "bytes) at %d bytes: '%.*s'\n",
                         FILENAME_LEN_MAX, SEGMENT_LEN, STR_FMT(segment)));
}

TEST_LIST = {
    {"check limits accepts bounded path", test_check_limits_accepts_bounded_path},
    {"check limits rejects overlong path", test_check_limits_rejects_overlong_path},
    {"check limits rejects overlong segment", test_check_limits_rejects_overlong_segment},
    {NULL, NULL},
};
