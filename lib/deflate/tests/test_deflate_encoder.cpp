#include "pr/array.h"
#include "pr/deflate.h"

#include "tools/harness/test.h"

extern "C" int prTestDeflateCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// How a read ended, in one value: its progress if it succeeds, or how it failed.
enum {
    Outcome_Input,
    Outcome_Output,
    Outcome_Done,
    Outcome_Truncated,
    Outcome_Invalid,
};
typedef uint8_t Outcome;

Outcome readStream(PrDeflateDecoder* decoder, PrByteView input, bool final, size_t* consumed,
        uint8_t* PR_NULLABLE output, size_t capacity, size_t* written) {
    PrDeflateRead read{};
    PrStatus status = prDeflateDecoderRead(decoder, input, final, output, capacity, &read);
    *consumed = read.consumed;
    *written = read.written;
    if (status == PrStatus_Truncated)
        return Outcome_Truncated;
    if (status != PrStatus_Ok)
        return Outcome_Invalid;
    // The progresses are the first outcomes, in the same order.
    return Outcome(read.progress);
}

const PrDeflateLevel kLevels[] = {PrDeflateLevel_Fast, PrDeflateLevel_Default, PrDeflateLevel_Small};

// Returns `size` bytes of xorshift32 output from `seed`. No match can shorten them.
Array<uint8_t> noise(uint32_t size, uint32_t seed) {
    Array<uint8_t> out(size);
    uint32_t x = seed;
    for (uint8_t& byte : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        byte = uint8_t(x);
    }
    return out;
}

// Returns words of a small vocabulary in an order that repeats only now and then, as text does.
Array<uint8_t> text(uint32_t size) {
    static const char* const kWords[] = {
        "the ",   "quick ", "brown ", "fox ",    "jumps ", "over ",
        "lazy ",  "dog ",   "and ",   "then ",   "runs ",  "away, ",
        "while ", "a ",     "cat ",   "sleeps. ",
    };
    Array<uint8_t> out;
    uint32_t x = 7;
    while (out.count() < size) {
        x = x * 1103515245u + 12345u;
        PrStringView word = kWords[(x >> 16) % 16];
        out.append(reinterpret_cast<const uint8_t*>(word.data), uint32_t(word.size));
    }
    out.resize(size);
    return out;
}

Array<uint8_t> encode(PrDeflateLevel level, const Array<uint8_t>& input) {
    Array<uint8_t> out(prDeflateEncodeBound(input.count()));
    const size_t size = prDeflateEncode(level, input, out.data());
    REQUIRE(size <= out.count());
    out.resize(uint32_t(size));
    return out;
}

Array<uint8_t> decode(const Array<uint8_t>& stream, uint32_t capacity) {
    PrDeflateDecoder* parser = prDeflateDecoderCreate();
    Array<uint8_t> out(capacity + 1);
    size_t consumed = 0;
    size_t written = 0;
    const Outcome result = readStream(parser, stream, true, &consumed, out.data(), out.count(), &written);
    prDeflateDecoderDestroy(parser);
    CHECK(result == Outcome_Done);
    CHECK(consumed == stream.count());
    out.resize(uint32_t(written));
    return out;
}

void checkRoundTrip(const Array<uint8_t>& input) {
    for (PrDeflateLevel level : kLevels) {
        CAPTURE(int(level));
        const Array<uint8_t> stream = encode(level, input);
        CHECK(decode(stream, input.count()) == input);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("Deflate encodes what its decoder decodes, at each level") {
    SUBCASE("nothing") {
        checkRoundTrip({});
    }
    SUBCASE("one byte, and a few") {
        checkRoundTrip(Array<uint8_t>({'a'}));
        checkRoundTrip(Array<uint8_t>({'a', 'b', 'a', 'b', 'a', 'b', 'a'}));
    }
    SUBCASE("text, over many blocks") {
        checkRoundTrip(text(300000));
    }
    SUBCASE("noise, stored") {
        checkRoundTrip(noise(200000, 1));
    }
    SUBCASE("runs of one byte, with matches of the longest length and the shortest distance") {
        Array<uint8_t> runs(100000);
        for (uint8_t& byte : runs)
            byte = 'z';
        checkRoundTrip(runs);
    }
    SUBCASE("noise repeated at the far end of the window") {
        Array<uint8_t> input = noise(32768, 3);
        const Array<uint8_t> copy = input;
        input.append(copy.span());
        input.append(copy.span().slice(0, 1000));
        checkRoundTrip(input);
    }
    SUBCASE("noise and text, mixed") {
        Array<uint8_t> input;
        for (uint32_t i = 0; i < 20; ++i) {
            const Array<uint8_t> a = noise(3000 + i * 97, i + 10);
            const Array<uint8_t> b = text(5000 + i * 31);
            input.append(a.span());
            input.append(b.span());
        }
        checkRoundTrip(input);
    }
}

TEST_CASE("Deflate output stays within its bound and shrinks what repeats") {
    const Array<uint8_t> random = noise(1000000, 9);
    CHECK(encode(PrDeflateLevel_Small, random).count() <= prDeflateEncodeBound(random.count()));
    // Noise grows by no more than the headers of its stored blocks.
    CHECK(encode(PrDeflateLevel_Default, random).count() < random.count() + random.count() / 1000);

    const Array<uint8_t> words = text(200000);
    const size_t fast = encode(PrDeflateLevel_Fast, words).count();
    const size_t standard = encode(PrDeflateLevel_Default, words).count();
    const size_t small = encode(PrDeflateLevel_Small, words).count();
    CHECK(standard < words.count() / 3);
    CHECK(small <= standard);
    CHECK(standard <= fast);
}

TEST_CASE("the public headers compile as C") {
    CHECK(prTestDeflateCApi() == 0);
}
