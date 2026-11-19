// Reading a Draco stream: its bytes, its variable-length integers, its bit sequences and its entropy coding.
//
// Every read is checked against the end of the bytes that it reads, and returns false there, so a stream from outside
// the program is never read past its end.

#pragma once

#include "pr/base.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Bytes

// A stream's bytes from `at` to `end`, which a read never passes.
struct DracoReader {
    const uint8_t* at;
    const uint8_t* end;

    size_t left() const { return size_t(end - at); }
};

// Read an integer of one, two or four bytes, least significant byte first. Return false at the end of the bytes.
inline bool dracoReadU8(DracoReader* reader, uint8_t* out) {
    if (reader->left() < 1)
        return false;
    *out = *reader->at++;
    return true;
}
inline bool dracoReadU16(DracoReader* reader, uint16_t* out) {
    if (reader->left() < 2)
        return false;
    *out = prMemReadLeU16(reader->at);
    reader->at += 2;
    return true;
}
inline bool dracoReadU32(DracoReader* reader, uint32_t* out) {
    if (reader->left() < 4)
        return false;
    *out = prMemReadLeU32(reader->at);
    reader->at += 4;
    return true;
}

// Reads a variable-length integer: seven bits in each byte from the least significant, and a set high bit on every byte
// but the last. A 32-bit integer has five bytes at most and a 64-bit one ten, and the bits of the last byte that the
// integer has no room for are dropped, as the reference decoder drops them.
bool dracoReadVarint32(DracoReader* reader, uint32_t* out);
bool dracoReadVarint64(DracoReader* reader, uint64_t* out);

// ---------------------------------------------------------------------------------------------------------------------
// Bit sequences

// A sequence of bits within a stream's bytes. The first bit is the least significant of the first byte.
struct DracoBits {
    const uint8_t* data;
    uint64_t count;             // the bits of the sequence
    uint64_t at;                // the bits read
};

// Begins a bit sequence at the reader's position, over `bytes` bytes, which the reader must have.
inline DracoBits dracoBitsBegin(const DracoReader* reader, size_t bytes) {
    return DracoBits{reader->at, uint64_t(bytes) * 8, 0};
}

// Reads `count` bits, at most 32, as an integer whose least significant bit is the first read. Returns false past the
// end of the sequence.
inline bool dracoBitsRead(DracoBits* bits, uint32_t count, uint32_t* out) {
    if (count > 32 || count > bits->count - bits->at)
        return false;
    uint64_t value = 0;
    uint32_t shift = uint32_t(bits->at & 7);
    const uint8_t* bytes = bits->data + (bits->at >> 3);
    uint32_t needed = (shift + count + 7) / 8;
    for (uint32_t i = 0; i < needed; ++i)
        value |= uint64_t(bytes[i]) << (8 * i);
    *out = uint32_t((value >> shift) & ((uint64_t(1) << count) - 1));
    bits->at += count;
    return true;
}

// Returns the number of whole bytes that the bits read take.
inline size_t dracoBitsBytes(const DracoBits* bits) {
    return size_t((bits->at + 7) / 8);
}

// ---------------------------------------------------------------------------------------------------------------------
// Entropy coding

// A decoder of bits coded with rANS at a fixed probability of zero (the bitstream's RabsDescRead). The coded bytes are
// read from their end backward.
struct DracoAnsBits {
    const uint8_t* PR_NULLABLE data;
    uint32_t offset;
    uint32_t state;
    uint32_t probabilityOfOne;
};

// Reads the probability and the size of a run of coded bits from the reader, and begins decoding it. The reader moves
// past the run.
bool dracoAnsBitsStart(DracoReader* reader, DracoAnsBits* out);

// Decodes the next bit. A run that is read past what was coded gives bits of no meaning, and reads nothing past its
// bytes.
inline bool dracoAnsBit(DracoAnsBits* ans) {
    constexpr uint32_t kBase = 4096;
    if (ans->state < kBase && ans->offset > 0)
        ans->state = ans->state * 256 + ans->data[--ans->offset];
    uint32_t state = ans->state;
    uint32_t quotient = state / 256;
    uint32_t remainder = state % 256;
    uint32_t scaled = quotient * ans->probabilityOfOne;
    bool bit = remainder < ans->probabilityOfOne;
    ans->state = bit ? scaled + remainder : state - scaled - ans->probabilityOfOne;
    return bit;
}

// Decodes `count` symbols into `out`, coded as the bitstream's DecodeSymbols gives: tagged, where rANS codes the number
// of bits of each run of `components` symbols and the symbols follow as bits, or raw, where rANS codes the symbols
// themselves. `count` is a multiple of `components`. Nothing is read for no symbols.
bool dracoDecodeSymbols(DracoReader* reader, uint32_t count, uint32_t components, uint32_t* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
