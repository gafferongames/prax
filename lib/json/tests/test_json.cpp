#include "pr/float.h"
#include "pr/json.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

extern "C" int prTestJsonCApi(void);

namespace pr_impl {

namespace {

bool parse(const char* text, PrJson* d) {
    return prJsonParse(d, text, 0, nullptr);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestJsonCApi() == 0);
}

TEST_CASE("scalars read as themselves") {
    PrJson* d = prJsonCreate();
    REQUIRE(parse(" 12.5e1 ", d));
    CHECK(prJsonKind(d, 0) == PrJsonKind_Number);
    CHECK(prJsonNumber(d, 0, 0) == 125);
    REQUIRE(parse("-0", d));
    CHECK(prJsonNumber(d, 0, 1) == 0);
    REQUIRE(parse("true", d));
    CHECK(prJsonKind(d, 0) == PrJsonKind_True);
    CHECK(prJsonNumber(d, 0, 5) == 1);
    REQUIRE(parse("null", d));
    CHECK(prJsonKind(d, 0) == PrJsonKind_Null);
    CHECK(prJsonNumber(d, 0, 5) == 5);
    REQUIRE(parse("\"\"", d));
    CHECK(prJsonString(d, 0) == "");
    CHECK(prJsonString(d, 0).data != nullptr);
    prJsonDestroy(d);
}

TEST_CASE("containers span what they hold, and members are a key then a value") {
    PrJson* d = prJsonCreate();
    REQUIRE(parse("{\"a\": [1, [2, 3], {}], \"b\": \"x\", \"a\": 4}", d));
    CHECK(prJsonCount(d, 0) == 3);
    CHECK(prJsonNext(d, 0) == prJsonValueCount(d));
    uint32_t a = prJsonElement(d, prJsonFirst(d, 0) + 1, 1);
    CHECK(prJsonKind(d, a) == PrJsonKind_Array);
    CHECK(prJsonFirst(d, prJsonElement(d, prJsonFirst(d, 0) + 1, 2)) == kPrJsonMissing);
    CHECK(prJsonNumber(d, prJsonElement(d, a, 1), 0) == 3);
    CHECK(prJsonElement(d, a, 2) == kPrJsonMissing);
    // The last of two members with the same name.
    CHECK(prJsonNumber(d, prJsonMember(d, 0, "a"), 0) == 4);
    CHECK(prJsonString(d, prJsonMember(d, 0, "b")) == "x");
    CHECK(prJsonMember(d, 0, "c") == kPrJsonMissing);
    uint32_t keys = 0;
    for (uint32_t i = prJsonFirst(d, 0); i != kPrJsonMissing; i = prJsonAfter(d, 0, i))
        ++keys;
    CHECK(keys == 3);
    uint32_t inner = prJsonMember(d, 0, "a");
    CHECK(prJsonKind(d, inner) == PrJsonKind_Number);
    prJsonDestroy(d);
}

TEST_CASE("escapes unescape, and a lone surrogate is a replacement") {
    PrJson* d = prJsonCreate();
    REQUIRE(parse("[\"a\\n\\u00e9\\ud83d\\ude00\\/\", \"\\udc00\", \"\xC3\xA9\"]", d));
    CHECK(prJsonString(d, prJsonElement(d, 0, 0)) == "a\n\xC3\xA9\xF0\x9F\x98\x80/");
    CHECK(prJsonString(d, prJsonElement(d, 0, 1)) == "\xEF\xBF\xBD");
    CHECK(prJsonString(d, prJsonElement(d, 0, 2)) == "\xC3\xA9");
    prJsonDestroy(d);
}

TEST_CASE("what is not JSON fails where it stops") {
    PrJson* d = prJsonCreate();
    size_t at = 0;
    const char* const texts[] = {
        "", "[1,]", "{\"a\" 1}", "[1 2]", "01", "1.", "\"a", "\"\t\"", "[}", "{1: 2}",
        "tru", "[1] x", "\"\\x\"",
    };
    for (const char* text : texts)
        CHECK_FALSE(prJsonParse(d, text, 0, &at));
    CHECK_FALSE(prJsonParse(d, "[1, 2 x", 0, &at));
    CHECK(at == 6);
    CHECK(prJsonValueCount(d) == 0);
    prJsonDestroy(d);
}

TEST_CASE("nesting past the limit fails") {
    PrJson* d = prJsonCreate();
    char text[2 * (kPrJsonMaxDepth + 1)];
    for (uint32_t i = 0; i <= kPrJsonMaxDepth; ++i) {
        text[i] = '[';
        text[2 * kPrJsonMaxDepth + 1 - i] = ']';
    }
    CHECK_FALSE(prJsonParse(d, PrStringView{text, sizeof(text)}, 0, nullptr));
    CHECK(prJsonParse(d, PrStringView{text + 1, sizeof(text) - 2}, 0, nullptr));
    prJsonDestroy(d);
}

TEST_CASE("a writer separates elements and members, and closes what it opened") {
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterBeginObject(w);
    prJsonWriterKey(w, "a");
    prJsonWriterBeginArray(w);
    prJsonWriterNumber(w, 1);
    prJsonWriterBeginArray(w);
    prJsonWriterEndArray(w);
    prJsonWriterBeginObject(w);
    prJsonWriterEndObject(w);
    prJsonWriterBool(w, false);
    prJsonWriterNull(w);
    prJsonWriterEndArray(w);
    prJsonWriterKey(w, "b");
    prJsonWriterString(w, "x");
    prJsonWriterEndObject(w);
    CHECK(prJsonWriterText(w) == "{\"a\":[1,[],{},false,null],\"b\":\"x\"}");
    prJsonWriterClear(w);
    CHECK(prJsonWriterText(w) == "");
    prJsonWriterNumber(w, 2);
    CHECK(prJsonWriterText(w) == "2");
    prJsonWriterDestroy(w);
}

TEST_CASE("a writer escapes strings and writes numbers in their shortest form") {
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterBeginArray(w);
    prJsonWriterString(w, "q\"b\\n\n\x01\xC3\xA9\xFF");
    const double numbers[] = {0.1, -2.5, 1e21, 1e-7, 123456789.0};
    for (double number : numbers)
        prJsonWriterNumber(w, number);
    prJsonWriterNumber(w, kPrF64Infinity);
    prJsonWriterEndArray(w);
    CHECK(prJsonWriterText(w) == "[\"q\\\"b\\\\n\\n\\u0001\xC3\xA9\xEF\xBF\xBD\",0.1,-2.5,1e+21,1e-7,123456789,null]");
    prJsonWriterDestroy(w);
}

TEST_CASE("a document's value writes back as the text that reads to it") {
    const char* text = "{\"a\":[1,\"\\u00e9\\n\",{\"b\":null}],\"c\":true}";
    PrJson* d = prJsonCreate();
    REQUIRE(parse(text, d));
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterValue(w, d, 0);
    CHECK(prJsonWriterText(w) == "{\"a\":[1,\"\xC3\xA9\\n\",{\"b\":null}],\"c\":true}");
    prJsonWriterClear(w);
    prJsonWriterBeginArray(w);
    prJsonWriterValue(w, d, prJsonMember(d, 0, "a"));
    prJsonWriterValue(w, d, prJsonMember(d, 0, "c"));
    prJsonWriterEndArray(w);
    CHECK(prJsonWriterText(w) == "[[1,\"\xC3\xA9\\n\",{\"b\":null}],true]");
    prJsonWriterDestroy(w);
    prJsonDestroy(d);
}

TEST_CASE("a JSON Pointer names a value by its members and elements") {
    // The example of RFC 6901, section 5.
    PrJson* d = prJsonCreate();
    REQUIRE(parse(R"({"foo": ["bar", "baz"], "": 0, "a/b": 1, "c%d": 2, "e^f": 3, "g|h": 4, "i\\j": 5,
        "k\"l": 6, " ": 7, "m~n": 8})", d));
    auto named = [&](const char* pointer) {
        PrJsonValueIndex index = kPrJsonMissing;
        PrStatus status = prJsonValueFromPointer(d, pointer, &index);
        if (status != PrStatus_Ok)
            return -1.0 - double(status);
        if (prJsonKind(d, index) == PrJsonKind_String)
            return prJsonString(d, index) == "baz" ? 100.0 : 101.0;
        return prJsonNumber(d, index, 1000);
    };
    CHECK(named("") == 1000);
    CHECK(named("/foo/1") == 100);
    CHECK(named("/foo/0") == 101);
    CHECK(named("/") == 0);
    CHECK(named("/a~1b") == 1);
    CHECK(named("/c%d") == 2);
    CHECK(named("/i\\j") == 5);
    CHECK(named("/ ") == 7);
    CHECK(named("/m~0n") == 8);
    CHECK(named("/foo/2") == -1.0 - double(PrStatus_NotFound));
    CHECK(named("/foo/-") == -1.0 - double(PrStatus_NotFound));
    CHECK(named("/missing") == -1.0 - double(PrStatus_NotFound));
    CHECK(named("/foo/0/deeper") == -1.0 - double(PrStatus_NotFound));
    CHECK(named("foo") == -1.0 - double(PrStatus_Syntax));
    CHECK(named("/m~2n") == -1.0 - double(PrStatus_Syntax));
    CHECK(named("/foo/01") == -1.0 - double(PrStatus_Syntax));
    prJsonDestroy(d);
}

TEST_CASE("a parse with spans records where each value's text is, and finds the value at an offset") {
    PrJson* d = prJsonCreate();
    PrStringView text = "\xEF\xBB\xBF{\"a\": [1, \"two\", {}],\n \"b/c\": {\"d~\": null}} ";
    REQUIRE(prJsonParse(d, text, 0, nullptr));
    CHECK(!prJsonHasSpans(d));
    REQUIRE(prJsonParse(d, text, PrJsonParseFlag_Spans, nullptr));
    REQUIRE(prJsonHasSpans(d));
    auto spanText = [&](PrJsonValueIndex index) {
        PrJsonSpan span = prJsonSpan(d, index);
        return prStringSlice(text, span.begin, span.end - span.begin);
    };
    // The pointer of a value, as text that the test owns.
    char pointerText[64];
    auto pointer = [&](PrJsonValueIndex index) {
        PrRawString raw = prJsonPointer(d, index);
        PrStringView view = prRawStringView(&raw);
        REQUIRE(view.size < sizeof(pointerText));
        prMemCopy(pointerText, view.data, view.size);
        prRawStringReset(&raw);
        return PrStringView{pointerText, view.size};
    };
    // The root's span runs from its brace, past the byte order mark, to its closing brace, without the space after it.
    CHECK(prJsonSpan(d, 0).begin == 3);
    CHECK(spanText(0) == prStringSlice(text, 3, text.size - 4));
    PrJsonValueIndex a = prJsonMember(d, 0, "a");
    CHECK(spanText(a) == "[1, \"two\", {}]");
    CHECK(spanText(a - 1) == "\"a\"");
    PrJsonValueIndex two = prJsonElement(d, a, 1);
    CHECK(spanText(two) == "\"two\"");
    CHECK(spanText(prJsonElement(d, a, 2)) == "{}");
    PrJsonValueIndex bc = prJsonMember(d, 0, "b/c");
    PrJsonValueIndex dt = prJsonMember(d, bc, "d~");
    CHECK(spanText(dt) == "null");

    // Each value's pointer reads back to it, a key's names its member, and a name escapes `/` and `~`.
    CHECK(pointer(0) == "");
    CHECK(pointer(a) == "/a");
    CHECK(pointer(a - 1) == "/a");
    CHECK(pointer(two) == "/a/1");
    CHECK(pointer(dt) == "/b~1c/d~0");
    for (PrJsonValueIndex i = 0; i < prJsonValueCount(d); ++i) {
        PrJsonValueIndex found = kPrJsonMissing;
        REQUIRE(prJsonValueFromPointer(d, pointer(i), &found) == PrStatus_Ok);
        bool key = prJsonKind(d, i) == PrJsonKind_String && found == i + 1;
        CHECK((found == i || key));
    }

    // An offset finds the innermost value that holds it: a scalar, a key, or the container around a separator.
    auto at = [&](PrStringView needle, uint32_t past) {
        return prJsonValueFromOffset(d, uint32_t(prStringFind(text, needle) + past));
    };
    CHECK(at("two", 1) == two);
    CHECK(at("\"a\"", 0) == a - 1);
    CHECK(at("1,", 1) == a);
    CHECK(at("{}", 1) == prJsonElement(d, a, 2));
    CHECK(at("\n", 0) == 0);
    CHECK(at("null", 3) == dt);
    CHECK(prJsonValueFromOffset(d, 0) == kPrJsonMissing);
    CHECK(prJsonValueFromOffset(d, uint32_t(text.size - 1)) == kPrJsonMissing);
    prJsonDestroy(d);
}
