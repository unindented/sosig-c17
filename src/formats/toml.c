#include "formats/toml.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/text.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * Size in bytes of the longest timestamp `toml_datum_format_rfc3339` writes,
 * `YYYY-MM-DDThh:mm:ss.uuuuuu+hh:mm`, including the `NUL`. tomlc17 accepts only a four-digit year,
 * in-range date and time fields, at most six fraction digits, and an offset within `±23:59`, so
 * every datum it parses fits.
 */
enum { RFC3339_SIZE = sizeof("YYYY-MM-DDThh:mm:ss.uuuuuu+hh:mm") };

// The components are formatted and negated as `int`, and `toml_datum_to_epoch`'s product stays
// inside `int64_t` because `year` is 16 bits. A vendor bump that widens a field must revisit both.
_Static_assert(sizeof(((toml_datum_t*)NULL)->u.ts.year) == sizeof(int16_t) &&
                   sizeof(((toml_datum_t*)NULL)->u.ts.usec) == sizeof(int32_t) &&
                   sizeof(((toml_datum_t*)NULL)->u.ts.tz) == sizeof(int16_t),
               "tomlc17 timestamp fields changed width");

/**
 * @brief Installs `toml_parse_document_realloc` as tomlc17's allocator. Run once through
 *        `pthread_once`.
 */
static void toml_parse_document_install_allocator(void);

/**
 * @brief Reallocates like `realloc`, calling `os_oom` instead of returning a failure.
 *
 * This is tomlc17's allocator once `toml_parse_document` installs it.
 *
 * @param ptr  Allocation to resize, or `NULL` to allocate.
 * @param size New size in bytes.
 * @return The resized allocation. `NULL` only for a zero `size`, as `realloc` allows.
 */
static void* toml_parse_document_realloc(void* ptr, size_t size);

/**
 * @brief Converts a proleptic Gregorian date to days since `1970-01-01`.
 *
 * @param year  Full Gregorian year.
 * @param month Month in the range `1` to `12`.
 * @param day   Day of month in the range `1` to `31`.
 * @return Signed day count relative to the Unix epoch (negative before `1970-01-01`).
 */
static int64_t days_from_civil(int year, unsigned month, unsigned day);

/**
 * @brief Reports whether a key matches one of a list of names.
 *
 * @param key             Key bytes.
 * @param known_keys      Names to match against. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of names in `known_keys`.
 * @return `true` when `key` equals one of `known_keys`, otherwise `false`.
 */
static bool is_known_key(struct Str key, const struct Str* known_keys, ptrdiff_t known_key_count);

int toml_parse_document(struct Str document,
                        struct Str file_path,
                        toml_result_t* parsed_out,
                        struct Str operation,
                        struct Log* log) {
  assert(document.len >= 0 && document.len <= INT_MAX);
  static pthread_once_t allocator_once = PTHREAD_ONCE_INIT;
  (void)pthread_once(&allocator_once, toml_parse_document_install_allocator);
  // The source name tomlc17 can attach to each datum goes unread here, and the error message never
  // includes it, so the document is parsed unnamed.
  *parsed_out = toml_parse(document.data, (int)document.len);
  if (!parsed_out->ok) {
    (void)log_error_in(log, file_path, "%.*s: %s", STR_FMT(operation), parsed_out->errmsg);
    // The call currently releases nothing. tomlc17 frees its datum tree and pool on failure, then
    // returns a zeroed result. The vendor requires callers to pass every result to `toml_free`.
    // Following that contract keeps this code correct if a later version retains data after
    // failure.
    toml_free(*parsed_out);
    return -1;
  }
  return 0;
}

bool toml_datum_is_text(toml_datum_t value) {
  // tomlc17 terminates the string but excludes that terminator from `len`, so scanning exactly
  // `len` bytes finds only a `NUL` that the parser decoded from an escape.
  return value.type == TOML_STRING && text_is_nul_free(toml_datum_str(value));
}

struct Str toml_datum_str(toml_datum_t value) {
  assert(value.type == TOML_STRING);
  return (struct Str){value.u.str.ptr, value.u.str.len};
}

int toml_datum_to_epoch(toml_datum_t value, int64_t* epoch_out) {
  int64_t day_seconds;
  int timezone_minutes;
  switch (value.type) {
    case TOML_DATE:
      day_seconds = 0;
      timezone_minutes = 0;
      break;
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
      day_seconds =
          (int64_t)value.u.ts.hour * 3600 + (int64_t)value.u.ts.minute * 60 + value.u.ts.second;
      timezone_minutes = value.type == TOML_DATETIMETZ ? value.u.ts.tz : 0;
      break;
    default:
      return -1;
  }
  const int64_t days =
      days_from_civil(value.u.ts.year, (unsigned)value.u.ts.month, (unsigned)value.u.ts.day);
  // Signed overflow below would be undefined behavior, and cannot arise. `value.u.ts.year` is an
  // `int16_t`, as the assertion above pins, so even at its extreme the day count stays near 1.2e7
  // and the product near 1e12, orders of magnitude inside `int64_t`.
  *epoch_out = days * 86400 + day_seconds - (int64_t)timezone_minutes * 60;
  return 0;
}

struct Str toml_datum_format_rfc3339(toml_datum_t value, struct Arena* arena) {
  assert(value.type == TOML_DATE || value.type == TOML_DATETIME || value.type == TOML_DATETIMETZ);
  // tomlc17 fills every time field of a `TOML_DATE` with `-1`, so a date-only datum takes midnight
  // rather than reading them.
  const bool has_time = value.type != TOML_DATE;
  // The fraction drops its trailing zeros. A zero fraction keeps a precision of `0`, which prints
  // no digits, so a whole second gets no `.` either.
  int fraction = has_time ? value.u.ts.usec : 0;
  int fraction_digits = fraction != 0 ? 6 : 0;
  while (fraction != 0 && fraction % 10 == 0) {
    fraction /= 10;
    fraction_digits--;
  }
  const int timezone_minutes = value.type == TOML_DATETIMETZ ? value.u.ts.tz : 0;
  const int offset_minutes = timezone_minutes < 0 ? -timezone_minutes : timezone_minutes;
  char buf[RFC3339_SIZE];
  const int n =
      snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%s%.*d%c%02d:%02d", value.u.ts.year,
               value.u.ts.month, value.u.ts.day, has_time ? value.u.ts.hour : 0,
               has_time ? value.u.ts.minute : 0, has_time ? value.u.ts.second : 0,
               fraction_digits > 0 ? "." : "", fraction_digits, fraction,
               timezone_minutes < 0 ? '-' : '+', offset_minutes / 60, offset_minutes % 60);
  // `RFC3339_SIZE` holds every timestamp tomlc17 can parse.
  assert(n > 0 && n < (int)sizeof(buf));
  struct Str timestamp = {buf, n};
  // A zero offset is spelled `Z` rather than `+00:00`.
  if (timezone_minutes == 0) {
    const ptrdiff_t offset_len = S("+00:00").len;
    buf[n - offset_len] = 'Z';
    timestamp = str_head(timestamp, n - offset_len + 1);
  }
  return str_clone(timestamp, arena);
}

int toml_require_known_keys(toml_datum_t table,
                            const struct Str* known_keys,
                            ptrdiff_t known_key_count,
                            struct Str key_kind,
                            struct Str key_prefix,
                            struct Str file_path,
                            struct Log* log) {
  int rc = 0;
  for (int32_t i = 0; i < table.u.tab.size; i++) {
    const struct Str key = {table.u.tab.key[i], table.u.tab.len[i]};
    if (is_known_key(key, known_keys, known_key_count)) {
      continue;
    }
    // A quoted key can hold a `NUL` decoded from an escape, where `%.*s` would stop and name a
    // different key, so such a key is rejected without being named, but with the table it is in. No
    // known key holds a `NUL`, so it is unknown too.
    if (!text_is_nul_free(key)) {
      rc = log_error_in(log, file_path, "%.*s key must not contain a NUL byte", STR_FMT(key_kind));
    } else {
      rc = log_error_in(log, file_path, "unknown %.*s key '%.*s%.*s'", STR_FMT(key_kind),
                        STR_FMT(key_prefix), STR_FMT(key));
    }
  }
  return rc;
}

static void toml_parse_document_install_allocator(void) {
  toml_option_t option = toml_default_option();
  option.mem_realloc = toml_parse_document_realloc;
  toml_set_option(option);
}

static void* toml_parse_document_realloc(void* ptr, size_t size) {
  void* result = realloc(ptr, size);
  if (result == NULL && size > 0) {
    os_oom();
  }
  return result;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day) {
  // This is Howard Hinnant's civil calendar algorithm, kept in its published form. `era`, `yoe`,
  // `doy` and `doe` are his variable names and the expression shapes are his, so a reader can check
  // this line by line against the derivation. That compatibility constraint exempts these names
  // from the naming rule. Expanding the names would break the line-by-line correspondence with the
  // published algorithm.
  //
  // `(unsigned)-3` is a deliberate modular wrap rather than a mistake. Conversion to an unsigned
  // type and unsigned overflow are both defined, and `month + (unsigned)-3` lands on `month - 3`
  // for every `month` above 2. The signed spelling a reader reaches for instead would not be
  // defined.
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? (unsigned)-3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool is_known_key(struct Str key, const struct Str* known_keys, ptrdiff_t known_key_count) {
  for (ptrdiff_t i = 0; i < known_key_count; i++) {
    if (str_eq(known_keys[i], key)) {
      return true;
    }
  }
  return false;
}
