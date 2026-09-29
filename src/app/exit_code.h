#ifndef SOSIG_EXIT_CODE_H
#define SOSIG_EXIT_CODE_H

/** Process exit codes returned by `main`, `cli_dispatch`, and the `cmd_*_run` entry points. */
enum ExitCode {
  /** The command completed successfully. */
  EXIT_CODE_OK = 0,

  /** The command ran but failed at runtime. */
  EXIT_CODE_FAILURE = 1,

  /** The command line could not be parsed. */
  EXIT_CODE_USAGE = 2,
};

/**
 * Highest exit status a code may use. Shells reserve 126 and above for their own meanings (command
 * not executable, command not found, and 128 plus a signal number), so a code there would be
 * misread as one of those. The `_Static_assert` below enforces this bound, and `test_exit_code.c`
 * pins the individual values.
 */
enum { EXIT_CODE_VALUE_MAX = 125 };

_Static_assert((int)EXIT_CODE_OK <= (int)EXIT_CODE_VALUE_MAX &&
                   (int)EXIT_CODE_FAILURE <= (int)EXIT_CODE_VALUE_MAX &&
                   (int)EXIT_CODE_USAGE <= (int)EXIT_CODE_VALUE_MAX,
               "exit codes must stay below the shell's reserved range");

#endif
