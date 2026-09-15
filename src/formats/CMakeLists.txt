target_sources(
  sosig_app
  PRIVATE html.c markdown.c toml.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES html.h markdown.h toml.h
)

if(SOSIG_BUILD_TESTING)
  sosig_add_unit_test(html LIBRARIES sosig_app)
  sosig_add_unit_test(markdown LIBRARIES sosig_app)
  sosig_add_unit_test(toml LIBRARIES sosig_app sosig_vendor_tomlc17)
endif()
