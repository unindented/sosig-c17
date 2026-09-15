target_sources(
  sosig_app
  PRIVATE fs.c pool.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES fs.h pool.h
)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS fs pool)
    sosig_add_unit_test(${module})
  endforeach()
endif()
