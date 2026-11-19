// Decoding the streams of meshopt compression (EXT_meshopt_compression, KHR_meshopt_compression).
//
// The extensions' bitstreams, as their appendices define them: the attribute codec in version 0 and version 1, the
// triangle codec, the index codec, and the four filters. Every read is checked against the end of the stream, so a
// stream from outside the program is never read past its end, and a stream that the extensions would call invalid fails
// rather than decoding to something.

#include "gltf/src/gltf_meshopt.h"

#include "pr/bit_cast.h"
#include "pr/float.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// The first byte of each kind of stream, whose low four bits are its version.
constexpr uint8_t kAttributesV0 = 0xa0;
constexpr uint8_t kAttributesV1 = 0xa1;
constexpr uint8_t kTriangles = 0xe1;
constexpr uint8_t kIndices = 0xd1;

// The attribute codec decodes blocks of at most this many bytes or elements, in groups of 16 elements, and its tail is
// padded to at least these sizes.
constexpr uint32_t kBlockBytes = 8192;
constexpr uint32_t kBlockElements = 256;
constexpr uint32_t kGroup = 16;
constexpr uint32_t kTailMinV0 = 32;
constexpr uint32_t kTailMinV1 = 24;

// The triangle codec's lookup table at the end of its stream, and the index codec's tail.
constexpr uint32_t kCodeauxSize = 16;
constexpr uint32_t kIndicesTail = 4;

// A stream's bytes from `at` to `end`, which a read never passes.
struct Stream {
    const uint8_t* at;
    const uint8_t* end;

    uint32_t left() const { return uint32_t(end - at); }
};

// ---------------------------------------------------------------------------------------------------------------------
// Attributes

// Decodes the deltas of one group of 16 bytes in a sentinel encoding of `bits` bits, or stored whole for 8 bits, or
// zero for 0 bits. A delta whose bits are all set is a sentinel, whose value is a byte after the group's packed deltas.
bool decodeGroup(Stream* stream, uint32_t bits, uint8_t* out) {
    if (bits == 0) {
        prMemZero(out, kGroup);
        return true;
    }
    uint32_t packed = kGroup * bits / 8;
    if (stream->left() < packed)
        return false;
    const uint8_t* bytes = stream->at;
    stream->at += packed;
    if (bits == 8) {
        prMemCopy(out, bytes, kGroup);
        return true;
    }
    uint32_t sentinel = (1u << bits) - 1;
    uint32_t perByte = 8 / bits;
    for (uint32_t i = 0; i < kGroup; ++i) {
        uint32_t byte = bytes[i / perByte];
        uint32_t slot = i % perByte;
        // One-bit deltas are packed from the least significant bit, and wider ones from the most significant.
        uint32_t shift = bits == 1 ? slot : 8 - bits * (slot + 1);
        uint32_t delta = (byte >> shift) & sentinel;
        if (delta == sentinel) {
            if (stream->left() < 1)
                return false;
            delta = *stream->at++;
        }
        out[i] = uint8_t(delta);
    }
    return true;
}

// Decodes the deltas of one byte of each of `count` elements, in groups whose bit lengths `bits` gives by the two bits
// of the group's header.
bool decodeBytes(Stream* stream, uint32_t count, const uint32_t bits[4], uint8_t* out) {
    uint32_t groups = (count + kGroup - 1) / kGroup;
    uint32_t headerSize = (groups + 3) / 4;
    if (stream->left() < headerSize)
        return false;
    const uint8_t* header = stream->at;
    stream->at += headerSize;
    for (uint32_t g = 0; g < groups; ++g) {
        uint32_t mode = (header[g / 4] >> ((g % 4) * 2)) & 3;
        if (!decodeGroup(stream, bits[mode], out + g * kGroup))
            return false;
    }
    return true;
}

uint32_t rotateRight(uint32_t value, uint32_t bits) {
    return bits == 0 ? value : (value >> bits) | (value << (32 - bits));
}

// Decodes one block of `count` elements of `stride` bytes into `out`, from the elements before it, `last`, which it
// then sets to its own last element. `channels` holds the mode of each four bytes, in version 1.
bool decodeBlock(Stream* stream, uint32_t count, uint32_t stride, bool v1, const uint8_t* PR_NULLABLE channels,
        uint8_t* last, uint8_t* out) {
    static const uint32_t kBitsV0[4] = {0, 2, 4, 8};
    static const uint32_t kBitsV1[5] = {0, 1, 2, 4, 8};
    // The deltas of the four bytes of one channel, a byte after another, each for every element of the block.
    uint8_t deltas[4][kBlockElements + kGroup];
    const uint8_t* control = stream->at;
    if (v1) {
        if (stream->left() < stride / 4)
            return false;
        stream->at += stride / 4;
    }
    for (uint32_t k = 0; k < stride; k += 4) {
        for (uint32_t j = 0; j < 4; ++j) {
            uint32_t mode = v1 ? (control[k / 4] >> (j * 2)) & 3 : 0;
            if (mode == 2) {
                prMemZero(deltas[j], count);
            } else if (mode == 3) {
                // The deltas stored as they are.
                if (stream->left() < count)
                    return false;
                prMemCopy(deltas[j], stream->at, count);
                stream->at += count;
            } else if (!decodeBytes(stream, count, v1 ? kBitsV1 + mode : kBitsV0, deltas[j])) {
                return false;
            }
        }
        uint8_t channel = v1 ? channels[k / 4] : 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* element = out + size_t(i) * stride + k;
            const uint8_t* previous = i == 0 ? last + k : element - stride;
            switch (channel & 15) {
                case 0:
                    // A byte's difference from the byte before it, zigzag encoded.
                    for (uint32_t j = 0; j < 4; ++j) {
                        uint8_t d = deltas[j][i];
                        uint8_t delta = (d & 1) != 0 ? uint8_t(~(d >> 1)) : uint8_t(d >> 1);
                        element[j] = uint8_t(previous[j] + delta);
                    }
                    break;
                case 1:
                    // Two bytes' difference, zigzag encoded, little-endian.
                    for (uint32_t j = 0; j < 4; j += 2) {
                        uint16_t d = uint16_t(deltas[j][i] | deltas[j + 1][i] << 8);
                        uint16_t delta = (d & 1) != 0 ? uint16_t(~(d >> 1)) : uint16_t(d >> 1);
                        uint16_t value = uint16_t(prMemReadLeU16(previous + j) + delta);
                        element[j] = uint8_t(value);
                        element[j + 1] = uint8_t(value >> 8);
                    }
                    break;
                default: {
                    // Four bytes' exclusive or with those before them, rotated left by the channel's high bits.
                    uint32_t d = uint32_t(deltas[0][i]) | uint32_t(deltas[1][i]) << 8 | uint32_t(deltas[2][i]) << 16 |
                        uint32_t(deltas[3][i]) << 24;
                    uint32_t value = rotateRight(d, uint32_t(channel >> 4)) ^ prMemReadLeU32(previous);
                    for (uint32_t j = 0; j < 4; ++j)
                        element[j] = uint8_t(value >> (8 * j));
                    break;
                }
            }
        }
    }
    prMemCopy(last, out + size_t(count - 1) * stride, stride);
    return true;
}

bool decodeAttributes(PrByteView source, uint32_t count, uint32_t stride, bool allowV1, uint8_t* out) {
    if (source.size < 1 || stride == 0 || stride % 4 != 0 || stride > 256)
        return false;
    bool v1 = source.data[0] == kAttributesV1;
    if (source.data[0] != kAttributesV0 && !(allowV1 && v1))
        return false;
    // The tail, the baseline element and in version 1 the channels' modes, padded to its least size.
    uint32_t tailSize = stride + (v1 ? stride / 4 : 0);
    uint32_t tailMin = v1 ? kTailMinV1 : kTailMinV0;
    uint32_t tailPadded = tailSize > tailMin ? tailSize : tailMin;
    if (source.size - 1 < tailPadded)
        return false;
    const uint8_t* tail = source.data + source.size - tailSize;
    const uint8_t* channels = v1 ? tail + stride : nullptr;
    for (uint32_t c = 0; v1 && c < stride / 4; ++c) {
        // Modes 0 and 1 use no rotation, and there are three modes.
        uint32_t mode = channels[c] & 15;
        if (mode >= 3 || (mode < 2 && (channels[c] >> 4) != 0))
            return false;
    }
    uint8_t last[256];
    prMemCopy(last, tail, stride);
    Stream stream = {source.data + 1, source.data + source.size - tailPadded};
    uint32_t perBlock = (kBlockBytes / stride) & ~(kGroup - 1);
    perBlock = perBlock < kBlockElements ? perBlock : kBlockElements;
    for (uint32_t first = 0; first < count; first += perBlock) {
        uint32_t block = count - first < perBlock ? count - first : perBlock;
        if (!decodeBlock(&stream, block, stride, v1, channels, last, out + size_t(first) * stride))
            return false;
    }
    // The blocks end where the tail's padding begins.
    return stream.at == stream.end;
}

// ---------------------------------------------------------------------------------------------------------------------
// Indices

// Reads an unsigned LEB128 integer of one to five bytes, of which the last has its high bit clear.
bool readVarint(Stream* stream, uint32_t* out) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        if (stream->left() < 1)
            return false;
        uint8_t byte = *stream->at++;
        value |= uint32_t(byte & 127) << (7 * i);
        if (byte < 128) {
            *out = value;
            return true;
        }
    }
    return false;
}

// Returns the signed value of a zigzag encoded integer.
uint32_t unzigzag(uint32_t value) {
    return (value & 1) != 0 ? ~(value >> 1) : value >> 1;
}

void writeIndex(uint8_t* out, uint32_t stride, uint32_t at, uint32_t index) {
    if (stride == 2)
        prMemWriteLeU16(out + size_t(at) * 2, uint16_t(index));
    else
        prMemWriteLeU32(out + size_t(at) * 4, index);
}

// The state of the triangle codec: its first-in, first-out queues of recent edges and vertices, with the number of
// entries that each has had written, and the next new vertex and the last explicit one.
struct TriangleState {
    uint32_t edges[16][2];
    uint32_t vertices[16];
    uint32_t edgeAt;
    uint32_t vertexAt;
    uint32_t edgeCount;
    uint32_t vertexCount;
    uint32_t next;
    uint32_t last;
};

void pushEdge(TriangleState* s, uint32_t a, uint32_t b) {
    s->edges[s->edgeAt][0] = a;
    s->edges[s->edgeAt][1] = b;
    s->edgeAt = (s->edgeAt + 1) & 15;
    ++s->edgeCount;
}

void pushVertex(TriangleState* s, uint32_t v) {
    s->vertices[s->vertexAt] = v;
    s->vertexAt = (s->vertexAt + 1) & 15;
    ++s->vertexCount;
}

// Reads the edge `back` entries before the most recent, which must have been written.
bool edgeAt(const TriangleState& s, uint32_t back, uint32_t* a, uint32_t* b) {
    if (back >= s.edgeCount)
        return false;
    const uint32_t* edge = s.edges[(s.edgeAt - 1 - back) & 15];
    *a = edge[0];
    *b = edge[1];
    return true;
}

bool vertexAt(const TriangleState& s, uint32_t back, uint32_t* v) {
    if (back >= s.vertexCount)
        return false;
    *v = s.vertices[(s.vertexAt - 1 - back) & 15];
    return true;
}

// Reads a vertex given explicitly, as a delta from the last one.
bool explicitVertex(Stream* stream, TriangleState* s, uint32_t* v) {
    uint32_t encoded = 0;
    if (!readVarint(stream, &encoded))
        return false;
    s->last += unzigzag(encoded);
    *v = s->last;
    return true;
}

bool decodeTriangles(PrByteView source, uint32_t count, uint32_t stride, uint8_t* out) {
    uint32_t triangles = count / 3;
    if (count % 3 != 0 || (stride != 2 && stride != 4) || source.size < 1 + size_t(triangles) + kCodeauxSize ||
            source.data[0] != kTriangles)
        return false;
    const uint8_t* codes = source.data + 1;
    const uint8_t* codeaux = source.data + source.size - kCodeauxSize;
    // The table's last two entries are zero, and no entry has a nibble of 15.
    if (codeaux[14] != 0 || codeaux[15] != 0)
        return false;
    for (uint32_t i = 0; i < kCodeauxSize; ++i) {
        if ((codeaux[i] >> 4) == 15 || (codeaux[i] & 15) == 15)
            return false;
    }
    Stream data = {codes + triangles, codeaux};
    TriangleState s = {};
    for (uint32_t t = 0; t < triangles; ++t) {
        uint32_t code = codes[t];
        uint32_t high = code >> 4;
        uint32_t low = code & 15;
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        if (high < 15) {
            // A recent edge and a third vertex: the next new one, a recent one, one beside the last, or an explicit
            // one.
            if (!edgeAt(s, high, &a, &b))
                return false;
            if (low == 0) {
                c = s.next++;
            } else if (low < 13) {
                if (!vertexAt(s, low, &c))
                    return false;
            } else if (low < 15) {
                c = low == 13 ? s.last - 1 : s.last + 1;
                s.last = c;
            } else if (!explicitVertex(&data, &s, &c)) {
                return false;
            }
            if (low == 0 || low >= 13)
                pushVertex(&s, c);
            pushEdge(&s, c, b);
            pushEdge(&s, a, c);
        } else {
            // Three vertices: the next new one or an explicit one, then for each of the others the next new one, a
            // recent one, or an explicit one, as the table or the next byte says.
            uint32_t aux = 0;
            bool fromTable = low < 14;
            if (fromTable) {
                aux = codeaux[low];
            } else {
                if (data.left() < 1)
                    return false;
                aux = *data.at++;
                if (aux == 0)
                    s.next = 0;
            }
            uint32_t z = aux >> 4;
            uint32_t w = aux & 15;
            if (low == 15) {
                if (!explicitVertex(&data, &s, &a))
                    return false;
            } else {
                a = s.next++;
            }
            if (z == 0)
                b = s.next++;
            else if (z < 15 && !vertexAt(s, z - 1, &b))
                return false;
            else if (z == 15 && !explicitVertex(&data, &s, &b))
                return false;
            if (w == 0)
                c = s.next++;
            else if (w < 15 && !vertexAt(s, w - 1, &c))
                return false;
            else if (w == 15 && !explicitVertex(&data, &s, &c))
                return false;
            pushVertex(&s, a);
            if (z == 0 || z == 15)
                pushVertex(&s, b);
            if (w == 0 || w == 15)
                pushVertex(&s, c);
            pushEdge(&s, b, a);
            pushEdge(&s, c, b);
            pushEdge(&s, a, c);
        }
        writeIndex(out, stride, t * 3, a);
        writeIndex(out, stride, t * 3 + 1, b);
        writeIndex(out, stride, t * 3 + 2, c);
    }
    // The extra data ends where the table begins.
    return data.at == data.end;
}

bool decodeIndices(PrByteView source, uint32_t count, uint32_t stride, uint8_t* out) {
    if ((stride != 2 && stride != 4) || source.size < 1 + kIndicesTail || source.data[0] != kIndices)
        return false;
    Stream data = {source.data + 1, source.data + source.size - kIndicesTail};
    // Each index is a delta from one of two baselines, which its low bit names.
    uint32_t last[2] = {};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t value = 0;
        if (!readVarint(&data, &value))
            return false;
        uint32_t baseline = value & 1;
        uint32_t delta = (value & 2) != 0 ? ~(value >> 2) : value >> 2;
        last[baseline] += delta;
        writeIndex(out, stride, i, last[baseline]);
    }
    return data.at == data.end;
}

// ---------------------------------------------------------------------------------------------------------------------
// Filters

// Reads and writes the components of a filter's elements: signed or unsigned integers of 8 or 16 bits.
int32_t readSigned(const uint8_t* at, bool wide) {
    return wide ? int32_t(int16_t(prMemReadLeU16(at))) : int32_t(int8_t(at[0]));
}

uint32_t readUnsigned(const uint8_t* at, bool wide) {
    return wide ? prMemReadLeU16(at) : at[0];
}

// Writes `value` rounded to the nearest integer, halves away from zero, clamped to [low, high]. A NaN, which input that
// the extension leaves unspecified can give, writes zero.
void writeRounded(uint8_t* at, bool wide, float value, float low, float high) {
    float rounded = prF32IsNan(value) ? 0 : prF32Clamp(prF32Round(value), low, high);
    int32_t integer = int32_t(rounded);
    if (wide)
        prMemWriteLeU16(at, uint16_t(integer));
    else
        at[0] = uint8_t(integer);
}

// Returns the magnitude of `magnitude` with the sign of `sign`, whose sign bit decides, as C's copysign.
float copySign(float magnitude, float sign) {
    uint32_t bits = (bitCast<uint32_t>(magnitude) & 0x7fffffffu) | (bitCast<uint32_t>(sign) & 0x80000000u);
    return bitCast<float>(bits);
}

// Unit vectors from octahedral form, in 8-bit or 16-bit signed components.
void filterOctahedral(uint8_t* data, uint32_t count, uint32_t stride) {
    bool wide = stride == 8;
    uint32_t size = wide ? 2 : 1;
    float most = wide ? 32767.0f : 127.0f;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* e = data + size_t(i) * stride;
        float one = float(readSigned(e + 2 * size, wide));
        float x = float(readSigned(e, wide)) / one;
        float y = float(readSigned(e + size, wide)) / one;
        float z = 1 - prF32Abs(x) - prF32Abs(y);
        // The negative hemisphere folds back: each of x and y less the magnitude of t with its own sign.
        float t = prF32Min(z, 0);
        x -= copySign(t, x);
        y -= copySign(t, y);
        float length = prF32Sqrt(x * x + y * y + z * z);
        writeRounded(e, wide, x / length * most, -most, most);
        writeRounded(e + size, wide, y / length * most, -most, most);
        writeRounded(e + 2 * size, wide, z / length * most, -most, most);
    }
}

// Unit quaternions from their three smallest components, in 16-bit signed components.
void filterQuaternion(uint8_t* data, uint32_t count, uint32_t stride) {
    constexpr float kRange = 0.70710678f;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* e = data + size_t(i) * stride;
        int32_t last = readSigned(e + 6, true);
        float one = float(last | 3);
        float x = float(readSigned(e, true)) / one * kRange;
        float y = float(readSigned(e + 2, true)) / one * kRange;
        float z = float(readSigned(e + 4, true)) / one * kRange;
        float w = prF32Sqrt(prF32Max(0, 1 - x * x - y * y - z * z));
        uint32_t largest = uint32_t(last) & 3;
        const float values[4] = {x, y, z, w};
        for (uint32_t k = 0; k < 4; ++k) {
            // The largest component is last, and the others follow it in turn.
            uint32_t at = (largest + 1 + k) % 4;
            writeRounded(e + 2 * at, true, values[k] * 32767.0f, -32767.0f, 32767.0f);
        }
    }
}

// Floats from a signed exponent in the high byte and a signed mantissa in the low 24 bits, exactly.
void filterExponential(uint8_t* data, uint32_t count, uint32_t stride) {
    for (size_t at = 0; at + 4 <= size_t(count) * stride; at += 4) {
        int32_t input = int32_t(prMemReadLeU32(data + at));
        int32_t exponent = input >> 24;
        int32_t mantissa = int32_t(uint32_t(input) << 8) >> 8;
        // A double holds 2 to any exponent of a byte, and the product rounds once, to the float.
        uint64_t scaleBits = uint64_t(exponent + 1023) << 52;
        double value = double(mantissa) * bitCast<double>(scaleBits);
        prMemWriteLeU32(data + at, bitCast<uint32_t>(float(value)));
    }
}

// Colors from YCoCg, with alpha whose high bit gives the number of bits, in 8-bit or 16-bit unsigned components.
void filterColor(uint8_t* data, uint32_t count, uint32_t stride) {
    bool wide = stride == 8;
    uint32_t size = wide ? 2 : 1;
    float most = wide ? 65535.0f : 255.0f;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* e = data + size_t(i) * stride;
        uint32_t alpha = readUnsigned(e + 3 * size, wide);
        int32_t msb = -1;
        for (int32_t bit = int32_t(size * 8) - 1; bit >= 0 && msb < 0; --bit) {
            if ((alpha >> bit) & 1)
                msb = bit;
        }
        int32_t scale = (1 << (msb + 1)) - 1;
        int32_t y = int32_t(readUnsigned(e, wide));
        int32_t co = readSigned(e + size, wide);
        int32_t cg = readSigned(e + 2 * size, wide);
        int32_t a = int32_t(alpha) & (scale >> 1);
        a = (a << 1) | (a & 1);
        const int32_t values[4] = {y + co - cg, y + cg, y - co - cg, a};
        // An alpha without its marker bit, which the extension leaves unspecified, scales by nothing.
        float factor = scale > 0 ? most / float(scale) : 0;
        for (uint32_t k = 0; k < 4; ++k)
            writeRounded(e + k * size, wide, float(values[k]) * factor, 0, most);
    }
}

} // namespace

bool gltfMeshoptDecode(PrByteView source, const PrGltfMeshopt& meshopt, uint8_t* out) {
    uint32_t count = meshopt.count;
    uint32_t stride = meshopt.byteStride;
    switch (meshopt.mode) {
        case PrGltfMeshoptMode_Triangles:
            return meshopt.filter == PrGltfMeshoptFilter_None && decodeTriangles(source, count, stride, out);
        case PrGltfMeshoptMode_Indices:
            return meshopt.filter == PrGltfMeshoptFilter_None && decodeIndices(source, count, stride, out);
        default:
            break;
    }
    if (!decodeAttributes(source, count, stride, meshopt.v1, out))
        return false;
    switch (meshopt.filter) {
        case PrGltfMeshoptFilter_Octahedral:
            if (stride != 4 && stride != 8)
                return false;
            filterOctahedral(out, count, stride);
            break;
        case PrGltfMeshoptFilter_Quaternion:
            if (stride != 8)
                return false;
            filterQuaternion(out, count, stride);
            break;
        case PrGltfMeshoptFilter_Exponential:
            filterExponential(out, count, stride);
            break;
        case PrGltfMeshoptFilter_Color:
            if (!meshopt.v1 || (stride != 4 && stride != 8))
                return false;
            filterColor(out, count, stride);
            break;
        default:
            break;
    }
    return true;
}

} // namespace pr_impl
