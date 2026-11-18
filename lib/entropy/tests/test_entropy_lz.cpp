#include "pr/entropy_lz.h"

#include "tools/harness/test.h"

extern "C" int prTestEntropyCApi(void);

TEST_CASE("a match copies from its distance back, and repeats a pattern nearer than its length") {
    // Every distance from one to past the word, each over a run longer than the word.
    for (uint32_t distance = 1; distance <= 12; ++distance) {
        CAPTURE(distance);
        uint8_t buffer[64 + kPrEntropyLzMatchSlack] = {};
        for (uint32_t i = 0; i < distance; ++i)
            buffer[i] = uint8_t(i + 1);
        prEntropyLzCopyMatch(buffer + distance, distance, 40);
        for (uint32_t i = 0; i < distance + 40; ++i)
            CHECK(buffer[i] == uint8_t(i % distance + 1));
    }
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestEntropyCApi() == 0);
}
