// Basis Universal ETC1S decoding (BasisLZ).
//
// A texture has two codebooks: endpoints, each a base color of five bits a channel and one of ETC1's eight tables of
// intensity modifiers, and selectors, each two bits for each of a block's 16 texels, which choose among the four colors
// that an endpoint makes. A slice is one image's blocks, in rows, each an index into each codebook. An endpoint index
// is predicted from the block to the left, above, or above and to the left, or is a difference from the last. A
// selector index is sent as it is, or as a place in a short history of those lately used, and runs of the history's
// first place are counted. Everything is coded with canonical Huffman codes, as Deflate's are.

#include "pr/basis_etc1s.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

#include "basis/src/basis_etc1s_bc.h"
#include "basis/src/basis_etc1s_eac.h"

namespace pr_impl {

constexpr uint32_t kMaxCodeLength = 16;
constexpr uint32_t kMaxSymbols = 1u << 14;
constexpr uint32_t kFastBits = 10;

// A canonical Huffman code. A code of at most kFastBits bits is found in `fast` by the stream's next bits: its symbol
// above its length. A longer one is found a bit at a time, among the codes of each length in turn.
struct BasisHuffman {
    uint32_t fast[1u << kFastBits];
    uint32_t firstCode[kMaxCodeLength + 1];
    uint32_t firstIndex[kMaxCodeLength + 1];
    uint32_t counts[kMaxCodeLength + 1];
    uint16_t* PR_NULLABLE sorted;   // the symbols, by code
    uint32_t symbols;               // the symbols that have a code
};

// An endpoint of the codebook: a base color of five bits a channel, and a table of modifiers.
struct BasisEndpoint {
    uint8_t color[3];
    uint8_t table;
};

// A selector of the codebook: a byte for each row of a block, two bits a texel from the lowest.
struct BasisSelector {
    uint8_t rows[4];
};

struct BasisEtc1s {
    BasisEndpoint* PR_NULLABLE endpoints;
    BasisSelector* PR_NULLABLE selectors;
    uint32_t endpointCount;
    uint32_t selectorCount;
    uint32_t historySize;
    uint32_t imageCount;
    const uint8_t* images;               // the image descriptors, in the global data
    BasisHuffman predictionCode;         // how the endpoints of four blocks are predicted
    BasisHuffman deltaCode;              // an endpoint index, less the last
    BasisHuffman selectorCode;           // a selector index, a place in the history, or a run
    BasisHuffman runCode;                // the length of a run
};

namespace {

// The symbols of the code that codes the lengths of another's codes: 17 lengths, then two runs of zero lengths and two
// runs of the length before, and the order in which their own lengths are sent.
constexpr uint32_t kLengthSymbols = 21;
constexpr uint8_t kLengthOrder[kLengthSymbols] = {
    17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16,
};

// The symbols that predict the endpoints of four blocks, two bits each, and the symbol that repeats the last of them.
constexpr uint32_t kPredictionRepeat = 4 * 4 * 4 * 4;
constexpr uint32_t kPredictionMinRepeat = 3;

// The shortest run of selectors that is counted, and the number of symbols that a run's count has: the last of them
// says that the count follows.
constexpr uint32_t kRunThreshold = 3;
constexpr uint32_t kRunSymbols = 64;

// ETC1's tables of intensity modifiers: the small and the large one of each.
constexpr int32_t kModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

// A stream of bits, read from the lowest bit of each byte. Bits past its end read as zero, and `consumed` then passes
// its size.
struct Bits {
    const uint8_t* data;
    size_t size;
    size_t next;
    uint64_t buffer;
    uint32_t held;
    uint64_t consumed;

    void fill(uint32_t count) {
        while (held < count) {
            uint64_t byte = next < size ? data[next] : 0;
            ++next;
            buffer |= byte << held;
            held += 8;
        }
    }

    // Returns the next `count` bits, at most 32.
    uint32_t get(uint32_t count) {
        fill(count);
        uint32_t value = uint32_t(buffer & ((uint64_t(1) << count) - 1));
        buffer >>= count;
        held -= count;
        consumed += count;
        return value;
    }

    // Returns true if every bit read was in the stream.
    bool valid() const { return consumed <= uint64_t(size) * 8; }
};

// Builds the code whose `count` symbols have the code lengths at `lengths`. Returns false for lengths that no code has.
bool buildHuffman(const uint8_t* lengths, uint32_t count, BasisHuffman* huffman) {
    prMemoryFree(huffman->sorted);
    *huffman = BasisHuffman{};
    for (uint32_t s = 0; s < count; ++s)
        huffman->counts[lengths[s]] += 1;
    huffman->counts[0] = 0;
    uint32_t next[kMaxCodeLength + 1] = {};
    uint32_t code = 0;
    uint32_t index = 0;
    uint64_t space = 0;
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length) {
        code = (code + huffman->counts[length - 1]) << 1;
        huffman->firstCode[length] = code;
        huffman->firstIndex[length] = index;
        next[length] = index;
        index += huffman->counts[length];
        space += uint64_t(huffman->counts[length]) << (kMaxCodeLength - length);
    }
    if (space > uint64_t(1) << kMaxCodeLength)
        return false;
    huffman->symbols = index;
    huffman->sorted = static_cast<uint16_t*>(prMemoryAlloc(sizeof(uint16_t) * (index != 0 ? index : 1)));
    for (uint32_t s = 0; s < count; ++s) {
        uint32_t length = lengths[s];
        if (length == 0)
            continue;
        uint32_t place = next[length]++;
        huffman->sorted[place] = uint16_t(s);
        if (length > kFastBits)
            continue;
        // A code's first bit is its highest, and the stream's next bit is its lowest.
        uint32_t symbolCode = huffman->firstCode[length] + (place - huffman->firstIndex[length]);
        uint32_t reversed = 0;
        for (uint32_t bit = 0; bit < length; ++bit)
            reversed |= ((symbolCode >> bit) & 1) << (length - 1 - bit);
        for (uint32_t high = 0; high < 1u << (kFastBits - length); ++high)
            huffman->fast[reversed | high << length] = s << 5 | length;
    }
    return true;
}

// Reads the next symbol of the code. A stream that holds no code of it sets `*corrupt`.
inline uint32_t decodeHuffman(const BasisHuffman& huffman, Bits& bits, bool* corrupt) {
    bits.fill(kMaxCodeLength);
    uint32_t entry = huffman.fast[bits.buffer & ((1u << kFastBits) - 1)];
    if (entry != 0) {
        uint32_t length = entry & 31;
        bits.buffer >>= length;
        bits.held -= length;
        bits.consumed += length;
        return entry >> 5;
    }
    uint32_t code = 0;
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length) {
        code = code << 1 | bits.get(1);
        uint32_t index = code - huffman.firstCode[length];
        if (code >= huffman.firstCode[length] && index < huffman.counts[length])
            return huffman.sorted[huffman.firstIndex[length] + index];
    }
    *corrupt = true;
    return 0;
}

// Reads the description of a code: the number of its symbols, the lengths of the codes of the lengths' own code, and
// then the lengths, coded with it.
bool readHuffman(Bits& bits, BasisHuffman* huffman, uint8_t* lengths) {
    uint32_t total = bits.get(14);
    if (total == 0)
        return buildHuffman(lengths, 0, huffman);
    uint32_t lengthCodes = bits.get(5);
    if (lengthCodes < 1 || lengthCodes > kLengthSymbols)
        return false;
    uint8_t lengthLengths[kLengthSymbols] = {};
    for (uint32_t i = 0; i < lengthCodes; ++i)
        lengthLengths[kLengthOrder[i]] = uint8_t(bits.get(3));
    BasisHuffman lengthCode = {};
    bool corrupt = !buildHuffman(lengthLengths, kLengthSymbols, &lengthCode);
    uint32_t at = 0;
    while (!corrupt && at < total) {
        uint32_t symbol = decodeHuffman(lengthCode, bits, &corrupt);
        if (symbol <= 16) {
            lengths[at++] = uint8_t(symbol);
            continue;
        }
        // A run: of zeros, three to ten or eleven to 138, or of the length before, three to six or seven to 134.
        bool zeros = symbol <= 18;
        bool longer = symbol == 18 || symbol == 20;
        uint32_t run = zeros ? (longer ? bits.get(7) + 11 : bits.get(3) + 3) : (longer ? bits.get(7) + 7 : bits.get(2) + 3);
        if (run > total - at || (!zeros && (at == 0 || lengths[at - 1] == 0))) {
            corrupt = true;
            break;
        }
        prMemFill(lengths + at, zeros ? 0 : lengths[at - 1], run);
        at += run;
    }
    prMemoryFree(lengthCode.sorted);
    return !corrupt && bits.valid() && buildHuffman(lengths, total, huffman);
}

// Reads the endpoint codebook: each endpoint is its differences from the one before, the table's index modulo 8 and
// each channel's value modulo 32, with one of three codes for a channel, by the value before.
bool readEndpoints(PrByteView data, BasisEtc1s* etc1s, uint8_t* lengths) {
    Bits bits = {data.data, data.size};
    BasisHuffman codes[4] = {};
    bool ok = true;
    for (BasisHuffman& code : codes)
        ok = ok && readHuffman(bits, &code, lengths);
    bool corrupt = !ok;
    if (ok) {
        bool gray = bits.get(1) != 0;
        uint32_t table = 0;
        uint32_t color[3] = {16, 16, 16};
        for (uint32_t i = 0; i < etc1s->endpointCount && !corrupt; ++i) {
            table = (table + decodeHuffman(codes[3], bits, &corrupt)) & 7;
            for (uint32_t c = 0; c < (gray ? 1u : 3u); ++c) {
                uint32_t range = color[c] <= 9 ? 0 : color[c] <= 21 ? 1 : 2;
                color[c] = (color[c] + decodeHuffman(codes[range], bits, &corrupt)) & 31;
            }
            if (gray)
                color[1] = color[2] = color[0];
            etc1s->endpoints[i] =
                BasisEndpoint{{uint8_t(color[0]), uint8_t(color[1]), uint8_t(color[2])}, uint8_t(table)};
        }
    }
    for (BasisHuffman& code : codes)
        prMemoryFree(code.sorted);
    return !corrupt && bits.valid();
}

// Reads the selector codebook: every selector as it is, or the first as it is and each other as its difference from the
// one before, a row at a time.
bool readSelectors(PrByteView data, BasisEtc1s* etc1s, uint8_t* lengths) {
    Bits bits = {data.data, data.size};
    if (bits.get(2) != 0)
        return false;
    bool raw = bits.get(1) != 0;
    BasisHuffman code = {};
    bool corrupt = !raw && !readHuffman(bits, &code, lengths);
    for (uint32_t i = 0; i < etc1s->selectorCount && !corrupt; ++i) {
        for (uint32_t row = 0; row < 4; ++row) {
            if (raw || i == 0)
                etc1s->selectors[i].rows[row] = uint8_t(bits.get(8));
            else
                etc1s->selectors[i].rows[row] =
                    uint8_t(decodeHuffman(code, bits, &corrupt) ^ etc1s->selectors[i - 1].rows[row]);
        }
    }
    prMemoryFree(code.sorted);
    return !corrupt && bits.valid();
}

// Returns a bit for each selector that the texels of a selector use, and sets `*lowest` and `*highest` to the lowest
// and the highest of them.
uint32_t selectorsUsed(const BasisSelector& texels, uint32_t* lowest, uint32_t* highest) {
    uint32_t used = 0;
    for (uint32_t row : texels.rows) {
        for (uint32_t x = 0; x < 4; ++x)
            used |= 1u << (row >> (2 * x) & 3);
    }
    *lowest = 0;
    while ((used >> *lowest & 1) == 0)
        ++*lowest;
    *highest = 3;
    while ((used >> *highest & 1) == 0)
        --*highest;
    return used;
}

// Returns the ends of a block of BC1, packed as BC1 packs a color, that the entries of one table make, one entry for
// each channel: the end in the byte at `shift` of each.
uint32_t packBc1(uint32_t red, uint32_t green, uint32_t blue, uint32_t shift) {
    return (red >> shift & 31) << 11 | (green >> shift & 63) << 5 | (blue >> shift & 31);
}

// Writes the two ends of a block of BC1 at `out`, and the index of each texel: the index of `indices` that its selector
// takes.
void putBc1(uint32_t first, uint32_t second, const uint8_t* indices, const BasisSelector& texels, uint8_t* out) {
    prMemWriteLeU16(out, uint16_t(first));
    prMemWriteLeU16(out + 2, uint16_t(second));
    for (uint32_t y = 0; y < 4; ++y) {
        uint32_t row = 0;
        for (uint32_t x = 0; x < 4; ++x)
            row |= uint32_t(indices[texels.rows[y] >> (2 * x) & 3]) << (2 * x);
        out[4 + y] = uint8_t(row);
    }
}

// Writes the block of BC1 at `out` that holds a block of `endpoint` whose texels have the selectors of `texels`, as
// Basis Universal's transcoder writes it. A block of one color has the ends that make it best a third of the way from
// the first. A block of the largest modifiers whose texels have the two outer selectors alone has their two colors as
// its ends. Any other has, for the run of its selectors, the placing of them among BC1's four colors whose ends leave
// the least error over its three channels. A block of `fourColors` never has equal ends, which BC1 reads as three
// colors and BC3 does not.
void writeBc1(const BasisEndpoint& endpoint, const uint8_t (*palette)[3], const BasisSelector& texels, bool fourColors,
        uint8_t* out) {
    uint32_t lowest = 0;
    uint32_t highest = 0;
    uint32_t used = selectorsUsed(texels, &lowest, &highest);
    // BC1's index of each color from the low end to the high, where the high end is the first.
    constexpr uint8_t kIndices[4] = {1, 3, 2, 0};
    uint8_t indices[4] = {};
    if (lowest == highest) {
        const uint8_t* color = palette[lowest];
        uint32_t red = kBasisBc1OneThird[0][color[0]];
        uint32_t green = kBasisBc1OneThird[1][color[1]];
        uint32_t blue = kBasisBc1OneThird[0][color[2]];
        uint32_t first = packBc1(red, green, blue, 0);
        uint32_t second = packBc1(red, green, blue, 8);
        uint8_t index = 2;
        if (fourColors && first == second) {
            index = 0;
            if (second > 0) {
                --second;
            } else {
                first = 1;
                index = 1;
            }
        }
        if (first < second) {
            uint32_t kept = first;
            first = second;
            second = kept;
            index ^= 1;
        }
        indices[lowest] = index;
        putBc1(first, second, indices, texels, out);
        return;
    }
    if (endpoint.table == 7 && used == 0x9) {
        const uint8_t* low = palette[0];
        const uint8_t* high = palette[3];
        uint32_t first = packBc1(kBasisBc1OneEnd[0][low[0]], kBasisBc1OneEnd[1][low[1]], kBasisBc1OneEnd[0][low[2]], 0);
        uint32_t second = packBc1(kBasisBc1OneEnd[0][high[0]], kBasisBc1OneEnd[1][high[1]], kBasisBc1OneEnd[0][high[2]], 0);
        indices[0] = 0;
        indices[3] = 1;
        if (first == second) {
            if (second > 0) {
                --second;
                indices[3] = 0;
            } else {
                first = 1;
                indices[0] = 1;
            }
        }
        if (first < second) {
            uint32_t kept = first;
            first = second;
            second = kept;
            indices[0] = 1;
            indices[3] = 0;
        }
        putBc1(first, second, indices, texels, out);
        return;
    }
    uint32_t run = 0;
    while (kBasisBc1Runs[run][0] != lowest || kBasisBc1Runs[run][1] != highest)
        ++run;
    const uint32_t* red = kBasisBc1Five[endpoint.table][endpoint.color[0]][run];
    const uint32_t* green = kBasisBc1Six[endpoint.table][endpoint.color[1]][run];
    const uint32_t* blue = kBasisBc1Five[endpoint.table][endpoint.color[2]][run];
    uint32_t best = 0;
    uint32_t bestError = 0xFFFFFFFFu;
    for (uint32_t placing = 0; placing < 10; ++placing) {
        uint32_t error = (red[placing] >> 16) + (green[placing] >> 16) + (blue[placing] >> 16);
        if (error < bestError) {
            bestError = error;
            best = placing;
        }
    }
    uint32_t low = packBc1(red[best], green[best], blue[best], 0);
    uint32_t high = packBc1(red[best], green[best], blue[best], 8);
    // The first end is the high one, unless it is the lesser: the indices are then the other way around.
    uint32_t flip = high < low ? 1 : 0;
    uint32_t first = flip != 0 ? low : high;
    uint32_t second = flip != 0 ? high : low;
    for (uint32_t s = 0; s < 4; ++s)
        indices[s] = uint8_t(kIndices[kBasisBc1Placings[best][s]] ^ flip);
    if (first == second) {
        uint8_t index = 0;
        if (fourColors) {
            if (second > 0) {
                --second;
            } else {
                first = 1;
                index = 1;
            }
        }
        for (uint8_t& each : indices)
            each = index;
    }
    putBc1(first, second, indices, texels, out);
}

// Writes the block of BC4 at `out` that comes nearest one channel of a block. A block of one value or two has them as
// its ends. A block of more has the ends that the tables give for the channel's base value, its table and the set of
// its selectors, and each selector at the index that they give it.
void writeBc4(const BasisEndpoint& endpoint, const uint8_t (*palette)[3], uint32_t component,
        const BasisSelector& texels, uint8_t* out) {
    uint32_t lowest = 0;
    uint32_t highest = 0;
    uint32_t used = selectorsUsed(texels, &lowest, &highest);
    uint32_t indices[4] = {};
    uint32_t first = palette[lowest][component];
    uint32_t second = palette[highest][component];
    if (used == (1u << lowest | 1u << highest)) {
        indices[highest] = lowest != highest ? 1 : 0;
    } else {
        uint32_t set = 0;
        while (kBasisBc4Sets[set] != used)
            ++set;
        uint32_t entry = kBasisBc4[endpoint.table][endpoint.color[component]][set];
        first = entry & 255;
        second = entry >> 8 & 255;
        for (uint32_t s = 0; s < 4; ++s)
            indices[s] = entry >> (16 + 3 * s) & 7;
    }
    uint64_t bits = 0;
    for (uint32_t texel = 0; texel < 16; ++texel)
        bits |= uint64_t(indices[texels.rows[texel / 4] >> (2 * (texel % 4)) & 3]) << (3 * texel);
    out[0] = uint8_t(first);
    out[1] = uint8_t(second);
    for (uint32_t k = 0; k < 6; ++k)
        out[2 + k] = uint8_t(bits >> (8 * k));
}

// Writes the block of ETC1 at `out` that is a block of `endpoint` whose texels have the selectors of `texels`: its two
// halves have the one color, the second as a difference of zero, and the one table. ETC1 orders its texels by column,
// and names a modifier by its sign and its size, where a selector counts the four from the lowest.
void writeEtc1(const BasisEndpoint& endpoint, const BasisSelector& texels, uint8_t* out) {
    // The high and the low bit of ETC1's index for each selector.
    constexpr uint32_t kHigh[4] = {1, 1, 0, 0};
    constexpr uint32_t kLow[4] = {1, 0, 0, 1};
    for (uint32_t c = 0; c < 3; ++c)
        out[c] = uint8_t(endpoint.color[c] << 3);
    // The flag that the second color is a difference. The halves lie side by side.
    out[3] = uint8_t(endpoint.table << 5 | endpoint.table << 2 | 2);
    uint32_t word = 0;
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint32_t selector = texels.rows[y] >> (2 * x) & 3;
            word |= kHigh[selector] << (16 + x * 4 + y) | kLow[selector] << (x * 4 + y);
        }
    }
    prMemWriteBeU32(out + 4, word);
}

// Writes the block of EAC at `out` that comes nearest one channel of a block, of 8 bits or of 11: the block that the
// tables hold for the channel's base value, its table and the run of its selectors, with each selector at the index
// that the tables give it. A block of one value is that value, with a modifier of zero.
void writeEac(const BasisEndpoint& endpoint, const uint8_t (*palette)[3], uint32_t component,
        const BasisSelector& texels, bool eleven, uint8_t* out) {
    uint32_t lowest = 0;
    uint32_t highest = 0;
    selectorsUsed(texels, &lowest, &highest);
    if (lowest == highest) {
        // The fourth index of the fourteenth table adds nothing.
        constexpr uint8_t kFourths[6] = {0x92, 0x49, 0x24, 0x92, 0x49, 0x24};
        out[0] = palette[lowest][component];
        out[1] = 1 << 4 | 13;
        prMemCopy(out + 2, kFourths, sizeof(kFourths));
        return;
    }
    uint32_t run = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (kBasisEacRuns[i][0] == lowest && kBasisEacRuns[i][1] == highest)
            run = i;
    }
    const uint32_t (*table)[32][4] = eleven ? kBasisEacEleven : kBasisEacAlpha;
    uint32_t entry = table[endpoint.table][endpoint.color[component]][run];
    out[0] = uint8_t(entry);
    out[1] = uint8_t(entry >> 8);
    uint64_t indices = 0;
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint32_t selector = texels.rows[y] >> (2 * x) & 3;
            indices |= uint64_t(entry >> (16 + 3 * selector) & 7) << (45 - 3 * (x * 4 + y));
        }
    }
    for (uint32_t k = 0; k < 6; ++k)
        out[2 + k] = uint8_t(indices >> (40 - 8 * k));
}

// Reads a count that is sent four bits at a time, or seven, each with a bit above that says another follows.
uint32_t readCount(Bits& bits, uint32_t chunk) {
    uint32_t value = 0;
    for (uint32_t shift = 0; shift < 32; shift += chunk) {
        uint32_t part = bits.get(chunk + 1);
        value |= (part & ((1u << chunk) - 1)) << shift;
        if (part >> chunk == 0)
            break;
    }
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrBasisEtc1s : BasisEtc1s {};

PrStatus prBasisEtc1sCreate(PrByteView global, uint32_t imageCount, PrBasisEtc1s* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    constexpr uint64_t kHeader = 20;
    constexpr uint64_t kImage = 20;
    if (global.size < kHeader + kImage * imageCount)
        return PrStatus_InvalidData;
    const uint8_t* at = global.data;
    uint32_t endpointCount = prMemReadLeU16(at);
    uint32_t selectorCount = prMemReadLeU16(at + 2);
    uint64_t endpointsSize = prMemReadLeU32(at + 4);
    uint64_t selectorsSize = prMemReadLeU32(at + 8);
    uint64_t tablesSize = prMemReadLeU32(at + 12);
    uint64_t extendedSize = prMemReadLeU32(at + 16);
    uint64_t data = kHeader + kImage * imageCount;
    if (endpointCount == 0 || selectorCount == 0 ||
            data + endpointsSize + selectorsSize + tablesSize + extendedSize != global.size)
        return PrStatus_InvalidData;
    // An image whose blocks may repeat those of the image before it is a frame of a video.
    for (uint32_t i = 0; i < imageCount; ++i) {
        if ((prMemReadLeU32(at + kHeader + kImage * i) & 2) != 0)
            return PrStatus_NotSupported;
    }
    PrBasisEtc1s* etc1s = allocZeroed<PrBasisEtc1s>();
    etc1s->endpointCount = endpointCount;
    etc1s->selectorCount = selectorCount;
    etc1s->imageCount = imageCount;
    etc1s->images = at + kHeader;
    etc1s->endpoints = static_cast<BasisEndpoint*>(prMemoryAlloc(sizeof(BasisEndpoint) * endpointCount));
    etc1s->selectors = static_cast<BasisSelector*>(prMemoryAlloc(sizeof(BasisSelector) * selectorCount));
    uint8_t* lengths = static_cast<uint8_t*>(prMemoryAlloc(kMaxSymbols));
    const uint8_t* tables = at + data + endpointsSize + selectorsSize;
    Bits bits = {tables, size_t(tablesSize)};
    bool ok = readEndpoints(PrByteView{at + data, size_t(endpointsSize)}, etc1s, lengths) &&
        readSelectors(PrByteView{at + data + endpointsSize, size_t(selectorsSize)}, etc1s, lengths) &&
        readHuffman(bits, &etc1s->predictionCode, lengths) && readHuffman(bits, &etc1s->deltaCode, lengths) &&
        readHuffman(bits, &etc1s->selectorCode, lengths) && readHuffman(bits, &etc1s->runCode, lengths);
    if (ok) {
        etc1s->historySize = bits.get(13);
        ok = bits.valid();
    }
    prMemoryFree(lengths);
    if (!ok) {
        prBasisEtc1sDestroy(etc1s);
        return PrStatus_InvalidData;
    }
    *out = etc1s;
    return PrStatus_Ok;
}

void prBasisEtc1sDestroy(PrBasisEtc1s* PR_NULLABLE etc1s) {
    if (etc1s == nullptr)
        return;
    prMemoryFree(etc1s->predictionCode.sorted);
    prMemoryFree(etc1s->deltaCode.sorted);
    prMemoryFree(etc1s->selectorCode.sorted);
    prMemoryFree(etc1s->runCode.sorted);
    prMemoryFree(etc1s->endpoints);
    prMemoryFree(etc1s->selectors);
    prMemoryFree(etc1s);
}

size_t prBasisEtc1sWorkingBytes(const PrBasisEtc1s* etc1s) {
    size_t symbols = etc1s->predictionCode.symbols + etc1s->deltaCode.symbols + etc1s->selectorCode.symbols +
        etc1s->runCode.symbols;
    return sizeof(BasisEtc1s) + sizeof(BasisEndpoint) * etc1s->endpointCount +
        sizeof(BasisSelector) * etc1s->selectorCount + sizeof(uint16_t) * symbols;
}

PrBasisEtc1sImage prBasisEtc1sImage(const PrBasisEtc1s* etc1s, uint32_t index) {
    PR_ASSERT(index < etc1s->imageCount);
    const uint8_t* at = etc1s->images + 20 * size_t(index);
    return PrBasisEtc1sImage{
        prMemReadLeU32(at + 4), prMemReadLeU32(at + 8), prMemReadLeU32(at + 12),
        prMemReadLeU32(at + 16),
    };
}

bool prBasisEtc1sDecodeSlice(const PrBasisEtc1s* etc1s, PrByteView slice, PrUVec2 extent, PrBasisEtc1sTarget target,
        uint8_t* out, size_t stride, uint32_t pixelBytes, uint32_t offset, uint32_t count, uint32_t component) {
    uint32_t blocksX = (extent.x + 3) / 4;
    uint32_t blocksY = (extent.y + 3) / 4;
    uint32_t historySize = etc1s->historySize;
    uint32_t selectorCount = etc1s->selectorCount;
    uint32_t endpointCount = etc1s->endpointCount;
    uint32_t runSymbol = selectorCount + historySize;

    // The endpoint indices of two rows of blocks, the predictions that a row of blocks leaves for the row below it, two
    // blocks to four bits, and the history of selectors, which starts as zeros.
    size_t rowBytes = sizeof(uint16_t) * blocksX;
    size_t storage = 2 * rowBytes + (blocksX + 1) / 2 + sizeof(uint16_t) * historySize;
    uint8_t* scratch = static_cast<uint8_t*>(prMemoryAlloc(storage));
    prMemZero(scratch, storage);
    uint16_t* rows[2] = {reinterpret_cast<uint16_t*>(scratch), reinterpret_cast<uint16_t*>(scratch + rowBytes)};
    uint16_t* history = reinterpret_cast<uint16_t*>(scratch + 2 * rowBytes);
    uint8_t* below = scratch + 2 * rowBytes + sizeof(uint16_t) * historySize;
    uint32_t rover = historySize / 2;

    Bits bits = {slice.data, slice.size};
    bool corrupt = false;
    uint32_t predictions = 0;
    uint32_t lastPredictions = 0;
    uint32_t predictionRepeats = 0;
    uint32_t lastEndpoint = 0;
    uint32_t selectorRun = 0;
    for (uint32_t blockY = 0; blockY < blocksY && !corrupt; ++blockY) {
        uint16_t* row = rows[blockY & 1];
        const uint16_t* above = rows[(blockY & 1) ^ 1];
        for (uint32_t blockX = 0; blockX < blocksX; ++blockX) {
            // A symbol predicts the endpoints of two blocks of this row and of the two below them, and comes with the
            // first of the four.
            if ((blockX & 1) == 0 && (blockY & 1) == 0) {
                if (predictionRepeats != 0) {
                    --predictionRepeats;
                    predictions = lastPredictions;
                } else {
                    predictions = decodeHuffman(etc1s->predictionCode, bits, &corrupt);
                    if (predictions == kPredictionRepeat) {
                        predictionRepeats = readCount(bits, 4) + kPredictionMinRepeat - 1;
                        predictions = lastPredictions;
                    } else {
                        lastPredictions = predictions;
                    }
                }
                below[blockX >> 1] = uint8_t(predictions >> 4);
            } else if ((blockX & 1) == 0) {
                predictions = below[blockX >> 1];
            }
            uint32_t prediction = predictions & 3;
            predictions >>= 2;
            // A block that has no such neighbor reads the index that a neighbor of no blocks would have, zero.
            uint32_t endpoint = lastEndpoint;
            if (prediction == 1) {
                endpoint = above[blockX];
            } else if (prediction == 2) {
                endpoint = blockX != 0 ? above[blockX - 1] : 0;
            } else if (prediction == 3) {
                endpoint += decodeHuffman(etc1s->deltaCode, bits, &corrupt);
                if (endpoint >= endpointCount)
                    endpoint -= endpointCount;
            }
            row[blockX] = uint16_t(endpoint);
            lastEndpoint = endpoint;

            uint32_t symbol = selectorCount;
            if (selectorRun != 0) {
                --selectorRun;
            } else {
                symbol = decodeHuffman(etc1s->selectorCode, bits, &corrupt);
                if (symbol == runSymbol) {
                    uint32_t run = decodeHuffman(etc1s->runCode, bits, &corrupt);
                    selectorRun = (run == kRunSymbols - 1 ? readCount(bits, 7) : run) + kRunThreshold - 1;
                    symbol = selectorCount;
                }
            }
            uint32_t selector = symbol;
            if (symbol >= selectorCount) {
                // A place in the history, which moves halfway to the front.
                uint32_t place = symbol - selectorCount;
                if (place >= historySize) {
                    corrupt = true;
                    break;
                }
                selector = history[place];
                if (place != 0) {
                    history[place] = history[place / 2];
                    history[place / 2] = uint16_t(selector);
                }
            } else if (historySize != 0) {
                history[rover++] = uint16_t(selector);
                if (rover == historySize)
                    rover = historySize / 2;
            }
            if (corrupt || endpoint >= endpointCount || selector >= selectorCount) {
                corrupt = true;
                break;
            }

            // The block's four colors: its base color, expanded to eight bits, with each modifier of its table.
            const BasisEndpoint& colors = etc1s->endpoints[endpoint];
            const int32_t* modifiers = kModifiers[colors.table];
            const int32_t deltas[4] = {-modifiers[1], -modifiers[0], modifiers[0], modifiers[1]};
            uint8_t palette[4][3];
            for (uint32_t c = 0; c < 3; ++c) {
                int32_t base = colors.color[c] << 3 | colors.color[c] >> 2;
                for (uint32_t s = 0; s < 4; ++s) {
                    int32_t value = base + deltas[s];
                    palette[s][c] = uint8_t(value < 0 ? 0 : value > 255 ? 255 : value);
                }
            }
            const BasisSelector& texels = etc1s->selectors[selector];
            if (target != PrBasisEtc1sTarget_Pixels) {
                uint8_t* block = out + size_t(blockY) * stride + size_t(blockX) * pixelBytes + offset;
                if (target == PrBasisEtc1sTarget_Bc1 || target == PrBasisEtc1sTarget_Bc3Color)
                    writeBc1(colors, palette, texels, target == PrBasisEtc1sTarget_Bc3Color, block);
                else if (target == PrBasisEtc1sTarget_Bc4)
                    writeBc4(colors, palette, component, texels, block);
                else if (target == PrBasisEtc1sTarget_Etc1)
                    writeEtc1(colors, texels, block);
                else
                    writeEac(colors, palette, component, texels, target == PrBasisEtc1sTarget_EacR11, block);
                continue;
            }
            uint32_t width = extent.x - blockX * 4 < 4 ? extent.x - blockX * 4 : 4;
            uint32_t height = extent.y - blockY * 4 < 4 ? extent.y - blockY * 4 : 4;
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t* pixel = out + (size_t(blockY) * 4 + y) * stride + size_t(blockX) * 4 * pixelBytes + offset;
                uint32_t bitsOfRow = texels.rows[y];
                for (uint32_t x = 0; x < width; ++x, pixel += pixelBytes, bitsOfRow >>= 2) {
                    const uint8_t* color = palette[bitsOfRow & 3];
                    if (count == 3) {
                        pixel[0] = color[0];
                        pixel[1] = color[1];
                        pixel[2] = color[2];
                    } else {
                        pixel[0] = color[component];
                    }
                }
            }
        }
    }
    prMemoryFree(scratch);
    return !corrupt && bits.valid();
}
