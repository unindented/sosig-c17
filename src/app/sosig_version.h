#ifndef SOSIG_VERSION_H
#define SOSIG_VERSION_H

/**
 * @brief Returns the version string fixed when the build tree was configured.
 *
 * @return The non-empty, statically allocated version string. The caller must not free it.
 */
const char* sosig_version_string(void);

#endif
