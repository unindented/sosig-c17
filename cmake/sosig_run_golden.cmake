# This script builds one copied fixture site twice. It checks that the second build leaves the
# generated output unchanged, then compares the result with the expected output. The release
# workflow runs it against a packaged binary, with `SOSIG_EMULATOR` naming a launcher such as
# `qemu-aarch64` when the binary targets another architecture.

foreach(required IN ITEMS SOSIG_EXECUTABLE SOSIG_SITE_DIR SOSIG_EXPECTED_DIR SOSIG_SCRATCH_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${SOSIG_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SOSIG_SCRATCH_DIR}")
file(COPY "${SOSIG_SITE_DIR}/" DESTINATION "${SOSIG_SCRATCH_DIR}")

set(actual_dir "${SOSIG_SCRATCH_DIR}/public")
set(first_dir "${SOSIG_SCRATCH_DIR}/first/public")

execute_process(
  COMMAND ${SOSIG_EMULATOR} "${SOSIG_EXECUTABLE}" build --workers 2 ${SOSIG_BUILD_ARGS}
  WORKING_DIRECTORY "${SOSIG_SCRATCH_DIR}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(
    FATAL_ERROR "first fixture site build failed with exit status ${build_result}:\n"
                "${build_stdout}${build_stderr}"
  )
endif()
file(COPY "${actual_dir}" DESTINATION "${SOSIG_SCRATCH_DIR}/first")

execute_process(
  COMMAND ${SOSIG_EMULATOR} "${SOSIG_EXECUTABLE}" build --workers 2 ${SOSIG_BUILD_ARGS}
  WORKING_DIRECTORY "${SOSIG_SCRATCH_DIR}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(
    FATAL_ERROR "second fixture site build failed with exit status ${build_result}:\n"
                "${build_stdout}${build_stderr}"
  )
endif()

file(GLOB_RECURSE first_files RELATIVE "${first_dir}" "${first_dir}/*")
foreach(file IN LISTS first_files)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${first_dir}/${file}" "${actual_dir}/${file}"
    RESULT_VARIABLE compare_result
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "second build changed generated output: '${file}'")
  endif()
endforeach()

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
