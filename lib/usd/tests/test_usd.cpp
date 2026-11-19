#include "usd/tests/usd_fixture.h"

extern "C" int prTestUsdCApi(void);

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestUsdCApi() == 0);
}
