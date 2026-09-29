target_sources(
  sosig_app
  PRIVATE entry_renderer.c manifest_builder.c page_renderer.c render_job.c site_writer.c template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES entry_renderer.h manifest_builder.h page_renderer.h render_job.h site_writer.h template.h
)
target_link_libraries(sosig_app PRIVATE sosig_vendor_mustache)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS entry_renderer manifest_builder page_renderer template)
    sosig_add_unit_test(${module})
  endforeach()
  # The entry- and page-renderer tests share the helpers that drive both render passes.
  target_sources(sosig_unit_entry_renderer PRIVATE test_render_support.c)
  target_sources(sosig_unit_page_renderer PRIVATE test_render_support.c)
endif()
