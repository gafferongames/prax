#include "pr/color_css.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

bool parsesTo(PrStringView text, PrColor8 expected) {
    PrColor8 color{};
    return prColor8FromCss(text, &color) && color.r == expected.r && color.g == expected.g && color.b == expected.b &&
        color.a == expected.a;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("color css: hex colors of three, four, six and eight digits") {
    CHECK(parsesTo("#f9f", PrColor8{255, 153, 255, 255}));
    CHECK(parsesTo("#f9f8", PrColor8{255, 153, 255, 136}));
    CHECK(parsesTo("#ECECFF", PrColor8{236, 236, 255, 255}));
    CHECK(parsesTo(" #9370db80 ", PrColor8{147, 112, 219, 128}));
    PrColor8 color{};
    CHECK_FALSE(prColor8FromCss("#12345", &color));
    CHECK_FALSE(prColor8FromCss("#ggg", &color));
}

TEST_CASE("color css: rgb() and hsl() in their legacy and modern syntaxes") {
    CHECK(parsesTo("rgb(255, 0, 128)", PrColor8{255, 0, 128, 255}));
    CHECK(parsesTo("rgba(232,232,232, 0.8)", PrColor8{232, 232, 232, 204}));
    CHECK(parsesTo("rgb(100% 50% 0% / 50%)", PrColor8{255, 128, 0, 128}));
    CHECK(parsesTo("hsl(120, 100%, 50%)", PrColor8{0, 255, 0, 255}));
    CHECK(parsesTo("hsla(240deg 100% 50% / 0.5)", PrColor8{0, 0, 255, 128}));
    PrColor8 color{};
    CHECK_FALSE(prColor8FromCss("rgb(1, 2 3)", &color));
    CHECK_FALSE(prColor8FromCss("rgb(1, 2, 3", &color));
    CHECK_FALSE(prColor8FromCss("lab(50 0 0)", &color));
}

TEST_CASE("color css: named colors in any case, and transparent") {
    CHECK(parsesTo("aliceblue", PrColor8{240, 248, 255, 255}));
    CHECK(parsesTo("LightGreen", PrColor8{144, 238, 144, 255}));
    CHECK(parsesTo("yellowgreen", PrColor8{154, 205, 50, 255}));
    CHECK(parsesTo("transparent", PrColor8{0, 0, 0, 0}));
    PrColor8 color{};
    CHECK_FALSE(prColor8FromCss("currentcolor", &color));
    CHECK_FALSE(prColor8FromCss("notacolor", &color));
    CHECK_FALSE(prColor8FromCss("", &color));
}

TEST_CASE("color css: a color keeps the precision that its text gives it, and its 8-bit form rounds") {
    PrColor color{};
    REQUIRE(prColorFromCss("hsl(240, 100%, 96.2745%)", &color));
    CHECK(color.r == doctest::Approx(0.925490f));
    CHECK(color.b == doctest::Approx(1.0f));
    REQUIRE(prColorFromCss("rgb(127.5, 0, 300)", &color));
    CHECK(color.r == doctest::Approx(0.5f));
    CHECK(color.b == 1.0f);
    REQUIRE(prColorFromCss("#ff000080", &color));
    CHECK(color.a == doctest::Approx(128.0f / 255));
    CHECK(parsesTo("hsl(240, 100%, 96.2745%)", PrColor8{236, 236, 255, 255}));
    CHECK_FALSE(prColorFromCss("hsl(1, 2%)", &color));
}
