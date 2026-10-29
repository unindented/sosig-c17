# This file isolates third-party sources from first-party warnings and analyzers.

add_library(sosig_vendor_acutest INTERFACE)
target_include_directories(
  sosig_vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/acutest>"
)

add_library(sosig_vendor_copt INTERFACE)
target_include_directories(
  sosig_vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/copt>"
)

# md4c, mustache4c and tomlc17 shape every page, so a change to the pinned version of one of them
# bumps `PAGE_RECIPE_VERSION` in `src/domain/build_cache.h`, which makes every page again.
add_library(sosig_vendor_md4c STATIC md4c/md4c.c md4c/md4c-html.c md4c/entity.c)
target_include_directories(
  sosig_vendor_md4c SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/md4c>"
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

foreach(target IN ITEMS sosig_vendor_md4c sosig_vendor_mustache sosig_vendor_tomlc17)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE sosig_build_vendor)
endforeach()
