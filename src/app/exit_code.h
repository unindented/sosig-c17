#ifndef SOSIG_EXIT_CODE_H
#define SOSIG_EXIT_CODE_H

/** Process exit codes returned by `main` and the `cmd_*_run` command entry points. */
enum ExitCode {
  /** The command completed successfully. */
  EXIT_CODE_OK = 0,

  /** The command ran but failed at runtime. */
  EXIT_CODE_FAILURE = 1,

  /** The command line could not be parsed. */
  EXIT_CODE_USAGE = 2,
};

/**
 * Highest value the range above allows. Nothing else in the build inspects these numbers, so a code
 * outside the range would compile and test clean.
 */
enum { EXIT_CODE_VALUE_MAX = 125 };

_Static_assert((int)EXIT_CODE_OK <= (int)EXIT_CODE_VALUE_MAX &&
                   (int)EXIT_CODE_FAILURE <= (int)EXIT_CODE_VALUE_MAX &&
                   (int)EXIT_CODE_USAGE <= (int)EXIT_CODE_VALUE_MAX,
               "exit codes must stay below the shell's reserved range");

#endif
