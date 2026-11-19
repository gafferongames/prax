#include "pr/count_of.h"

#include "usd/src/usd_path.h"
#include "usd/tests/usd_fixture.h"

namespace pr_impl {

namespace {

struct Tables {
    UsdPathTable paths = {};
    UsdMapTable maps = {};

    Tables() {
        usdPathTableInit(&paths);
        usdMapTableInit(&maps, &paths);
    }
    ~Tables() {
        usdMapTableReset(&maps);
        usdPathTableReset(&paths);
    }

    UsdPath path(PrStringView text) {
        return usdPathParse(&paths, text);
    }

    String text(UsdPath path) {
        PrTextBuffer buffer = {};
        usdPathText(&paths, path, &buffer);
        String result(prTextBufferView(&buffer));
        prTextBufferReset(&buffer);
        return result;
    }

    UsdMapId map(PrStringView source, PrStringView target, bool identity = false) {
        UsdMapPair pairs[2] = {{path(source), path(target)}, {kUsdPathRoot, kUsdPathRoot}};
        return usdMapMake(&maps, pairs, identity ? 2 : 1, PrUsdLayerOffset{0.0, 1.0});
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a path is one number, and its text is the text that was read") {
    Tables t;
    CHECK(t.path("/") == kUsdPathRoot);
    CHECK(t.path("") == kUsdPathNone);
    CHECK(t.path("A/B") == kUsdPathNone);
    for (PrStringView text : usdEach<PrStringView>("/A", "/A/B", "/A{v=x}", "/A{v=x}B", "/A{v=x}B/C{w=}", "/A.size",
            "/A{v=x}.size", "/A/B.rel[/C].attr", "/A{v=x}{w=y}B")) {
        UsdPath path = t.path(text);
        REQUIRE(path != kUsdPathNone);
        CHECK(t.path(text) == path);
        CHECK(t.text(path) == text);
        CHECK(usdPathLookup(&t.paths, text) == path);
    }
    CHECK(usdPathLookup(&t.paths, "/A/Missing") == kUsdPathNone);
    // Text that the grammar does not make is no path, even where its parts could be read as one.
    for (PrStringView text : usdEach<PrStringView>("//A", "/A//B", "/A/", "/9A", "/A/B-C", "/A/{v=x}", "/A{v=x}/B",
            "/A.", "/A.9size", "/A.size:", "/A{v}", "/A{v=x=y}", "/A{9v=x}", "/A{v=x y}")) {
        CAPTURE(text);
        CHECK(t.path(text) == kUsdPathNone);
        CHECK(usdPathLookup(&t.paths, text) == kUsdPathNone);
    }
    // The spaces that the text format allows about a variant selection's names are not part of the names.
    CHECK(t.path("/A{ v = x }B") == t.path("/A{v=x}B"));
    CHECK(usdPathLookup(&t.paths, "/A{v=missing}") == kUsdPathNone);
}

TEST_CASE("a path knows its depth, its prim and its path without variants") {
    Tables t;
    UsdPath path = t.path("/A{v=x}B/C{w=y}.size");
    CHECK(usdPathNode(&t.paths, path).depth == 6);
    CHECK(usdPathNode(&t.paths, path).primDepth == 3);
    CHECK(usdPathNode(&t.paths, path).stripped == t.path("/A/B/C.size"));
    CHECK(usdPathPrim(&t.paths, path) == t.path("/A{v=x}B/C{w=y}"));
    CHECK(usdPathNode(&t.paths, t.path("/A/B")).stripped == t.path("/A/B"));
    CHECK(usdPathName(&t.paths, t.path("/A/B")) == "B");
}

TEST_CASE("a prefix is replaced part by part") {
    Tables t;
    CHECK(usdPathHasPrefix(&t.paths, t.path("/A/B/C"), t.path("/A/B")));
    CHECK(usdPathHasPrefix(&t.paths, t.path("/A/B"), t.path("/A/B")));
    CHECK(usdPathHasPrefix(&t.paths, t.path("/A/B"), kUsdPathRoot));
    CHECK(!usdPathHasPrefix(&t.paths, t.path("/A/Bc"), t.path("/A/B")));
    CHECK(!usdPathHasPrefix(&t.paths, t.path("/A"), t.path("/A/B")));
    CHECK(usdPathReplacePrefix(&t.paths, t.path("/A/B/C.size"), t.path("/A/B"), t.path("/X")) == t.path("/X/C.size"));
    CHECK(usdPathReplacePrefix(&t.paths, t.path("/A/B"), t.path("/A/B"), t.path("/X{v=y}")) == t.path("/X{v=y}"));
    CHECK(usdPathReplacePrefix(&t.paths, t.path("/A/B"), kUsdPathRoot, t.path("/X")) == t.path("/X/A/B"));
    CHECK(usdPathReplacePrefix(&t.paths, t.path("/A/B"), t.path("/C"), t.path("/X")) == kUsdPathNone);
}

TEST_CASE("names order as the elements of a path do") {
    // The order that OpenUSD gives these names, which the specification's example (usd-core/#element-ordering) differs
    // from: a digit comes before an underscore, and an underscore before a letter.
    const PrStringView names[] = {
        "_", "__", "_a", "_foobar", "a", "a0", "a1", "a01", "a1_", "a1a", "a:_", "a:b", "a_",
        "a_1", "a_b", "AB", "Ab", "aB", "ab", "abc1", "abc01", "abc001", "abc2", "abc10", "B", "b", "foo", "foo0",
        "foo1", "foo00001bar", "foo001bar001abc", "foo001bar002abc", "foo0001bar0002xyz", "foo_bar", "foob", "Foobar",
        "foobar", "primvars:st", "primvars_st", "primvarsast",
    };
    for (uint32_t i = 0; i + 1 < countOf(names); ++i) {
        CAPTURE(i);
        CHECK(usdNameCompare(names[i], names[i + 1]) < 0);
        CHECK(usdNameCompare(names[i + 1], names[i]) > 0);
    }
    CHECK(usdNameCompare("abc", "abc") == 0);
    CHECK(usdNameCompare("a", "ab") < 0);
}

TEST_CASE("a mapping maps the paths under its pairs, and no others") {
    Tables t;
    UsdMapId reference = t.map("/Ref", "/A");
    CHECK(usdMapToTarget(&t.maps, reference, t.path("/Ref/Child")) == t.path("/A/Child"));
    CHECK(usdMapToTarget(&t.maps, reference, t.path("/Ref.size")) == t.path("/A.size"));
    CHECK(usdMapToTarget(&t.maps, reference, t.path("/Other")) == kUsdPathNone);
    CHECK(usdMapToSource(&t.maps, reference, t.path("/A/Child")) == t.path("/Ref/Child"));
    CHECK(usdMapToSource(&t.maps, reference, t.path("/Ref")) == kUsdPathNone);

    // With the identity, a path outside the pair maps to itself, unless the pair claims its image.
    UsdMapId inherit = t.map("/Class", "/A", true);
    CHECK(usdMapToTarget(&t.maps, inherit, t.path("/Other")) == t.path("/Other"));
    CHECK(usdMapToTarget(&t.maps, inherit, t.path("/Class/B")) == t.path("/A/B"));
    CHECK(usdMapToTarget(&t.maps, inherit, t.path("/A/B")) == kUsdPathNone);
    CHECK(usdMapToSource(&t.maps, inherit, t.path("/Class")) == kUsdPathNone);

    CHECK(usdMapToTarget(&t.maps, kUsdMapIdentity, t.path("/A")) == t.path("/A"));
    CHECK(usdMapToTarget(&t.maps, kUsdMapNull, t.path("/A")) == kUsdPathNone);
}

TEST_CASE("equal mappings are one mapping, without the pairs that others imply") {
    Tables t;
    CHECK(t.map("/Ref", "/A") == t.map("/Ref", "/A"));
    CHECK(t.map("/Ref", "/A") != t.map("/Ref", "/A", true));
    CHECK(t.map("/A", "/A", true) == kUsdMapIdentity);
    UsdMapPair pairs[3] = {
        {t.path("/Ref"), t.path("/A")}, {t.path("/Ref/B"), t.path("/A/B")},
        {t.path("/Ref/C"), t.path("/A/D")},
    };
    UsdMapId made = usdMapMake(&t.maps, pairs, 3, PrUsdLayerOffset{0.0, 1.0});
    CHECK(t.maps.maps.data[made].count == 2);
    CHECK(usdMapToTarget(&t.maps, made, t.path("/Ref/C/X")) == t.path("/A/D/X"));
    CHECK(usdMapToTarget(&t.maps, made, t.path("/Ref/D")) == kUsdPathNone);
    CHECK(usdMapAddRootIdentity(&t.maps, t.map("/Ref", "/A")) == t.map("/Ref", "/A", true));
}

TEST_CASE("mappings compose and invert, with their retimings") {
    Tables t;
    UsdMapPair inner{t.path("/Model"), t.path("/Ref")};
    UsdMapPair outer{t.path("/Ref"), t.path("/A")};
    UsdMapId g = usdMapMake(&t.maps, &inner, 1, PrUsdLayerOffset{10.0, 2.0});
    UsdMapId f = usdMapMake(&t.maps, &outer, 1, PrUsdLayerOffset{5.0, 3.0});
    UsdMapId composed = usdMapCompose(&t.maps, f, g);
    CHECK(usdMapToTarget(&t.maps, composed, t.path("/Model/X")) == t.path("/A/X"));
    CHECK(t.maps.maps.data[composed].offset.offset == 35.0);
    CHECK(t.maps.maps.data[composed].offset.scale == 6.0);
    CHECK(usdMapCompose(&t.maps, f, g) == composed);
    CHECK(usdMapCompose(&t.maps, kUsdMapIdentity, g) == g);
    CHECK(usdMapCompose(&t.maps, f, t.map("/Elsewhere", "/Else")) == kUsdMapNull);

    UsdMapId inverse = usdMapInverse(&t.maps, composed);
    CHECK(usdMapToTarget(&t.maps, inverse, t.path("/A/X")) == t.path("/Model/X"));
    CHECK(t.maps.maps.data[inverse].offset.scale == 1.0 / 6.0);
    CHECK(usdMapInverse(&t.maps, inverse) == composed);

    // An inherit's mapping moved across a reference keeps the identity for the paths outside both.
    UsdMapId transfer = t.map("/Ref", "/A", true);
    UsdMapId implied = usdMapCompose(&t.maps, transfer,
        usdMapCompose(&t.maps, t.map("/Class", "/Ref", true), usdMapInverse(&t.maps, transfer)));
    CHECK(implied == t.map("/Class", "/A", true));
}

TEST_CASE("a mapping maps the text of a path as it maps the path") {
    Tables t;
    UsdMapPair pairs[3] = {
        {t.path("/Ref"), t.path("/A")}, {t.path("/Ref/B"), t.path("/A/C")},
        {kUsdPathRoot, kUsdPathRoot},
    };
    UsdMapId map = usdMapMake(&t.maps, pairs, 3, PrUsdLayerOffset{0.0, 1.0});
    struct Case {
        PrStringView source;
        PrStringView target;
    };
    const Case cases[] = {
        {"/Ref", "/A"}, {"/Ref/X.size", "/A/X.size"}, {"/Ref/B/Y", "/A/C/Y"}, {"/Other/Z", "/Other/Z"},
        {"/Refx", "/Refx"}, {"/A/X", ""}, {"/Ref/C", ""}, {"/", "/"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.source);
        PrTextBuffer out = {};
        bool maps = usdMapTextToTarget(&t.maps, map, c.source, &out);
        CHECK(maps == (c.target.size != 0));
        CHECK(prTextBufferView(&out) == c.target);
        UsdPath mapped = usdMapToTarget(&t.maps, map, t.path(c.source));
        CHECK(t.text(mapped) == c.target);
        prTextBufferReset(&out);
    }
    PrTextBuffer out = {};
    UsdMapId toRoot = t.map("/Ref", "/A");
    CHECK(usdMapTextToTarget(&t.maps, toRoot, "/Ref/B", &out));
    CHECK(prTextBufferView(&out) == "/A/B");
    prTextBufferReset(&out);
}

TEST_CASE("a chain of mappings maps a path by each in turn, where one mapping of them all would map more") {
    Tables t;
    // An inherit of </Rig/Sym> by </Rig/L>, whose layer stack relocates </Rig/L/Brow> to </Rig/Anim/L>, and above it a
    // reference of </Rig> by </Face>, whose layer stack relocates </Face/Anim/L> to </Face/Eyes/L>.
    UsdMapPair inherit[3] = {
        {t.path("/Rig/Sym"), t.path("/Rig/L")}, {t.path("/Rig/Sym/Brow"), t.path("/Rig/Anim/L")},
        {kUsdPathRoot, kUsdPathRoot},
    };
    UsdMapId inner = usdMapMake(&t.maps, inherit, 3, PrUsdLayerOffset{0.0, 1.0});
    UsdMapPair reference[2] = {{t.path("/Rig"), t.path("/Face")}, {t.path("/Rig/Anim/L"), t.path("/Face/Eyes/L")}};
    UsdMapId outer = usdMapMake(&t.maps, reference, 2, PrUsdLayerOffset{0.0, 1.0});
    uint32_t chain = usdMapChainAdd(&t.maps, inner, usdMapChainAdd(&t.maps, outer, kUsdMapChainNone));
    CHECK(usdMapChainAdd(&t.maps, inner, usdMapChainAdd(&t.maps, outer, kUsdMapChainNone)) == chain);
    CHECK(usdMapChainAdd(&t.maps, kUsdMapIdentity, chain) == chain);
    struct Case {
        PrStringView source;
        PrStringView target;
    };
    // The inherit does not map </Rig/Anim/L>, the image of the class's own prim, though the two mappings as one do.
    const Case cases[] = {{"/Rig/Sym/Brow.x", "/Face/Eyes/L.x"}, {"/Rig/Anim/R.x", "/Face/Anim/R.x"}, {"/Rig/Anim/L.x", ""}};
    for (const Case& c : cases) {
        CAPTURE(c.source);
        PrTextBuffer out = {};
        CHECK(usdMapChainTextToTarget(&t.maps, chain, c.source, &out) == (c.target.size != 0));
        CHECK(prTextBufferView(&out) == c.target);
        prTextBufferReset(&out);
    }
    PrTextBuffer out = {};
    CHECK(usdMapTextToTarget(&t.maps, usdMapCompose(&t.maps, outer, inner), "/Rig/Anim/L.x", &out));
    CHECK(usdMapChainTextToTarget(&t.maps, kUsdMapChainNone, "/Any", &out));
    prTextBufferReset(&out);
}
