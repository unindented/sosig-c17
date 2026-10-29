target_sources(
  sosig_app
  PRIVATE build_cache.c content_entry.c frontmatter.c manifest.c permalink.c site_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES build_cache.h content_entry.h frontmatter.h manifest.h permalink.h site_config.h
)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS build_cache content_entry frontmatter manifest permalink site_config)
    sosig_add_unit_test(${module})
  endforeach()
endif()
