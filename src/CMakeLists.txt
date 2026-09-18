# The private application library contains project code except `main()`. Unit tests link to the same
# implementation.

configure_file(
  app/sosig_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/app/sosig_version.c" @ONLY
)

add_library(sosig_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/app/sosig_version.c")

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
          sosig_vendor_md4c
          sosig_vendor_mustache
          Threads::Threads
)

add_executable(sosig app/main.c)
target_link_libraries(sosig PRIVATE sosig_build_project sosig_app)

if(PROJECT_IS_TOP_LEVEL)
  sosig_enable_project_analysis(sosig_app)
  sosig_enable_project_analysis(sosig)
endif()
