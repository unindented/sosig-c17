# Build system

`sosig` requires CMake 3.25 or later. This document explains the build targets and their relationships.

## Layout

The top-level CMake file controls the build. It sets project policy and finds the thread dependency. It also loads build settings and adds each source directory.

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Build the private application library and the `sosig` executable |
| `src/*/CMakeLists.txt` | Add sources, header file sets, and local unit tests |
| `tests/CMakeLists.txt` | Add the golden test suite and the shared unit-test support library |
| `vendor/CMakeLists.txt` | Build each vendored dependency |
| `cmake/sosig_build_profiles.cmake` | Set warnings, optimization, fortification, and sanitizers |
| `cmake/sosig_quality.cmake` | Configure formatting and static analysis |
| `cmake/sosig_testing.cmake` | Add local unit tests |
| `cmake/sosig_packaging.cmake` | Configure installation and CPack archives |
| `cmake/sosig_run_golden_site.cmake` | Run one golden test |
| `cmake/sosig_restyle_graphviz_svg.cmake` | Add light and dark styles to the target graph |
| `cmake/toolchains/` | Configure Zig for Linux `musl` targets |
| `CMakeGraphVizOptions.cmake` | Set filters and layout for the target graph |

## Target graph

`sosig_app` is a private static library. It contains all application source files except `main.c`. The `sosig` executable and unit tests link to it. `sosig_app` links to `sosig_vendor_sharedstuff` for the arena and string buffer implementations.

The library is static because it supports one product. It does not provide a public ABI.

Each vendored project has a separate target. Header-only dependencies use interface libraries.

`sosig_app` links to `sosig_vendor_tomlc17` as a public dependency because `formats/toml.h` exposes a tomlc17 type. The other vendored dependencies are private.

`Threads::Threads` is a private dependency of `sosig_app`. CMake adds this link requirement to each executable that links to the static library.

Source directories group code by function. They do not define separate libraries. Each local `CMakeLists.txt` adds files to `sosig_app`. More libraries would add link boundaries without independent APIs.

### Generated dependency graph

CMake generates this graph from the `Release` configuration. The graph shows CMake targets and link relationships. It does not show dependencies between source modules. Graph options remove test-only targets.

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

`SOSIG_SANITIZER` accepts `none`, `address`, or `thread`. It adds sanitizer options to standard CMake configurations. It does not create custom build types.

## Linting and formatting

CMake sets `clang-tidy` and `cppcheck` as properties of first-party targets. Neither tool checks vendored code.

Tests run `cppcheck`. Tests do not run `clang-tidy` because deliberate failure cases cause analyzer errors.

Cross builds do not run either host analyzer. Zig supplies target headers that the host analyzers cannot find when they repeat the compile command.

The `sosig_format` and `sosig_lint` targets use an explicit list of first-party files. Add each new source or test to its target and this list.

This duplication is deliberate. A configure-time glob can omit a new file until CMake configures the project again.

## Tests

`SOSIG_BUILD_TESTING` controls test creation. It is true by default for a top-level build. It is false by default for a child build.

Each source directory adds its local unit tests. The top-level `tests/` directory adds the golden test suite and `sosig_test_support`. Each CTest case has a `sosig` label. It also has a `unit` or `golden` label.

`sosig_test_support` holds the unit-test plumbing that several `test_*.c` files need: a fixture-root creator, a recursive fixture-tree remover, a fixture-file writer, and a capture-stream reader. Each was duplicated verbatim per test file before. `test_support.c` defines `TEST_NO_MAIN`, so acutest's `main` and run state stay in each test executable's own translation unit and the link resolves `acutest_check_` and `acutest_abort_` against it. A failure raised inside the support library is reported against `test_support.c` and fails the test that reached it.

A parent that enables `sosig` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts discovery at the build root. A child call cannot create the root test file. CMake prints this requirement when a child enables tests.

The production render limit is 256 MiB. A failure test at this limit uses too much memory. `sosig_template_test_variant` builds `template.c` with a 64 KiB limit. It gets all other symbols from `sosig_app`.

The test source uses the same limit. This arrangement checks the failure path without a change to the production limit. It also avoids a copy of the application library.

Each golden test uses a separate scratch tree. The CMake script collects expected and actual files after `sosig` runs. These runtime globs are not build inputs. Fixture site changes do not require CMake to configure the project again.

## Version

CMake stores the numeric release version once in `project(VERSION)`. It generates one source file that defines `sosig_version_string()`. A version change rebuilds this source file and relinks its targets.

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
