#ifndef SOSIG_TEST_RENDER_SUPPORT_H
#define SOSIG_TEST_RENDER_SUPPORT_H

#include <stddef.h>

struct PathList;
struct RenderJobSet;
struct SiteConfig;
struct StringBuffer;

/**
 * @brief Runs the page-render pass over filled render-job slots.
 *
 * Collects, sorts and dates the published entries the way the build does before its page pass.
 *
 * @param site_config  Configuration used for rendering. Must not be `NULL`.
 * @param render_jobs  Render-job slots filled by the entry pass. Must not be `NULL`.
 * @param worker_count Worker threads the page pass runs on.
 * @param error_out    Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `1` on test-plumbing failure.
 */
int render_pages(const struct SiteConfig* site_config,
                 struct RenderJobSet* render_jobs,
                 size_t worker_count,
                 struct StringBuffer* error_out);

/**
 * @brief Renders named fixture sources through both worker-pool passes on one worker.
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
 * @param render_jobs_out     Receives the allocated render-job slots. Must be zero-initialized, so
 *                            `render_job_set_free` is safe whether or not it is written.
 * @param error_out           Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `1` on test-plumbing failure.
 */
int render_sources(const char* root_dir,
                   const char* const* relative_paths,
                   size_t relative_path_count,
                   const char* permalink_override,
                   struct SiteConfig* site_config,
                   struct PathList* source_paths,
                   struct RenderJobSet* render_jobs_out,
                   struct StringBuffer* error_out);

/**
 * @brief Renders one fixture source through both worker-pool passes on one worker.
 *
 * Equivalent to `render_sources` with a single path.
 *
 * @param root_dir             Fixture root used as the working directory. Must not be `NULL`.
 * @param source_relative_path Source path relative to `root_dir`. Must not be `NULL`.
 * @param permalink_override   Replacement permalink, or `NULL` to keep the configured value.
 * @param site_config          Configuration populated from the fixture. Must not be `NULL`.
 * @param source_paths         Path list populated with the source. Must not be `NULL`.
 * @param render_jobs_out      Receives the allocated render-job slots. Must be zero-initialized, so
 *                             `render_job_set_free` is safe whether or not it is written.
 * @param error_out            Buffer that receives any render diagnostic. Must not be `NULL`.
 * @return `0` on success, `-1` on render failure, or `1` on test-plumbing failure.
 */
int render_single_source(const char* root_dir,
                         const char* source_relative_path,
                         const char* permalink_override,
                         struct SiteConfig* site_config,
                         struct PathList* source_paths,
                         struct RenderJobSet* render_jobs_out,
                         struct StringBuffer* error_out);

/**
 * @brief Reads a page the render pass wrote below a fixture root.
 *
 * Pages in these tests are short, so a fixed-size read holds any of them whole.
 *
 * @param root_dir      Fixture root the output path is relative to. Must not be `NULL`.
 * @param relative_path Output path relative to `root_dir`. Must not be `NULL`.
 * @return Terminated file contents the caller must `free`, or `NULL` when the file cannot be read.
 */
char* read_output(const char* root_dir, const char* relative_path);

#endif
