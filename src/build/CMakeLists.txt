target_sources(
  sosig_app
  PRIVATE entry_renderer.c job.c manifest_builder.c output_path.c output_pruner.c output_writer.c
          page_renderer.c site_writer.c template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES entry_renderer.h job.h manifest_builder.h output_path.h output_pruner.h output_writer.h
               page_renderer.h site_writer.h template.h
)
target_link_libraries(sosig_app PRIVATE sosig_vendor_mustache)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS entry_renderer output_path template)
    sosig_add_unit_test(${module})
  endforeach()
  # A case-insensitive filesystem is a fact of the real host, so the prune's case is on the POSIX
  # one.
  sosig_add_unit_test(output_pruner_case)
  # These read and write only through `runtime/os.h`, so they run against the in-memory host.
  foreach(module IN ITEMS job manifest_builder output_pruner output_writer page_renderer site_writer)
    sosig_add_unit_test(${module} OS memory)
  endforeach()
endif()
