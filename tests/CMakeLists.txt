# The golden test suite uses several modules. Keep it in the top-level `tests/` directory.

set(sosig_fixture_sites site-file-permalink site-index-permalink)

function(sosig_add_golden_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "SITE;EXPECTED" "BUILD_ARGS")
  add_test(
    NAME "${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DSOSIG_EXECUTABLE=$<TARGET_FILE:sosig>" "-DSOSIG_SITE_DIR=${arg_SITE}"
      "-DSOSIG_EXPECTED_DIR=${arg_EXPECTED}"
      "-DSOSIG_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/${name}"
      "-DSOSIG_BUILD_ARGS=${arg_BUILD_ARGS}" -P
      "${PROJECT_SOURCE_DIR}/cmake/sosig_run_golden_site.cmake"
  )
  set_tests_properties(
    "${name}"
    PROPERTIES LABELS "sosig;golden"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
               ${arg_UNPARSED_ARGUMENTS}
  )
endfunction()

foreach(site IN LISTS sosig_fixture_sites)
  set(site_dir "${CMAKE_CURRENT_LIST_DIR}/fixtures/${site}")
  if(NOT IS_DIRECTORY "${site_dir}")
    message(FATAL_ERROR "fixture site '${site}' is listed but '${site_dir}' does not exist")
  endif()

  sosig_add_golden_test(
    "sosig.golden.${site}"
    SITE "${site_dir}"
    EXPECTED "${CMAKE_CURRENT_LIST_DIR}/expected/${site}"
  )
  sosig_add_golden_test(
    "sosig.golden.${site}.verbose"
    SITE "${site_dir}"
    EXPECTED "${CMAKE_CURRENT_LIST_DIR}/expected/${site}"
    BUILD_ARGS -v
  )
endforeach()
