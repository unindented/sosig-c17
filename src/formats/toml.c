#include "formats/toml.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shared/arena.h"

/**
 * Size in bytes of a `YYYY-MM-DDT00:00:00Z` date-only timestamp. A well-formed date needs 21 bytes
 * with the `NUL`, but the malformed case sets the size. `year`, `month` and `day` are each
 * `int16_t` in the vendored `toml_datum_t`, so at their extreme `%04d-%02d-%02d` prints three
 * six-character fields and the whole string reaches 30 bytes, 31 with the `NUL`. This 32 is
 * therefore headroom the malformed case needs, not a round number. The `snprintf` truncation check
 * in `toml_datum_format_rfc3339` never fires on this path, and shrinking this constant to the
 * well-formed 21 would make it fire.
 */
enum { RFC3339_DATE_SIZE = 32 };

/**
 * Size in bytes of a full `YYYY-MM-DDThh:mm:ss<tz>` timestamp. A well-formed value needs 33 bytes
 * with the `NUL`: `YYYY-MM-DDThh:mm:ss` is 19, the longest `.uuuuuu` fraction 7, the longest
 * `+hh:mm` offset 6. Unlike the date buffer above, this one is *not* sized for the malformed case.
 * Six `int16_t` fields at their extreme reach 41 bytes before the fraction and offset, 55 in total.
 * The truncation check therefore carries this one. It is reachable rather than defensive.
 */
enum { RFC3339_DATETIME_SIZE = 48 };

/**
 * Size in bytes of a `+hh:mm`/`-hh:mm` timezone suffix (or `Z`). A real offset is 6 characters, 7
 * bytes with the `NUL`. The worst `int16_t` `tz` is 7 characters, because `%c%02d:%02d` prints
 * `-546:08` for -32768 and `+546:07` for 32767. 8 bytes is therefore the exact requirement with no
 * slack, and the truncation check in `toml_datum_format_rfc3339_timezone` cannot fire.
 */
enum { RFC3339_TIMEZONE_SIZE = 8 };

/**
 * Size in bytes of a `.uuuuuu` fractional-seconds suffix (six digits) plus a `NUL`. This is exact,
 * not rounded. `.` plus six digits plus the terminator is 8. This is sized for the well-formed case
 * only. `usec` is `int32_t`, so an out-of-range value prints up to 12 characters and truncates,
 * which makes the check in `toml_datum_format_rfc3339_fraction` reachable.
 *
 * That check bounds the formatted length, not the value, so it is not what keeps a malformed
 * fraction out of the output. A `usec` from -99999 to -1 prints as 7 characters, fits, and comes
 * out verbatim as `.-00001`. Two other things rule that out. tomlc17 leaves `usec` at `0` for a
 * datetime carrying no fraction, and `toml_datum_format_rfc3339` reaches this helper only for
 * `TOML_DATETIME` and `TOML_DATETIMETZ`, while `TOML_DATE` goes through its own branch. `TOML_DATE`
 * is the type tomlc17 gives a `usec` of -1. Folding that branch into the general path would emit
 * `.-00001` rather than return `NULL`.
 */
enum { RFC3339_FRACTION_SIZE = 8 };

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
 * @brief Formats an RFC 3339 fractional-seconds suffix, trimming trailing zeros.
 *
 * Writes an empty string when `fraction_usec` is zero. Otherwise it writes a `.uuuuuu` suffix with
 * trailing zeros removed.
 *
 * @param fraction_usec Microseconds in the range `0` to `999999`.
 * @param text_out      Destination buffer that receives the terminated suffix. Must hold at least
 *                      one byte: the zero-`fraction_usec` path writes the terminator without
 *                      consulting `text_out_len`, so a smaller buffer is a write past the
 * end rather than a reported truncation. Must not be `NULL`.
 * @param text_out_len  Size of `text_out` in bytes.
 * @return `0` on success, or `-1` when the `.uuuuuu` suffix was truncated. The zero-`fraction_usec`
 *         path cannot truncate, and cannot detect a buffer too small.
 */
static int toml_datum_format_rfc3339_fraction(int fraction_usec,
                                              char* text_out,
                                              size_t text_out_len) __attribute__((nonnull(2)));

/**
 * @brief Formats an RFC 3339 timezone suffix as `Z` or a signed `±hh:mm` offset.
 *
 * @param timezone_minutes Offset from UTC in minutes. `0` yields `Z`.
 * @param text_out         Destination buffer that receives the terminated suffix. Must hold at
 *                         least two bytes: the `Z` path writes both without consulting
 *                         `text_out_len`, so a smaller buffer is a write past the end
 * rather than a reported truncation. Must not be `NULL`.
 * @param text_out_len     Size of `text_out` in bytes.
 * @return `0` on success, or `-1` when the `±hh:mm` form was truncated. The `Z` path cannot
 *         truncate, and cannot detect a buffer too small.
 */
static int toml_datum_format_rfc3339_timezone(int timezone_minutes,
                                              char* text_out,
                                              size_t text_out_len) __attribute__((nonnull(2)));

bool toml_datum_is_text(toml_datum_t value) {
  // Use `memchr` over exactly `len` bytes instead of testing `strlen(ptr) != len`. tomlc17
  // terminates the string but excludes that terminator from `len`. This scan finds only a `NUL`
  // that the parser decoded from an escape. It does not find the appended terminator.
  return value.type == TOML_STRING &&
         memchr(value.u.str.ptr, '\0', (size_t)value.u.str.len) == NULL;
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
  // `int16_t`, so even at its extreme the day count stays near 1.2e7 and the product near 1e12,
  // orders of magnitude inside `int64_t`. The bound comes from the field width in the vendored
  // `toml_datum_t`'s `ts` struct, which is invisible at this line.
  *epoch_out = days * 86400 + day_seconds - (int64_t)timezone_minutes * 60;
  return 0;
}

char* toml_datum_format_rfc3339(toml_datum_t value, struct Arena* arena) {
  if (value.type == TOML_DATE) {
    char buf[RFC3339_DATE_SIZE];
    // Every one of the four `snprintf` sites in this file checks its result against the destination
    // capacity, two as this ternary and two as the complementary guard against `text_out_len`.
    // `snprintf` returns the length it *would* have written, so a value at or past the buffer size
    // means the output was truncated. A negative value means an encoding error.
    //
    // The checks are uniform, but what they carry is not. Only the datetime and fraction buffers
    // are small enough for a malformed component to overflow, so only those two checks are
    // reachable. This one and the timezone one cannot fire, because their buffers are sized for the
    // extremes of the vendored field widths. See the constants above, where each records which case
    // sized it. Keeping all four uniform is deliberate. The components come straight from the
    // vendored struct, nothing here clamps them. A field widening on a vendor bump would move the
    // boundary between the two groups without any of these lines changing.
    const int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02dT00:00:00Z", value.u.ts.year,
                           value.u.ts.month, value.u.ts.day);
    return n > 0 && (size_t)n < sizeof(buf) ? arena_strdup(arena, buf) : NULL;
  }
  if (value.type != TOML_DATETIME && value.type != TOML_DATETIMETZ) {
    return NULL;
  }
  char frac[RFC3339_FRACTION_SIZE];
  if (toml_datum_format_rfc3339_fraction(value.u.ts.usec, frac, sizeof(frac)) != 0) {
    return NULL;
  }
  const int timezone_minutes = value.type == TOML_DATETIMETZ ? value.u.ts.tz : 0;
  char tz[RFC3339_TIMEZONE_SIZE];
  if (toml_datum_format_rfc3339_timezone(timezone_minutes, tz, sizeof(tz)) != 0) {
    return NULL;
  }
  char buf[RFC3339_DATETIME_SIZE];
  const int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%s%s", value.u.ts.year,
                         value.u.ts.month, value.u.ts.day, value.u.ts.hour, value.u.ts.minute,
                         value.u.ts.second, frac, tz);
  return n > 0 && (size_t)n < sizeof(buf) ? arena_strdup(arena, buf) : NULL;
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

static int toml_datum_format_rfc3339_fraction(int fraction_usec,
                                              char* text_out,
                                              size_t text_out_len) {
  if (fraction_usec == 0) {
    text_out[0] = '\0';
    return 0;
  }
  const int n = snprintf(text_out, text_out_len, ".%06d", fraction_usec);
  if (n < 0 || (size_t)n >= text_out_len) {
    return -1;
  }
  size_t end = (size_t)n;
  while (end > 1 && text_out[end - 1] == '0') {
    end--;
  }
  text_out[end] = '\0';
  return 0;
}

static int toml_datum_format_rfc3339_timezone(int timezone_minutes,
                                              char* text_out,
                                              size_t text_out_len) {
  if (timezone_minutes == 0) {
    text_out[0] = 'Z';
    text_out[1] = '\0';
    return 0;
  }
  // Negating is defined only because `tz` is an `int16_t` in the vendored `toml_datum_t`, so the
  // worst case is `-(-32768)`, which fits an `int` on every target here. `-INT_MIN` would be signed
  // overflow. Re-check on a vendor bump that widens the field.
  const int minutes = timezone_minutes < 0 ? -timezone_minutes : timezone_minutes;
  const int sign = timezone_minutes < 0 ? '-' : '+';
  const int n = snprintf(text_out, text_out_len, "%c%02d:%02d", sign, minutes / 60, minutes % 60);
  if (n < 0 || (size_t)n >= text_out_len) {
    return -1;
  }
  return 0;
}
