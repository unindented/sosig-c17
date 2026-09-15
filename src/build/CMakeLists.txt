target_sources(
  sosig_app
  PRIVATE entry_renderer.c manifest_builder.c page_renderer.c render_job.c site_writer.c template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES entry_renderer.h manifest_builder.h page_renderer.h render_job.h site_writer.h template.h
)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS entry_renderer manifest_builder page_renderer)
    sosig_add_unit_test(${module})
  endforeach()

  # The production render limit is too large for a low-cost failure test. This private target builds
  # `template.c` with a smaller limit. It gets all other symbols from the application library.
  set(sosig_template_test_output_len_max 65536)
  add_library(sosig_template_test_variant STATIC template.c)
  target_compile_definitions(
    sosig_template_test_variant
    PRIVATE "SOSIG_RENDER_OUTPUT_LEN_MAX=${sosig_template_test_output_len_max}"
  )
  target_link_libraries(
    sosig_template_test_variant
    PRIVATE sosig_build_project sosig_vendor_mustache sosig_app
  )
  if(PROJECT_IS_TOP_LEVEL)
    sosig_enable_project_analysis(sosig_template_test_variant)
  endif()

  sosig_add_unit_test(template EXTRA_LIBRARIES sosig_template_test_variant)
  target_compile_definitions(
    sosig_unit_template PRIVATE "SOSIG_RENDER_OUTPUT_LEN_MAX=${sosig_template_test_output_len_max}"
  )
endif()
