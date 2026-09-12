#ifndef SOSIG_CMD_CONFIG_H
#define SOSIG_CMD_CONFIG_H

#include "app/exit_code.h"

/**
 * @brief Runs the `config` command: loads `sosig.toml` and prints the resolved configuration.
 *
 * On success prints the effective configuration to `stdout`. On failure prints the reason to
 * `stderr`, whether `sosig.toml` could not be loaded or `stdout` could not be written.
 *
 * @return `EXIT_CODE_OK` on success, or `EXIT_CODE_FAILURE` when `sosig.toml` cannot be loaded or
 *         the resolved configuration cannot be written to `stdout`.
 */
enum ExitCode cmd_config_run(void);

#endif
