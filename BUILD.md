# Build system

`sosig` requires CMake 3.25 or later. This document explains the build targets and their relationships.

## Layout

The top-level CMake file controls the build. It sets project policy, finds the thread dependency, loads build settings, and adds each source directory.

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Build the private application library and the `sosig` executable |
| `src/*/CMakeLists.txt` | Add sources, header file sets, and local unit tests; `src/runtime/CMakeLists.txt` also defines the POSIX host library `sosig_os_posix` |
| `tests/CMakeLists.txt` | Add the golden test suite, the shared unit-test support library, and the in-memory host |
| `vendor/CMakeLists.txt` | Build each vendored dependency |
| `cmake/sosig_build_profiles.cmake` | Set warnings, optimization, fortification, and sanitizers |
| `cmake/sosig_quality.cmake` | Configure formatting and static analysis |
| `cmake/sosig_testing.cmake` | Register a unit test and choose the host it links |
| `cmake/sosig_packaging.cmake` | Configure installation and CPack archives |
| `cmake/sosig_run_golden.cmake` | Run one golden test |
| `cmake/sosig_restyle_graphviz_svg.cmake` | Add light and dark styles to the target graph |
| `cmake/toolchains/` | Configure Zig for Linux `musl` targets |
| `CMakeGraphVizOptions.cmake` | Set filters and layout for the target graph |

## Target graph

`sosig_app` is a private static library. It contains all application source files except `main.c` and `runtime/fs.c`. The `sosig` executable and unit tests link to it. `runtime/fs.c`, the POSIX implementation of `runtime/os.h`, is the object library `sosig_os_posix`, so each executable chooses its host when it is linked: `sosig` and the tests registered with the default `OS posix` link it, and the tests registered with `sosig_add_unit_test(<module> OS memory)` link `sosig_os_memory` (`tests/os_memory.c`) instead. Both are object libraries because `sosig_app` refers to them, and a static archive listed before its user would be skipped by a single-pass linker. `sosig_app` links to `sosig_vendor_sharedstuff` for the arena, counted string, output buffer, and diagnostic log implementations. The arena calls `os_oom` when an allocation does not fit, and `sharedstuff` leaves that function to the application: `main.c` defines the production one, which prints the log collected so far and `sosig: out of memory` with the file the thread was working on, and exits with `EXIT_CODE_FAILURE`. `sosig_test_support` defines the unit-test one.

The library is static because it supports one product. It does not provide a public ABI.

Each vendored project has a separate target. Header-only dependencies use interface libraries.

`sosig_app` links to `sosig_vendor_tomlc17` and `sosig_vendor_sharedstuff` as public dependencies because application headers expose their types. The other vendored dependencies are private.

`Threads::Threads` is a private dependency of `sosig_app`. CMake adds this link requirement to each executable that links to the static library.

Source directories group code by function. Apart from the host implementation, they do not define separate libraries. Each local `CMakeLists.txt` adds files to `sosig_app` and links the private vendored dependency that only its code uses. More libraries would add link boundaries without independent APIs.

### Generated dependency graph

CMake generates this graph from the `release` preset. The graph shows CMake targets and link relationships. It does not show dependencies between source modules. Graph options remove test-only targets, such as the in-memory host `sosig_os_memory`.

![CMake target dependency graph](media/dependencies.svg)

> [!tip]
> Use these commands to update the graph:
>
> ```sh
> cmake --preset release --graphviz=build/release/target-dependencies.dot
> dot -Tsvg build/release/target-dependencies.dot \
>   -o build/release/target-dependencies.raw.svg
> cmake \
>   -DSOSIG_GRAPHVIZ_SVG_INPUT=build/release/target-dependencies.raw.svg \
>   -DSOSIG_GRAPHVIZ_SVG_OUTPUT=media/dependencies.svg \
>   -P cmake/sosig_restyle_graphviz_svg.cmake
> ```

> [!important]
> CMake reads `CMakeGraphVizOptions.cmake` automatically when you use `--graphviz`. Do not include this file from `CMakeLists.txt`.

## Build profiles

The build uses three interface targets:

- `sosig_build_project` sets strict first-party warnings and sanitizer options.
- `sosig_build_tests` sets the common warning group and sanitizer options.
- `sosig_build_vendor` sets optimization and supported sanitizer options for vendored code.

All three targets require C17 or later. A parent project can select a newer standard. It cannot select an older standard.

Presets use `CMAKE_COMPILE_WARNING_AS_ERROR` to treat warnings as errors. The project does not force this setting on a parent build.

Generator expressions select configuration options at build time. The same rules work with single-config and multi-config generators.

`SOSIG_SANITIZER` accepts `none`, `address`, or `thread`. The `address` option enables AddressSanitizer and UndefinedBehaviorSanitizer. The `thread` option enables ThreadSanitizer. It adds sanitizer options to standard CMake configurations. It does not create custom build types.

The address-sanitizer profile instruments first-party and vendored code. Vendored code disables the unsupported `null` and `object-size` checks. First-party sources retain the full sanitizer set.

## Linting and formatting

CMake sets `clang-tidy` and `cppcheck` as properties of first-party targets. Neither tool checks vendored sources. cppcheck reads the vendored `sharedstuff` headers to expand `STR_FMT`. It also analyzes the inline functions those headers define.

Tests run `cppcheck`. Tests do not run `clang-tidy` because deliberate failure cases cause analyzer errors.

Cross builds do not run either host analyzer. Zig supplies target headers that the host analyzers cannot find when they repeat the compile command.

The `sosig_format` and `sosig_lint` targets run `clang-format` and then `cwrap`, which refills comment prose to 100 columns, the `.clang-format` column limit. Either tool is skipped when it is not installed. CI and the Linux image install a pinned `cwrap` release.

Both targets use an explicit list of first-party files. Add each new source or test to its target and this list.

This duplication is deliberate. A configure-time glob can omit a new file until CMake configures the project again.

## Tests

`SOSIG_BUILD_TESTING` controls test creation. It is true by default for a top-level build. It is false by default for a child build.

Each source directory adds its local unit tests. The top-level `tests/` directory adds the golden test suite, `sosig_test_support`, and the in-memory host `sosig_os_memory`. Each CTest case has a `sosig` label. It also has a `unit` or `golden` label.

`sosig_add_unit_test` takes `OS memory` to link the in-memory host, and then compiles the test with `SOSIG_TEST_OS_MEMORY` set to 1. Every unit test registered without `OS memory` links the POSIX host: the POSIX layer's own tests and the test-support test, which need a real filesystem; the suites that build their fixtures in a real temporary directory; and the tests that reach no host. New tests use the in-memory host. `sosig_add_unit_test` also takes `NAME` when one source builds more than one test. `src/runtime/test_os.c` uses both: it builds `sosig.os_posix`, which works in a real temporary directory, and `sosig.os_memory`. These are the contract tests of `runtime/os.h`. Both must pass. Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails instead of stopping the run.

`sharedstuff` has its own compile-fail tests for `buf_printf` and `log_error`. They check that a call whose arguments disagree with its format does not compile: a raw `struct Str` for `%.*s`, a missing argument, a `ptrdiff_t` for `%d`, a format that is not a literal, a non-string given to `STR_FMT`, and a disagreement through a forwarding wrapper. They run in the `sharedstuff` repository. The vendored copy holds no tests.

`sosig_test_support` holds the unit-test plumbing that several `test_*.c` files need: a fixture-root creator, a recursive fixture-tree remover, a fixture-file writer, a working-directory switch, standard-stream capture and restore, an arena over a fresh reservation, a log over a fresh reservation and exact-text log checks, and an out-of-memory check. It also defines the unit-test `os_oom`, which exits with a status of its own, so `test_is_oom` can run code in a child process and tell running out of memory apart from a crash. It is an object library, so that definition is linked into every test executable whatever the link order. Its own unit test is `tests/test_test_support.c`. `test_support.c` defines `TEST_NO_MAIN`, so acutest's `main` and run state stay in each test executable's own translation unit and the link resolves acutest's declarations, such as `acutest_check_`, `acutest_message_` and `acutest_abort_`, against it. A failure raised inside the support library is reported against `test_support.c` and fails the test that reached it.

A parent that enables `sosig` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts discovery at the build root. A child call cannot create the root test file. CMake prints this requirement when a child enables tests.

Each unit test links the same `sosig_app` as the executable. No test target compiles an application source a second time with different definitions. The production render limit is 256 MiB, and a failure test at this limit uses too much memory. The template test passes a 64 KiB limit to `template_render_file_limited` instead.

Each golden test uses a separate scratch tree. It builds the site twice and verifies that the second build leaves every generated file unchanged. The CMake script then compares the output inventory with `tests/expected/<name>` and compares every file byte for byte.

The CMake script collects expected and actual files after `sosig` runs. These runtime globs are not build inputs. Fixture site changes do not require CMake to configure the project again.

## Version

CMake stores the numeric release version once in `project(VERSION)`. It generates one source file that defines `sosig_version_string()` and `sosig_generator_string()`. A version change rebuilds this source file and relinks its targets.

Release versions use `MAJOR.MINOR.PATCH`. The `project(VERSION)` command accepts only numeric components. A prerelease suffix requires a second version value.

## Install

The install contains the executable and its documentation. Both use the `sosig_runtime` component.

The project has no export set or development component because it does not install a library or header. `GNUInstallDirs` selects the install paths.

## Packaging

### Release packages

CPack creates packages for the different targets `sosig-<version>-<target>.tar.gz`.

The macOS deployment target is `13.0` by default. A builder can select a newer target through the cache.

Linux `musl` builds use the two Zig toolchain files. These files select `llvm-strip` because a host strip tool cannot read the cross-built ELF files.

### Source package

CPack can also create a source package `sosig-<version>-source.tar.gz`. The ignore list keeps CPack's repository and temporary-file exclusions. It also omits build products.
