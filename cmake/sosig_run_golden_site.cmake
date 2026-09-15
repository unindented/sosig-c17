# This script builds one copied fixture site. It compares the result with the expected output.

foreach(required IN ITEMS SOSIG_EXECUTABLE SOSIG_SITE_DIR SOSIG_EXPECTED_DIR SOSIG_SCRATCH_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${SOSIG_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SOSIG_SCRATCH_DIR}")
file(COPY "${SOSIG_SITE_DIR}/" DESTINATION "${SOSIG_SCRATCH_DIR}")

execute_process(
  COMMAND "${SOSIG_EXECUTABLE}" build ${SOSIG_BUILD_ARGS}
  WORKING_DIRECTORY "${SOSIG_SCRATCH_DIR}"
  RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "fixture site build failed with exit status ${build_result}")
endif()

set(actual_dir "${SOSIG_SCRATCH_DIR}/public")

# Collect both file lists after `sosig` runs. Runtime lists are not build inputs. Fixture site
# changes do not require CMake to run again.
file(GLOB_RECURSE expected_files RELATIVE "${SOSIG_EXPECTED_DIR}" "${SOSIG_EXPECTED_DIR}/*")
file(GLOB_RECURSE actual_files RELATIVE "${actual_dir}" "${actual_dir}/*")
list(SORT expected_files)
list(SORT actual_files)

set(missing_files ${expected_files})
if(actual_files)
  list(REMOVE_ITEM missing_files ${actual_files})
endif()
if(missing_files)
  list(JOIN missing_files "', '" missing_files_display)
  message(FATAL_ERROR "expected output not produced: '${missing_files_display}'")
endif()

set(extra_files ${actual_files})
if(expected_files)
  list(REMOVE_ITEM extra_files ${expected_files})
endif()
if(extra_files)
  list(JOIN extra_files "', '" extra_files_display)
  message(FATAL_ERROR "output not expected: '${extra_files_display}'")
endif()

foreach(file IN LISTS expected_files)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${SOSIG_EXPECTED_DIR}/${file}"
            "${actual_dir}/${file}"
    RESULT_VARIABLE compare_result
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "output differs from expected: '${file}'")
  endif()
endforeach()
