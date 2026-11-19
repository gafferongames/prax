#include "pr/array.h"
#include "pr/float16.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/vec.h"

#include "usd/tests/usd_fixture.h"

namespace pr_impl {

namespace {

// The types of a value representation that the tests write, as the format numbers them.
enum {
    Rep_Bool = 1,
    Rep_UChar = 2,
    Rep_Int = 3,
    Rep_UInt = 4,
    Rep_Int64 = 5,
    Rep_UInt64 = 6,
    Rep_Half = 7,
    Rep_Float = 8,
    Rep_Double = 9,
    Rep_String = 10,
    Rep_Token = 11,
    Rep_Asset = 12,
    Rep_Matrix2d = 13,
    Rep_Matrix4d = 15,
    Rep_Quatf = 17,
    Rep_Vec2h = 21,
    Rep_Vec3d = 23,
    Rep_Vec3f = 24,
    Rep_Vec3i = 26,
    Rep_Dictionary = 31,
    Rep_TokenListOp = 32,
    Rep_PathListOp = 34,
    Rep_ReferenceListOp = 35,
    Rep_PathVector = 40,
    Rep_TokenVector = 41,
    Rep_Specifier = 42,
    Rep_Variability = 44,
    Rep_VariantSelectionMap = 45,
    Rep_TimeSamples = 46,
    Rep_Payload = 47,
    Rep_DoubleVector = 48,
    Rep_LayerOffsetVector = 49,
    Rep_StringVector = 50,
    Rep_ValueBlock = 51,
    Rep_Value = 52,
    Rep_UnregisteredValue = 53,
    Rep_PayloadListOp = 55,
    Rep_TimeCode = 56,
    Rep_Relocates = 58,
    Rep_Spline = 59,
};
typedef uint32_t Rep;

constexpr uint64_t kArray = uint64_t(1) << 63;
constexpr uint64_t kInlined = uint64_t(1) << 62;
constexpr uint64_t kCompressed = uint64_t(1) << 61;

// The forms of a spec, as the format numbers them.
enum {
    Form_Attribute = 1,
    Form_Prim = 6,
    Form_Layer = 7,
    Form_Relationship = 8,
    Form_Variant = 10,
    Form_VariantSet = 11,
};
typedef uint32_t Form;

uint64_t inlined(uint32_t type, uint32_t payload) {
    return kInlined | uint64_t(type) << 48 | payload;
}

uint64_t stored(uint32_t type, uint64_t offset, uint64_t flags = 0) {
    return flags | uint64_t(type) << 48 | offset;
}

// A crate file that a test writes: values, then the sections, then the table of contents. Every block that the format
// compresses is one LZ4 sequence of literals, and every coded integer is written at its full width.
struct CrateWriter {
    Array<uint8_t> bytes;
    Array<String> tokens;
    Array<uint32_t> strings;
    Array<uint32_t> fieldTokens;
    Array<uint64_t> fieldReps;
    Array<uint32_t> fieldSets;
    Array<uint32_t> pathIndices;
    Array<int32_t> pathElements;
    Array<int32_t> pathJumps;
    uint32_t pathCount = 0;
    Array<uint32_t> specPaths;
    Array<uint32_t> specFieldSets;
    Array<uint32_t> specForms;
    uint8_t version[3] = {0, 8, 0};
    Array<uint64_t> sectionStarts;

    CrateWriter() {
        bytes.resize(88);
        prMemZero(bytes.data(), 88);
        prMemCopy(bytes.data(), "PXR-USDC", 8);
        tokens.push(String(";-)"));
    }

    uint32_t token(PrStringView text) {
        for (uint32_t i = 0; i < tokens.count(); ++i) {
            if (tokens[i] == text)
                return i;
        }
        tokens.push(String(text));
        return tokens.count() - 1;
    }

    uint32_t string(PrStringView text) {
        strings.push(token(text));
        return strings.count() - 1;
    }

    // Appends values to the file, aligned to eight bytes, and returns where they begin.
    uint64_t offset() {
        while (bytes.count() % 8 != 0)
            bytes.push(0);
        return bytes.count();
    }
    template <typename T>
    void put(T value) {
        uint8_t raw[sizeof(T)];
        prMemCopy(raw, &value, sizeof(T));
        bytes.append(raw, sizeof(T));
    }

    uint32_t field(PrStringView name, uint64_t rep) {
        fieldTokens.push(token(name));
        fieldReps.push(rep);
        return fieldTokens.count() - 1;
    }

    // Adds a field set of the fields given, and returns where it begins.
    uint32_t fieldSet(Span<const uint32_t> fields) {
        uint32_t start = fieldSets.count();
        for (uint32_t each : fields)
            fieldSets.push(each);
        fieldSets.push(0xffffffffu);
        return start;
    }

    // Adds an entry of the paths' tree, and returns the path's index.
    uint32_t path(PrStringView element, bool isProperty, int32_t jump) {
        pathIndices.push(pathCount);
        pathElements.push(isProperty ? -int32_t(token(element)) : int32_t(token(element)));
        pathJumps.push(jump);
        return pathCount++;
    }

    void spec(uint32_t pathIndex, uint32_t form, uint32_t set) {
        specPaths.push(pathIndex);
        specFieldSets.push(set);
        specForms.push(form);
    }

    // Appends compressed data: no chunks, and one sequence of literals.
    void compressed(const Array<uint8_t>& data) {
        bytes.push(0);
        size_t size = data.count();
        bytes.push(uint8_t((size < 15 ? size : 15) << 4));
        if (size >= 15) {
            size_t left = size - 15;
            for (; left >= 255; left -= 255)
                bytes.push(255);
            bytes.push(uint8_t(left));
        }
        bytes.append(data.data(), data.count());
    }

    static uint64_t compressedSize(size_t size) {
        return 2 + (size >= 15 ? (size - 15) / 255 + 1 : 0) + size;
    }

    // Appends integers in the format's coding, their compressed size before them.
    template <typename T>
    void integers(const Array<T>& values) {
        Array<uint8_t> coded;
        if (values.count() != 0) {
            coded.resize(4 + (values.count() * 2 + 7) / 8);
            prMemZero(coded.data(), coded.count());
            for (uint32_t i = 0; i < values.count(); ++i)
                coded[4 + i / 4] |= uint8_t(3 << ((i % 4) * 2));
            uint32_t previous = 0;
            for (uint32_t i = 0; i < values.count(); ++i) {
                uint32_t difference = uint32_t(values[i]) - previous;
                previous = uint32_t(values[i]);
                uint8_t raw[4];
                prMemCopy(raw, &difference, 4);
                coded.append(raw, 4);
            }
            put(compressedSize(coded.count()));
            compressed(coded);
        } else {
            put(uint64_t(0));
        }
    }

    void beginSection() {
        sectionStarts.push(offset());
    }

    // Writes the sections and the table of contents, and returns the file.
    PrByteView finish() {
        beginSection();
        Array<uint8_t> text;
        for (const String& each : tokens) {
            text.append(reinterpret_cast<const uint8_t*>(each.data()), each.count());
            text.push(0);
        }
        put(uint64_t(tokens.count()));
        put(uint64_t(text.count()));
        put(compressedSize(text.count()));
        compressed(text);

        beginSection();
        put(uint64_t(strings.count()));
        for (uint32_t each : strings)
            put(each);

        beginSection();
        put(uint64_t(fieldTokens.count()));
        integers(fieldTokens);
        Array<uint8_t> reps;
        reps.resize(fieldReps.count() * 8);
        if (fieldReps.count() != 0)
            prMemCopy(reps.data(), fieldReps.data(), reps.count());
        put(compressedSize(reps.count()));
        compressed(reps);

        beginSection();
        put(uint64_t(fieldSets.count()));
        integers(fieldSets);

        beginSection();
        put(uint64_t(pathCount));
        put(uint64_t(pathIndices.count()));
        integers(pathIndices);
        integers(pathElements);
        integers(pathJumps);

        beginSection();
        put(uint64_t(specPaths.count()));
        integers(specPaths);
        integers(specFieldSets);
        integers(specForms);

        uint64_t toc = offset();
        static const char* const kNames[] = {"TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "PATHS", "SPECS"};
        put(uint64_t(6));
        for (uint32_t i = 0; i < 6; ++i) {
            char name[16] = {};
            prMemCopy(name, kNames[i], PrStringView(kNames[i]).size);
            bytes.append(reinterpret_cast<const uint8_t*>(name), 16);
            put(sectionStarts[i]);
            put((i + 1 < 6 ? sectionStarts[i + 1] : toc) - sectionStarts[i]);
        }
        prMemCopy(bytes.data() + 8, version, 3);
        prMemCopy(bytes.data() + 16, &toc, 8);
        return PrByteView{bytes.data(), bytes.count()};
    }
};

// Adds the specs of a layer with one prim, /A, that has one attribute, /A.x, of the type named, whose `default` is the
// representation `rep`, and whatever other fields `more` gives the attribute.
void addAttributeLayer(CrateWriter& w, PrStringView typeName, uint64_t rep, Span<const uint32_t> more = {}) {
    uint64_t prims = w.offset();
    w.put(uint64_t(1));
    w.put(w.token("A"));
    uint64_t properties = w.offset();
    w.put(uint64_t(1));
    w.put(w.token("x"));
    const uint32_t layerFields[] = {w.field("primChildren", stored(Rep_TokenVector, prims))};
    const uint32_t primFields[] = {
        w.field("specifier", inlined(Rep_Specifier, 0)),
        w.field("properties", stored(Rep_TokenVector, properties)),
    };
    Array<uint32_t> attributeFields;
    attributeFields.push(w.field("typeName", inlined(Rep_Token, w.token(typeName))));
    attributeFields.push(w.field("default", rep));
    for (uint32_t each : more)
        attributeFields.push(each);
    uint32_t root = w.path("", false, -1);
    uint32_t prim = w.path("A", false, -1);
    uint32_t attribute = w.path("x", true, -2);
    w.spec(root, Form_Layer, w.fieldSet(layerFields));
    w.spec(prim, Form_Prim, w.fieldSet(primFields));
    w.spec(attribute, Form_Attribute,
        w.fieldSet(Span<const uint32_t>(attributeFields.data(), attributeFields.count())));
}

// Reads the layer whose attribute /A.x has the default that `write` writes and returns the representation of.
template <typename Write>
UsdRead attributeLayer(PrStringView typeName, Write write, const uint8_t* PR_NULLABLE version = nullptr) {
    CrateWriter w;
    if (version != nullptr)
        prMemCopy(w.version, version, 3);
    uint64_t rep = write(w);
    addAttributeLayer(w, typeName, rep);
    return usdReadBytes(w.finish());
}

PrUsdValue defaultOf(const UsdRead& read) {
    REQUIRE(read.status == PrStatus_Ok);
    return read.value("/A.x", "default");
}

// The default of /A.x for a representation that needs nothing written.
PrUsdValue inlinedDefault(UsdRead& keep, PrStringView typeName, uint64_t rep) {
    keep = attributeLayer(typeName, [&](CrateWriter&) { return rep; });
    return defaultOf(keep);
}

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    prMemCopy(&bits, &value, 4);
    return bits;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a crate file reads into the specs that its paths, field sets and lists of children give") {
    CrateWriter w;
    addAttributeLayer(w, "float", inlined(Rep_Float, floatBits(1.5f)));
    UsdRead read = usdReadBytes(w.finish());
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prUsdLayerFormat(read.layer) == PrUsdFormat_Crate);
    CHECK(prUsdLayerVersion(read.layer) == "0.8.0");
    REQUIRE(prUsdLayerSpecCount(read.layer) == 3);
    CHECK(read.spec("/").prims.count == 1);
    CHECK(read.spec("/A").form == PrUsdSpecForm_Prim);
    CHECK(read.spec("/A").properties.count == 1);
    CHECK(usdScalar<PrUsdSpecifier>(read.value("/A", "specifier")) == PrUsdSpecifier_Def);
    CHECK(read.spec("/A.x").form == PrUsdSpecForm_Attribute);
    CHECK(usdScalar<PrStringView>(read.value("/A.x", "typeName")) == "float");
    CHECK(usdScalar<float>(read.value("/A.x", "default")) == 1.5f);
    // The lists of children are the layer's lists, and the fields that a format may leave out have their fallbacks.
    CHECK(!read.has("/", "primChildren"));
    CHECK(!read.has("/A", "properties"));
    CHECK(!usdScalar<bool>(read.value("/A.x", "custom")));
    CHECK(usdScalar<PrUsdVariability>(read.value("/A.x", "variability")) == PrUsdVariability_Varying);
}

TEST_CASE("a crate file's versions are read from 0.4.0 to 0.12.0, and need a codec") {
    CHECK(prUsdCrateVersionIsSupported(0, 4, 0));
    CHECK(prUsdCrateVersionIsSupported(0, 12, 0));
    CHECK(!prUsdCrateVersionIsSupported(0, 3, 0));
    CHECK(!prUsdCrateVersionIsSupported(0, 13, 0));
    CHECK(!prUsdCrateVersionIsSupported(1, 0, 0));
    for (uint8_t minor : usdEach(uint8_t(3), uint8_t(13))) {
        CrateWriter w;
        w.version[1] = minor;
        addAttributeLayer(w, "float", inlined(Rep_Float, 0));
        CHECK(usdReadBytes(w.finish()).status == PrStatus_NotSupported);
    }
    CrateWriter w;
    addAttributeLayer(w, "float", inlined(Rep_Float, 0));
    PrBlob* file = prBlobCopy(w.finish());
    PrUsdLayer* layer = nullptr;
    CHECK(prUsdLayerCreate(file, nullptr, &layer) == PrStatus_NotSupported);
    CHECK(layer == nullptr);
    prBlobRelease(file);
}

TEST_CASE("a value that fits its representation is stored in it") {
    UsdRead keep;
    CHECK(usdScalar<bool>(inlinedDefault(keep, "bool", inlined(Rep_Bool, 1))));
    CHECK(usdScalar<uint8_t>(inlinedDefault(keep, "uchar", inlined(Rep_UChar, 200))) == 200);
    CHECK(usdScalar<int32_t>(inlinedDefault(keep, "int", inlined(Rep_Int, uint32_t(-5)))) == -5);
    CHECK(usdScalar<uint32_t>(inlinedDefault(keep, "uint", inlined(Rep_UInt, 4000000000u))) == 4000000000u);
    // A wide integer is stored as a narrow one, and a double as a float, where the value is the same.
    CHECK(usdScalar<int64_t>(inlinedDefault(keep, "int64", inlined(Rep_Int64, uint32_t(-5)))) == -5);
    CHECK(usdScalar<uint64_t>(inlinedDefault(keep, "uint64", inlined(Rep_UInt64, 4000000000u))) == 4000000000u);
    CHECK(usdScalar<double>(inlinedDefault(keep, "double", inlined(Rep_Double, floatBits(-2.5f)))) == -2.5);
    CHECK(usdScalar<double>(inlinedDefault(keep, "timecode", inlined(Rep_TimeCode, floatBits(24)))) == 24);
    CHECK(inlinedDefault(keep, "timecode", inlined(Rep_TimeCode, 0)).type == PrUsdType_TimeCode);
    CHECK(prFloat16ToFloat(usdScalar<PrFloat16>(inlinedDefault(keep, "half", inlined(Rep_Half, 0x3c00)))) == 1.0f);
    CHECK(usdScalar<float>(inlinedDefault(keep, "float", inlined(Rep_Float, floatBits(0.1f)))) == 0.1f);
    // The components of a vector, and the diagonal of a matrix, each in a signed byte.
    CHECK(usdScalar<PrVec3>(inlinedDefault(keep, "float3", inlined(Rep_Vec3f, 0x00fe0301))) == PrVec3{1, 3, -2});
    CHECK(usdScalar<PrDVec3>(inlinedDefault(keep, "double3", inlined(Rep_Vec3d, 0x00030201))) == PrDVec3{1, 2, 3});
    CHECK(usdScalar<PrIVec3>(inlinedDefault(keep, "int3", inlined(Rep_Vec3i, 0x00ff0201))) == PrIVec3{1, 2, -1});
    PrDMatrix4 matrix = usdScalar<PrDMatrix4>(inlinedDefault(keep, "matrix4d", inlined(Rep_Matrix4d, 0x01fe0302)));
    CHECK(matrix.m[0][0] == 2);
    CHECK(matrix.m[1][1] == 3);
    CHECK(matrix.m[2][2] == -2);
    CHECK(matrix.m[3][3] == 1);
    CHECK(matrix.m[0][1] == 0);
    // Two halves are their own four bytes.
    const PrFloat16* halves = static_cast<const PrFloat16*>(
        inlinedDefault(keep, "half2", inlined(Rep_Vec2h, 0x40003c00)).data);
    CHECK(prFloat16ToFloat(halves[0]) == 1.0f);
    CHECK(prFloat16ToFloat(halves[1]) == 2.0f);
    CHECK(inlinedDefault(keep, "float", inlined(Rep_ValueBlock, 0)).type == PrUsdType_Block);
    CHECK(static_cast<const PrUsdDictionary*>(inlinedDefault(keep, "x", inlined(Rep_Dictionary, 0)).data)->count == 0);

    UsdRead text = attributeLayer("string", [](CrateWriter& w) { return inlined(Rep_String, w.string("text")); });
    CHECK(usdScalar<PrStringView>(defaultOf(text)) == "text");
    CHECK(defaultOf(text).type == PrUsdType_String);
    UsdRead asset = attributeLayer("asset", [](CrateWriter& w) { return inlined(Rep_Asset, w.token("a.png")); });
    CHECK(usdScalar<PrStringView>(defaultOf(asset)) == "a.png");
    CHECK(defaultOf(asset).type == PrUsdType_Asset);
}

TEST_CASE("a value that does not fit its representation is read where the representation says") {
    UsdRead wide = attributeLayer("double", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(0.1);
        return stored(Rep_Double, at);
    });
    CHECK(usdScalar<double>(defaultOf(wide)) == 0.1);

    UsdRead vector = attributeLayer("float3", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(1.5f);
        w.put(2.5f);
        w.put(3.5f);
        return stored(Rep_Vec3f, at);
    });
    CHECK(usdScalar<PrVec3>(defaultOf(vector)) == PrVec3{1.5f, 2.5f, 3.5f});

    // A quaternion's real part is last in the file, as it is in the value.
    UsdRead quaternion = attributeLayer("quatf", [](CrateWriter& w) {
        uint64_t at = w.offset();
        for (float each : usdEach(2.0f, 3.0f, 4.0f, 1.0f))
            w.put(each);
        return stored(Rep_Quatf, at);
    });
    CHECK(usdScalar<PrQuat>(defaultOf(quaternion)).w == 1);
    CHECK(usdScalar<PrQuat>(defaultOf(quaternion)).x == 2);

    // A matrix is rows in the file, and the value is their transpose.
    UsdRead matrix = attributeLayer("matrix2d", [](CrateWriter& w) {
        uint64_t at = w.offset();
        for (double each : usdEach(1.0, 2.0, 3.0, 4.0))
            w.put(each);
        return stored(Rep_Matrix2d, at);
    });
    CHECK(usdScalar<PrDMatrix2>(defaultOf(matrix)).m[0][1] == 3);
    CHECK(usdScalar<PrDMatrix2>(defaultOf(matrix)).m[1][0] == 2);

    // A value that points to a value.
    UsdRead nested = attributeLayer("int", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(int64_t(8));
        w.put(inlined(Rep_Int, 7));
        return stored(Rep_Value, at);
    });
    CHECK(usdScalar<int32_t>(defaultOf(nested)) == 7);
}

TEST_CASE("arrays read as they are stored, or from the format's codings") {
    UsdRead empty = attributeLayer("int[]", [](CrateWriter&) { return stored(Rep_Int, 0, kArray); });
    CHECK(defaultOf(empty).isArray);
    CHECK(defaultOf(empty).count == 0);
    CHECK(defaultOf(empty).type == PrUsdType_Int);

    UsdRead plain = attributeLayer("float3[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(2));
        for (float each : usdEach(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f))
            w.put(each);
        return stored(Rep_Vec3f, at, kArray);
    });
    CHECK(usdElements<PrVec3>(defaultOf(plain), 2)[1] == PrVec3{4, 5, 6});

    // Before version 0.7.0 an array's count is four bytes, and before 0.5.0 its number of dimensions precedes it.
    const uint8_t narrow[] = {0, 6, 0};
    UsdRead counted = attributeLayer("int[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint32_t(2));
        w.put(int32_t(-3));
        w.put(int32_t(4));
        return stored(Rep_Int, at, kArray);
    }, narrow);
    CHECK(usdElements<int32_t>(defaultOf(counted), 2)[0] == -3);
    const uint8_t ranked[] = {0, 4, 0};
    UsdRead dimensioned = attributeLayer("int[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint32_t(1));
        w.put(uint32_t(1));
        w.put(int32_t(9));
        return stored(Rep_Int, at, kArray);
    }, ranked);
    CHECK(usdElements<int32_t>(defaultOf(dimensioned), 1)[0] == 9);

    UsdRead coded = attributeLayer("int[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(3));
        Array<int32_t> values;
        for (int32_t each : usdEach(10, -20, 30))
            values.push(each);
        w.integers(values);
        return stored(Rep_Int, at, kArray | kCompressed);
    });
    CHECK(usdElements<int32_t>(defaultOf(coded), 3)[1] == -20);

    // Floats that are whole numbers are coded as integers, and floats of few values as positions in a table.
    UsdRead whole = attributeLayer("float[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(2));
        w.put(uint8_t('i'));
        Array<int32_t> values;
        values.push(-7);
        values.push(8);
        w.integers(values);
        return stored(Rep_Float, at, kArray | kCompressed);
    });
    CHECK(usdElements<float>(defaultOf(whole), 2)[0] == -7.0f);
    UsdRead table = attributeLayer("double[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(3));
        w.put(uint8_t('t'));
        w.put(uint32_t(2));
        w.put(0.25);
        w.put(0.75);
        Array<uint32_t> positions;
        for (uint32_t each : usdEach(1u, 0u, 1u))
            positions.push(each);
        w.integers(positions);
        return stored(Rep_Double, at, kArray | kCompressed);
    });
    CHECK(usdElements<double>(defaultOf(table), 3)[0] == 0.75);
    CHECK(usdElements<double>(defaultOf(table), 3)[1] == 0.25);

    // The tokens of an array are tokens, and its strings and asset paths are strings.
    UsdRead tokens = attributeLayer("token[]", [](CrateWriter& w) {
        uint32_t a = w.token("a");
        uint32_t b = w.token("b");
        uint64_t at = w.offset();
        w.put(uint64_t(2));
        w.put(a);
        w.put(b);
        return stored(Rep_Token, at, kArray);
    });
    CHECK(usdElements<PrStringView>(defaultOf(tokens), 2)[1] == "b");
    UsdRead assets = attributeLayer("asset[]", [](CrateWriter& w) {
        uint32_t a = w.string("a.png");
        uint64_t at = w.offset();
        w.put(uint64_t(1));
        w.put(a);
        return stored(Rep_Asset, at, kArray);
    });
    CHECK(usdElements<PrStringView>(defaultOf(assets), 1)[0] == "a.png");
    CHECK(defaultOf(assets).type == PrUsdType_Asset);

    UsdRead flags = attributeLayer("bool[]", [](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(3));
        for (uint8_t each : usdEach(uint8_t(0), uint8_t(1), uint8_t(7)))
            w.put(each);
        return stored(Rep_Bool, at, kArray);
    });
    CHECK(usdElements<bool>(defaultOf(flags), 3)[2] == true);
}

TEST_CASE("the fields of composition read as the specification's records") {
    // The paths: the path of no prim, which the tree of paths never reaches, the root, and /A.
    const uint32_t kNoPath = 0;
    const uint32_t kA = 2;
    CrateWriter w;
    uint64_t primsAt = w.offset();
    w.put(uint64_t(1));
    w.put(w.token("A"));

    uint32_t asset = w.string("./ref.usda");
    uint32_t none = w.string("");
    uint32_t key = w.string("note");
    uint64_t referencesAt = w.offset();
    w.put(uint8_t(1 << 5));
    w.put(uint64_t(2));
    // A reference with a retiming and custom data, and an internal one.
    w.put(asset);
    w.put(kA);
    w.put(1.0);
    w.put(2.0);
    w.put(uint64_t(1));
    w.put(key);
    w.put(int64_t(8));
    w.put(inlined(Rep_Int, 3));
    w.put(none);
    w.put(kA);
    w.put(0.0);
    w.put(1.0);
    w.put(uint64_t(0));

    uint64_t payloadsAt = w.offset();
    w.put(uint8_t(1 | 2));
    w.put(uint64_t(1));
    w.put(asset);
    w.put(kNoPath);
    w.put(5.0);
    w.put(1.0);

    uint64_t inheritsAt = w.offset();
    w.put(uint8_t(1 << 6 | 1 << 3));
    w.put(uint64_t(1));
    w.put(kA);
    w.put(uint64_t(1));
    w.put(kA);

    uint64_t schemasAt = w.offset();
    w.put(uint8_t(1 << 2 | 1 << 4));
    w.put(uint64_t(1));
    w.put(w.token("AddedAPI"));
    w.put(uint64_t(1));
    w.put(w.token("OrderedAPI"));

    uint32_t setName = w.string("shape");
    uint32_t variantName = w.string("round");
    uint64_t selectionAt = w.offset();
    w.put(uint64_t(1));
    w.put(setName);
    w.put(variantName);

    uint32_t sublayer = w.string("sub.usda");
    uint64_t sublayersAt = w.offset();
    w.put(uint64_t(1));
    w.put(sublayer);
    uint64_t offsetsAt = w.offset();
    w.put(uint64_t(1));
    w.put(3.0);
    w.put(0.5);
    uint64_t relocatesAt = w.offset();
    w.put(uint64_t(1));
    w.put(kA);
    w.put(kNoPath);

    const uint32_t layerFields[] = {
        w.field("primChildren", stored(Rep_TokenVector, primsAt)),
        w.field("subLayers", stored(Rep_StringVector, sublayersAt)),
        w.field("subLayerOffsets", stored(Rep_LayerOffsetVector, offsetsAt)),
        w.field("layerRelocates", stored(Rep_Relocates, relocatesAt)),
    };
    const uint32_t primFields[] = {
        w.field("specifier", inlined(Rep_Specifier, 2)),
        w.field("references", stored(Rep_ReferenceListOp, referencesAt)),
        w.field("payload", stored(Rep_PayloadListOp, payloadsAt)),
        w.field("inheritPaths", stored(Rep_PathListOp, inheritsAt)),
        w.field("apiSchemas", stored(Rep_TokenListOp, schemasAt)),
        w.field("variantSelection", stored(Rep_VariantSelectionMap, selectionAt)),
    };
    ++w.pathCount;
    uint32_t root = w.path("", false, -1);
    uint32_t prim = w.path("A", false, -2);
    REQUIRE(prim == kA);
    w.spec(root, Form_Layer, w.fieldSet(layerFields));
    w.spec(prim, Form_Prim, w.fieldSet(primFields));
    UsdRead read = usdReadBytes(w.finish());
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(usdScalar<PrUsdSpecifier>(read.value("/A", "specifier")) == PrUsdSpecifier_Class);

    const PrUsdListOp& references = usdListOp(read.value("/A", "references"));
    CHECK(references.itemType == PrUsdType_Reference);
    CHECK(!references.isExplicit);
    REQUIRE(references.items[PrUsdListOpList_Prepended].count == 2);
    const PrUsdReference* items = static_cast<const PrUsdReference*>(references.items[PrUsdListOpList_Prepended].data);
    CHECK(items[0].assetPath == "./ref.usda");
    CHECK(items[0].primPath == "/A");
    CHECK(items[0].layerOffset.offset == 1);
    CHECK(items[0].layerOffset.scale == 2);
    REQUIRE(items[0].customData != nullptr);
    CHECK(items[0].customData->entries[0].key == "note");
    CHECK(usdScalar<int32_t>(items[0].customData->entries[0].value) == 3);
    CHECK(items[1].assetPath == "");
    CHECK(items[1].customData == nullptr);

    const PrUsdListOp& payload = usdListOp(read.value("/A", "payload"));
    CHECK(payload.isExplicit);
    const PrUsdReference* payloads = static_cast<const PrUsdReference*>(payload.items[PrUsdListOpList_Explicit].data);
    REQUIRE(payload.items[PrUsdListOpList_Explicit].count == 1);
    CHECK(payloads[0].primPath == "");
    CHECK(payloads[0].layerOffset.offset == 5);

    const PrUsdListOp& inherits = usdListOp(read.value("/A", "inheritPaths"));
    CHECK(inherits.items[PrUsdListOpList_Appended].count == 1);
    CHECK(inherits.items[PrUsdListOpList_Deleted].count == 1);
    const PrUsdListOp& schemas = usdListOp(read.value("/A", "apiSchemas"));
    CHECK(static_cast<const PrStringView*>(schemas.items[PrUsdListOpList_Added].data)[0] == "AddedAPI");
    CHECK(static_cast<const PrStringView*>(schemas.items[PrUsdListOpList_Ordered].data)[0] == "OrderedAPI");

    const PrUsdVariantSelection* selection = usdElements<PrUsdVariantSelection>(read.value("/A", "variantSelection"), 1);
    CHECK(selection[0].variantSet == "shape");
    CHECK(selection[0].variant == "round");

    REQUIRE(prUsdLayerSublayerCount(read.layer) == 1);
    CHECK(prUsdLayerSublayers(read.layer)[0].assetPath == "sub.usda");
    CHECK(prUsdLayerSublayers(read.layer)[0].layerOffset.offset == 3);
    CHECK(prUsdLayerSublayers(read.layer)[0].layerOffset.scale == 0.5);
    CHECK(read.value("/", "subLayers").type == PrUsdType_Asset);
    const PrUsdRelocate* relocates = usdElements<PrUsdRelocate>(read.value("/", "layerRelocates"), 1);
    CHECK(relocates[0].source == "/A");
    CHECK(relocates[0].target == "");
}

TEST_CASE("dictionaries, time samples and values of older versions read") {
    UsdRead dictionary = attributeLayer("x", [](CrateWriter& w) {
        uint32_t first = w.string("zebra");
        uint32_t second = w.string("apple");
        uint64_t at = w.offset();
        w.put(uint64_t(2));
        w.put(first);
        w.put(int64_t(8));
        w.put(inlined(Rep_Int, 1));
        w.put(second);
        w.put(int64_t(8));
        w.put(inlined(Rep_Dictionary, 0));
        return stored(Rep_Dictionary, at);
    });
    const PrUsdDictionary* entries = static_cast<const PrUsdDictionary*>(defaultOf(dictionary).data);
    REQUIRE(entries->count == 2);
    CHECK(entries->entries[0].key == "apple");
    CHECK(entries->entries[0].value.type == PrUsdType_Dictionary);
    CHECK(usdScalar<int32_t>(entries->entries[1].value) == 1);

    // Time samples: an offset to the times, which are a vector of doubles, then an offset to the values.
    CrateWriter w;
    uint64_t times = w.offset();
    w.put(uint64_t(2));
    w.put(2.0);
    w.put(1.0);
    uint64_t samples = w.offset();
    w.put(int64_t(8));
    w.put(stored(Rep_DoubleVector, times));
    w.put(int64_t(8));
    w.put(uint64_t(2));
    w.put(inlined(Rep_Float, floatBits(20)));
    w.put(inlined(Rep_ValueBlock, 0));
    const uint32_t more[] = {
        w.field("timeSamples", stored(Rep_TimeSamples, samples)),
        w.field("variability", inlined(Rep_Variability, 1)),
        w.field("unknown", stored(Rep_UnregisteredValue, w.offset())),
    };
    w.put(int64_t(8));
    w.put(inlined(Rep_String, w.string("(1, 2)")));
    addAttributeLayer(w, "float", inlined(Rep_Float, 0), more);
    UsdRead sampled = usdReadBytes(w.finish());
    REQUIRE(sampled.status == PrStatus_Ok);
    const PrUsdTimeSamples* read = static_cast<const PrUsdTimeSamples*>(sampled.value("/A.x", "timeSamples").data);
    REQUIRE(read->count == 2);
    // The samples are sorted by time, whatever order the file has them in.
    CHECK(read->times[0] == 1);
    CHECK(read->values[0].type == PrUsdType_Block);
    CHECK(read->times[1] == 2);
    CHECK(usdScalar<float>(read->values[1]) == 20);
    CHECK(usdScalar<PrUsdVariability>(sampled.value("/A.x", "variability")) == PrUsdVariability_Uniform);
    CHECK(sampled.value("/A.x", "unknown").type == PrUsdType_Unregistered);
    CHECK(usdScalar<PrStringView>(sampled.value("/A.x", "unknown")) == "(1, 2)");

    // Before version 0.8.0 a prim's payload is one payload without a retiming, which reads as the explicit list op that
    // it stands for.
    const uint8_t old[] = {0, 7, 0};
    UsdRead payload = attributeLayer("x", [](CrateWriter& w2) {
        uint32_t asset = w2.string("p.usda");
        uint64_t at = w2.offset();
        w2.put(asset);
        w2.put(uint32_t(1));
        return stored(Rep_Payload, at);
    }, old);
    const PrUsdListOp& listOp = usdListOp(defaultOf(payload));
    CHECK(listOp.isExplicit);
    CHECK(listOp.itemType == PrUsdType_Payload);
    REQUIRE(listOp.items[PrUsdListOpList_Explicit].count == 1);
    const PrUsdReference* item = static_cast<const PrUsdReference*>(listOp.items[PrUsdListOpList_Explicit].data);
    CHECK(item->assetPath == "p.usda");
    CHECK(item->primPath == "/A");
    CHECK(item->layerOffset.scale == 1);
}

TEST_CASE("a crate file whose values break the format is refused") {
    auto statusOf = [](PrStringView typeName, uint64_t rep) {
        return attributeLayer(typeName, [&](CrateWriter&) { return rep; }).status;
    };
    // Offsets and counts that leave the file.
    CHECK(statusOf("double", stored(Rep_Double, 1 << 30)) == PrStatus_InvalidData);
    CHECK(statusOf("int[]", stored(Rep_Int, 1 << 30, kArray)) == PrStatus_InvalidData);
    CHECK(statusOf("x", stored(Rep_Dictionary, 1 << 30)) == PrStatus_InvalidData);
    CHECK(statusOf("x", stored(Rep_TimeSamples, 1 << 30)) == PrStatus_InvalidData);
    CHECK(statusOf("x", stored(Rep_TokenListOp, 1 << 30)) == PrStatus_InvalidData);
    CHECK(statusOf("string", inlined(Rep_String, 1000)) == PrStatus_InvalidData);
    CHECK(statusOf("token", inlined(Rep_Token, 1000)) == PrStatus_InvalidData);
    CHECK(statusOf("string", stored(Rep_String, 8)) == PrStatus_InvalidData);
    // Types and flags that the format does not define.
    CHECK(statusOf("x", inlined(0, 0)) == PrStatus_InvalidData);
    CHECK(statusOf("x", inlined(200, 0)) == PrStatus_InvalidData);
    CHECK(statusOf("x", inlined(Rep_Specifier, 3)) == PrStatus_InvalidData);
    CHECK(statusOf("x", inlined(Rep_Variability, 9)) == PrStatus_InvalidData);
    CHECK(statusOf("x", inlined(Rep_Int, 1) | kArray) == PrStatus_InvalidData);
    CHECK(statusOf("x", stored(Rep_Dictionary, 8, kArray)) == PrStatus_InvalidData);
    CHECK(statusOf("x", inlined(Rep_Quatf, 1)) == PrStatus_InvalidData);
    CHECK(statusOf("x", stored(Rep_Spline, 8)) == PrStatus_NotSupported);

    auto statusWith = [](auto write) { return attributeLayer("x", write).status; };
    // An array that states more elements than the file holds.
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(1) << 40);
        w.put(int32_t(1));
        return stored(Rep_Int, at, kArray);
    }) == PrStatus_Limit);
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(1000));
        w.put(int32_t(1));
        return stored(Rep_Vec3f, at, kArray);
    }) == PrStatus_InvalidData);
    // A compressed array whose coded integers are fewer than it states, and one of a type that is never compressed.
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(100));
        Array<int32_t> values;
        values.push(1);
        w.integers(values);
        return stored(Rep_Int, at, kArray | kCompressed);
    }) == PrStatus_InvalidData);
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(1));
        w.put(uint64_t(0));
        return stored(Rep_Vec3f, at, kArray | kCompressed);
    }) == PrStatus_InvalidData);
    // A table of floats whose positions leave it, and a coding that the format does not define.
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(1));
        w.put(uint8_t('t'));
        w.put(uint32_t(1));
        w.put(0.5f);
        Array<uint32_t> positions;
        positions.push(1);
        w.integers(positions);
        return stored(Rep_Float, at, kArray | kCompressed);
    }) == PrStatus_InvalidData);
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(1));
        w.put(uint8_t('z'));
        return stored(Rep_Float, at, kArray | kCompressed);
    }) == PrStatus_InvalidData);
    // Compressed data that is not an LZ4 block.
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint64_t(4));
        w.put(uint64_t(4));
        for (uint8_t each : usdEach(uint8_t(0), uint8_t(0x10), uint8_t(1), uint8_t(0)))
            w.put(each);
        return stored(Rep_Int, at, kArray | kCompressed);
    }) == PrStatus_InvalidData);
    // A value that points to itself.
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(int64_t(8));
        w.put(stored(Rep_Value, at));
        return stored(Rep_Value, at);
    }) == PrStatus_InvalidData);
    // Time samples with more times than values, and with a time that is not a number.
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t times = w.offset();
        w.put(uint64_t(2));
        w.put(1.0);
        w.put(2.0);
        uint64_t at = w.offset();
        w.put(int64_t(8));
        w.put(stored(Rep_DoubleVector, times));
        w.put(int64_t(8));
        w.put(uint64_t(1));
        w.put(inlined(Rep_Float, 0));
        return stored(Rep_TimeSamples, at);
    }) == PrStatus_InvalidData);
    // A list op that names an item twice, and one whose path the file does not have.
    CHECK(statusWith([](CrateWriter& w) {
        uint32_t token = w.token("TwiceAPI");
        uint64_t at = w.offset();
        w.put(uint8_t(1 << 5));
        w.put(uint64_t(2));
        w.put(token);
        w.put(token);
        return stored(Rep_TokenListOp, at);
    }) == PrStatus_InvalidData);
    CHECK(statusWith([](CrateWriter& w) {
        uint64_t at = w.offset();
        w.put(uint8_t(1 << 5));
        w.put(uint64_t(1));
        w.put(uint32_t(77));
        return stored(Rep_PathListOp, at);
    }) == PrStatus_InvalidData);
    // A reference to a property.
    CHECK(statusWith([](CrateWriter& w) {
        uint32_t asset = w.string("a.usda");
        uint64_t at = w.offset();
        w.put(uint8_t(1 | 2));
        w.put(uint64_t(1));
        w.put(asset);
        w.put(uint32_t(2));
        w.put(0.0);
        w.put(1.0);
        return stored(Rep_PayloadListOp, at);
    }) == PrStatus_InvalidData);
}

TEST_CASE("a crate file whose structure breaks the format is refused") {
    auto statusOf = [](auto change) {
        CrateWriter w;
        addAttributeLayer(w, "float", inlined(Rep_Float, 0));
        change(w);
        return usdReadBytes(w.finish()).status;
    };
    CHECK(statusOf([](CrateWriter&) {}) == PrStatus_Ok);
    CHECK(statusOf([](CrateWriter& w) { w.fieldTokens[0] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.fieldSets[0] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.fieldSets[w.fieldSets.count() - 1] = 0; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.strings.push(1000); }) == PrStatus_InvalidData);
    // The paths' tree: an index, a token and a jump that leave it, an entry that is reached twice, and elements that
    // make no path.
    CHECK(statusOf([](CrateWriter& w) { w.pathIndices[1] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.pathElements[1] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.pathJumps[1] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.pathJumps[2] = -1; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.pathJumps[0] = 2; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.pathElements[1] = int32_t(w.token("not a name")); }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.pathElements[1] = -int32_t(w.token("A")); }) == PrStatus_InvalidData);
    // The specs: a path, a field set and a form that the file does not have, two specs of one path, and no layer spec.
    CHECK(statusOf([](CrateWriter& w) { w.specPaths[1] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.specFieldSets[1] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.specForms[1] = 1000; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.specPaths[2] = w.specPaths[1]; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.specForms[0] = Form_Prim; }) == PrStatus_InvalidData);
    // The children: a child whose form its list does not hold, a list that names no spec, and a spec that no list
    // names.
    CHECK(statusOf([](CrateWriter& w) { w.specForms[1] = Form_Attribute; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.specForms[2] = Form_Prim; }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.specForms[2] = Form_Relationship; }) == PrStatus_Ok);
    CHECK(statusOf([](CrateWriter& w) { w.tokens[w.token("A")] = String("B"); }) == PrStatus_Ok);
    CHECK(statusOf([](CrateWriter& w) {
        w.specPaths.resize(2);
        w.specFieldSets.resize(2);
        w.specForms.resize(2);
    }) == PrStatus_InvalidData);
    CHECK(statusOf([](CrateWriter& w) { w.fieldTokens[2] = w.token("comment"); }) == PrStatus_InvalidData);
    // A legacy spec of a relationship's target is no spec of the specification, and is passed over.
    CHECK(statusOf([](CrateWriter& w) {
        const uint32_t none[] = {w.field("comment", inlined(Rep_String, w.string("legacy")))};
        w.pathJumps[2] = -1;
        uint32_t target = w.path("legacy", true, -2);
        w.spec(target, 9, w.fieldSet(none));
    }) == PrStatus_Ok);
}

TEST_CASE("a crate file that is cut short, or changed at any one byte, is read or refused and never trusted") {
    CrateWriter w;
    uint64_t at = w.offset();
    w.put(uint64_t(2));
    for (float each : usdEach(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f))
        w.put(each);
    addAttributeLayer(w, "float3[]", stored(Rep_Vec3f, at, kArray));
    PrByteView file = w.finish();
    REQUIRE(usdReadBytes(file).status == PrStatus_Ok);
    // The table of contents is at the end, so no shorter file reads.
    for (size_t size = 0; size < file.size; ++size) {
        CAPTURE(size);
        CHECK(usdReadBytes(PrByteView{file.data, size}).status != PrStatus_Ok);
    }
    // Each byte in turn takes each of three other values, and the file reads as a layer or is refused. A layer that
    // reads is read in full.
    Array<uint8_t> changed;
    changed.append(file.data, uint32_t(file.size));
    uint32_t reads = 0;
    for (size_t i = 0; i < file.size; ++i) {
        for (uint8_t value : usdEach(uint8_t(0x00), uint8_t(0xff), uint8_t(file.data[i] ^ 0x41))) {
            if (value == file.data[i])
                continue;
            changed[uint32_t(i)] = value;
            UsdRead read = usdReadBytes(PrByteView{changed.data(), changed.count()});
            if (read.status != PrStatus_Ok)
                continue;
            ++reads;
            const PrUsdSpec* specs = prUsdLayerSpecs(read.layer);
            for (uint32_t s = 0; s < prUsdLayerSpecCount(read.layer); ++s) {
                for (uint32_t f = 0; f < specs[s].fieldCount; ++f) {
                    const PrUsdValue& value2 = specs[s].fields[f].value;
                    if (value2.type == PrUsdType_Float3 && value2.isArray && value2.count != 0) {
                        const PrVec3* points = static_cast<const PrVec3*>(value2.data);
                        CHECK(points[value2.count - 1].x == points[value2.count - 1].x);
                    }
                }
            }
        }
        changed[uint32_t(i)] = file.data[i];
    }
    CHECK(reads > 0);
}
