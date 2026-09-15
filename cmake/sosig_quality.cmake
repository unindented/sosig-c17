# This file configures formatting and static analysis for first-party sources.

include_guard(GLOBAL)

set(sosig_owned_sources
    "${PROJECT_SOURCE_DIR}/src/app/cli.c"
    "${PROJECT_SOURCE_DIR}/src/app/cli.h"
    "${PROJECT_SOURCE_DIR}/src/app/cli_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/app/cli_dispatch.h"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_build.c"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_build.h"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_config.c"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_config.h"
    "${PROJECT_SOURCE_DIR}/src/app/exit_code.h"
    "${PROJECT_SOURCE_DIR}/src/app/main.c"
    "${PROJECT_SOURCE_DIR}/src/app/sosig_version.h"
    "${PROJECT_SOURCE_DIR}/src/app/test_cli.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cli_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cmd_build.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cmd_config.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_exit_code.c"
    "${PROJECT_SOURCE_DIR}/src/build/entry_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/entry_renderer.h"
    "${PROJECT_SOURCE_DIR}/src/build/manifest_builder.c"
    "${PROJECT_SOURCE_DIR}/src/build/manifest_builder.h"
    "${PROJECT_SOURCE_DIR}/src/build/page_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/page_renderer.h"
    "${PROJECT_SOURCE_DIR}/src/build/render_job.c"
    "${PROJECT_SOURCE_DIR}/src/build/render_job.h"
    "${PROJECT_SOURCE_DIR}/src/build/site_writer.c"
    "${PROJECT_SOURCE_DIR}/src/build/site_writer.h"
    "${PROJECT_SOURCE_DIR}/src/build/template.c"
    "${PROJECT_SOURCE_DIR}/src/build/template.h"
    "${PROJECT_SOURCE_DIR}/src/build/test_entry_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_manifest_builder.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_page_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_template.c"
    "${PROJECT_SOURCE_DIR}/src/core/arena.c"
    "${PROJECT_SOURCE_DIR}/src/core/arena.h"
    "${PROJECT_SOURCE_DIR}/src/core/ascii.h"
    "${PROJECT_SOURCE_DIR}/src/core/error.c"
    "${PROJECT_SOURCE_DIR}/src/core/error.h"
    "${PROJECT_SOURCE_DIR}/src/core/grow.c"
    "${PROJECT_SOURCE_DIR}/src/core/grow.h"
    "${PROJECT_SOURCE_DIR}/src/core/parse.c"
    "${PROJECT_SOURCE_DIR}/src/core/parse.h"
    "${PROJECT_SOURCE_DIR}/src/core/path.c"
    "${PROJECT_SOURCE_DIR}/src/core/path.h"
    "${PROJECT_SOURCE_DIR}/src/core/path_list.c"
    "${PROJECT_SOURCE_DIR}/src/core/path_list.h"
    "${PROJECT_SOURCE_DIR}/src/core/string_buffer.c"
    "${PROJECT_SOURCE_DIR}/src/core/string_buffer.h"
    "${PROJECT_SOURCE_DIR}/src/core/text.c"
    "${PROJECT_SOURCE_DIR}/src/core/text.h"
    "${PROJECT_SOURCE_DIR}/src/core/test_arena.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_ascii.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_error.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_grow.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_parse.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_path.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_path_list.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_string_buffer.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_text.c"
    "${PROJECT_SOURCE_DIR}/src/domain/content_entry.c"
    "${PROJECT_SOURCE_DIR}/src/domain/content_entry.h"
    "${PROJECT_SOURCE_DIR}/src/domain/frontmatter.c"
    "${PROJECT_SOURCE_DIR}/src/domain/frontmatter.h"
    "${PROJECT_SOURCE_DIR}/src/domain/manifest.c"
    "${PROJECT_SOURCE_DIR}/src/domain/manifest.h"
    "${PROJECT_SOURCE_DIR}/src/domain/permalink.c"
    "${PROJECT_SOURCE_DIR}/src/domain/permalink.h"
    "${PROJECT_SOURCE_DIR}/src/domain/site_config.c"
    "${PROJECT_SOURCE_DIR}/src/domain/site_config.h"
    "${PROJECT_SOURCE_DIR}/src/domain/test_content_entry.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_frontmatter.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_manifest.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_permalink.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_site_config.c"
    "${PROJECT_SOURCE_DIR}/src/formats/html.c"
    "${PROJECT_SOURCE_DIR}/src/formats/html.h"
    "${PROJECT_SOURCE_DIR}/src/formats/markdown.c"
    "${PROJECT_SOURCE_DIR}/src/formats/markdown.h"
    "${PROJECT_SOURCE_DIR}/src/formats/toml.c"
    "${PROJECT_SOURCE_DIR}/src/formats/toml.h"
    "${PROJECT_SOURCE_DIR}/src/formats/test_html.c"
    "${PROJECT_SOURCE_DIR}/src/formats/test_markdown.c"
    "${PROJECT_SOURCE_DIR}/src/formats/test_toml.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/pool.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/pool.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_pool.c"
    "${PROJECT_SOURCE_DIR}/tests/test_support.c"
    "${PROJECT_SOURCE_DIR}/tests/test_support.h"
)
set(sosig_configured_c_source "${PROJECT_SOURCE_DIR}/src/app/sosig_version.c.in")

find_program(SOSIG_CLANG_FORMAT NAMES clang-format-22 clang-format)
find_program(SOSIG_CLANG_TIDY NAMES clang-tidy-22 clang-tidy)
find_program(SOSIG_CPPCHECK NAMES cppcheck)

set(sosig_clang_tidy_command "${SOSIG_CLANG_TIDY}")
set(sosig_cppcheck_command "${SOSIG_CPPCHECK}")
set(sosig_cppcheck_standard "c17")
if(NOT CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME)
  message(VERBOSE "cross compiling; clang-tidy and cppcheck are disabled")
  set(sosig_clang_tidy_command "")
  set(sosig_cppcheck_command "")
endif()

function(sosig_enable_project_analysis target)
  if(sosig_clang_tidy_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CLANG_TIDY
               "${sosig_clang_tidy_command};--quiet;--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
  endif()
  if(sosig_cppcheck_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CPPCHECK
               "${sosig_cppcheck_command};--enable=warning,performance,portability;--std=${sosig_cppcheck_standard};--error-exitcode=1;--quiet"
    )
  endif()
endfunction()

function(sosig_enable_test_analysis target)
  if(sosig_cppcheck_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CPPCHECK
               "${sosig_cppcheck_command};--enable=warning,performance,portability;--std=${sosig_cppcheck_standard};--error-exitcode=1;--quiet"
    )
  endif()
endfunction()

if(SOSIG_CLANG_FORMAT)
  add_custom_target(
    sosig_format
    COMMAND "${SOSIG_CLANG_FORMAT}" -i ${sosig_owned_sources}
    COMMAND
      "${SOSIG_CLANG_FORMAT}" -i --assume-filename=sosig_version.c
      "${sosig_configured_c_source}"
    COMMENT "Formatting first-party sources"
    COMMAND_EXPAND_LISTS VERBATIM
  )
  add_custom_target(
    sosig_lint
    COMMAND "${SOSIG_CLANG_FORMAT}" --dry-run --Werror ${sosig_owned_sources}
    COMMAND
      "${SOSIG_CLANG_FORMAT}" --dry-run --Werror --assume-filename=sosig_version.c
      "${sosig_configured_c_source}"
    COMMENT "Checking first-party source formatting"
    COMMAND_EXPAND_LISTS VERBATIM
  )
else()
  add_custom_target(sosig_format COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found; skipping")
  add_custom_target(sosig_lint COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found; skipping")
endif()
