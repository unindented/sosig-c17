target_sources(
  sosig_app
  PRIVATE arena.c error.c grow.c parse.c path.c path_list.c string_buffer.c text.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES arena.h ascii.h error.h grow.h parse.h path.h path_list.h string_buffer.h text.h
)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS arena ascii error grow parse path path_list string_buffer text)
    sosig_add_unit_test(${module})
  endforeach()
endif()
