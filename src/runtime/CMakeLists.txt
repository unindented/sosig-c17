target_sources(
  sosig_app
  PRIVATE memory.c pool.c walk.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES memory.h os.h pool.h walk.h
)

# The POSIX implementation of `os.h` stays out of `sosig_app`, so each executable chooses its host
# when it is linked: the `sosig` executable and every unit test registered without `OS memory` link
# this one, and the tests registered with it link the in-memory one from `tests/`. It is an object
# library because `sosig_app` refers to it, and a static archive listed before its user would be
# skipped by a single-pass linker.
add_library(sosig_os_posix OBJECT fs.c)
target_sources(
  sosig_os_posix PUBLIC FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/src" FILES fs.h
)
target_link_libraries(sosig_os_posix PRIVATE sosig_build_project sosig_app)
if(PROJECT_IS_TOP_LEVEL)
  sosig_enable_project_analysis(sosig_os_posix)
endif()

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS fs pool)
    sosig_add_unit_test(${module})
  endforeach()
  sosig_add_unit_test(memory OS memory)
  sosig_add_unit_test(walk OS memory)
  # The contract tests of `os.h`: one source, run against each host.
  sosig_add_unit_test(os NAME os_posix)
  sosig_add_unit_test(os NAME os_memory OS memory)
endif()
