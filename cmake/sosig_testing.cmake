# This helper registers unit tests next to their source modules.

include_guard(GLOBAL)

function(sosig_add_unit_test module)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "LIBRARIES")
  set(target "sosig_unit_${module}")
  add_executable(${target} "test_${module}.c")
  target_link_libraries(
    ${target} PRIVATE sosig_build_tests sosig_vendor_acutest ${arg_LIBRARIES}
  )

  add_test(NAME "sosig.${module}" COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(
    "sosig.${module}"
    PROPERTIES LABELS "sosig;unit"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )

  if(PROJECT_IS_TOP_LEVEL)
    sosig_enable_test_analysis(${target})
  endif()
endfunction()
