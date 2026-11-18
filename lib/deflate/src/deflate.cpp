// Deflate: decoding, a piece of input at a time, and encoding of a whole input at once.
//
// rfc1951 - https://www.rfc-editor.org/rfc/rfc1951

#include "pr/deflate.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/entropy_lz.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/sort.h"

namespace pr_impl {
namespace {

// What a read needs next, or how the stream ended.
enum {
    InflateResult_Input,        // more input
    InflateResult_Output,       // more room for output
    InflateResult_Done,         // the stream ended
    InflateResult_Truncated,    // the final input ended inside the stream
    InflateResult_Invalid,      // the stream is corrupt
};
typedef uint8_t InflateResult;

// The longest code in a Deflate Huffman table, and the largest number of symbols in a table, which is the 288 of the
// literal and length alphabet (rfc 1951 3.2.5, 3.2.7).
constexpr uint32_t kCodeMaxLength = 15;
constexpr uint32_t kMaxSymbols = 288;

// The bytes of output that a distance can reach back over (rfc 1951 3.2.5).
constexpr uint32_t kHistorySize = 32768;

// The number of bits that the lookup of a table indexes. A code no longer than this decodes in one lookup. A longer
// code walks the canonical code, starting from the bits that the lookup read.
constexpr uint32_t kFastBits = 10;

// The longest match.
constexpr uint32_t kMaxMatch = 258;

// The largest number of bits that one literal or match reads. A match reads a 15-bit code and 5 extra bits for the
// length, and a 15-bit code and 13 extra bits for the distance.
constexpr uint32_t kSymbolMaxBits = 48;

// The lengths encoded by the codes of the literal alphabet from symbol 257 up, and the extra bits that each reads (rfc
// 1951 3.2.5).
constexpr uint16_t kLengthBase[] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
constexpr uint8_t kLengthExtra[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

// The distances encoded by the codes of the distance alphabet, and their extra bits (rfc 1951 3.2.5).
constexpr uint16_t kDistanceBase[] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
constexpr uint8_t kDistanceExtra[] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

// The order in which the lengths of the code length alphabet are stored (rfc 1951 3.2.7).
constexpr uint8_t kCodeLengthOrder[] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5, 11, 4,  12, 3, 13, 2, 14, 1, 15};

// A canonical Huffman code: code lengths, symbols in code-assignment order, and a lookup table for the short codes. The
// lookup is indexed by the next bits of the stream, first bit lowest. It indexes as many bits as the longest code has,
// up to kFastBits, so that a small table builds quickly. An entry is symbol << 4 | length, or zero for a longer code.
struct Huffman {
    uint16_t counts[kCodeMaxLength + 1];
    uint16_t symbols[kMaxSymbols];
    uint16_t fast[1u << kFastBits];
    uint32_t fastMask;
    uint32_t maximum;
};

// The two-bit block encoding.
enum {
    InflateBlock_Stored,
    InflateBlock_Fixed,
    InflateBlock_Dynamic,
    InflateBlock_Reserved,
};
typedef uint8_t InflateBlock;

// Resumable fields of a Deflate block.
enum {
    InflatePhase_Block,
    InflatePhase_StoredLength,
    InflatePhase_StoredComplement,
    InflatePhase_StoredBytes,
    InflatePhase_DynamicCounts,
    InflatePhase_CodeLengths,
    InflatePhase_TableLengths,
    InflatePhase_RepeatLength,
    InflatePhase_Symbol,
    InflatePhase_Literal,
    InflatePhase_Length,
    InflatePhase_DistanceSymbol,
    InflatePhase_Distance,
    InflatePhase_Copy,
    InflatePhase_Done,
    InflatePhase_Invalid,
};
typedef uint8_t InflatePhase;

// The state that persists across calls: bits, codes and output history. No input spans are kept across a call. The bit
// buffer holds only the bits of a partly read byte, or, between calls, the bits of an incomplete code.
struct Deflate {
    // The literal and length codes, and the distance codes, that a dynamic block defines.
    Huffman literals;
    Huffman distances;

    // The codes that the current block is decoded with: the fixed codes, or the codes that a dynamic block defines.
    const Huffman* literalCodes;
    const Huffman* distanceCodes;

    Huffman codes;
    uint8_t lengths[kMaxSymbols + 32];
    uint8_t codeLengths[19];
    uint8_t history[kHistorySize];
    uint32_t bits;
    uint32_t bitCount;
    uint32_t historyAt;
    uint32_t historySize;
    uint32_t windowLimit;
    uint32_t literalCount;
    uint32_t distanceCount;
    uint32_t codeCount;
    uint32_t tableAt;
    uint32_t remaining;
    uint32_t distance;
    uint32_t extra;
    uint8_t literal;
    bool finalBlock;
    InflatePhase phase;
};

// The caller's current spans and the progress through them. The Deflate data of this call starts at offset `base` of
// the output. A match that reaches back no further than `base` reads only the output. A match that reaches further also
// reads the history.
struct InflateSpan {
    PrByteView input;
    size_t consumed;
    uint8_t* output;
    size_t capacity;
    size_t written;
    size_t base;
};

void resetDeflate(Deflate& parser) {
    parser.bits = parser.bitCount = 0;
    parser.historyAt = parser.historySize = 0;
    parser.windowLimit = kHistorySize;
    parser.phase = InflatePhase_Block;
}

bool readBits(Deflate& parser, InflateSpan& span, uint32_t count, uint32_t& value) {
    while (parser.bitCount < count) {
        if (span.consumed == span.input.size)
            return false;
        parser.bits |= uint32_t(span.input.data[span.consumed++]) << parser.bitCount;
        parser.bitCount += 8;
    }
    value = parser.bits & ((1u << count) - 1);
    parser.bits >>= count;
    parser.bitCount -= count;
    return true;
}

// Returns the byte `distance` back from the end of the output. The history holds it if this call has not written that
// far.
uint8_t byteBack(const Deflate& parser, const InflateSpan& span, uint32_t distance) {
    size_t produced = span.written - span.base;
    if (distance <= produced)
        return span.output[span.written - distance];
    return parser.history[(parser.historyAt + kHistorySize - (distance - uint32_t(produced))) % kHistorySize];
}

// Returns true if a match `distance` back reaches only data that the stream produced, within its window.
bool reaches(const Deflate& parser, const InflateSpan& span, uint32_t distance) {
    return distance <= parser.historySize + (span.written - span.base) && distance <= parser.windowLimit;
}

// Appends this call's output to the history.
void keepHistory(Deflate& parser, const uint8_t* data, size_t size) {
    if (size >= kHistorySize) {
        prMemCopy(parser.history, data + size - kHistorySize, kHistorySize);
        parser.historyAt = 0;
        parser.historySize = kHistorySize;
        return;
    }
    uint32_t count = uint32_t(size);
    uint32_t first = kHistorySize - parser.historyAt < count ? kHistorySize - parser.historyAt : count;
    prMemCopy(parser.history + parser.historyAt, data, first);
    prMemCopy(parser.history, data + first, count - first);
    parser.historyAt = (parser.historyAt + count) % kHistorySize;
    parser.historySize = parser.historySize + count < kHistorySize ? parser.historySize + count : kHistorySize;
}

constexpr bool buildHuffman(Huffman& table, const uint8_t* lengths, uint32_t count) {
    for (uint16_t& c : table.counts)
        c = 0;
    table.maximum = 0;
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
        ++table.counts[lengths[symbol]];
        if (lengths[symbol] > table.maximum)
            table.maximum = lengths[symbol];
    }
    int32_t left = 1;
    for (uint32_t length = 1; length <= kCodeMaxLength; ++length) {
        left = (left << 1) - table.counts[length];
        if (left < 0)
            return false;
    }
    // RFC 1951 3.2.7 permits a one-bit singleton and an unused distance table.
    if (left && table.maximum > 1)
        return false;
    uint16_t offsets[kCodeMaxLength + 1]{};
    for (uint32_t length = 1; length < kCodeMaxLength; ++length)
        offsets[length + 1] = uint16_t(offsets[length] + table.counts[length]);
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        if (lengths[symbol])
            table.symbols[offsets[lengths[symbol]]++] = uint16_t(symbol);
    // Each short code fills the entries whose low bits are the code, because the stream sends the first bit first.
    const uint32_t fastBits = table.maximum < kFastBits ? table.maximum : kFastBits;
    table.fastMask = (1u << fastBits) - 1;
    for (uint32_t at = 0; at <= table.fastMask; ++at)
        table.fast[at] = 0;
    uint32_t code = 0;
    uint32_t index = 0;
    for (uint32_t length = 1; length <= fastBits; ++length) {
        for (uint32_t k = 0; k < table.counts[length]; ++k, ++code, ++index) {
            uint32_t reversed = 0;
            for (uint32_t bit = 0; bit < length; ++bit)
                reversed |= ((code >> bit) & 1) << (length - 1 - bit);
            uint16_t entry = uint16_t(uint32_t(table.symbols[index]) << 4 | length);
            for (uint32_t at = reversed; at <= table.fastMask; at += 1u << length)
                table.fast[at] = entry;
        }
        code <<= 1;
    }
    return true;
}

// The outcome of decoding one Huffman symbol. It is distinct from a terminal stream result.
enum {
    SymbolResult_Decoded,
    SymbolResult_Input,
    SymbolResult_Invalid,
};
typedef uint8_t SymbolResult;

// Decodes the symbol whose code starts `bits`, of which the low `available` bits come from the stream. Sets `length` to
// the length of the code.
SymbolResult decodeSymbol(const Huffman& table, uint64_t bits, uint32_t available, uint32_t& symbol, uint32_t& length) {
    const uint32_t entry = table.fast[bits & table.fastMask];
    if (entry != 0) {
        length = entry & 15;
        symbol = entry >> 4;
        return length <= available ? SymbolResult_Decoded : SymbolResult_Input;
    }
    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;
    for (length = 1; length <= table.maximum; ++length) {
        if (length > available)
            return SymbolResult_Input;
        code |= uint32_t(bits >> (length - 1)) & 1;
        const uint32_t count = table.counts[length];
        if (code >= first && code - first < count) {
            symbol = table.symbols[index + code - first];
            return SymbolResult_Decoded;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return SymbolResult_Invalid;
}

// Reads a symbol from the bits held and the input that follows. If there is not enough input, the bits of the
// incomplete code are held for the next call.
SymbolResult readSymbol(Deflate& parser, InflateSpan& span, const Huffman& table, uint32_t& symbol) {
    uint64_t bits = parser.bits;
    uint32_t available = parser.bitCount;
    for (size_t at = span.consumed; available <= kCodeMaxLength && at < span.input.size; ++at) {
        bits |= uint64_t(span.input.data[at]) << available;
        available += 8;
    }
    uint32_t length = 0;
    const SymbolResult result = decodeSymbol(table, bits, available, symbol, length);
    if (result == SymbolResult_Decoded) {
        uint32_t value = 0;
        readBits(parser, span, length, value);
    } else if (result == SymbolResult_Input) {
        // Having fewer bits than the code needs means that every byte of the input was peeked.
        parser.bits = uint32_t(bits);
        parser.bitCount = available;
        span.consumed = span.input.size;
    }
    return result;
}

// The fixed codes (rfc 1951 3.2.6), built at compile time.
struct FixedCodes {
    Huffman literals;
    Huffman distances;
};

constexpr FixedCodes fixedCodes() {
    FixedCodes codes{};
    uint8_t lengths[288]{};
    for (uint32_t i = 0; i < 288; ++i)
        lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    buildHuffman(codes.literals, lengths, 288);
    for (uint32_t i = 0; i < 32; ++i)
        lengths[i] = 5;
    buildHuffman(codes.distances, lengths, 32);
    return codes;
}

constexpr FixedCodes kFixedCodes = fixedCodes();

void useFixed(Deflate& parser) {
    parser.literalCodes = &kFixedCodes.literals;
    parser.distanceCodes = &kFixedCodes.distances;
}

void endBlock(Deflate& parser) {
    parser.phase = parser.finalBlock ? InflatePhase_Done : InflatePhase_Block;
}

// Decodes literals and matches with the bits in a register, while the input holds more than one literal or match can
// read and the output has room for the longest match. It stops at the end of the block, and sets the phase to Invalid
// for bad data. Whole bytes that it read ahead go back to the input, so the bits held are a partial byte as before.
void decodeFast(Deflate& parser, InflateSpan& span) {
    const uint8_t* in = span.input.data;
    const size_t inputEnd = span.input.size;
    size_t at = span.consumed;
    uint8_t* out = span.output;
    size_t written = span.written;
    uint64_t bits = parser.bits;
    uint32_t count = parser.bitCount;
    size_t fetched = 0;
    auto take = [&](uint32_t n) {
        const uint32_t value = uint32_t(bits & ((uint64_t(1) << n) - 1));
        bits >>= n;
        count -= n;
        return value;
    };
    while (span.capacity - written >= kMaxMatch + kPrEntropyLzMatchSlack) {
        // Fills the register to at least 56 bits with one read of eight bytes, or one byte at a time near the end of
        // the input. It stops if the rest of the input may not hold a whole literal or match.
        if (inputEnd - at >= 8) {
            bits |= prMemReadLeU64(in + at) << count;
            const uint32_t added = (63 - count) / 8;
            at += added;
            fetched += added;
            count += added * 8;
        } else {
            for (; count <= 56 && at < inputEnd; ++at, ++fetched, count += 8)
                bits |= uint64_t(in[at]) << count;
            if (count < kSymbolMaxBits)
                break;
        }
        uint32_t symbol = 0;
        uint32_t length = 0;
        if (decodeSymbol(*parser.literalCodes, bits, count, symbol, length) != SymbolResult_Decoded) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        take(length);
        if (symbol < 256) {
            out[written++] = uint8_t(symbol);
            continue;
        }
        if (symbol == 256) {
            endBlock(parser);
            break;
        }
        if (symbol > 285) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        const uint32_t matchLength = kLengthBase[symbol - 257] + take(kLengthExtra[symbol - 257]);
        if (decodeSymbol(*parser.distanceCodes, bits, count, symbol, length) != SymbolResult_Decoded ||
                symbol >= countOf(kDistanceBase)) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        take(length);
        const uint32_t distance = kDistanceBase[symbol] + take(kDistanceExtra[symbol]);
        span.written = written;
        if (!reaches(parser, span, distance)) {
            parser.phase = InflatePhase_Invalid;
            break;
        }
        const size_t produced = written - span.base;
        if (distance <= produced) {
            prEntropyLzCopyMatch(out + written, distance, matchLength);
        } else {
            for (uint32_t k = 0; k < matchLength; ++k) {
                span.written = written + k;
                out[written + k] = byteBack(parser, span, distance);
            }
        }
        written += matchLength;
    }
    const uint32_t whole = count / 8 < fetched ? count / 8 : uint32_t(fetched);
    at -= whole;
    count -= whole * 8;
    parser.bits = uint32_t(bits & ((uint64_t(1) << count) - 1));
    parser.bitCount = count;
    span.consumed = at;
    span.written = written;
}

InflateResult decodeDeflate(Deflate& parser, InflateSpan& span) {
    for (;;) {
        uint32_t value;
        switch (parser.phase) {
            case InflatePhase_Block: {
                if (!readBits(parser, span, 3, value))
                    return InflateResult_Input;
                parser.finalBlock = (value & 1) != 0;
                const InflateBlock block = uint8_t(value >> 1);
                switch (block) {
                    case InflateBlock_Stored:
                        // A code that waited for input took every bit held, so only a partial byte is left.
                        PR_ASSERT(parser.bitCount < 8);
                        parser.bits = parser.bitCount = 0;
                        parser.phase = InflatePhase_StoredLength;
                        break;
                    case InflateBlock_Fixed:
                        useFixed(parser);
                        parser.phase = InflatePhase_Symbol;
                        break;
                    case InflateBlock_Dynamic:
                        parser.phase = InflatePhase_DynamicCounts;
                        break;
                    case InflateBlock_Reserved:
                        return InflateResult_Invalid;
                }
                break;
            }
            case InflatePhase_StoredLength:
                if (!readBits(parser, span, 16, parser.remaining))
                    return InflateResult_Input;
                parser.phase = InflatePhase_StoredComplement;
                break;
            case InflatePhase_StoredComplement:
                if (!readBits(parser, span, 16, value))
                    return InflateResult_Input;
                if ((parser.remaining ^ 0xffffu) != value)
                    return InflateResult_Invalid;
                parser.phase = InflatePhase_StoredBytes;
                break;
            case InflatePhase_StoredBytes:
                if (!parser.remaining) {
                    endBlock(parser);
                    break;
                }
                if (span.written == span.capacity)
                    return InflateResult_Output;
                if (parser.bitCount == 0) {
                    size_t count = parser.remaining;
                    count = count < span.input.size - span.consumed ? count : span.input.size - span.consumed;
                    count = count < span.capacity - span.written ? count : span.capacity - span.written;
                    if (count == 0)
                        return InflateResult_Input;
                    prMemCopy(span.output + span.written, span.input.data + span.consumed, count);
                    span.consumed += count;
                    span.written += count;
                    parser.remaining -= uint32_t(count);
                    break;
                }
                if (!readBits(parser, span, 8, value))
                    return InflateResult_Input;
                span.output[span.written++] = uint8_t(value);
                --parser.remaining;
                break;
            case InflatePhase_DynamicCounts:
                if (!readBits(parser, span, 14, value))
                    return InflateResult_Input;
                parser.literalCount = (value & 31) + 257;
                parser.distanceCount = ((value >> 5) & 31) + 1;
                parser.codeCount = (value >> 10) + 4;
                if (parser.literalCount > 286)
                    return InflateResult_Invalid;
                prMemZero(parser.codeLengths, sizeof(parser.codeLengths));
                parser.tableAt = 0;
                parser.phase = InflatePhase_CodeLengths;
                break;
            case InflatePhase_CodeLengths:
                if (parser.tableAt < parser.codeCount) {
                    if (!readBits(parser, span, 3, value))
                        return InflateResult_Input;
                    parser.codeLengths[kCodeLengthOrder[parser.tableAt++]] = uint8_t(value);
                    break;
                }
                if (!buildHuffman(parser.codes, parser.codeLengths, uint32_t(countOf(parser.codeLengths))) ||
                        !parser.codes.maximum || (parser.codes.maximum == 1 && parser.codes.counts[1] != 2))
                    return InflateResult_Invalid;
                parser.tableAt = 0;
                parser.phase = InflatePhase_TableLengths;
                break;
            case InflatePhase_TableLengths: {
                if (parser.tableAt == parser.literalCount + parser.distanceCount) {
                    if (!parser.lengths[256] || !buildHuffman(parser.literals, parser.lengths, parser.literalCount) ||
                            !buildHuffman(parser.distances, parser.lengths + parser.literalCount, parser.distanceCount))
                        return InflateResult_Invalid;
                    parser.literalCodes = &parser.literals;
                    parser.distanceCodes = &parser.distances;
                    parser.phase = InflatePhase_Symbol;
                    break;
                }
                const SymbolResult result = readSymbol(parser, span, parser.codes, value);
                if (result != SymbolResult_Decoded)
                    return result == SymbolResult_Input ? InflateResult_Input : InflateResult_Invalid;
                if (value < 16) {
                    parser.lengths[parser.tableAt++] = uint8_t(value);
                } else {
                    if (value == 16 && !parser.tableAt)
                        return InflateResult_Invalid;
                    parser.literal = value == 16 ? parser.lengths[parser.tableAt - 1] : 0;
                    parser.remaining = value == 18 ? 11 : 3;
                    parser.extra = value == 16 ? 2 : value == 17 ? 3 : 7;
                    parser.phase = InflatePhase_RepeatLength;
                }
                break;
            }
            case InflatePhase_RepeatLength:
                if (!readBits(parser, span, parser.extra, value))
                    return InflateResult_Input;
                parser.remaining += value;
                if (parser.remaining > parser.literalCount + parser.distanceCount - parser.tableAt)
                    return InflateResult_Invalid;
                while (parser.remaining) {
                    parser.lengths[parser.tableAt++] = parser.literal;
                    --parser.remaining;
                }
                parser.phase = InflatePhase_TableLengths;
                break;
            case InflatePhase_Symbol: {
                if (parser.bitCount + (span.input.size - span.consumed) * 8 >= kSymbolMaxBits &&
                        span.capacity - span.written >= kMaxMatch + kPrEntropyLzMatchSlack) {
                    const size_t consumed = span.consumed;
                    const size_t written = span.written;
                    const InflatePhase phase = parser.phase;
                    decodeFast(parser, span);
                    if (span.consumed != consumed || span.written != written || parser.phase != phase)
                        break;
                }
                const SymbolResult result = readSymbol(parser, span, *parser.literalCodes, value);
                if (result != SymbolResult_Decoded)
                    return result == SymbolResult_Input ? InflateResult_Input : InflateResult_Invalid;
                if (value < 256) {
                    parser.literal = uint8_t(value);
                    parser.phase = InflatePhase_Literal;
                } else if (value == 256) {
                    endBlock(parser);
                } else {
                    if (value > 285)
                        return InflateResult_Invalid;
                    parser.remaining = kLengthBase[value - 257];
                    parser.extra = kLengthExtra[value - 257];
                    parser.phase = InflatePhase_Length;
                }
                break;
            }
            case InflatePhase_Literal:
                if (span.written == span.capacity)
                    return InflateResult_Output;
                span.output[span.written++] = parser.literal;
                parser.phase = InflatePhase_Symbol;
                break;
            case InflatePhase_Length:
                if (!readBits(parser, span, parser.extra, value))
                    return InflateResult_Input;
                parser.remaining += value;
                parser.phase = InflatePhase_DistanceSymbol;
                break;
            case InflatePhase_DistanceSymbol: {
                const SymbolResult result = readSymbol(parser, span, *parser.distanceCodes, value);
                if (result != SymbolResult_Decoded)
                    return result == SymbolResult_Input ? InflateResult_Input : InflateResult_Invalid;
                if (value >= countOf(kDistanceBase))
                    return InflateResult_Invalid;
                parser.distance = kDistanceBase[value];
                parser.extra = kDistanceExtra[value];
                parser.phase = InflatePhase_Distance;
                break;
            }
            case InflatePhase_Distance:
                if (!readBits(parser, span, parser.extra, value))
                    return InflateResult_Input;
                parser.distance += value;
                if (!reaches(parser, span, parser.distance))
                    return InflateResult_Invalid;
                parser.phase = InflatePhase_Copy;
                break;
            case InflatePhase_Copy:
                if (!parser.remaining) {
                    parser.phase = InflatePhase_Symbol;
                    break;
                }
                if (span.written == span.capacity)
                    return InflateResult_Output;
                // Copies as much of the match as the output has room for. The copy reads from the history while the
                // match reaches back past this call's output, and then from the output.
                while (parser.remaining != 0 && span.written != span.capacity &&
                        parser.distance > span.written - span.base) {
                    span.output[span.written] = byteBack(parser, span, parser.distance);
                    ++span.written;
                    --parser.remaining;
                }
                for (; parser.remaining != 0 && span.written != span.capacity; --parser.remaining, ++span.written)
                    span.output[span.written] = span.output[span.written - parser.distance];
                break;
            case InflatePhase_Done:
                return InflateResult_Done;
            case InflatePhase_Invalid:
                return InflateResult_Invalid;
        }
    }
}

// Decodes the Deflate data, and adds the output of this call to the history.
InflateResult readDeflate(Deflate& parser, InflateSpan& span) {
    span.base = span.written;
    const InflateResult result = decodeDeflate(parser, span);
    keepHistory(parser, span.output + span.base, span.written - span.base);
    return result;
}

// The shortest match (rfc 1951 3.2.5). A match reaches back over kHistorySize bytes at most, and is kMaxMatch long at
// most.
constexpr uint32_t kMinMatch = 3;

// The number of bits in the hash of the three bytes at a position. Each hash heads a chain of earlier positions with
// the same hash.
constexpr uint32_t kHashBits = 15;

// The number of literals and matches that a block holds before it is written.
constexpr uint32_t kBlockTokens = 16384;

// The sizes of the alphabets (rfc 1951 3.2.5, 3.2.7), the end-of-block symbol of the literal and length alphabet, and
// the longest code in each alphabet.
constexpr uint32_t kLiteralSymbols = 286;
constexpr uint32_t kDistanceSymbols = 30;
constexpr uint32_t kCodeLengthSymbols = 19;
constexpr uint32_t kEndOfBlock = 256;
constexpr uint32_t kMaxCodeLength = 15;
constexpr uint32_t kMaxCodeLengthCodeLength = 7;

// The largest stored block (rfc 1951 3.2.4).
constexpr uint32_t kStoredMax = 65535;

// The code of each match length and each distance, built at compile time. Distances up to 256 are indexed by distance
// minus one. Longer distances are indexed by distance minus one, divided by 128.
struct SymbolTables {
    uint8_t length[kMaxMatch + 1];  // by length
    uint8_t nearDistance[256];      // by distance minus one
    uint8_t farDistance[256];       // by distance minus one, divided by 128
};

constexpr SymbolTables symbolTables() {
    SymbolTables tables{};
    for (uint32_t code = 0; code < 29; ++code)
        for (uint32_t length = kLengthBase[code]; length < kLengthBase[code] + (1u << kLengthExtra[code]); ++length)
            if (length <= kMaxMatch)
                tables.length[length] = uint8_t(code);
    tables.length[kMaxMatch] = 28;
    for (uint32_t code = 0; code < 30; ++code) {
        for (uint32_t distance =
                kDistanceBase[code]; distance < kDistanceBase[code] + (1u << kDistanceExtra[code]); ++distance) {
            if (distance <= 256)
                tables.nearDistance[distance - 1] = uint8_t(code);
            else
                tables.farDistance[(distance - 1) >> 7] = uint8_t(code);
        }
    }
    return tables;
}

constexpr SymbolTables kSymbols = symbolTables();

inline uint32_t distanceSymbol(uint32_t distance) {
    return distance <= 256 ? kSymbols.nearDistance[distance - 1] : kSymbols.farDistance[(distance - 1) >> 7];
}

// How long each level searches for matches. `chain` is the number of earlier positions it tries, and `nice` is the
// match length at which it stops. Once it holds a match of `good` bytes, it tries a quarter as many positions. It
// defers a match shorter than `lazy` bytes to look for a longer one at the next position, and defers none if `lazy` is
// zero.
struct Effort {
    uint32_t chain;
    uint32_t nice;
    uint32_t good;
    uint32_t lazy;
};

constexpr Effort kEfforts[] = {
    {8, 32, 4, 0},          // PrDeflateLevel_Fast
    {128, 128, 8, 16},      // PrDeflateLevel_Default
    {1024, 258, 32, 258},   // PrDeflateLevel_Small
};

// A literal, or a match of `length` bytes at `distance`. A literal's distance is zero and its length is the byte.
struct Token {
    uint16_t length;
    uint16_t distance;
};

// The bits of the output, first bit lowest.
struct BitWriter {
    uint8_t* out;
    size_t at;
    uint64_t bits;
    uint32_t count;
};

inline void put(BitWriter& w, uint32_t value, uint32_t count) {
    w.bits |= uint64_t(value) << w.count;
    w.count += count;
    while (w.count >= 8) {
        w.out[w.at++] = uint8_t(w.bits);
        w.bits >>= 8;
        w.count -= 8;
    }
}

void alignToByte(BitWriter& w) {
    if (w.count != 0)
        put(w, 0, 8 - w.count);
}

// The code of a symbol and its length. The bits of the code are reversed so that the stream sends the first bit first.
struct Code {
    uint16_t bits;
    uint8_t length;
};

inline void putCode(BitWriter& w, Code code) {
    put(w, code.bits, code.length);
}

// Sets the lengths of a prefix code for symbols with the given `frequencies`, with no code longer than `limit`. The
// lengths are those of the Huffman code. Any code over the limit is shortened, and others are lengthened to keep the
// code complete. A code with one symbol has length one.
void buildLengths(const uint32_t* frequencies, uint32_t count, uint32_t limit, uint8_t* lengths) {
    uint16_t symbols[kLiteralSymbols];
    uint32_t used = 0;
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
        lengths[symbol] = 0;
        if (frequencies[symbol] != 0)
            symbols[used++] = uint16_t(symbol);
    }
    if (used == 0)
        return;
    if (used == 1) {
        lengths[symbols[0]] = 1;
        return;
    }
    sortItems(Span(symbols, used), [&](uint16_t a, uint16_t b) {
        return frequencies[a] != frequencies[b] ? frequencies[a] < frequencies[b] : a < b;
    });

    // The Huffman tree. The leaves come first, in order of frequency, and then the internal nodes in the order that
    // they form, which is also their order of weight.
    uint32_t weights[2 * kLiteralSymbols];
    uint16_t parents[2 * kLiteralSymbols];
    for (uint32_t i = 0; i < used; ++i)
        weights[i] = frequencies[symbols[i]];
    uint32_t leaf = 0;
    uint32_t node = used;
    uint32_t next = used;
    auto lightest = [&]() {
        if (leaf < used && (node == next || weights[leaf] <= weights[node]))
            return leaf++;
        return node++;
    };
    for (uint32_t i = 0; i + 1 < used; ++i) {
        const uint32_t a = lightest();
        const uint32_t b = lightest();
        weights[next] = weights[a] + weights[b];
        parents[a] = parents[b] = uint16_t(next);
        ++next;
    }
    // Depths are assigned from the root, which is the last node, down. A parent comes after its children.
    uint8_t depths[2 * kLiteralSymbols];
    depths[next - 1] = 0;
    uint32_t counts[2 * kLiteralSymbols]{};
    for (uint32_t i = next - 1; i-- > 0;) {
        depths[i] = uint8_t(depths[parents[i]] + 1);
        if (i < used)
            ++counts[depths[i]];
    }

    // Codes over the limit move to the limit. Then, one at a time, a code at the limit is dropped and a shorter code
    // splits in two, until the code is complete again.
    for (uint32_t length = limit + 1; length < 2 * kLiteralSymbols; ++length) {
        counts[limit] += counts[length];
        counts[length] = 0;
    }
    uint32_t total = 0;
    for (uint32_t length = 1; length <= limit; ++length)
        total += counts[length] << (limit - length);
    while (total != (1u << limit)) {
        --counts[limit];
        for (uint32_t length = limit - 1; length > 0; --length) {
            if (counts[length] != 0) {
                --counts[length];
                counts[length + 1] += 2;
                break;
            }
        }
        --total;
    }
    // The longest codes go to the least frequent symbols.
    uint32_t at = 0;
    for (uint32_t length = limit; length > 0; --length)
        for (uint32_t k = 0; k < counts[length]; ++k)
            lengths[symbols[at++]] = uint8_t(length);
}

// Builds the canonical codes for `lengths` (rfc 1951 3.2.2).
void buildCodes(const uint8_t* lengths, uint32_t count, Code* codes) {
    uint32_t perLength[kMaxCodeLength + 1]{};
    for (uint32_t symbol = 0; symbol < count; ++symbol)
        ++perLength[lengths[symbol]];
    perLength[0] = 0;
    uint32_t nextCode[kMaxCodeLength + 2]{};
    uint32_t code = 0;
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length) {
        code = (code + perLength[length - 1]) << 1;
        nextCode[length] = code;
    }
    for (uint32_t symbol = 0; symbol < count; ++symbol) {
        const uint32_t length = lengths[symbol];
        uint32_t reversed = 0;
        if (length != 0) {
            const uint32_t value = nextCode[length]++;
            for (uint32_t bit = 0; bit < length; ++bit)
                reversed |= ((value >> bit) & 1) << (length - 1 - bit);
        }
        codes[symbol] = Code{uint16_t(reversed), uint8_t(length)};
    }
}

// The lengths of the fixed codes (rfc 1951 3.2.6).
struct FixedLengths {
    uint8_t literals[288];
    uint8_t distances[kDistanceSymbols];
};

constexpr FixedLengths fixedLengths() {
    FixedLengths fixed{};
    for (uint32_t i = 0; i < 288; ++i)
        fixed.literals[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (uint8_t& length : fixed.distances)
        length = 5;
    return fixed;
}

constexpr FixedLengths kFixedLengths = fixedLengths();

// The encoder's state: the chains of earlier positions by hash, and the block being gathered.
struct Encoder {
    PrByteView input;
    Effort effort;
    int32_t heads[1u << kHashBits];     // by hash, the last position with that hash, or -1
    int32_t chain[kHistorySize];        // by position within the window, the previous position with the same hash
    Token tokens[kBlockTokens];
    uint32_t tokenCount;
    uint32_t literalFrequencies[kLiteralSymbols];
    uint32_t distanceFrequencies[kDistanceSymbols];
    size_t blockStart;                  // the input position where the block's bytes start
    BitWriter out;
};

inline uint32_t hashAt(const uint8_t* bytes) {
    const uint32_t three = uint32_t(bytes[0]) << 16 | uint32_t(bytes[1]) << 8 | bytes[2];
    return (three * 2654435761u) >> (32 - kHashBits);
}

inline void insert(Encoder& e, size_t position) {
    if (position + kMinMatch > e.input.size)
        return;
    const uint32_t hash = hashAt(e.input.data + position);
    e.chain[position & (kHistorySize - 1)] = e.heads[hash];
    e.heads[hash] = int32_t(position);
}

// Returns the number of bytes, up to `limit`, that `there` and `here` have in common at their start.
inline uint32_t commonLength(const uint8_t* there, const uint8_t* here, uint32_t limit) {
    uint32_t length = 0;
    while (length + 8 <= limit) {
        const uint64_t differ = prMemReadLeU64(there + length) ^ prMemReadLeU64(here + length);
        if (differ != 0)
            return length + prU64LowestBit(differ) / 8;
        length += 8;
    }
    while (length < limit && there[length] == here[length])
        ++length;
    return length;
}

// Returns the length of the longest match at `position` that is longer than `shorter`, and sets `distance` to its
// distance. Returns zero if no earlier position gives such a match.
uint32_t findMatch(const Encoder& e, size_t position, uint32_t shorter, uint32_t* distance) {
    const size_t left = e.input.size - position;
    if (left < kMinMatch)
        return 0;
    const uint32_t limit = left < kMaxMatch ? uint32_t(left) : kMaxMatch;
    const uint8_t* here = e.input.data + position;
    uint32_t best = shorter < kMinMatch - 1 ? kMinMatch - 1 : shorter;
    if (best >= limit)
        return 0;
    uint32_t found = 0;
    int32_t candidate = e.heads[hashAt(here)];
    uint32_t tries = best >= e.effort.good ? e.effort.chain / 4 : e.effort.chain;
    for (; tries != 0 && candidate >= 0; --tries) {
        const size_t back = position - size_t(candidate);
        if (back == 0 || back > kHistorySize)
            break;
        const uint8_t* there = e.input.data + candidate;
        if (there[best] == here[best] && there[0] == here[0]) {
            const uint32_t length = commonLength(there, here, limit);
            if (length > best) {
                best = length;
                found = length;
                *distance = uint32_t(back);
                if (length >= e.effort.nice || length == limit)
                    break;
            }
        }
        const int32_t earlier = e.chain[size_t(candidate) & (kHistorySize - 1)];
        if (earlier >= candidate)
            break;
        candidate = earlier;
    }
    return found;
}

void writeBlock(Encoder& e, size_t end, bool final);

void addToken(Encoder& e, Token token, size_t end) {
    e.tokens[e.tokenCount++] = token;
    if (token.distance == 0) {
        ++e.literalFrequencies[token.length];
    } else {
        ++e.literalFrequencies[257 + kSymbols.length[token.length]];
        ++e.distanceFrequencies[distanceSymbol(token.distance)];
    }
    if (e.tokenCount == kBlockTokens)
        writeBlock(e, end, false);
}

// Returns the number of bits that the tokens take when coded with the code lengths `literals` and `distances`. The
// count includes the end-of-block symbol, whose frequency is one.
uint64_t dataBits(const Encoder& e, const uint8_t* literals, const uint8_t* distances) {
    uint64_t bits = 0;
    for (uint32_t symbol = 0; symbol < kLiteralSymbols; ++symbol) {
        const uint64_t extra = symbol > 256 ? kLengthExtra[symbol - 257] : 0;
        bits += uint64_t(e.literalFrequencies[symbol]) * (literals[symbol] + extra);
    }
    for (uint32_t symbol = 0; symbol < kDistanceSymbols; ++symbol)
        bits += uint64_t(e.distanceFrequencies[symbol]) * (distances[symbol] + kDistanceExtra[symbol]);
    return bits;
}

void writeTokens(Encoder& e, const Code* literals, const Code* distances) {
    for (uint32_t i = 0; i < e.tokenCount; ++i) {
        const Token token = e.tokens[i];
        if (token.distance == 0) {
            putCode(e.out, literals[token.length]);
            continue;
        }
        const uint32_t lengthCode = kSymbols.length[token.length];
        putCode(e.out, literals[257 + lengthCode]);
        put(e.out, token.length - kLengthBase[lengthCode], kLengthExtra[lengthCode]);
        const uint32_t distanceCode = distanceSymbol(token.distance);
        putCode(e.out, distances[distanceCode]);
        put(e.out, token.distance - kDistanceBase[distanceCode], kDistanceExtra[distanceCode]);
    }
    putCode(e.out, literals[kEndOfBlock]);
}

// A run-length coded code length: a code length symbol and its extra bits (rfc 1951 3.2.7).
struct LengthToken {
    uint8_t symbol;
    uint8_t extra;
};

// Codes the code lengths with the repeat symbols. Returns the number of tokens written to `out`.
uint32_t runLengths(const uint8_t* lengths, uint32_t count, LengthToken* out) {
    uint32_t tokens = 0;
    for (uint32_t i = 0; i < count;) {
        const uint8_t length = lengths[i];
        uint32_t run = 1;
        while (i + run < count && lengths[i + run] == length)
            ++run;
        i += run;
        if (length == 0) {
            while (run >= 11) {
                const uint32_t take = run < 138 ? run : 138;
                out[tokens++] = LengthToken{18, uint8_t(take - 11)};
                run -= take;
            }
            if (run >= 3) {
                out[tokens++] = LengthToken{17, uint8_t(run - 3)};
                run = 0;
            }
        } else {
            out[tokens++] = LengthToken{length, 0};
            --run;
            while (run >= 3) {
                const uint32_t take = run < 6 ? run : 6;
                out[tokens++] = LengthToken{16, uint8_t(take - 3)};
                run -= take;
            }
        }
        while (run-- > 0)
            out[tokens++] = LengthToken{length, 0};
    }
    return tokens;
}

// Writes the gathered tokens as a block that covers the input from the start of the block to `end`. The block is
// stored, coded with the fixed codes, or coded with its own codes, whichever takes the fewest bits.
void writeBlock(Encoder& e, size_t end, bool final) {
    e.literalFrequencies[kEndOfBlock] = 1;

    // The block's own codes. Each has at least two symbols, so that each is complete.
    uint32_t literalCounts[kLiteralSymbols];
    prMemCopy(literalCounts, e.literalFrequencies, sizeof(literalCounts));
    uint32_t distanceCounts[kDistanceSymbols];
    prMemCopy(distanceCounts, e.distanceFrequencies, sizeof(distanceCounts));
    if (literalCounts[0] == 0 && literalCounts[1] == 0)
        literalCounts[0] = 1;
    uint32_t usedDistances = 0;
    for (uint32_t count : distanceCounts)
        usedDistances += count != 0;
    if (usedDistances < 2) {
        distanceCounts[0] += distanceCounts[0] == 0;
        distanceCounts[1] += distanceCounts[1] == 0;
    }
    uint8_t lengths[kLiteralSymbols + kDistanceSymbols];
    buildLengths(literalCounts, kLiteralSymbols, kMaxCodeLength, lengths);
    uint8_t* distanceLengths = lengths + kLiteralSymbols;
    buildLengths(distanceCounts, kDistanceSymbols, kMaxCodeLength, distanceLengths);
    uint32_t literalCount = kLiteralSymbols;
    while (literalCount > 257 && lengths[literalCount - 1] == 0)
        --literalCount;
    uint32_t distanceCount = kDistanceSymbols;
    while (distanceCount > 1 && distanceLengths[distanceCount - 1] == 0)
        --distanceCount;
    uint8_t sequence[kLiteralSymbols + kDistanceSymbols];
    prMemCopy(sequence, lengths, literalCount);
    prMemCopy(sequence + literalCount, distanceLengths, distanceCount);
    LengthToken lengthTokens[kLiteralSymbols + kDistanceSymbols];
    const uint32_t lengthTokenCount = runLengths(sequence, literalCount + distanceCount, lengthTokens);
    uint32_t lengthFrequencies[kCodeLengthSymbols]{};
    for (uint32_t i = 0; i < lengthTokenCount; ++i)
        ++lengthFrequencies[lengthTokens[i].symbol];
    uint8_t lengthLengths[kCodeLengthSymbols];
    buildLengths(lengthFrequencies, kCodeLengthSymbols, kMaxCodeLengthCodeLength, lengthLengths);
    uint32_t lengthCount = kCodeLengthSymbols;
    while (lengthCount > 4 && lengthLengths[kCodeLengthOrder[lengthCount - 1]] == 0)
        --lengthCount;
    uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3 * uint64_t(lengthCount) + dataBits(e, lengths, distanceLengths);
    static constexpr uint8_t kRepeatExtra[3] = {2, 3, 7};
    for (uint32_t i = 0; i < lengthTokenCount; ++i) {
        const uint8_t symbol = lengthTokens[i].symbol;
        dynamicBits += lengthLengths[symbol] + (symbol >= 16 ? kRepeatExtra[symbol - 16] : 0);
    }

    const uint64_t fixedBits = 3 + dataBits(e, kFixedLengths.literals, kFixedLengths.distances);

    // Stored blocks hold at most kStoredMax bytes. Each has a three-bit header, padding bits to the byte boundary, and
    // the length and its complement.
    const size_t size = end - e.blockStart;
    const size_t chunks = size == 0 ? 1 : (size + kStoredMax - 1) / kStoredMax;
    const uint64_t firstPad = (8 - (e.out.count + 3) % 8) % 8;
    const uint64_t storedBits = 3 + firstPad + 32 + (chunks - 1) * (3 + 5 + 32) + 8 * uint64_t(size);

    if (storedBits <= fixedBits && storedBits <= dynamicBits) {
        size_t at = e.blockStart;
        for (size_t c = 0; c < chunks; ++c) {
            const uint32_t length = uint32_t(end - at < kStoredMax ? end - at : kStoredMax);
            put(e.out, final && c + 1 == chunks ? 1 : 0, 1);
            put(e.out, 0, 2);
            alignToByte(e.out);
            put(e.out, length, 16);
            put(e.out, ~length & 0xffff, 16);
            if (length != 0)
                prMemCopy(e.out.out + e.out.at, e.input.data + at, length);
            e.out.at += length;
            at += length;
        }
    } else if (fixedBits <= dynamicBits) {
        Code literals[288];
        Code distances[kDistanceSymbols];
        buildCodes(kFixedLengths.literals, 288, literals);
        buildCodes(kFixedLengths.distances, kDistanceSymbols, distances);
        put(e.out, final ? 1 : 0, 1);
        put(e.out, 1, 2);
        writeTokens(e, literals, distances);
    } else {
        Code literals[kLiteralSymbols];
        Code distances[kDistanceSymbols];
        Code lengthCodes[kCodeLengthSymbols];
        buildCodes(lengths, kLiteralSymbols, literals);
        buildCodes(distanceLengths, kDistanceSymbols, distances);
        buildCodes(lengthLengths, kCodeLengthSymbols, lengthCodes);
        put(e.out, final ? 1 : 0, 1);
        put(e.out, 2, 2);
        put(e.out, literalCount - 257, 5);
        put(e.out, distanceCount - 1, 5);
        put(e.out, lengthCount - 4, 4);
        for (uint32_t i = 0; i < lengthCount; ++i)
            put(e.out, lengthLengths[kCodeLengthOrder[i]], 3);
        for (uint32_t i = 0; i < lengthTokenCount; ++i) {
            const LengthToken token = lengthTokens[i];
            putCode(e.out, lengthCodes[token.symbol]);
            if (token.symbol >= 16)
                put(e.out, token.extra, kRepeatExtra[token.symbol - 16]);
        }
        writeTokens(e, literals, distances);
    }

    e.tokenCount = 0;
    prMemZero(e.literalFrequencies, sizeof(e.literalFrequencies));
    prMemZero(e.distanceFrequencies, sizeof(e.distanceFrequencies));
    e.blockStart = end;
}

// Writes the Deflate data of the input as literals and matches. Matches are found greedily, or with each match deferred
// while the next position has a longer one.
void encodeData(Encoder& e) {
    const size_t size = e.input.size;
    const uint8_t* in = e.input.data;
    size_t position = 0;
    while (position < size) {
        uint32_t distance = 0;
        uint32_t length = findMatch(e, position, 0, &distance);
        insert(e, position);
        if (length == 0) {
            addToken(e, Token{in[position], 0}, position + 1);
            ++position;
            continue;
        }
        while (length < e.effort.lazy && position + 1 < size) {
            uint32_t laterDistance = 0;
            const uint32_t later = findMatch(e, position + 1, length, &laterDistance);
            if (later == 0)
                break;
            addToken(e, Token{in[position], 0}, position + 1);
            ++position;
            insert(e, position);
            length = later;
            distance = laterDistance;
        }
        for (size_t covered = position + 1; covered < position + length; ++covered)
            insert(e, covered);
        position += length;
        addToken(e, Token{uint16_t(length), uint16_t(distance)}, position);
    }
    writeBlock(e, size, true);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

struct PrDeflateDecoder : Deflate {};

PrDeflateDecoder* prDeflateDecoderCreate(void) {
    PrDeflateDecoder* decoder = allocZeroed<PrDeflateDecoder>();
    resetDeflate(*decoder);
    return decoder;
}

void prDeflateDecoderReset(PrDeflateDecoder* decoder) {
    resetDeflate(*decoder);
}

void prDeflateDecoderDestroy(PrDeflateDecoder* PR_NULLABLE decoder) {
    prMemoryFree(decoder);
}

PrStatus prDeflateDecoderRead(PrDeflateDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
        size_t capacity, PrDeflateRead* out) {
    PR_ASSERT(input.data != nullptr || input.size == 0);
    PR_ASSERT(output != nullptr || capacity == 0);
    InflateSpan span{input, 0, output, capacity, 0, 0};
    InflateResult result = readDeflate(*decoder, span);
    if (result == InflateResult_Input && final)
        result = InflateResult_Truncated;
    if (result == InflateResult_Invalid || result == InflateResult_Truncated)
        decoder->phase = InflatePhase_Invalid;
    *out = PrDeflateRead{.consumed = span.consumed, .written = span.written};
    switch (result) {
        case InflateResult_Input: out->progress = PrDeflateProgress_Input; return PrStatus_Ok;
        case InflateResult_Output: out->progress = PrDeflateProgress_Output; return PrStatus_Ok;
        case InflateResult_Done: out->progress = PrDeflateProgress_Done; return PrStatus_Ok;
        case InflateResult_Truncated: return PrStatus_Truncated;
        default: return PrStatus_InvalidData;
    }
}

size_t prDeflateEncodeBound(size_t size) {
    // Each block takes no more space than it would if stored, and each block other than the last covers at least
    // kBlockTokens bytes. A stored block of up to kStoredMax bytes adds at most six bytes: its header, the padding bits
    // to the byte boundary, and its length and complement.
    const size_t blocks = size / kBlockTokens + 1;
    return size + 6 * (size / kStoredMax + blocks) + 1;
}

size_t prDeflateEncode(PrDeflateLevel level, PrByteView input, uint8_t* output) {
    PR_ASSERT(level < countOf(kEfforts));
    Encoder* e = alloc<Encoder>();
    e->input = input;
    e->effort = kEfforts[level];
    prMemFill(e->heads, 0xff, sizeof(e->heads));
    e->tokenCount = 0;
    prMemZero(e->literalFrequencies, sizeof(e->literalFrequencies));
    prMemZero(e->distanceFrequencies, sizeof(e->distanceFrequencies));
    e->blockStart = 0;
    e->out = BitWriter{output, 0, 0, 0};
    encodeData(*e);
    alignToByte(e->out);
    const size_t at = e->out.at;
    prMemoryFree(e);
    PR_ASSERT(at <= prDeflateEncodeBound(input.size));
    return at;
}
