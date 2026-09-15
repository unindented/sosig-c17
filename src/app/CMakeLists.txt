target_sources(
  sosig_app
  PRIVATE cli.c cli_dispatch.c cmd_build.c cmd_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli.h cli_dispatch.h cmd_build.h cmd_config.h exit_code.h sosig_version.h
)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS cli cli_dispatch cmd_build cmd_config exit_code)
    sosig_add_unit_test(${module})
  endforeach()
endif()
