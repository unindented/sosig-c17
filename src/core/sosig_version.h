#ifndef SOSIG_VERSION_H
#define SOSIG_VERSION_H

/**
 * @brief Returns the version string fixed when the build tree was configured.
 *
 * @return The non-empty, statically allocated version string. The caller must not free it.
 */
const char* sosig_version_string(void);

/**
 * @brief Returns the tool name followed by its version, such as `sosig 1.2.3`.
 *
 * `--version` prints this, and templates expose it as `generator`, for a page's
 * `<meta name="generator">` tag.
 *
 * @return The statically allocated generator string. The caller must not free it.
 */
const char* sosig_generator_string(void);

#endif
