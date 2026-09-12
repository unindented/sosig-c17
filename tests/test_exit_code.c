#include <acutest.h>

#include "app/exit_code.h"

// The exit codes hold the numbers they document. These are interface, not bookkeeping: a shell or
// CI wrapper invoking `sosig` branches on them, and `2` for a usage error is the convention every
// option parser sets. `exit_code.h` records that nothing in the tree pinned the individual values,
// only the `0`-`125` range bounded by its own `_Static_assert`. This is the assertion that fails if
// one of those values changes. Written as literals rather than against the enumeration names
// deliberately: a comparison to `EXIT_CODE_USAGE` moves with an edit to it and pins nothing.
static void test_exit_codes_have_documented_values(void) {
  TEST_CHECK(EXIT_CODE_OK == 0);
  TEST_CHECK(EXIT_CODE_FAILURE == 1);
  TEST_CHECK(EXIT_CODE_USAGE == 2);
}

TEST_LIST = {{"exit codes have documented values", test_exit_codes_have_documented_values},
             {NULL, NULL}};
