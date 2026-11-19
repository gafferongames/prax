// Reading a Draco stream: its variable-length integers and its entropy coding.

#include "draco/src/draco_stream.h"

namespace pr_impl {

namespace {

// The state that a rANS decoder of bits starts from, and the base that it renormalizes to (the bitstream's L_RANS_BASE
// and IO_BASE).
constexpr uint32_t kAnsBitsBase = 4096;
constexpr uint32_t kAnsIoBase = 256;

// The symbols of tagged coding are numbers of bits, which five bits hold.
constexpr uint32_t kTagBitLength = 5;

// The greatest number of bits of the symbols that raw coding codes.
constexpr uint32_t kRawMaxBitLength = 18;

// One symbol of a rANS alphabet: its probability and the sum of those before it, in units of the precision.
struct AnsSymbol {
    uint32_t probability;
    uint32_t cumulative;
};

// What a remainder of the precision gives the decoder's next state: the probability of the symbol whose range holds it,
// and the remainder less the sum of the probabilities before the symbol. The next state depends on these alone, so a
// table of them by remainder takes the lookup of the symbol out of the chain from one state to the next.
struct AnsSlot {
    uint32_t probability;
    uint32_t offset;
};

// The greatest precision, in bits, that a table of slots is made for: 256 KiB, which stays in a core's cache.
constexpr uint32_t kSlotPrecisionBits = 15;

// A decoder of symbols coded with rANS over an alphabet whose probabilities the stream gives (the bitstream's
// RansRead).
struct AnsSymbols {
    RawArray<AnsSymbol> symbols;
    RawArray<uint32_t> lookup;          // by a remainder of the precision: the symbol whose range holds it
    RawArray<AnsSlot> slots;            // by a remainder of the precision, if the precision is small
    uint32_t precision;
    uint32_t base;                      // the least state of a decoder that has bytes left
    const uint8_t* data;
    uint32_t offset;
    uint32_t state;
};

void ansSymbolsReset(AnsSymbols* ans) {
    ans->symbols.reset();
    ans->lookup.reset();
    ans->slots.reset();
}

// Returns the bits of precision of the decoder of symbols of `bitLength` bits.
uint32_t ansPrecisionBits(uint32_t bitLength) {
    uint32_t bits = 3 * bitLength / 2;
    return bits < 12 ? 12 : bits > 20 ? 20 : bits;
}

// Reads the alphabet of a decoder: the number of symbols and the probability of each, which sum to the precision (the
// bitstream's BuildSymbolTables). `decoded` is the number of symbols that the decoder is to decode.
bool ansSymbolsCreate(DracoReader* reader, uint32_t precisionBits, uint32_t decoded, AnsSymbols* ans) {
    ans->precision = 1u << precisionBits;
    ans->base = ans->precision * 4;
    uint32_t count = 0;
    if (!dracoReadVarint32(reader, &count))
        return false;
    // A symbol takes a byte, or a share of a byte that skips up to 64 symbols of no probability. An alphabet of no
    // symbols codes nothing.
    if (count == 0 || count / 64 > reader->left())
        return false;
    ans->symbols.resizeZeroed(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t data = 0;
        if (!dracoReadU8(reader, &data))
            return false;
        uint32_t token = data & 3;
        if (token == 3) {
            // The symbol and the `data >> 2` after it have no probability.
            uint32_t skipped = data >> 2;
            if (skipped >= count - i)
                return false;
            i += skipped;
            continue;
        }
        uint32_t probability = data >> 2;
        for (uint32_t b = 0; b < token; ++b) {
            uint8_t extra = 0;
            if (!dracoReadU8(reader, &extra))
                return false;
            probability |= uint32_t(extra) << (8 * (b + 1) - 2);
        }
        ans->symbols.data[i].probability = probability;
    }

    ans->lookup.resize(ans->precision);
    uint64_t cumulative = 0;
    for (uint32_t i = 0; i < count; ++i) {
        AnsSymbol& symbol = ans->symbols.data[i];
        uint64_t end = cumulative + symbol.probability;
        if (end > ans->precision)
            return false;
        symbol.cumulative = uint32_t(cumulative);
        for (uint64_t at = cumulative; at < end; ++at)
            ans->lookup.data[at] = i;
        cumulative = end;
    }
    if (cumulative != ans->precision)
        return false;
    // The table of slots is worth making for as many symbols as it has entries.
    if (precisionBits <= kSlotPrecisionBits && decoded >= ans->precision) {
        ans->slots.resize(ans->precision);
        for (uint32_t at = 0; at < ans->precision; ++at) {
            const AnsSymbol& symbol = ans->symbols.data[ans->lookup.data[at]];
            ans->slots.data[at] = AnsSlot{symbol.probability, at - symbol.cumulative};
        }
    }
    return true;
}

// Reads the size of the coded symbols and begins decoding them (the bitstream's RansInitDecoder). The reader moves past
// them.
bool ansSymbolsStart(DracoReader* reader, AnsSymbols* ans) {
    uint64_t size = 0;
    if (!dracoReadVarint64(reader, &size) || size > reader->left() || size < 1 || size > 0x7fffffffu)
        return false;
    const uint8_t* data = reader->at;
    reader->at += size;
    uint32_t offset = uint32_t(size);
    uint32_t tail = data[offset - 1] >> 6;
    if (offset < tail + 1)
        return false;
    offset -= tail + 1;
    uint32_t state = 0;
    for (uint32_t i = tail + 1; i-- > 0;)
        state = state << 8 | data[offset + i];
    state &= (1u << (8 * (tail + 1) - 2)) - 1;
    state += ans->base;
    if (state >= ans->base * kAnsIoBase)
        return false;
    ans->data = data;
    ans->offset = offset;
    ans->state = state;
    return true;
}

// Decodes `count` symbols, `run` at a time: `each` is given the index of the run's first symbol and the symbol, and
// returns false to stop. The decoder's state is kept in registers, and the precision is a power of two, so a symbol
// costs a shift, a mask and two loads.
template <typename Each>
inline bool ansSymbolsDecode(AnsSymbols* ans, uint32_t precisionBits, uint32_t count, uint32_t run, Each each) {
    const AnsSymbol* symbols = ans->symbols.data;
    const uint32_t* lookup = ans->lookup.data;
    const uint8_t* data = ans->data;
    uint32_t base = ans->base;
    uint32_t mask = ans->precision - 1;
    uint32_t state = ans->state;
    uint32_t offset = ans->offset;
    if (ans->slots.count != 0) {
        const AnsSlot* slots = ans->slots.data;
        for (uint32_t i = 0; i < count; i += run) {
            while (state < base && offset > 0)
                state = state * kAnsIoBase + data[--offset];
            uint32_t remainder = state & mask;
            AnsSlot slot = slots[remainder];
            state = (state >> precisionBits) * slot.probability + slot.offset;
            if (!each(i, lookup[remainder]))
                return false;
        }
        return true;
    }
    for (uint32_t i = 0; i < count; i += run) {
        while (state < base && offset > 0)
            state = state * kAnsIoBase + data[--offset];
        uint32_t remainder = state & mask;
        uint32_t value = lookup[remainder];
        AnsSymbol symbol = symbols[value];
        state = (state >> precisionBits) * symbol.probability + remainder - symbol.cumulative;
        if (!each(i, value))
            return false;
    }
    return true;
}

bool decodeTagged(DracoReader* reader, uint32_t count, uint32_t components, uint32_t* out) {
    AnsSymbols tags = {};
    uint32_t precisionBits = ansPrecisionBits(kTagBitLength);
    bool ok = ansSymbolsCreate(reader, precisionBits, count / components, &tags) && ansSymbolsStart(reader, &tags);
    // The symbols are bits after the coded tags, to the end of a byte.
    DracoBits bits = dracoBitsBegin(reader, reader->left());
    ok = ok && ansSymbolsDecode(&tags, precisionBits, count, components, [&](uint32_t i, uint32_t bitLength) {
        // The bits of a run are checked against the end of the sequence once.
        if (bitLength > 32 || uint64_t(bitLength) * components > bits.count - bits.at)
            return false;
        for (uint32_t c = 0; c < components; ++c)
            dracoBitsRead(&bits, bitLength, &out[i + c]);
        return true;
    });
    if (ok)
        reader->at += dracoBitsBytes(&bits);
    ansSymbolsReset(&tags);
    return ok;
}

bool decodeRaw(DracoReader* reader, uint32_t count, uint32_t* out) {
    uint8_t maxBitLength = 0;
    if (!dracoReadU8(reader, &maxBitLength) || maxBitLength < 1 || maxBitLength > kRawMaxBitLength)
        return false;
    AnsSymbols symbols = {};
    uint32_t precisionBits = ansPrecisionBits(maxBitLength);
    bool ok = ansSymbolsCreate(reader, precisionBits, count, &symbols) && ansSymbolsStart(reader, &symbols);
    ok = ok && ansSymbolsDecode(&symbols, precisionBits, count, 1, [&](uint32_t i, uint32_t value) {
        out[i] = value;
        return true;
    });
    ansSymbolsReset(&symbols);
    return ok;
}

template <typename T>
bool readVarint(DracoReader* reader, uint32_t maxBytes, T* out) {
    T value = 0;
    for (uint32_t i = 0; i < maxBytes; ++i) {
        uint8_t byte = 0;
        if (!dracoReadU8(reader, &byte))
            return false;
        value |= T(T(byte & 0x7f) << (7 * i));
        if ((byte & 0x80) == 0) {
            *out = value;
            return true;
        }
    }
    return false;
}

} // namespace

bool dracoReadVarint32(DracoReader* reader, uint32_t* out) {
    return readVarint<uint32_t>(reader, 5, out);
}

bool dracoReadVarint64(DracoReader* reader, uint64_t* out) {
    return readVarint<uint64_t>(reader, 10, out);
}

bool dracoAnsBitsStart(DracoReader* reader, DracoAnsBits* out) {
    uint8_t probabilityOfZero = 0;
    uint32_t size = 0;
    if (!dracoReadU8(reader, &probabilityOfZero) || !dracoReadVarint32(reader, &size) || size > reader->left() || size < 1) {
        return false;
    }
    const uint8_t* data = reader->at;
    reader->at += size;
    // The last byte's high two bits say how many bytes the state takes: one, two or three.
    uint32_t tail = data[size - 1] >> 6;
    if (tail == 3 || size < tail + 1)
        return false;
    uint32_t offset = size - (tail + 1);
    uint32_t state = 0;
    for (uint32_t i = tail + 1; i-- > 0;)
        state = state << 8 | data[offset + i];
    state &= (1u << (8 * (tail + 1) - 2)) - 1;
    state += kAnsBitsBase;
    if (state >= kAnsBitsBase * kAnsIoBase)
        return false;
    out->data = data;
    out->offset = offset;
    out->state = state;
    // The probability of a one is what is left of 256, in eight bits: a probability of zero of 0 leaves none.
    out->probabilityOfOne = uint8_t(256 - probabilityOfZero);
    return true;
}

bool dracoDecodeSymbols(DracoReader* reader, uint32_t count, uint32_t components, uint32_t* out) {
    if (count == 0)
        return true;
    uint8_t scheme = 0;
    if (!dracoReadU8(reader, &scheme))
        return false;
    if (scheme == 0)
        return decodeTagged(reader, count, components, out);
    if (scheme == 1)
        return decodeRaw(reader, count, out);
    return false;
}

} // namespace pr_impl
