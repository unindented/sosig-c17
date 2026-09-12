#ifndef SOSIG_TOML_H
#define SOSIG_TOML_H

#include <stdbool.h>
#include <stdint.h>
#include <tomlc17.h>

struct Arena;

// Every function here. Every caller that threads a datum onward, takes `toml_datum_t` by value
// rather than by pointer, against the general rule that a non-trivial struct is passed by pointer.
// This is the tomlc17 boundary. `toml_get` returns a datum by value (see
// `vendor/tomlc17/tomlc17.h`), so the alternative is a local copy per call whose only purpose is to
// have an address to take. Matching the library's shape at its own boundary keeps the translation
// in one layer, which is where the codebase confines every other external convention. Do not spread
// the by-value habit to structs this repository owns.

/**
 * @brief Reports whether a datum is a string whose bytes hold no embedded `NUL`.
 *
 * TOML permits an escape that decodes to `U+0000`, and tomlc17 encodes it as a `NUL` byte inside an
 * otherwise ordinary string value. Copying such a value would break the codebase's text invariant
 * (see `fs_read_file`) and silently truncate the rendered output at the `NUL`, so every caller that
 * turns a TOML string into an owned string checks this first.
 *
 * A datum's bytes are borrowed, not owned. `value.u.str.ptr` points into the `toml_result_t`'s own
 * storage and dies with `toml_free`, and `len` excludes the terminator. A caller that stored that
 * pointer rather than copying the bytes would compile, pass its test, and dangle the moment the
 * parse result was released.
 *
 * @param value Datum to classify.
 * @return `true` when `value` is a string with no embedded `NUL`, `false` otherwise.
 */
bool toml_datum_is_text(toml_datum_t value);

/**
 * @brief Converts a TOML date/datetime datum to Unix epoch seconds.
 *
 * It treats a date-only or timezone-less datetime as UTC. It truncates sub-second precision (the
 * result is whole seconds). It writes `epoch_out` only on success. It trusts the component ranges
 * because tomlc17 validates them while parsing.
 *
 * @param value     Datum to convert. Must be a TOML date, datetime, or datetime-with-timezone.
 * @param epoch_out Receives the resulting Unix timestamp in seconds. Must not be `NULL`.
 * @return `0` on success, or `-1` for any other datum type.
 */
int toml_datum_to_epoch(toml_datum_t value, int64_t* epoch_out) __attribute__((nonnull(2)));

/**
 * @brief Formats a TOML date/datetime datum into a normalized RFC 3339 timestamp string.
 *
 * It expands a date-only datum to midnight UTC. It preserves fractional seconds when present and
 * trims trailing zeros. It emits a datetime carrying an offset with that offset (`Z` for `+00:00`).
 * It treats a timezone-less datetime as UTC and emits it with `Z`, matching `toml_datum_to_epoch`.
 * It stores the result in arena-owned storage.
 *
 * @param value Datum to format. Must be a TOML date, datetime, or datetime-with-timezone.
 * @param arena Arena that owns the returned string. Must not be `NULL`.
 * @return Terminated timestamp owned by the arena, or `NULL` for any other type or on failure.
 */
char* toml_datum_format_rfc3339(toml_datum_t value, struct Arena* arena)
    __attribute__((nonnull(2)));

#endif
