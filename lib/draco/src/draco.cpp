// Decoding a Draco stream: its header and metadata, sequential connectivity, and the values of its attributes.
//
// The stream is the one that the Draco bitstream specification describes, in the order that it describes it. Where the
// specification is unclear or differs from the reference decoder, the stream is read as the reference decoder reads it,
// since a container relies on every decoder giving a mesh's points in the same order.

#include "pr/draco.h"

#include "pr/alloc.h"
#include "pr/bit_cast.h"
#include "pr/float.h"

#include "draco/src/draco_decoder.h"
#include "draco/src/draco_edgebreaker.h"
#include "draco/src/draco_prediction.h"

namespace pr_impl {

namespace {

// The version of the bitstream that the specification describes, and the minor version that the reference encoder gives
// a point cloud.
constexpr uint8_t kVersionMajor = 2;
constexpr uint8_t kVersionMinor = 2;
constexpr uint8_t kVersionMinorPointCloud = 3;

// The flag of a stream's header that says metadata follows it.
constexpr uint16_t kFlagMetadata = 0x8000;

// The deepest that metadata nests, which is the reference decoder's limit.
constexpr uint32_t kMetadataMaxDepth = 1000;

// The bitstream's number for no prediction.
constexpr int kPredictionNone = -2;

// The numbers of bits that the transforms take.
constexpr uint32_t kQuantizationMaxBits = 30;
constexpr uint32_t kOctahedralMinBits = 2;

bool isInteger(PrDracoComponentType type) {
    return type <= PrDracoComponentType_Uint32;
}

// ---------------------------------------------------------------------------------------------------------------------
// Header and metadata

bool readHeader(Draco* draco, uint16_t* flags) {
    DracoReader* reader = &draco->reader;
    if (reader->left() < 5 || !prMemEquals(reader->at, "DRACO", 5))
        return false;
    reader->at += 5;
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t type = 0;
    uint8_t method = 0;
    if (!dracoReadU8(reader, &major) || !dracoReadU8(reader, &minor) || !dracoReadU8(reader, &type) ||
            !dracoReadU8(reader, &method) || !dracoReadU16(reader, flags)) {
        return false;
    }
    if (type > PrDracoGeometry_TriangleMesh || method > DracoMethod_Edgebreaker)
        return false;
    draco->geometry = type;
    draco->method = method;
    bool pointCloud = type == PrDracoGeometry_PointCloud;
    if (major != kVersionMajor || (minor != kVersionMinor && !(pointCloud && minor == kVersionMinorPointCloud)))
        return dracoFail(draco, PrStatus_NotSupported);
    if (pointCloud && method != DracoMethod_Sequential)
        return dracoFail(draco, PrStatus_NotSupported);
    return true;
}

// Reads a name of metadata: a length in a byte, and its bytes.
bool skipName(DracoReader* reader) {
    uint8_t size = 0;
    if (!dracoReadU8(reader, &size) || size > reader->left())
        return false;
    reader->at += size;
    return true;
}

// Reads one element of metadata, which nests: its entries, each a name and a value, and its children, each a name and
// an element. The geometry keeps none of it. A value has at least one byte, and its size is a variable-length integer,
// as the reference decoder reads it.
bool skipMetadata(DracoReader* reader) {
    // The children yet to read at each depth.
    RawArray<uint32_t> pending;
    bool ok = true;
    bool named = false;
    for (;;) {
        if (named && !skipName(reader)) {
            ok = false;
            break;
        }
        uint32_t entries = 0;
        if (!dracoReadVarint32(reader, &entries)) {
            ok = false;
            break;
        }
        for (uint32_t i = 0; i < entries && ok; ++i) {
            uint32_t size = 0;
            ok = skipName(reader) && dracoReadVarint32(reader, &size) && size != 0 && size <= reader->left();
            if (ok)
                reader->at += size;
        }
        uint32_t children = 0;
        if (!ok || !dracoReadVarint32(reader, &children) || children > reader->left() ||
                pending.count >= kMetadataMaxDepth) {
            ok = false;
            break;
        }
        pending.push(children);
        while (pending.count != 0 && pending.last() == 0)
            pending.pop();
        if (pending.count == 0)
            break;
        --pending.last();
        named = true;
    }
    pending.reset();
    return ok;
}

// Reads the metadata of the attributes, each under the id of its attribute, and then that of the geometry.
bool readMetadata(Draco* draco) {
    DracoReader* reader = &draco->reader;
    uint32_t count = 0;
    if (!dracoReadVarint32(reader, &count))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        if (!dracoReadVarint32(reader, &id) || !skipMetadata(reader))
            return false;
    }
    return skipMetadata(reader);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sequential connectivity

bool checkLimits(Draco* draco, uint64_t points, uint64_t faces) {
    if (points > draco->limits.maxPoints || faces > draco->limits.maxFaces)
        return dracoFail(draco, PrStatus_Limit);
    return true;
}

// Reads the faces of a mesh whose indices are stored in order, as differences that rANS codes or as integers of the
// least size that holds a point (the bitstream's Sequential Connectivity Decoder).
bool decodeSequentialFaces(Draco* draco) {
    DracoReader* reader = &draco->reader;
    uint32_t faceCount = 0;
    uint32_t pointCount = 0;
    if (!dracoReadVarint32(reader, &faceCount) || !dracoReadVarint32(reader, &pointCount))
        return false;
    // A face takes three bytes at least, which is the reference decoder's check too.
    if (faceCount > reader->left() / 3)
        return false;
    if (!checkLimits(draco, pointCount, faceCount))
        return false;
    uint8_t method = 0;
    if (!dracoReadU8(reader, &method))
        return false;
    uint32_t count = faceCount * 3;
    draco->pointCount = pointCount;
    draco->faceCount = faceCount;
    draco->faces.resize(count);
    uint32_t* faces = draco->faces.data;
    if (method == 0) {
        // Each index is its difference from the one before, with its sign in the low bit.
        if (!dracoDecodeSymbols(reader, count, 1, faces))
            return false;
        uint32_t last = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t magnitude = faces[i] >> 1;
            if ((faces[i] & 1) != 0) {
                if (magnitude > last)
                    return false;
                last -= magnitude;
            } else {
                if (magnitude > 0x7fffffffu - last)
                    return false;
                last += magnitude;
            }
            faces[i] = last;
        }
    } else if (pointCount < 256) {
        if (reader->left() < count)
            return false;
        for (uint32_t i = 0; i < count; ++i)
            faces[i] = *reader->at++;
    } else if (pointCount < (1u << 16)) {
        if (reader->left() / 2 < count)
            return false;
        for (uint32_t i = 0; i < count; ++i, reader->at += 2)
            faces[i] = prMemReadLeU16(reader->at);
    } else if (pointCount < (1u << 21)) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!dracoReadVarint32(reader, &faces[i]))
                return false;
        }
    } else {
        if (reader->left() / 4 < count)
            return false;
        for (uint32_t i = 0; i < count; ++i, reader->at += 4)
            faces[i] = prMemReadLeU32(reader->at);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (faces[i] >= pointCount)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Attributes

// Reads the attributes of one group: what each is, and then how each is coded (the bitstream's
// ParseAttributeDecodersData).
bool readAttributes(Draco* draco, uint32_t sequence) {
    DracoReader* reader = &draco->reader;
    uint32_t count = 0;
    if (!dracoReadVarint32(reader, &count) || count == 0)
        return false;
    uint32_t first = draco->attributes.count;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t kind = 0;
        uint8_t dataType = 0;
        uint8_t componentCount = 0;
        uint8_t normalized = 0;
        uint32_t id = 0;
        if (!dracoReadU8(reader, &kind) || !dracoReadU8(reader, &dataType) || !dracoReadU8(reader, &componentCount) ||
                !dracoReadU8(reader, &normalized) || !dracoReadVarint32(reader, &id)) {
            return false;
        }
        // The bitstream numbers the types of components from one.
        if (kind > PrDracoAttributeKind_Generic || dataType < 1 || dataType > PrDracoComponentType_Bool + 1 ||
                componentCount == 0) {
            return false;
        }
        DracoAttributeState attribute = {};
        attribute.info.id = id;
        attribute.info.kind = kind;
        attribute.info.componentType = PrDracoComponentType(dataType - 1);
        attribute.info.componentCount = componentCount;
        attribute.info.normalized = normalized != 0;
        attribute.sequence = sequence;
        draco->attributes.push(attribute);
    }
    for (uint32_t i = 0; i < count; ++i) {
        DracoAttributeState& attribute = draco->attributes.data[first + i];
        uint8_t coding = 0;
        if (!dracoReadU8(reader, &coding) || coding >= DracoCoding_Count)
            return false;
        attribute.coding = coding;
        attribute.portableComponents = attribute.info.componentCount;
        bool isFloat = attribute.info.componentType == PrDracoComponentType_Float32;
        if (coding == DracoCoding_Integer && !isInteger(attribute.info.componentType))
            return false;
        if (coding == DracoCoding_Quantization && !isFloat)
            return false;
        if (coding == DracoCoding_Normals) {
            if (!isFloat || attribute.info.componentCount != 3)
                return false;
            attribute.portableComponents = 2;
        }
    }
    return true;
}

// Reads the integers of an attribute's values: how they are predicted, their corrections as symbols or as stored
// integers, and what the prediction needs, and then undoes the prediction (the bitstream's
// SequentialIntegerAttributeDecoder_DecodeIntegerValues).
bool decodeIntegers(Draco* draco, DracoAttributeState* attribute) {
    DracoReader* reader = &draco->reader;
    const DracoSequence& sequence = draco->sequences.data[attribute->sequence];
    uint8_t method = 0;
    if (!dracoReadU8(reader, &method))
        return false;
    int prediction = int8_t(method);
    uint8_t transform = 0;
    if (prediction != kPredictionNone && !dracoReadU8(reader, &transform))
        return false;

    uint32_t components = attribute->portableComponents;
    uint64_t count = uint64_t(sequence.valueCount) * components;
    if (count > 0x7fffffffu)
        return dracoFail(draco, PrStatus_Limit);
    attribute->portable.resizeZeroed(uint32_t(count));
    attribute->hasPortable = true;
    uint32_t* symbols = reinterpret_cast<uint32_t*>(attribute->portable.data);

    uint8_t compressed = 0;
    if (!dracoReadU8(reader, &compressed))
        return false;
    if (compressed != 0) {
        if (!dracoDecodeSymbols(reader, uint32_t(count), components, symbols))
            return false;
    } else {
        // The integers are stored whole, in a number of bytes each that the stream gives.
        uint8_t size = 0;
        if (!dracoReadU8(reader, &size))
            return false;
        if (count != 0 && (size > 4 || (size != 0 && reader->left() / size < count)))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t b = 0; b < size; ++b)
                symbols[i] |= uint32_t(*reader->at++) << (8 * b);
        }
    }

    // A correction is a signed integer with its sign in the low bit, except for those of a normal's prediction, which
    // are not negative.
    bool positive = prediction != kPredictionNone && attribute->coding == DracoCoding_Normals;
    if (!positive) {
        for (uint32_t i = 0; i < count; ++i)
            symbols[i] = (symbols[i] >> 1) ^ (0u - (symbols[i] & 1));
    }
    if (prediction == kPredictionNone)
        return true;
    return dracoDecodePrediction(draco, attribute, prediction, int8_t(transform), attribute->portable.data);
}

// Reads what an attribute's transform needs to give its values back (the bitstream's ParseQuantizationData and
// ParseQuantizationBits).
bool readTransform(Draco* draco, DracoAttributeState* attribute) {
    DracoReader* reader = &draco->reader;
    if (attribute->coding == DracoCoding_Quantization) {
        uint32_t components = attribute->info.componentCount;
        if (reader->left() / 4 < components + 1)
            return false;
        attribute->minimums.resize(components);
        for (uint32_t c = 0; c < components; ++c, reader->at += 4)
            attribute->minimums.data[c] = bitCast<float>(prMemReadLeU32(reader->at));
        attribute->range = bitCast<float>(prMemReadLeU32(reader->at));
        reader->at += 4;
        return dracoReadU8(reader, &attribute->quantizationBits) && attribute->quantizationBits >= 1 &&
            attribute->quantizationBits <= kQuantizationMaxBits;
    }
    if (attribute->coding == DracoCoding_Normals) {
        return dracoReadU8(reader, &attribute->quantizationBits) && attribute->quantizationBits >= kOctahedralMinBits &&
            attribute->quantizationBits <= kQuantizationMaxBits;
    }
    return true;
}

// Writes the unit vector that the octahedral coordinates (s, t) of `scale` stand for, with the arithmetic of the
// reference decoder in single precision, each operation rounded on its own.
void octahedralToUnitVector(int32_t s, int32_t t, float scale, uint8_t* out) {
    float y = float(s) * scale;
    y = y - 1.0f;
    float z = float(t) * scale;
    z = z - 1.0f;
    float x = 1.0f - prF32Abs(y);
    x = x - prF32Abs(z);
    float offset = -x;
    offset = offset < 0 ? 0 : offset;
    y += y < 0 ? offset : -offset;
    z += z < 0 ? offset : -offset;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float squared = xx + yy;
    squared = squared + zz;
    float vector[3] = {0, 0, 0};
    if (double(squared) >= 1e-6) {
        float d = 1.0f / prF32Sqrt(squared);
        vector[0] = x * d;
        vector[1] = y * d;
        vector[2] = z * d;
    }
    for (uint32_t c = 0; c < 3; ++c)
        prMemWriteLeU32(out + 4 * c, bitCast<uint32_t>(vector[c]));
}

// Writes the low `size` bytes of `value`, least significant first.
inline void writeInteger(uint8_t* out, uint32_t value, uint32_t size) {
    for (uint32_t b = 0; b < size; ++b)
        out[b] = uint8_t(value >> (8 * b));
}

// Gives each point the value of the attribute that its sequence maps it to, with the attribute's transform undone (the
// bitstream's TransformAttributesToOriginalFormat).
void storeValues(Draco* draco, DracoAttributeState* attribute) {
    const DracoSequence& sequence = draco->sequences.data[attribute->sequence];
    const uint32_t* pointValue = sequence.pointValue.count != 0 ? sequence.pointValue.data : nullptr;
    uint32_t components = attribute->info.componentCount;
    uint32_t componentSize = prDracoComponentTypeSize(attribute->info.componentType);
    uint32_t elementSize = components * componentSize;
    uint32_t pointCount = draco->pointCount;
    const int32_t* portable = attribute->portable.data;

    if (attribute->coding == DracoCoding_Generic) {
        // The values were read whole, by value, into the attribute's own array.
        if (pointValue == nullptr)
            return;
        RawArray<uint8_t> byValue = attribute->values;
        attribute->values = {};
        attribute->values.resize(pointCount * elementSize);
        for (uint32_t p = 0; p < pointCount; ++p)
            prMemCopy(attribute->values.data + size_t(p) * elementSize,
                byValue.data + size_t(pointValue[p]) * elementSize, elementSize);
        byValue.reset();
        return;
    }

    attribute->values.resize(pointCount * elementSize);
    uint8_t* out = attribute->values.data;
    if (attribute->coding == DracoCoding_Integer) {
        for (uint32_t p = 0; p < pointCount; ++p) {
            const int32_t* value = portable + size_t(pointValue != nullptr ? pointValue[p] : p) * components;
            for (uint32_t c = 0; c < components; ++c, out += componentSize)
                writeInteger(out, uint32_t(value[c]), componentSize);
        }
    } else if (attribute->coding == DracoCoding_Quantization) {
        // The reference decoder's arithmetic: one step of the range for each unit, then the least value.
        uint32_t maxQuantized = (1u << attribute->quantizationBits) - 1;
        float delta = attribute->range / float(maxQuantized);
        for (uint32_t p = 0; p < pointCount; ++p) {
            const int32_t* value = portable + size_t(pointValue != nullptr ? pointValue[p] : p) * components;
            for (uint32_t c = 0; c < components; ++c, out += 4) {
                float scaled = float(value[c]) * delta;
                float result = scaled + attribute->minimums.data[c];
                prMemWriteLeU32(out, bitCast<uint32_t>(result));
            }
        }
    } else {
        uint32_t maxValue = (1u << attribute->quantizationBits) - 2;
        float scale = 2.0f / float(maxValue);
        for (uint32_t p = 0; p < pointCount; ++p, out += 12) {
            const int32_t* value = portable + size_t(pointValue != nullptr ? pointValue[p] : p) * 2;
            octahedralToUnitVector(value[0], value[1], scale, out);
        }
    }
}

// Decodes the attributes of one group: the integers of every attribute, then what every transform needs, then every
// value (the bitstream's DecodeAttributeData).
bool decodeAttributes(Draco* draco, uint32_t first, uint32_t count) {
    DracoReader* reader = &draco->reader;
    for (uint32_t i = 0; i < count; ++i) {
        DracoAttributeState* attribute = &draco->attributes.data[first + i];
        const DracoSequence& sequence = draco->sequences.data[attribute->sequence];
        uint64_t elements = sequence.valueCount > draco->pointCount ? sequence.valueCount : draco->pointCount;
        if (elements * attribute->info.componentCount * prDracoComponentTypeSize(attribute->info.componentType) >
                0x7fffffffu) {
            return dracoFail(draco, PrStatus_Limit);
        }
        if (attribute->coding != DracoCoding_Generic) {
            if (!decodeIntegers(draco, attribute))
                return false;
            continue;
        }
        uint32_t elementSize = attribute->info.componentCount * prDracoComponentTypeSize(attribute->info.componentType);
        if (reader->left() / elementSize < sequence.valueCount)
            return false;
        size_t size = size_t(sequence.valueCount) * elementSize;
        attribute->values.resize(uint32_t(size));
        prMemCopy(attribute->values.data, reader->at, size);
        reader->at += size;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!readTransform(draco, &draco->attributes.data[first + i]))
            return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        storeValues(draco, &draco->attributes.data[first + i]);
    return true;
}

bool decode(Draco* draco) {
    DracoReader* reader = &draco->reader;
    uint16_t flags = 0;
    if (!readHeader(draco, &flags))
        return false;
    if ((flags & kFlagMetadata) != 0 && !readMetadata(draco))
        return false;

    bool edgebreaker = false;
    if (draco->geometry == PrDracoGeometry_PointCloud) {
        uint32_t pointCount = 0;
        if (!dracoReadU32(reader, &pointCount) || pointCount > 0x7fffffffu)
            return false;
        if (!checkLimits(draco, pointCount, 0))
            return false;
        draco->pointCount = pointCount;
    } else if (draco->method == DracoMethod_Sequential) {
        if (!decodeSequentialFaces(draco))
            return false;
    } else {
        edgebreaker = true;
        if (!dracoDecodeEdgebreaker(draco))
            return false;
    }

    uint8_t groupCount = 0;
    if (!dracoReadU8(reader, &groupCount))
        return false;
    if (edgebreaker) {
        if (!dracoReadEdgebreakerSequences(draco, groupCount))
            return false;
    } else {
        // Every group's values are in the order of the points.
        for (uint32_t i = 0; i < groupCount; ++i) {
            DracoSequence sequence = {};
            sequence.valueCount = draco->pointCount;
            draco->sequences.push(sequence);
        }
    }
    uint32_t firsts[256];
    for (uint32_t i = 0; i < groupCount; ++i) {
        firsts[i] = draco->attributes.count;
        if (!readAttributes(draco, i))
            return false;
    }
    firsts[groupCount] = draco->attributes.count;
    for (uint32_t i = 0; i < groupCount; ++i) {
        if (!decodeAttributes(draco, firsts[i], firsts[i + 1] - firsts[i]))
            return false;
    }
    for (uint32_t i = 0; i < draco->attributes.count; ++i)
        draco->attributeInfos.push(draco->attributes.data[i].info);
    return true;
}

// Frees everything that decoding built and a geometry does not keep.
void freeWorking(Draco* draco) {
    draco->table.cornerVertex.reset();
    draco->table.opposite.reset();
    draco->table.vertexCorner.reset();
    for (uint32_t i = 0; i < draco->seamTables.count; ++i) {
        DracoSeamTable& seams = draco->seamTables.data[i];
        seams.cornerOnSeam.reset();
        seams.vertexOnSeam.reset();
        seams.opposite.reset();
        seams.cornerVertex.reset();
        seams.vertexCorner.reset();
    }
    draco->seamTables.reset();
    for (uint32_t i = 0; i < draco->sequences.count; ++i) {
        DracoSequence& sequence = draco->sequences.data[i];
        sequence.valueCorner.reset();
        sequence.vertexValue.reset();
        sequence.valuePoint.reset();
        sequence.pointValue.reset();
    }
    draco->sequences.reset();
    for (uint32_t i = 0; i < draco->attributes.count; ++i) {
        draco->attributes.data[i].portable.reset();
        draco->attributes.data[i].minimums.reset();
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDraco : Draco {};

PrDracoLimits prDracoLimitsDefault(void) {
    return PrDracoLimits{.maxPoints = 1u << 24, .maxFaces = 1u << 24};
}

PrStatus prDracoCreate(PrByteView stream, const PrDracoLimits* PR_NULLABLE limits, PrDraco* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDraco* draco = allocZeroed<PrDraco>();
    draco->limits = limits != nullptr ? *limits : prDracoLimitsDefault();
    draco->status = PrStatus_InvalidData;
    draco->reader = DracoReader{stream.data, stream.data + stream.size};
    bool ok = stream.data != nullptr && decode(draco);
    freeWorking(draco);
    if (!ok) {
        PrStatus status = draco->status;
        prDracoDestroy(draco);
        return status;
    }
    draco->reader = DracoReader{nullptr, nullptr};
    *out = draco;
    return PrStatus_Ok;
}

void prDracoDestroy(PrDraco* PR_NULLABLE draco) {
    if (draco == nullptr)
        return;
    freeWorking(draco);
    for (uint32_t i = 0; i < draco->attributes.count; ++i)
        draco->attributes.data[i].values.reset();
    draco->attributes.reset();
    draco->attributeInfos.reset();
    draco->faces.reset();
    prMemoryFree(draco);
}

PrDracoGeometry prDracoGeometry(const PrDraco* draco) {
    return draco->geometry;
}

uint32_t prDracoPointCount(const PrDraco* draco) {
    return draco->pointCount;
}

uint32_t prDracoFaceCount(const PrDraco* draco) {
    return draco->faceCount;
}

const uint32_t* PR_NULLABLE prDracoFaces(const PrDraco* draco) {
    return draco->faces.data;
}

uint32_t prDracoAttributeCount(const PrDraco* draco) {
    return draco->attributeInfos.count;
}

const PrDracoAttribute* PR_NULLABLE prDracoAttributes(const PrDraco* draco) {
    return draco->attributeInfos.data;
}

uint32_t prDracoFindAttribute(const PrDraco* draco, uint32_t id) {
    for (uint32_t i = 0; i < draco->attributeInfos.count; ++i) {
        if (draco->attributeInfos.data[i].id == id)
            return i;
    }
    return kPrDracoNone;
}

uint32_t prDracoComponentTypeSize(PrDracoComponentType type) {
    switch (type) {
        case PrDracoComponentType_Int8:
        case PrDracoComponentType_Uint8:
        case PrDracoComponentType_Bool:
            return 1;
        case PrDracoComponentType_Int16:
        case PrDracoComponentType_Uint16:
            return 2;
        case PrDracoComponentType_Int64:
        case PrDracoComponentType_Uint64:
        case PrDracoComponentType_Float64:
            return 8;
        default:
            return 4;
    }
}

PrByteView prDracoAttributeValues(const PrDraco* draco, uint32_t attribute) {
    PR_ASSERT(attribute < draco->attributes.count);
    const RawArray<uint8_t>& values = draco->attributes.data[attribute].values;
    return PrByteView{values.data, values.count};
}
