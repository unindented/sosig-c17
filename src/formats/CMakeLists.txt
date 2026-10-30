target_sources(
  sosig_app
  PRIVATE html.c markdown.c org.c toml.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES html.h markdown.h org.h toml.h
)

target_link_libraries(sosig_app PRIVATE sosig_vendor_md4c sosig_vendor_org4c)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS html markdown org toml)
    sosig_add_unit_test(${module})
  endforeach()
endif()
