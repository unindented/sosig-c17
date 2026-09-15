target_sources(
  sosig_app
  PRIVATE html.c markdown.c toml.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES html.h markdown.h toml.h
)

if(SOSIG_BUILD_TESTING)
  sosig_add_unit_test(html)
  sosig_add_unit_test(markdown)
  sosig_add_unit_test(toml)
endif()
