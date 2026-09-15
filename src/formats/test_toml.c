#include <acutest.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tomlc17.h>

#include "core/arena.h"
#include "formats/toml.h"

/**
 * @brief Parses a one-value TOML document.
 *
 * @param value      TOML value text assigned to key `x`.
 * @param result_out Parse result the caller must release with `toml_free`.
 * @return The datum bound to `x`, borrowing storage from `result_out`.
 */
static toml_datum_t parse_value(const char* value, toml_result_t* result_out) {
  char doc[128];
  const int n = snprintf(doc, sizeof(doc), "x = %s\n", value);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(doc));
  *result_out = toml_parse_named(doc, n, "test");
  TEST_ASSERT(result_out->ok);
  return toml_get(result_out->toptab, "x");
}

// A plain string is text. A string carrying a `NUL` decoded from an escape is not. This is one of
// the two boundaries where external bytes become an owned `NUL`-free C string, so a datum that
// passes here is one `strlen` can measure. Without the escape case the check would pass for the
// wrong reason, since no `NUL` can appear literally in a TOML document.
static void test_is_text_rejects_embedded_nul(void) {
  toml_result_t result;
  TEST_CHECK(toml_datum_is_text(parse_value("\"plain\"", &result)));
  toml_free(result);

  TEST_CHECK(!toml_datum_is_text(parse_value("\"pre\\u0000post\"", &result)));
  toml_free(result);

  // A non-string datum is not text either, so no caller reaches the copy path with an integer.
  TEST_CHECK(!toml_datum_is_text(parse_value("7", &result)));
  toml_free(result);
}

// The Unix epoch anchor converts to a zero timestamp.
static void test_unix_epoch_anchor_converts_to_zero(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("1970-01-01T00:00:00Z", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == 0);
  toml_free(result);
}

// A date before the epoch converts to a negative count, which is the half of the documented signed
// range every other assertion in this file misses. All of them are at or after 1970, so a day count
// or epoch product narrowed to 32 bits would still pass them while `1900-01-01` wrapped. The epoch
// is the sole sort key for generated output, so a wrapped value would sort a 1900-dated entry as
// the newest post. Both expected values came from a run.
static void test_pre_epoch_dates_convert_to_negative(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("1969-12-31T23:59:59Z", &result);
  int64_t epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == -1);
  toml_free(result);

  // Below `INT32_MIN`, so a 32-bit narrowing anywhere on the path wraps this one.
  toml_result_t early_result;
  toml_datum_t early_value = parse_value("1900-01-01", &early_result);
  int64_t early_epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(early_value, &early_epoch) == 0);
  TEST_CHECK(early_epoch == -2208988800);
  toml_free(early_result);
}

// `2000-02-29` is a leap day in a century year, the case `days_from_civil`'s hand-rolled
// `yoe / 4 - yoe / 100` era arithmetic gets wrong if the 400-year rule is dropped. The expected
// epoch came from a run, not from prediction.
static void test_leap_day_converts_in_century_year(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("2000-02-29T00:00:00Z", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == 951782400);
  toml_free(result);

  // The two dates below exercise the conversion's `yoe / 100` century correction. `2000` cannot:
  // its year-of-era is `0`, so that term is `0` however it is spelled. The assertion above passes
  // with the correction removed. A year-of-era at or above `100` is what separates them, and
  // neither of these is a leap year in the Gregorian rule that the correction implements. The epoch
  // is the sole sort key for generated output, so a wrong one silently reorders posts rather than
  // failing anything. Both values came from a run.
  toml_result_t yoe_300;
  toml_datum_t yoe_300_value = parse_value("1900-03-01T00:00:00Z", &yoe_300);
  int64_t yoe_300_epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(yoe_300_value, &yoe_300_epoch) == 0);
  TEST_CHECK(yoe_300_epoch == -2203891200);
  toml_free(yoe_300);

  toml_result_t yoe_100;
  toml_datum_t yoe_100_value = parse_value("2100-03-01T00:00:00Z", &yoe_100);
  int64_t yoe_100_epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(yoe_100_value, &yoe_100_epoch) == 0);
  TEST_CHECK(yoe_100_epoch == 4107542400);
  toml_free(yoe_100);
}

// A date-only value normalizes to midnight UTC, both as an epoch and as a formatted string.
static void test_date_only_normalizes_to_midnight_utc(void) {
  struct Arena arena;
  arena_init(&arena);
  toml_result_t result;
  toml_datum_t value = parse_value("2030-01-02", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == 1893542400);
  const char* text = toml_datum_format_rfc3339(value, &arena);
  TEST_CHECK(text != NULL && strcmp(text, "2030-01-02T00:00:00Z") == 0);
  toml_free(result);
  arena_free(&arena);
}

// A UTC datetime round-trips unchanged.
static void test_utc_datetime_round_trips(void) {
  struct Arena arena;
  arena_init(&arena);
  toml_result_t result;
  toml_datum_t value = parse_value("2026-07-01T12:00:00Z", &result);
  const char* text = toml_datum_format_rfc3339(value, &arena);
  TEST_CHECK(text != NULL && strcmp(text, "2026-07-01T12:00:00Z") == 0);
  toml_free(result);
  arena_free(&arena);
}

// Negative and positive offsets convert to the same UTC instant.
static void test_offsets_convert_to_same_utc_instant(void) {
  struct Arena arena;
  arena_init(&arena);
  toml_result_t neg;
  toml_result_t pos;
  toml_result_t utc;
  toml_datum_t neg_value = parse_value("2026-07-05T09:00:00-07:00", &neg);
  toml_datum_t pos_value = parse_value("2026-07-05T18:00:00+02:00", &pos);
  toml_datum_t utc_value = parse_value("2026-07-05T16:00:00Z", &utc);

  int64_t neg_epoch = -1;
  int64_t pos_epoch = -1;
  int64_t utc_epoch = -1;
  const char* neg_text = toml_datum_format_rfc3339(neg_value, &arena);
  const char* pos_text = toml_datum_format_rfc3339(pos_value, &arena);
  TEST_CHECK(toml_datum_to_epoch(neg_value, &neg_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(pos_value, &pos_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(utc_value, &utc_epoch) == 0);

  TEST_CHECK(neg_epoch == utc_epoch);
  TEST_CHECK(pos_epoch == utc_epoch);
  TEST_CHECK(neg_text != NULL && strcmp(neg_text, "2026-07-05T09:00:00-07:00") == 0);
  TEST_CHECK(pos_text != NULL && strcmp(pos_text, "2026-07-05T18:00:00+02:00") == 0);

  toml_free(neg);
  toml_free(pos);
  toml_free(utc);
  arena_free(&arena);
}

// Fractional seconds are preserved (trailing zeros trimmed) and truncated for the epoch.
static void test_fractional_seconds_preserved(void) {
  struct Arena arena;
  arena_init(&arena);
  toml_result_t half;
  toml_result_t micros;
  toml_result_t trailing;
  toml_result_t whole;
  toml_datum_t half_value = parse_value("2026-07-01T12:00:00.5Z", &half);
  toml_datum_t micros_value = parse_value("2026-07-01T12:00:00.123456Z", &micros);
  toml_datum_t trailing_value = parse_value("2026-07-01T12:00:00.120000Z", &trailing);
  toml_datum_t whole_value = parse_value("2026-07-01T12:00:00Z", &whole);

  const char* half_text = toml_datum_format_rfc3339(half_value, &arena);
  const char* micros_text = toml_datum_format_rfc3339(micros_value, &arena);
  const char* trailing_text = toml_datum_format_rfc3339(trailing_value, &arena);
  TEST_CHECK(half_text != NULL && strcmp(half_text, "2026-07-01T12:00:00.5Z") == 0);
  TEST_CHECK(micros_text != NULL && strcmp(micros_text, "2026-07-01T12:00:00.123456Z") == 0);
  TEST_CHECK(trailing_text != NULL && strcmp(trailing_text, "2026-07-01T12:00:00.12Z") == 0);

  // A fraction of all zeros yields no fractional part at all, not a bare `.`. That is a separate
  // branch from the trimming above rather than its extreme: `toml_datum_format_rfc3339_fraction`
  // returns early on a zero microsecond count and never formats or trims, so `.12` cannot reach it.
  // A frontmatter date written with an explicit `.000000` is the reachable way in.
  toml_result_t zeros;
  toml_datum_t zeros_value = parse_value("2026-07-01T12:00:00.000000Z", &zeros);
  const char* zeros_text = toml_datum_format_rfc3339(zeros_value, &arena);
  TEST_CHECK(zeros_text != NULL && strcmp(zeros_text, "2026-07-01T12:00:00Z") == 0);
  toml_free(zeros);

  // The epoch drops sub-second precision, so fractional value matches its whole-second sibling.
  int64_t half_epoch = -1;
  int64_t whole_epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(half_value, &half_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(whole_value, &whole_epoch) == 0);
  TEST_CHECK(half_epoch == whole_epoch);

  toml_free(half);
  toml_free(micros);
  toml_free(trailing);
  toml_free(whole);
  arena_free(&arena);
}

// A timezone-less datetime is treated as UTC: emitted with `Z` and converted as the UTC instant.
static void test_timezoneless_datetime_treated_as_utc(void) {
  struct Arena arena;
  arena_init(&arena);
  toml_result_t local;
  toml_result_t utc;
  toml_datum_t local_value = parse_value("2026-07-01T12:00:00", &local);
  toml_datum_t utc_value = parse_value("2026-07-01T12:00:00Z", &utc);

  const char* text = toml_datum_format_rfc3339(local_value, &arena);
  TEST_CHECK(text != NULL && strcmp(text, "2026-07-01T12:00:00Z") == 0);

  int64_t local_epoch = -1;
  int64_t utc_epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(local_value, &local_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(utc_value, &utc_epoch) == 0);
  TEST_CHECK(local_epoch == utc_epoch);

  toml_free(local);
  toml_free(utc);
  arena_free(&arena);
}

// A datum that is neither a date nor a datetime is rejected. The test uses two forms with different
// failure modes. A string populates `u.str` and cannot produce a plausible timestamp. A time-only
// datum uses the same `u.ts` payload as accepted types. Only its `year`, `month`, and `day` fields
// differ; each contains `-1`. Adding `TOML_TIME` to the accepted set is a plausible slip. It would
// turn `date = 12:00:00` into a garbage epoch and an `-001--1--1T12:00:00Z` timestamp in the page
// and the feed instead of a diagnostic.
static void test_rejects_non_datetime_values(void) {
  struct Arena arena;
  arena_init(&arena);
  toml_result_t result;
  toml_datum_t value = parse_value("\"not a date\"", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == -1);
  // `epoch_out` is written only on success, so the sentinel set above survives. A caller keeping a
  // previously computed epoch in the same variable across a failed call depends on this.
  TEST_CHECK(epoch == -1);
  TEST_CHECK(toml_datum_format_rfc3339(value, &arena) == NULL);
  toml_free(result);

  toml_result_t time_result;
  toml_datum_t time_value = parse_value("12:00:00", &time_result);
  int64_t time_epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(time_value, &time_epoch) == -1);
  TEST_CHECK(time_epoch == -1);
  TEST_CHECK(toml_datum_format_rfc3339(time_value, &arena) == NULL);
  toml_free(time_result);

  arena_free(&arena);
}

TEST_LIST = {{"is text rejects embedded nul", test_is_text_rejects_embedded_nul},
             {"unix epoch anchor converts to zero", test_unix_epoch_anchor_converts_to_zero},
             {"pre epoch dates convert to negative", test_pre_epoch_dates_convert_to_negative},
             {"leap day converts in century year", test_leap_day_converts_in_century_year},
             {"date only normalizes to midnight utc", test_date_only_normalizes_to_midnight_utc},
             {"utc datetime round trips", test_utc_datetime_round_trips},
             {"offsets convert to same utc instant", test_offsets_convert_to_same_utc_instant},
             {"fractional seconds preserved", test_fractional_seconds_preserved},
             {"timezoneless datetime treated as utc", test_timezoneless_datetime_treated_as_utc},
             {"rejects non-datetime values", test_rejects_non_datetime_values},
             {NULL, NULL}};
