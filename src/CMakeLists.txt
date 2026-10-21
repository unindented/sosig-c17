# The private application library contains project code except `app/main.c` and the POSIX host in
# `runtime/fs.c`. The `sosig` executable is built from `app/main.c` and links the POSIX host. Each
# unit test links the same library, the host it is registered with, and the unit-test `os_oom` from
# `tests/`.

configure_file(
  core/sosig_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/core/sosig_version.c" @ONLY
)

add_library(sosig_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/core/sosig_version.c")

add_subdirectory(app)
add_subdirectory(build)
add_subdirectory(core)
add_subdirectory(domain)
add_subdirectory(formats)
add_subdirectory(runtime)

target_link_libraries(
  sosig_app
  PUBLIC sosig_vendor_tomlc17
         sosig_vendor_sharedstuff
  PRIVATE sosig_build_project
          sosig_vendor_copt
          Threads::Threads
)

add_executable(sosig app/main.c)
target_link_libraries(sosig PRIVATE sosig_build_project sosig_app sosig_os_posix)

if(PROJECT_IS_TOP_LEVEL)
  sosig_enable_project_analysis(sosig_app)
  sosig_enable_project_analysis(sosig)
endif()
