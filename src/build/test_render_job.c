#include <acutest.h>
#include <string.h>

#include "build/render_job.h"

// A slot's first diagnostic is kept and a later one for the same slot is dropped.
static void test_set_error_keeps_the_first_diagnostic(void) {
  struct RenderJob result = {0};
  render_job_set_error(&result, "first %d", 1);
  render_job_set_error(&result, "second %d", 2);
  TEST_CHECK(strcmp(result.error_message, "first 1") == 0);
  TEST_MSG("actual: '%s'", result.error_message);
}

TEST_LIST = {
    {"set error keeps the first diagnostic", test_set_error_keeps_the_first_diagnostic},
    {NULL, NULL},
};
