# This file isolates third-party sources from first-party warnings and analyzers.

add_library(sosig_vendor_acutest INTERFACE)
target_include_directories(
  sosig_vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/acutest>"
)

add_library(sosig_vendor_copt INTERFACE)
target_include_directories(
  sosig_vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/copt>"
)

# md4c, org4c, mustache4c, tomlc17, and tree-sitter and its grammars shape every page, so a change
# to the pinned version of one of them bumps `PAGE_RECIPE_VERSION` in `src/domain/build_cache.h`,
# which makes every page again.
add_library(sosig_vendor_md4c STATIC md4c/md4c.c)
target_include_directories(
  sosig_vendor_md4c SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/md4c>"
)

add_library(sosig_vendor_org4c STATIC org4c/org4c.c org4c/entity.c)
target_include_directories(
  sosig_vendor_org4c SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/org4c>"
)

add_library(sosig_vendor_mustache STATIC mustache4c/mustache.c)
target_include_directories(
  sosig_vendor_mustache SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/mustache4c>"
)

add_library(
  sosig_vendor_sharedstuff STATIC sharedstuff/src/arena.c sharedstuff/src/buf.c
                                  sharedstuff/src/log.c sharedstuff/src/str.c
)
target_include_directories(
  sosig_vendor_sharedstuff SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sharedstuff/include>"
)
target_link_libraries(sosig_vendor_sharedstuff PRIVATE sosig_build_project)

add_library(sosig_vendor_tomlc17 STATIC tomlc17/tomlc17.c)
target_include_directories(
  sosig_vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/tomlc17>"
)

foreach(target IN ITEMS sosig_vendor_md4c sosig_vendor_org4c sosig_vendor_mustache
                        sosig_vendor_tomlc17
)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE sosig_build_vendor)
endforeach()

# The tree-sitter runtime is one unity source, compiled with the definitions upstream's CMake gives
# it.
add_library(sosig_vendor_tree_sitter STATIC tree-sitter/src/lib.c)
target_include_directories(
  sosig_vendor_tree_sitter SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/tree-sitter/include>"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tree-sitter/src"
)
target_compile_definitions(
  sosig_vendor_tree_sitter PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE
                                   _DARWIN_C_SOURCE
)
target_compile_options(
  sosig_vendor_tree_sitter PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>
)
target_link_libraries(sosig_vendor_tree_sitter PRIVATE sosig_build_vendor)

# Every grammar's scanner defines its `create` function without a prototype, and the runtime calls
# it through `void *(*)(void)`, which Clang's `-fsanitize=function`, part of `-fsanitize=undefined`
# on Linux, reports as a call through the wrong type. The check is turned off where the call is
# made. It is a source option, which comes after the sanitizer options `sosig_build_vendor` gives the
# target, because a later `-fsanitize` would turn the check back on.
if(SOSIG_SANITIZER STREQUAL "address")
  check_c_compiler_flag(-fno-sanitize=function sosig_have_no_sanitize_function)
  if(sosig_have_no_sanitize_function)
    set_source_files_properties(
      tree-sitter/src/lib.c TARGET_DIRECTORY sosig_vendor_tree_sitter
      PROPERTIES COMPILE_OPTIONS -fno-sanitize=function
    )
  endif()
endif()

# Adds a static library for one tree-sitter grammar from `SOURCES`, with the highlight query that
# `QUERIES` make, concatenated in order. The query is embedded as the byte array
# `tree_sitter_<name>_highlights`, `tree_sitter_<name>_highlights_len` bytes long, in a source
# generated at configure time. Reading each file as hexadecimal keeps every byte as it is, so
# neither CMake nor the C compiler expands anything in a query.
function(sosig_add_tree_sitter_grammar name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;QUERIES")
  set(bytes "")
  foreach(query IN LISTS arg_QUERIES)
    set(path "${CMAKE_CURRENT_LIST_DIR}/${query}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," query_bytes "${hex}")
    string(APPEND bytes "${query_bytes}")
  endforeach()
  set(query_source "${CMAKE_CURRENT_BINARY_DIR}/tree_sitter_${name}_highlights.c")
  file(
    CONFIGURE
    OUTPUT "${query_source}"
    CONTENT
      "#include <stdint.h>\n\nconst char tree_sitter_${name}_highlights[] = {${bytes}};\nconst uint32_t tree_sitter_${name}_highlights_len = (uint32_t)sizeof(tree_sitter_${name}_highlights);\n"
    @ONLY
  )

  set(target "sosig_vendor_tree_sitter_${name}")
  add_library(${target} STATIC ${arg_SOURCES} "${query_source}")
  list(GET arg_SOURCES 0 parser)
  get_filename_component(source_dir "${CMAKE_CURRENT_LIST_DIR}/${parser}" DIRECTORY)
  target_include_directories(${target} PRIVATE "${source_dir}")
  # A scanner allocates through the runtime's allocator, which sosig replaces, rather than through
  # `malloc`, and classifies characters the same way on every host, through sosig's
  # `tree_sitter_wctype.h`, which explains why.
  target_compile_definitions(${target} PRIVATE TREE_SITTER_REUSE_ALLOCATOR)
  target_compile_options(
    ${target} PRIVATE -include "${PROJECT_SOURCE_DIR}/src/formats/tree_sitter_wctype.h"
  )
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE sosig_build_vendor sosig_vendor_tree_sitter)
endfunction()

# Each query list is in last-wins order, the precedence tree-sitter's highlighter has used since
# 2024: on one node, the later pattern wins. C, bash and JavaScript take their queries in the order
# their `tree-sitter.json` lists. TypeScript's `tree-sitter.json` still lists its own query before
# JavaScript's, for the first-wins precedence it was written for, so TypeScript and TSX take the
# reverse, which lets TypeScript's patterns win over JavaScript's as upstream meant.
# `highlights-params.scm` does not compile against either TypeScript grammar, so TypeScript and TSX
# end with sosig's `src/formats/highlight_typescript_params.scm`, which captures the same parameters
# in their grammars.
sosig_add_tree_sitter_grammar(
  c SOURCES tree-sitter-c/src/parser.c QUERIES tree-sitter-c/queries/highlights.scm
)
sosig_add_tree_sitter_grammar(
  bash
  SOURCES tree-sitter-bash/src/parser.c tree-sitter-bash/src/scanner.c
  QUERIES tree-sitter-bash/queries/highlights.scm
)
sosig_add_tree_sitter_grammar(
  javascript
  SOURCES tree-sitter-javascript/src/parser.c tree-sitter-javascript/src/scanner.c
  QUERIES tree-sitter-javascript/queries/highlights.scm
          tree-sitter-javascript/queries/highlights-jsx.scm
          tree-sitter-javascript/queries/highlights-params.scm
)
sosig_add_tree_sitter_grammar(
  typescript
  SOURCES tree-sitter-typescript/typescript/src/parser.c
          tree-sitter-typescript/typescript/src/scanner.c
  QUERIES tree-sitter-javascript/queries/highlights.scm tree-sitter-typescript/queries/highlights.scm
          ../src/formats/highlight_typescript_params.scm
)
sosig_add_tree_sitter_grammar(
  tsx
  SOURCES tree-sitter-typescript/tsx/src/parser.c tree-sitter-typescript/tsx/src/scanner.c
  QUERIES tree-sitter-javascript/queries/highlights.scm
          tree-sitter-javascript/queries/highlights-jsx.scm
          tree-sitter-typescript/queries/highlights.scm
          ../src/formats/highlight_typescript_params.scm
)
