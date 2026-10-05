#ifndef SOSIG_TEST_RENDER_SUPPORT_H
#define SOSIG_TEST_RENDER_SUPPORT_H

#include <stddef.h>

struct ContentEntry;
struct PathList;
struct SiteConfig;
struct StringBuffer;

/**
 * @brief Runs the page phase over entry slots the parse phase filled.
 *
 * Collects, sorts and dates the published entries the way the build does before its page phase.
 *
 * @param site_config        Configuration used for rendering. Must not be `NULL`.
 * @param source_entries     Entry slots filled by the parse phase. Must not be `NULL`.
 * @param source_entry_count Number of slots in `source_entries`.
 * @param worker_count       Worker threads the page phase runs on.
 * @param error_out          Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
int render_pages(const struct SiteConfig* site_config,
                 struct ContentEntry* const* source_entries,
                 size_t source_entry_count,
                 size_t worker_count,
                 struct StringBuffer* error_out);

/**
 * @brief Renders named fixture sources through both parallel phases on one worker.
 *
 * Loads `sosig.toml` from `root_dir` and runs with `root_dir` as the working directory, restoring
 * the previous one before returning.
 *
 * @param root_dir            Fixture root used as the working directory. Must not be `NULL`.
 * @param relative_paths      Source paths relative to `root_dir`. Must not be `NULL`.
 * @param relative_path_count Number of entries in `relative_paths`.
 * @param permalink_override  Replacement permalink, or `NULL` to keep the configured value.
 * @param site_config         Configuration populated from the fixture. Must not be `NULL`.
 * @param source_paths        Path list populated with the sources. Must not be `NULL`.
 * @param source_entries      One entry slot per path in `relative_paths`, each `NULL`-initialized,
 *                            so `entry_renderer_free_entries` is safe whether or not it is written.
 *                            Must not be `NULL`.
 * @param error_out           Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
int render_sources(const char* root_dir,
                   const char* const* relative_paths,
                   size_t relative_path_count,
                   const char* permalink_override,
                   struct SiteConfig* site_config,
                   struct PathList* source_paths,
                   struct ContentEntry** source_entries,
                   struct StringBuffer* error_out);

/**
 * @brief Renders one fixture source through both parallel phases on one worker.
 *
 * Equivalent to `render_sources` with a single path.
 *
 * @param root_dir             Fixture root used as the working directory. Must not be `NULL`.
 * @param source_relative_path Source path relative to `root_dir`. Must not be `NULL`.
 * @param permalink_override   Replacement permalink, or `NULL` to keep the configured value.
 * @param site_config          Configuration populated from the fixture. Must not be `NULL`.
 * @param source_paths         Path list populated with the source. Must not be `NULL`.
 * @param source_entries       One `NULL`-initialized entry slot, so `entry_renderer_free_entries`
 *                             is safe whether or not it is written. Must not be `NULL`.
 * @param error_out            Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` on test-plumbing
 *         failure.
 */
int render_single_source(const char* root_dir,
                         const char* source_relative_path,
                         const char* permalink_override,
                         struct SiteConfig* site_config,
                         struct PathList* source_paths,
                         struct ContentEntry** source_entries,
                         struct StringBuffer* error_out);

/**
 * @brief Reads a page the page phase wrote below a fixture root.
 *
 * Pages in these tests are short, so a fixed-size read holds any of them whole.
 *
 * @param root_dir      Fixture root the output path is relative to. Must not be `NULL`.
 * @param relative_path Output path relative to `root_dir`. Must not be `NULL`.
 * @return Terminated file contents the caller must `free`, or `NULL` when the file cannot be read.
 */
char* read_output(const char* root_dir, const char* relative_path);

#endif
