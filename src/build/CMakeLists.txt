target_sources(
  sosig_app
  PRIVATE entry_renderer.c job.c manifest_builder.c output_path.c page_renderer.c site_writer.c
          template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES entry_renderer.h job.h manifest_builder.h output_path.h page_renderer.h site_writer.h
               template.h
)
target_link_libraries(sosig_app PRIVATE sosig_vendor_mustache)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS entry_renderer job manifest_builder output_path page_renderer site_writer
                          template
  )
    sosig_add_unit_test(${module})
  endforeach()
  # The entry-renderer tests drive both parallel phases through these helpers.
  target_sources(sosig_unit_entry_renderer PRIVATE test_render_support.c)
endif()
