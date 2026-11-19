// OpenUSD's schemas of geometry, shading and light, read from a composed stage.

#include "pr/usd_geom.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "usd/src/usd_compose.h"

namespace pr_impl {

namespace {

constexpr double kDegrees = 3.14159265358979323846 / 180.0;

// ---------------------------------------------------------------------------------------------------------------------
// Values

// Resolves the attribute `name` of the prim at `time`. Returns false if the prim has no such attribute or no value.
bool attributeValue(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        PrUsdValue* out) {
    uint32_t property = prUsdStageFindProperty(stage, prim, name);
    if (property == kPrUsdNone || prUsdStageProperties(stage)[property].form != PrUsdSpecForm_Attribute)
        return false;
    return prUsdStageAttributeValue(stage, prim, property, time, scratch, out);
}

// Returns element `index` of a value of a scalar type of numbers as a double. Returns false for another type.
bool numberAt(const PrUsdValue& value, uint32_t index, double* out) {
    switch (value.type) {
        case PrUsdType_Bool:
            *out = static_cast<const bool*>(value.data)[index] ? 1 : 0;
            return true;
        case PrUsdType_Int:
            *out = static_cast<const int32_t*>(value.data)[index];
            return true;
        case PrUsdType_UInt:
            *out = static_cast<const uint32_t*>(value.data)[index];
            return true;
        case PrUsdType_Half:
            *out = double(prFloat16ToFloat(static_cast<const PrFloat16*>(value.data)[index]));
            return true;
        case PrUsdType_Float:
            *out = double(static_cast<const float*>(value.data)[index]);
            return true;
        case PrUsdType_Double:
        case PrUsdType_TimeCode:
            *out = static_cast<const double*>(value.data)[index];
            return true;
        default:
            return false;
    }
}

// Returns the number of components of an element of a vector type of floats, doubles or halves, or of a scalar of them,
// with its component type in `*component`. Returns zero for any other type.
uint32_t vectorWidth(PrUsdType type, PrUsdType* component) {
    switch (type) {
        case PrUsdType_Half:
        case PrUsdType_Float:
        case PrUsdType_Double:
            *component = type;
            return 1;
        case PrUsdType_Half2:
        case PrUsdType_Half3:
        case PrUsdType_Half4:
            *component = PrUsdType_Half;
            return 2 + uint32_t(type - PrUsdType_Half2);
        case PrUsdType_Float2:
        case PrUsdType_Float3:
        case PrUsdType_Float4:
            *component = PrUsdType_Float;
            return 2 + uint32_t(type - PrUsdType_Float2);
        case PrUsdType_Double2:
        case PrUsdType_Double3:
        case PrUsdType_Double4:
            *component = PrUsdType_Double;
            return 2 + uint32_t(type - PrUsdType_Double2);
        case PrUsdType_Quath:
            *component = PrUsdType_Half;
            return 4;
        case PrUsdType_Quatf:
            *component = PrUsdType_Float;
            return 4;
        case PrUsdType_Quatd:
            *component = PrUsdType_Double;
            return 4;
        default:
            return 0;
    }
}

// Reads `width` components of element `index` of a vector value as doubles. Returns false if the value's elements have
// another number of components, or are not numbers.
bool vectorAt(const PrUsdValue& value, uint32_t index, uint32_t width, double* out) {
    PrUsdType component = PrUsdType_Block;
    if (vectorWidth(value.type, &component) != width || index >= value.count)
        return false;
    for (uint32_t i = 0; i < width; ++i) {
        size_t at = size_t(index) * width + i;
        switch (component) {
            case PrUsdType_Half:
                out[i] = double(prFloat16ToFloat(static_cast<const PrFloat16*>(value.data)[at]));
                break;
            case PrUsdType_Float:
                out[i] = double(static_cast<const float*>(value.data)[at]);
                break;
            default:
                out[i] = static_cast<const double*>(value.data)[at];
                break;
        }
    }
    return true;
}

// Reads a number that the prim's attribute `name` holds at `time` into `*out`. Returns false, leaving `*out`, if there
// is no value or it is no number.
bool readNumber(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        double* out) {
    PrUsdValue value{};
    if (!attributeValue(stage, prim, name, time, scratch, &value) || value.isArray || value.count != 1)
        return false;
    return numberAt(value, 0, out);
}

bool readFloat(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        float* out) {
    double number = 0;
    if (!readNumber(stage, prim, name, time, scratch, &number))
        return false;
    *out = float(number);
    return true;
}

// Reads `width` floats of a vector that the prim's attribute `name` holds at `time` into `out`.
bool readVector(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        uint32_t width, float* out) {
    PrUsdValue value{};
    double read[4];
    if (!attributeValue(stage, prim, name, time, scratch, &value) || value.isArray || !vectorAt(value, 0, width, read))
        return false;
    for (uint32_t i = 0; i < width; ++i)
        out[i] = float(read[i]);
    return true;
}

// Reads a token or a string that the prim's attribute `name` holds at `time`.
bool readText(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        PrStringView* out) {
    PrUsdValue value{};
    if (!attributeValue(stage, prim, name, time, scratch, &value) || value.isArray || value.count != 1)
        return false;
    if (value.type != PrUsdType_Token && value.type != PrUsdType_String && value.type != PrUsdType_Asset)
        return false;
    *out = *static_cast<const PrStringView*>(value.data);
    return true;
}

bool readBool(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch, bool* out) {
    double number = 0;
    if (!readNumber(stage, prim, name, time, scratch, &number))
        return false;
    *out = number != 0;
    return true;
}

// Reads an array of ints that the prim's attribute `name` holds at `time`. Returns false if there is none, or if its
// elements are not ints.
bool readInts(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        const int32_t** data, uint32_t* count) {
    PrUsdValue value{};
    if (!attributeValue(stage, prim, name, time, scratch, &value) || value.type != PrUsdType_Int)
        return false;
    *data = static_cast<const int32_t*>(value.data);
    *count = value.count;
    return true;
}

// Returns an array of `value`'s elements as `width` floats each, the value's own storage if it holds floats, and
// otherwise converted into the scratch. Returns null if its elements are not vectors of `width` numbers.
const float* PR_NULLABLE floatsOf(const PrUsdValue& value, uint32_t width, PrUsdScratch* scratch) {
    PrUsdType component = PrUsdType_Block;
    if (vectorWidth(value.type, &component) != width)
        return nullptr;
    if (component == PrUsdType_Float || value.count == 0)
        return static_cast<const float*>(value.data);
    size_t count = size_t(value.count) * width;
    float* out = static_cast<float*>(prUsdScratchAlloc(scratch, count * sizeof(float), alignof(float)));
    for (size_t i = 0; i < count; ++i) {
        if (component == PrUsdType_Half)
            out[i] = float(prFloat16ToFloat(static_cast<const PrFloat16*>(value.data)[i]));
        else
            out[i] = float(static_cast<const double*>(value.data)[i]);
    }
    return out;
}

// Splits a target path into the path of its prim and the name of its property, which is empty for a prim's path.
void splitTarget(PrStringView path, PrStringView* prim, PrStringView* property) {
    size_t slash = 0;
    for (size_t i = 0; i < path.size; ++i) {
        if (path.data[i] == '/')
            slash = i;
    }
    for (size_t i = slash; i < path.size; ++i) {
        if (path.data[i] == '.') {
            *prim = PrStringView{path.data, i};
            *property = PrStringView{path.data + i + 1, path.size - i - 1};
            return;
        }
    }
    *prim = path;
    *property = PrStringView{};
}

// Returns the prim that holds what the prim at `path` composes, through instances.
PrUsdPrimIndex primAt(const PrUsdStage* stage, PrStringView path) {
    PrUsdPrimIndex prim = prUsdStageFindPrim(stage, path);
    return prim != kPrUsdNone ? prim : prUsdStageFindProxy(stage, path);
}

// Resolves the targets of the prim's relationship or connections of its attribute `name`. Returns false if it has none.
bool targetsOf(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, PrUsdScratch* scratch,
        const PrStringView** paths, uint32_t* count) {
    uint32_t property = prUsdStageFindProperty(stage, prim, name);
    PrUsdValue value{};
    if (property == kPrUsdNone || !prUsdStageTargets(stage, prim, property, true, scratch, &value, nullptr))
        return false;
    *paths = static_cast<const PrStringView*>(value.data);
    *count = value.count;
    return value.count != 0;
}

// The type names of the schemas that this module reads.
struct KindName {
    PrStringView name;
    PrUsdGeomKind kind;
};

constexpr KindName kKinds[] = {
    {"BasisCurves", PrUsdGeomKind_Gprim},
    {"Camera", PrUsdGeomKind_Camera},
    {"Capsule", PrUsdGeomKind_Shape},
    {"Capsule_1", PrUsdGeomKind_Gprim},
    {"Cone", PrUsdGeomKind_Shape},
    {"Cube", PrUsdGeomKind_Shape},
    {"Cylinder", PrUsdGeomKind_Shape},
    {"CylinderLight", PrUsdGeomKind_Light},
    {"Cylinder_1", PrUsdGeomKind_Shape},
    {"DiskLight", PrUsdGeomKind_Light},
    {"DistantLight", PrUsdGeomKind_Light},
    {"DomeLight", PrUsdGeomKind_Light},
    {"DomeLight_1", PrUsdGeomKind_Light},
    {"GeomSubset", PrUsdGeomKind_Subset},
    {"GeometryLight", PrUsdGeomKind_Light},
    {"HermiteCurves", PrUsdGeomKind_Gprim},
    {"Material", PrUsdGeomKind_Material},
    {"Mesh", PrUsdGeomKind_Mesh},
    {"NodeGraph", PrUsdGeomKind_Shader},
    {"NurbsCurves", PrUsdGeomKind_Gprim},
    {"NurbsPatch", PrUsdGeomKind_Gprim},
    {"Plane", PrUsdGeomKind_Shape},
    {"PointInstancer", PrUsdGeomKind_PointInstancer},
    {"Points", PrUsdGeomKind_Gprim},
    {"PortalLight", PrUsdGeomKind_Light},
    {"RectLight", PrUsdGeomKind_Light},
    {"Scope", PrUsdGeomKind_Scope},
    {"Shader", PrUsdGeomKind_Shader},
    {"SkelRoot", PrUsdGeomKind_Xform},
    {"Skeleton", PrUsdGeomKind_Xform},
    {"SpatialAudio", PrUsdGeomKind_Xform},
    {"Sphere", PrUsdGeomKind_Shape},
    {"SphereLight", PrUsdGeomKind_Light},
    {"TetMesh", PrUsdGeomKind_Gprim},
    {"Volume", PrUsdGeomKind_Gprim},
    {"Xform", PrUsdGeomKind_Xform},
};

// ---------------------------------------------------------------------------------------------------------------------
// Transforms

PrDMatrix4 translation(const double* t) {
    PrDMatrix4 m = prDMatrix4Identity();
    m.m[0][3] = t[0];
    m.m[1][3] = t[1];
    m.m[2][3] = t[2];
    return m;
}

PrDMatrix4 scaling(const double* s) {
    PrDMatrix4 m = prDMatrix4Identity();
    m.m[0][0] = s[0];
    m.m[1][1] = s[1];
    m.m[2][2] = s[2];
    return m;
}

// Returns a rotation by `degrees` counterclockwise about the axis `axis`, 0 for x, 1 for y and 2 for z, seen from the
// axis's positive end.
PrDMatrix4 rotation(uint32_t axis, double degrees) {
    double c = prF64Cos(degrees * kDegrees);
    double s = prF64Sin(degrees * kDegrees);
    // An exact quarter turn has no rounding error in its cosine.
    if (prF64Abs(c) < 1e-15)
        c = 0;
    PrDMatrix4 m = prDMatrix4Identity();
    uint32_t a = (axis + 1) % 3;
    uint32_t b = (axis + 2) % 3;
    m.m[a][a] = c;
    m.m[a][b] = -s;
    m.m[b][a] = s;
    m.m[b][b] = c;
    return m;
}

// Returns the rotation of the quaternion x*i + y*j + z*k + w as OpenUSD turns one into a transform (GfRotation): by
// twice the arccosine of its real part about the direction of its imaginary part, which a quaternion of unit length
// gives exactly and one of another length gives as its real part says.
PrDMatrix4 quaternionMatrix(double x, double y, double z, double w) {
    double length = prF64Sqrt(x * x + y * y + z * z);
    PrDMatrix4 m = prDMatrix4Identity();
    if (!(length > 1e-10))
        return m;
    double angle = 2 * prF64Acos(w < -1 ? -1 : (w > 1 ? 1 : w));
    double c = prF64Cos(angle);
    double s = prF64Sin(angle);
    double t = 1 - c;
    x /= length;
    y /= length;
    z /= length;
    m.m[0][0] = t * x * x + c;
    m.m[0][1] = t * x * y - s * z;
    m.m[0][2] = t * x * z + s * y;
    m.m[1][0] = t * x * y + s * z;
    m.m[1][1] = t * y * y + c;
    m.m[1][2] = t * y * z - s * x;
    m.m[2][0] = t * x * z - s * y;
    m.m[2][1] = t * y * z + s * x;
    m.m[2][2] = t * z * z + c;
    return m;
}

// Returns `a` times `b`, as column vectors: a point maps through `b` and then through `a`.
PrDMatrix4 times(const PrDMatrix4& a, const PrDMatrix4& b) {
    return prDMatrix4Concat(&b, &a);
}

// The kinds of transform operation, by the second part of an operation's name.
struct OpKind {
    PrStringView name;
    uint8_t kind;          // an Op
};

enum {
    Op_Translate,
    Op_TranslateX,
    Op_TranslateY,
    Op_TranslateZ,
    Op_Scale,
    Op_ScaleX,
    Op_ScaleY,
    Op_ScaleZ,
    Op_RotateX,
    Op_RotateY,
    Op_RotateZ,
    Op_RotateXYZ,
    Op_RotateXZY,
    Op_RotateYXZ,
    Op_RotateYZX,
    Op_RotateZXY,
    Op_RotateZYX,
    Op_Orient,
    Op_Transform,
};
typedef uint8_t Op;

constexpr OpKind kOps[] = {
    {"orient", Op_Orient},
    {"rotateX", Op_RotateX},
    {"rotateXYZ", Op_RotateXYZ},
    {"rotateXZY", Op_RotateXZY},
    {"rotateY", Op_RotateY},
    {"rotateYXZ", Op_RotateYXZ},
    {"rotateYZX", Op_RotateYZX},
    {"rotateZ", Op_RotateZ},
    {"rotateZXY", Op_RotateZXY},
    {"rotateZYX", Op_RotateZYX},
    {"scale", Op_Scale},
    {"scaleX", Op_ScaleX},
    {"scaleY", Op_ScaleY},
    {"scaleZ", Op_ScaleZ},
    {"transform", Op_Transform},
    {"translate", Op_Translate},
    {"translateX", Op_TranslateX},
    {"translateY", Op_TranslateY},
    {"translateZ", Op_TranslateZ},
};

// Finds the kind of the operation of the attribute `name`, `xformOp:<kind>` with an optional suffix.
bool opKindOf(PrStringView name, Op* out) {
    PrStringView prefix = "xformOp:";
    if (!prStringStartsWith(name, prefix))
        return false;
    PrStringView rest{name.data + prefix.size, name.size - prefix.size};
    size_t end = 0;
    while (end < rest.size && rest.data[end] != ':')
        ++end;
    PrStringView kind{rest.data, end};
    for (const OpKind& op : kOps) {
        if (op.name == kind) {
            *out = op.kind;
            return true;
        }
    }
    return false;
}

// Returns the transform of one operation from its value. Returns false for a value of a type that the operation does
// not take.
bool opMatrix(Op kind, const PrUsdValue& value, PrDMatrix4* out) {
    if (value.isArray || value.count != 1)
        return false;
    double v[4] = {};
    switch (kind) {
        case Op_Translate:
            if (!vectorAt(value, 0, 3, v))
                return false;
            *out = translation(v);
            return true;
        case Op_Scale:
            if (!vectorAt(value, 0, 3, v))
                return false;
            *out = scaling(v);
            return true;
        case Op_TranslateX:
        case Op_TranslateY:
        case Op_TranslateZ: {
            double t[3] = {0, 0, 0};
            if (!numberAt(value, 0, &t[kind - Op_TranslateX]))
                return false;
            *out = translation(t);
            return true;
        }
        case Op_ScaleX:
        case Op_ScaleY:
        case Op_ScaleZ: {
            double s[3] = {1, 1, 1};
            if (!numberAt(value, 0, &s[kind - Op_ScaleX]))
                return false;
            *out = scaling(s);
            return true;
        }
        case Op_RotateX:
        case Op_RotateY:
        case Op_RotateZ:
            if (!numberAt(value, 0, v))
                return false;
            *out = rotation(uint32_t(kind - Op_RotateX), v[0]);
            return true;
        case Op_Orient:
            if (!vectorAt(value, 0, 4, v))
                return false;
            // A quaternion's imaginary parts come first, and then its real part.
            *out = quaternionMatrix(v[0], v[1], v[2], v[3]);
            return true;
        case Op_Transform:
            if (value.type != PrUsdType_Matrix4d)
                return false;
            *out = *static_cast<const PrDMatrix4*>(value.data);
            return true;
        default: {
            if (!vectorAt(value, 0, 3, v))
                return false;
            // The axes in the order that the name gives, which is the order in which they turn a point.
            static const uint8_t kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
            const uint8_t* order = kOrders[kind - Op_RotateXYZ];
            PrDMatrix4 m = prDMatrix4Identity();
            for (uint32_t i = 0; i < 3; ++i)
                m = times(rotation(order[i], v[order[i]]), m);
            *out = m;
            return true;
        }
    }
}

// The operations of a prim's order.
struct OrderItem {
    uint32_t property;
    Op kind;
    bool inverse;
};

// Reads the prim's `xformOpOrder` into `items`. Returns false if it names an operation that the prim does not author or
// that the schema does not know.
bool readOrder(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch, RawArray<OrderItem>* items,
        bool* resetsStack) {
    *resetsStack = false;
    PrUsdValue order{};
    if (!attributeValue(stage, prim, "xformOpOrder", kPrF64Nan, scratch, &order) || order.type != PrUsdType_Token)
        return true;
    const PrStringView* names = static_cast<const PrStringView*>(order.data);
    for (uint32_t i = 0; i < order.count; ++i) {
        PrStringView name = names[i];
        if (name == "!resetXformStack!") {
            // Only the first entry may reset the stack, and what came before it is discarded.
            items->clear();
            *resetsStack = true;
            continue;
        }
        OrderItem item{};
        PrStringView invert = "!invert!";
        if (prStringStartsWith(name, invert)) {
            item.inverse = true;
            name = PrStringView{name.data + invert.size, name.size - invert.size};
        }
        if (!opKindOf(name, &item.kind))
            return false;
        item.property = prUsdStageFindProperty(stage, prim, name);
        if (item.property == kPrUsdNone)
            return false;
        items->push(item);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shading networks

// Returns the shader's `info:id`, or the empty view.
PrStringView shaderId(const PrUsdStage* stage, PrUsdPrimIndex shader, PrUsdScratch* scratch) {
    PrStringView id{};
    readText(stage, shader, "info:id", kPrF64Nan, scratch, &id);
    return id;
}

// An output of a shader that a connection reaches: the shader, and the output's name without `outputs:`.
struct Source {
    PrUsdPrimIndex shader;
    PrStringView output;
};

// Follows the connection of the attribute `name` of `prim` to the output of a shader, through the inputs and outputs of
// node graphs and materials, which pass a connection on. Returns false if the attribute connects to nothing, and sets
// `*value` to the prim and the attribute whose value decides where the chain ends at an input without a connection.
bool follow(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, PrUsdScratch* scratch, Source* out,
        PrUsdPrimIndex* valuePrim, PrStringView* valueName) {
    *valuePrim = prim;
    *valueName = name;
    for (uint32_t depth = 0; depth < 32; ++depth) {
        const PrStringView* paths = nullptr;
        uint32_t count = 0;
        if (!targetsOf(stage, prim, name, scratch, &paths, &count))
            return false;
        PrStringView primPath{};
        PrStringView property{};
        splitTarget(paths[0], &primPath, &property);
        PrUsdPrimIndex target = primAt(stage, primPath);
        if (target == kPrUsdNone)
            return false;
        PrUsdGeomKind kind = prUsdGeomPrimKind(stage, target);
        bool passes = kind == PrUsdGeomKind_Material || prUsdStagePrims(stage)[target].typeName == "NodeGraph";
        if (!passes) {
            if (!prStringStartsWith(property, "outputs:"))
                return false;
            *out = Source{target, PrStringView{property.data + 8, property.size - 8}};
            return true;
        }
        // An input or an output of a node graph or a material passes on its own connection, or its own value.
        prim = target;
        name = property;
        *valuePrim = prim;
        *valueName = name;
    }
    return false;
}

// Reads the value that an input of a shader has, following connections to the inputs of node graphs and materials, into
// `width` floats. Returns false if no value decides it, or if a connection reaches a shader's output.
bool inputNumbers(const PrUsdStage* stage, PrUsdPrimIndex shader, PrStringView name, double time, PrUsdScratch* scratch,
        uint32_t width, float* out) {
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (follow(stage, shader, name, scratch, &source, &valuePrim, &valueName))
        return false;
    if (width == 1)
        return readFloat(stage, valuePrim, valueName, time, scratch, out);
    return readVector(stage, valuePrim, valueName, time, scratch, width, out);
}

bool inputText(const PrUsdStage* stage, PrUsdPrimIndex shader, PrStringView name, double time, PrUsdScratch* scratch,
        PrStringView* out) {
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (follow(stage, shader, name, scratch, &source, &valuePrim, &valueName))
        return false;
    return readText(stage, valuePrim, valueName, time, scratch, out);
}

PrUsdTextureWrap wrapOf(PrStringView name) {
    if (name == "mirror")
        return PrUsdTextureWrap_Mirror;
    if (name == "clamp")
        return PrUsdTextureWrap_Clamp;
    if (name == "black")
        return PrUsdTextureWrap_Black;
    return PrUsdTextureWrap_Repeat;
}

// Reads the coordinates that a UsdUVTexture's `inputs:st` reaches: a UsdPrimvarReader_float2's primitive variable, and
// a UsdTransform2d between the two if there is one.
void readCoordinates(const PrUsdStage* stage, PrUsdPrimIndex shader, double time, PrUsdScratch* scratch, PrUsdTexture* texture) {
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (!follow(stage, shader, "inputs:st", scratch, &source, &valuePrim, &valueName))
        return;
    if (shaderId(stage, source.shader, scratch) == "UsdTransform2d") {
        float translation[2] = {0, 0};
        float rotation = 0;
        float scale[2] = {1, 1};
        inputNumbers(stage, source.shader, "inputs:translation", time, scratch, 2, translation);
        inputNumbers(stage, source.shader, "inputs:rotation", time, scratch, 1, &rotation);
        inputNumbers(stage, source.shader, "inputs:scale", time, scratch, 2, scale);
        PrAffine2 scaled =
            prAffine2Concat(prAffine2Scale({scale[0], scale[1]}), prAffine2Rotate(prF32DegreesToRadians(rotation)));
        texture->transform = prAffine2Concat(scaled, prAffine2Translate({translation[0], translation[1]}));
        if (!follow(stage, source.shader, "inputs:in", scratch, &source, &valuePrim, &valueName))
            return;
    }
    if (prStringStartsWith(shaderId(stage, source.shader, scratch), "UsdPrimvarReader_"))
        inputText(stage, source.shader, "inputs:varname", time, scratch, &texture->primvar);
}

// Returns the color space that a color space's name decodes a file by, as the transfer function that the name implies:
// sRGB for the spaces of sRGB's transfer function, by MaterialX's names and the ACES configuration's, and raw for any
// other, such as the linear `lin_rec709`, `acescg` and `lin_ap1`.
PrUsdTextureColorSpace colorSpaceOf(PrStringView name) {
    if (name == "srgb_texture" || name == "srgb_rec709_scene" || name == "sRGB")
        return PrUsdTextureColorSpace_Srgb;
    return PrUsdTextureColorSpace_Raw;
}

// Reads the UsdUVTexture `shader` and its output `output` into `*out`. Returns false if the shader is no UsdUVTexture,
// or the output is none of its own.
bool readTexture(const PrUsdStage* stage, PrUsdPrimIndex shader, PrStringView output, double time, PrUsdScratch* scratch,
        PrUsdTexture* out) {
    if (shaderId(stage, shader, scratch) != "UsdUVTexture")
        return false;
    PrUsdTexture texture{
        .shader = shader,
        .fileProperty = kPrUsdNone,
        .scale = {1, 1, 1, 1},
        .fallback = {0, 0, 0, 1},
        .transform = prAffine2Identity(),
    };
    if (output == "r")
        texture.channel = PrUsdTextureChannel_R;
    else if (output == "g")
        texture.channel = PrUsdTextureChannel_G;
    else if (output == "b")
        texture.channel = PrUsdTextureChannel_B;
    else if (output == "a")
        texture.channel = PrUsdTextureChannel_A;
    else if (output == "rgb")
        texture.channel = PrUsdTextureChannel_Rgb;
    else
        return false;
    // The file may come through a material's input, which then decides the layer that the path is relative to.
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (!follow(stage, shader, "inputs:file", scratch, &source, &valuePrim, &valueName)) {
        PrUsdValue file{};
        if (attributeValue(stage, valuePrim, valueName, time, scratch, &file) && file.type == PrUsdType_Asset &&
                !file.isArray) {
            texture.shader = valuePrim;
            texture.fileProperty = prUsdStageFindProperty(stage, valuePrim, valueName);
            texture.file = *static_cast<const PrStringView*>(file.data);
        }
    }
    PrStringView text{};
    if (inputText(stage, shader, "inputs:wrapS", time, scratch, &text))
        texture.wrapS = wrapOf(text);
    if (inputText(stage, shader, "inputs:wrapT", time, scratch, &text))
        texture.wrapT = wrapOf(text);
    if (inputText(stage, shader, "inputs:sourceColorSpace", time, scratch, &text)) {
        if (text == "raw")
            texture.colorSpace = PrUsdTextureColorSpace_Raw;
        else if (text == "sRGB")
            texture.colorSpace = PrUsdTextureColorSpace_Srgb;
    }
    // A texture left to `auto` takes the color space that its file's attribute names, if it names one.
    PrUsdValue space{};
    if (texture.colorSpace == PrUsdTextureColorSpace_Auto && texture.fileProperty != kPrUsdNone &&
            prUsdStagePropertyField(stage, texture.shader, texture.fileProperty, "colorSpace", scratch, &space) &&
            space.type == PrUsdType_Token && space.count == 1)
        texture.colorSpace = colorSpaceOf(*static_cast<const PrStringView*>(space.data));
    inputNumbers(stage, shader, "inputs:scale", time, scratch, 4, texture.scale);
    inputNumbers(stage, shader, "inputs:bias", time, scratch, 4, texture.bias);
    inputNumbers(stage, shader, "inputs:fallback", time, scratch, 4, texture.fallback);
    readCoordinates(stage, shader, time, scratch, &texture);
    *out = texture;
    return true;
}

// The inputs of UsdPreviewSurface: their names, their widths and their fallbacks, by PrUsdPreviewInput.
struct PreviewInputInfo {
    PrStringView name;
    uint32_t width;
    float fallback[3];
};

constexpr PreviewInputInfo kPreviewInputs[PrUsdPreviewInput_Count] = {
    {"inputs:diffuseColor", 3, {0.18f, 0.18f, 0.18f}},
    {"inputs:emissiveColor", 3, {0, 0, 0}},
    {"inputs:specularColor", 3, {0, 0, 0}},
    {"inputs:metallic", 1, {0}},
    {"inputs:roughness", 1, {0.5f}},
    {"inputs:clearcoat", 1, {0}},
    {"inputs:clearcoatRoughness", 1, {0.01f}},
    {"inputs:opacity", 1, {1}},
    {"inputs:opacityThreshold", 1, {0}},
    {"inputs:ior", 1, {1.5f}},
    {"inputs:normal", 3, {0, 0, 1}},
    {"inputs:displacement", 1, {0}},
    {"inputs:occlusion", 1, {1}},
};

// ---------------------------------------------------------------------------------------------------------------------
// MaterialX networks

// The types that end a MaterialX node definition's name, after its category.
constexpr PrStringView kMtlxTypes[] = {
    "BSDF", "EDF", "VDF", "boolean", "color3", "color4", "displacementshader", "filename", "float", "integer",
    "lightshader", "material", "matrix33", "matrix44", "string", "surfaceshader", "vector2", "vector3", "vector4",
    "volumeshader",
};

// Returns the number of floats of an attribute's type, by its name, or 0 for a type of no floats.
uint32_t floatsOfType(PrStringView typeName) {
    if (typeName == "float" || typeName == "int" || typeName == "bool" || typeName == "half" || typeName == "double")
        return 1;
    if (prStringEndsWith(typeName, "2f") || typeName == "float2" || typeName == "half2" || typeName == "double2")
        return 2;
    if (prStringEndsWith(typeName, "3f") || typeName == "float3" || typeName == "half3" || typeName == "double3")
        return 3;
    if (prStringEndsWith(typeName, "4f") || typeName == "float4" || typeName == "half4" || typeName == "double4")
        return 4;
    return 0;
}

// Returns the number of floats of the prim's property `name`, by its declared type, or 0 if it has none of floats.
uint32_t floatsOf(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, PrUsdScratch* scratch) {
    uint32_t property = prUsdStageFindProperty(stage, prim, name);
    PrUsdValue type{};
    if (property == kPrUsdNone || !prUsdStagePropertyField(stage, prim, property, "typeName", scratch, &type) ||
            type.type != PrUsdType_Token || type.count != 1)
        return 0;
    return floatsOfType(*static_cast<const PrStringView*>(type.data));
}

// What an input of a MaterialX network folds to: `width` elements, each a constant `b[i]` plus, with a source that
// varies over a surface, the sum of `m[i][j]` times the source's channel j. The source is a texture's texel, or else
// the value of the primitive variable `primvar`, `primvarFallback` where the surface has none. The whole is then
// multiplied element by element by the primitive variable `tint`, if there is one.
struct Folded {
    uint32_t width;
    bool textured;
    bool normal;
    bool approximated;
    PrUsdTexture texture;
    float m[4][4];
    float b[4];
    PrStringView primvar;
    float primvarFallback[4];
    PrStringView tint;
    float tintFallback[4];
};

// Returns true if a folded value varies over a surface: it reads a texture or a primitive variable.
bool varies(const Folded& f) {
    return f.textured || f.primvar.size != 0;
}

// Returns true if two folded values that vary read the same source.
bool sameSource(const Folded& a, const Folded& b) {
    if (a.textured != b.textured)
        return false;
    if (a.textured)
        return a.texture.shader == b.texture.shader && a.texture.file == b.texture.file;
    return a.primvar == b.primvar;
}

// Returns true if a folded value is a primitive variable's value as it is, which may multiply another as a tint.
bool isPlainPrimvar(const Folded& f) {
    if (f.textured || f.primvar.size == 0 || f.tint.size != 0)
        return false;
    for (uint32_t i = 0; i < f.width; ++i) {
        for (uint32_t j = 0; j < 4; ++j) {
            if (f.m[i][j] != (i == j ? 1.0f : 0.0f))
                return false;
        }
        if (f.b[i] != 0)
            return false;
    }
    return true;
}

Folded constantOf(const float* values, uint32_t width) {
    Folded f{.width = width};
    for (uint32_t i = 0; i < width; ++i)
        f.b[i] = values[i];
    return f;
}

// Returns `f` with `width` elements: one element spread over all, a color of three given an alpha of 1, and the first
// elements of a wider value.
Folded resized(const Folded& f, uint32_t width) {
    if (f.width == width || f.width == 0)
        return f;
    Folded out = f;
    out.width = width;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t from = f.width == 1 ? 0 : i;
        bool kept = i < width && (f.width == 1 || i < f.width);
        for (uint32_t j = 0; j < 4; ++j)
            out.m[i][j] = kept ? f.m[from][j] : 0;
        out.b[i] = kept ? f.b[from] : (i == 3 ? 1.0f : 0.0f);
    }
    return out;
}

// Returns `a * ka + b * kb + c`, element by element, of two values of one width, which need at most one source that
// varies between them. With two sources it keeps `a`'s, and says so.
Folded combined(const Folded& a, const float* ka, const Folded& b, const float* kb, const float* c) {
    bool both = varies(a) && varies(b);
    bool same = both && sameSource(a, b);
    Folded out = varies(a) ? a : b;
    out.width = a.width;
    out.normal = a.normal || b.normal;
    out.approximated = a.approximated || b.approximated || (both && !same) || (b.tint.size != 0 && b.tint != a.tint);
    for (uint32_t i = 0; i < out.width; ++i) {
        for (uint32_t j = 0; j < 4; ++j) {
            float fromA = varies(a) ? a.m[i][j] * ka[i] : 0;
            float fromB = varies(b) && (!varies(a) || same) ? b.m[i][j] * kb[i] : 0;
            out.m[i][j] = fromA + fromB;
        }
        out.b[i] = a.b[i] * ka[i] + b.b[i] * kb[i] + c[i];
    }
    return out;
}

// What folding reads a network with.
struct Fold {
    const PrUsdStage* stage;
    double time;
    PrUsdScratch* scratch;
    uint32_t depth;
};

Folded foldOutput(Fold& f, Source source, uint32_t width);

// Folds the input `name` of a node, of `width` elements where its type does not say, with `fallback` where nothing
// decides it.
Folded foldInput(Fold& f, PrUsdPrimIndex node, PrStringView name, uint32_t width, const float* fallback) {
    uint32_t declared = floatsOf(f.stage, node, name, f.scratch);
    if (declared != 0)
        width = declared;
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (follow(f.stage, node, name, f.scratch, &source, &valuePrim, &valueName))
        return resized(foldOutput(f, source, width), width);
    float values[4] = {};
    for (uint32_t i = 0; i < width; ++i)
        values[i] = fallback[i];
    if (width == 1)
        readFloat(f.stage, valuePrim, valueName, f.time, f.scratch, values);
    else
        readVector(f.stage, valuePrim, valueName, f.time, f.scratch, width, values);
    return constantOf(values, width);
}

// Folds an input of a node to `width` elements, with one fallback for every element.
Folded foldAs(Fold& f, PrUsdPrimIndex node, PrStringView name, uint32_t width, float fallback) {
    const float values[4] = {fallback, fallback, fallback, fallback};
    return resized(foldInput(f, node, name, width, values), width);
}

PrUsdTextureWrap addressOf(PrStringView mode) {
    if (mode == "clamp")
        return PrUsdTextureWrap_Clamp;
    if (mode == "mirror")
        return PrUsdTextureWrap_Mirror;
    if (mode == "constant")
        return PrUsdTextureWrap_Black;
    return PrUsdTextureWrap_Repeat;
}

// Writes to `*out` the transform of a place2d node, by its definition in the specification ("place2d"): the coordinates
// less the pivot, and then for the order SRT, divided by the scale, rotated counterclockwise by the rotation's degrees
// and less the offset, or for TRS, less the offset, rotated and divided by the scale, and last the pivot added back.
// Returns false for a scale with a component of zero, which divides by it.
bool placementOf(Fold& f, PrUsdPrimIndex node, PrAffine2* out) {
    float pivot[2] = {0, 0};
    float scale[2] = {1, 1};
    float rotate = 0;
    float offset[2] = {0, 0};
    float order = 0;
    inputNumbers(f.stage, node, "inputs:pivot", f.time, f.scratch, 2, pivot);
    inputNumbers(f.stage, node, "inputs:scale", f.time, f.scratch, 2, scale);
    inputNumbers(f.stage, node, "inputs:rotate", f.time, f.scratch, 1, &rotate);
    inputNumbers(f.stage, node, "inputs:offset", f.time, f.scratch, 2, offset);
    inputNumbers(f.stage, node, "inputs:operationorder", f.time, f.scratch, 1, &order);
    if (scale[0] == 0 || scale[1] == 0)
        return false;
    PrAffine2 divide = prAffine2Scale({1 / scale[0], 1 / scale[1]});
    PrAffine2 turn = prAffine2Rotate(prF32DegreesToRadians(rotate));
    PrAffine2 move = prAffine2Translate({-offset[0], -offset[1]});
    PrAffine2 inner = prAffine2Concat(prAffine2Concat(divide, turn), move);
    if (order >= 1)
        inner = prAffine2Concat(prAffine2Concat(move, turn), divide);
    PrAffine2 around = prAffine2Concat(prAffine2Translate({-pivot[0], -pivot[1]}), inner);
    *out = prAffine2Concat(around, prAffine2Translate({pivot[0], pivot[1]}));
    return true;
}

// Reads the coordinates that an image's `texcoord` reaches into the texture: a texcoord node's set or a geompropvalue
// node's primitive variable, and `st` where nothing connects them.
void readMtlxCoordinates(Fold& f, PrUsdPrimIndex image, PrUsdTexture* texture, bool* approximated) {
    texture->primvar = "st";
    texture->transform = prAffine2Identity();
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (!follow(f.stage, image, "inputs:texcoord", f.scratch, &source, &valuePrim, &valueName))
        return;
    PrStringView category = prUsdMtlxNodeCategory(shaderId(f.stage, source.shader, f.scratch));
    // Placements apply in the order that the coordinates meet them, the one nearest the image last.
    for (uint32_t depth = 0; category == "place2d" && depth < 64; ++depth) {
        PrAffine2 place{};
        if (!placementOf(f, source.shader, &place)) {
            *approximated = true;
            break;
        }
        texture->transform = prAffine2Concat(place, texture->transform);
        if (!follow(f.stage, source.shader, "inputs:texcoord", f.scratch, &source, &valuePrim, &valueName))
            return;
        category = prUsdMtlxNodeCategory(shaderId(f.stage, source.shader, f.scratch));
    }
    if (category == "texcoord") {
        float index = 0;
        inputNumbers(f.stage, source.shader, "inputs:index", f.time, f.scratch, 1, &index);
        texture->primvar = "st";
        if (index >= 1)
            texture->primvar = "st1";
        *approximated = *approximated || index >= 2;
    } else if (category == "geompropvalue") {
        inputText(f.stage, source.shader, "inputs:geomprop", f.time, f.scratch, &texture->primvar);
    } else {
        *approximated = true;
    }
}

// Folds an image or a tiled image node's output of `width` elements: the texel's first channels, or the node's default
// where it names no file.
Folded foldImage(Fold& f, PrUsdPrimIndex node, uint32_t width, bool tiled) {
    Folded out{.width = width, .textured = true};
    PrUsdTexture& texture = out.texture;
    texture = PrUsdTexture{
        .shader = node,
        .fileProperty = kPrUsdNone,
        .scale = {1, 1, 1, 1},
        .transform = prAffine2Identity(),
    };
    Source source{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    PrUsdValue file{};
    if (follow(f.stage, node, "inputs:file", f.scratch, &source, &valuePrim, &valueName) ||
            !attributeValue(f.stage, valuePrim, valueName, f.time, f.scratch, &file) || file.type != PrUsdType_Asset ||
            file.isArray || file.count != 1) {
        float fallback[4] = {0, 0, 0, 0};
        inputNumbers(f.stage, node, "inputs:default", f.time, f.scratch, width, fallback);
        return constantOf(fallback, width);
    }
    texture.shader = valuePrim;
    texture.fileProperty = prUsdStageFindProperty(f.stage, valuePrim, valueName);
    texture.file = *static_cast<const PrStringView*>(file.data);
    PrUsdValue space{};
    texture.colorSpace = PrUsdTextureColorSpace_Raw;
    if (prUsdStagePropertyField(f.stage, valuePrim, texture.fileProperty, "colorSpace", f.scratch, &space) &&
            space.type == PrUsdType_Token && space.count == 1)
        texture.colorSpace = colorSpaceOf(*static_cast<const PrStringView*>(space.data));
    PrStringView mode{};
    if (inputText(f.stage, node, "inputs:uaddressmode", f.time, f.scratch, &mode))
        texture.wrapS = addressOf(mode);
    if (inputText(f.stage, node, "inputs:vaddressmode", f.time, f.scratch, &mode))
        texture.wrapT = addressOf(mode);
    inputNumbers(f.stage, node, "inputs:default", f.time, f.scratch, width, texture.fallback);
    readMtlxCoordinates(f, node, &texture, &out.approximated);
    if (tiled) {
        // The coordinates times the tiling, less the offset ("tiledimage").
        float tiling[2] = {1, 1};
        float offset[2] = {0, 0};
        inputNumbers(f.stage, node, "inputs:uvtiling", f.time, f.scratch, 2, tiling);
        inputNumbers(f.stage, node, "inputs:uvoffset", f.time, f.scratch, 2, offset);
        PrAffine2 tile =
            prAffine2Concat(prAffine2Scale({tiling[0], tiling[1]}), prAffine2Translate({-offset[0], -offset[1]}));
        texture.transform = prAffine2Concat(texture.transform, tile);
    }
    for (uint32_t i = 0; i < width; ++i)
        out.m[i][i] = 1;
    return out;
}

// Returns the element that a channel's letter names, as a swizzle or a separate node's output names it, or 4 for none.
uint32_t channelOf(char c) {
    switch (c) {
        case 'r':
        case 'x':
            return 0;
        case 'g':
        case 'y':
            return 1;
        case 'b':
        case 'z':
            return 2;
        case 'a':
        case 'w':
            return 3;
        default:
            return 4;
    }
}

// Returns element `index` of `f` as a value of one element, or its first if it has no such element.
Folded elementOf(const Folded& f, uint32_t index) {
    Folded out = f;
    out.width = 1;
    if (index >= f.width)
        index = 0;
    for (uint32_t j = 0; j < 4; ++j)
        out.m[0][j] = f.m[index][j];
    out.b[0] = f.b[index];
    return out;
}

// Returns the number of floats of a node's output `output`, by its declared type, or 0.
uint32_t outputFloats(Fold& f, PrUsdPrimIndex node, PrStringView output) {
    char name[64] = "outputs:";
    if (output.size > sizeof(name) - 8)
        return 0;
    for (size_t i = 0; i < output.size; ++i)
        name[8 + i] = output.data[i];
    return floatsOf(f.stage, node, PrStringView{name, 8 + output.size}, f.scratch);
}

// Folds the arithmetic and channel nodes of MaterialX's standard library by their definitions in the specification
// ("Standard Node Definitions"), an image as its texture, and any other node as its first input.
Folded foldNode(Fold& f, PrUsdPrimIndex node, PrStringView category, PrStringView output, uint32_t w) {
    const float zero[4] = {0, 0, 0, 0};
    const float one[4] = {1, 1, 1, 1};
    const float minus[4] = {-1, -1, -1, -1};
    if (category == "image" || category == "tiledimage")
        return foldImage(f, node, w, category == "tiledimage");
    if (category == "constant")
        return foldAs(f, node, "inputs:value", w, 0);
    if (category == "multiply" || category == "divide") {
        Folded a = foldAs(f, node, "inputs:in1", w, 0);
        Folded b = foldAs(f, node, "inputs:in2", w, 1);
        if (!varies(b)) {
            float k[4];
            for (uint32_t i = 0; i < 4; ++i)
                k[i] = category == "divide" ? (b.b[i] != 0 ? 1 / b.b[i] : 0) : b.b[i];
            return combined(a, k, constantOf(zero, w), zero, zero);
        }
        if (!varies(a) && category == "multiply")
            return combined(b, a.b, constantOf(zero, w), zero, zero);
        // A primitive variable's value as it is multiplies the other as a tint.
        bool tintsA = category == "multiply" && isPlainPrimvar(b) && a.tint.size == 0;
        bool tintsB = category == "multiply" && !tintsA && isPlainPrimvar(a) && b.tint.size == 0;
        if (tintsA || tintsB) {
            const Folded& tint = tintsA ? b : a;
            Folded out = tintsA ? a : b;
            out.tint = tint.primvar;
            for (uint32_t i = 0; i < 4; ++i)
                out.tintFallback[i] = tint.primvarFallback[i];
            return out;
        }
        a.approximated = true;
        return a;
    }
    if (category == "add" || category == "subtract") {
        Folded a = foldAs(f, node, "inputs:in1", w, 0);
        Folded b = foldAs(f, node, "inputs:in2", w, 0);
        return combined(a, one, b, category == "add" ? one : minus, zero);
    }
    if (category == "mix") {
        Folded fg = foldAs(f, node, "inputs:fg", w, 0);
        Folded bg = foldAs(f, node, "inputs:bg", w, 0);
        Folded amount = foldAs(f, node, "inputs:mix", 1, 0);
        float k[4];
        float rest[4];
        for (uint32_t i = 0; i < 4; ++i) {
            k[i] = amount.b[0];
            rest[i] = 1 - amount.b[0];
        }
        Folded out = combined(fg, k, bg, rest, zero);
        out.approximated = out.approximated || amount.textured;
        return out;
    }
    if (category == "invert") {
        Folded in = foldAs(f, node, "inputs:in", w, 0);
        Folded amount = foldAs(f, node, "inputs:amount", w, 1);
        return combined(in, minus, constantOf(zero, w), zero, amount.b);
    }
    if (category == "remap") {
        Folded in = foldAs(f, node, "inputs:in", w, 0);
        Folded inLow = foldAs(f, node, "inputs:inlow", w, 0);
        Folded inHigh = foldAs(f, node, "inputs:inhigh", w, 1);
        Folded outLow = foldAs(f, node, "inputs:outlow", w, 0);
        Folded outHigh = foldAs(f, node, "inputs:outhigh", w, 1);
        float k[4];
        float c[4];
        for (uint32_t i = 0; i < 4; ++i) {
            float span = inHigh.b[i] - inLow.b[i];
            k[i] = span != 0 ? (outHigh.b[i] - outLow.b[i]) / span : 0;
            c[i] = outLow.b[i] - inLow.b[i] * k[i];
        }
        return combined(in, k, constantOf(zero, w), zero, c);
    }
    if (category == "extract") {
        float index = 0;
        inputNumbers(f.stage, node, "inputs:index", f.time, f.scratch, 1, &index);
        return elementOf(foldInput(f, node, "inputs:in", 4, zero), index > 0 ? uint32_t(index) : 0);
    }
    if (category == "separate2" || category == "separate3" || category == "separate4") {
        Folded in = foldInput(f, node, "inputs:in", 4, zero);
        return elementOf(in, output.size == 4 ? channelOf(output.data[3]) : 0);
    }
    if (category == "swizzle") {
        Folded in = foldInput(f, node, "inputs:in", 4, zero);
        PrStringView channels{};
        inputText(f.stage, node, "inputs:channels", f.time, f.scratch, &channels);
        Folded out = in;
        out.width = w;
        for (uint32_t i = 0; i < w; ++i) {
            uint32_t from = i < channels.size ? channelOf(channels.data[i]) : i;
            Folded element = elementOf(in, from < 4 ? from : i);
            for (uint32_t j = 0; j < 4; ++j)
                out.m[i][j] = element.m[0][j];
            out.b[i] = element.b[0];
        }
        return out;
    }
    if (category == "luminance") {
        Folded in = foldAs(f, node, "inputs:in", 3, 0);
        // The specification's default coefficients, of ACEScg's luminance.
        float coefficients[3] = {0.2722287f, 0.6740818f, 0.0536895f};
        inputNumbers(f.stage, node, "inputs:lumacoeffs", f.time, f.scratch, 3, coefficients);
        Folded out = in;
        out.width = w;
        for (uint32_t i = 0; i < w && i < 3; ++i) {
            for (uint32_t j = 0; j < 4; ++j)
                out.m[i][j] =
                    in.m[0][j] * coefficients[0] + in.m[1][j] * coefficients[1] + in.m[2][j] * coefficients[2];
            out.b[i] = in.b[0] * coefficients[0] + in.b[1] * coefficients[1] + in.b[2] * coefficients[2];
        }
        return out;
    }
    if (category == "normalmap") {
        // A tangent-space normal of [0, 1] in each channel to [-1, 1], with its x and y scaled.
        Folded in = foldAs(f, node, "inputs:in", 3, 0.5f);
        float scale = 1;
        inputNumbers(f.stage, node, "inputs:scale", f.time, f.scratch, 1, &scale);
        const float k[4] = {2 * scale, 2 * scale, 2, 0};
        const float c[4] = {-scale, -scale, -1, 0};
        Folded out = combined(in, k, constantOf(zero, 3), zero, c);
        out.normal = true;
        return out;
    }
    if (category == "switch") {
        // `which` chooses in1 to in10, from 0.
        float which = 0;
        inputNumbers(f.stage, node, "inputs:which", f.time, f.scratch, 1, &which);
        uint32_t chosen = which >= 1 ? uint32_t(prF32Min(which, 9)) + 1 : 1;
        char name[16] = "inputs:in";
        size_t size = 9;
        if (chosen == 10) {
            name[size++] = '1';
            name[size++] = '0';
        } else {
            name[size++] = char('0' + chosen);
        }
        return foldAs(f, node, PrStringView{name, size}, w, 0);
    }
    if (category == "geompropvalue") {
        // The value is the primitive variable's, element by element, and the default where the surface has none.
        Folded fallback = foldAs(f, node, "inputs:default", w, 0);
        Folded out{.width = w};
        inputText(f.stage, node, "inputs:geomprop", f.time, f.scratch, &out.primvar);
        if (out.primvar.size == 0)
            return fallback;
        for (uint32_t i = 0; i < w; ++i) {
            out.m[i][i] = 1;
            out.primvarFallback[i] = fallback.b[i];
        }
        out.approximated = varies(fallback);
        return out;
    }
    if (category == "dot" || category == "convert")
        return foldInput(f, node, "inputs:in", w, zero);
    Folded out{};
    if (prUsdStageFindProperty(f.stage, node, "inputs:in") != kPrUsdNone)
        out = foldInput(f, node, "inputs:in", w, zero);
    else if (prUsdStageFindProperty(f.stage, node, "inputs:in1") != kPrUsdNone)
        out = foldInput(f, node, "inputs:in1", w, zero);
    else
        out = foldAs(f, node, "inputs:default", w, 0);
    out.approximated = true;
    return out;
}

Folded foldOutput(Fold& f, Source source, uint32_t width) {
    const float zero[4] = {0, 0, 0, 0};
    uint32_t declared = outputFloats(f, source.shader, source.output);
    uint32_t w = declared != 0 ? declared : width;
    if (f.depth >= 64) {
        Folded out = constantOf(zero, w);
        out.approximated = true;
        return out;
    }
    ++f.depth;
    PrStringView category = prUsdMtlxNodeCategory(shaderId(f.stage, source.shader, f.scratch));
    Folded out = resized(foldNode(f, source.shader, category, source.output, w), w);
    --f.depth;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lights

// Reads an input of a light, which UsdLux names in the `inputs:` namespace.
bool lightFloats(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        uint32_t width, float* out) {
    PrTextBuffer prefixed = {};
    prTextBufferAppend(&prefixed, "inputs:");
    prTextBufferAppend(&prefixed, name);
    PrStringView full = prTextBufferView(&prefixed);
    bool read = false;
    if (width == 1)
        read = readFloat(stage, prim, full, time, scratch, out);
    else
        read = readVector(stage, prim, full, time, scratch, width, out);
    prTextBufferReset(&prefixed);
    return read;
}

bool lightBool(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time, PrUsdScratch* scratch,
        bool* out) {
    float value = 0;
    if (!lightFloats(stage, prim, name, time, scratch, 1, &value))
        return false;
    *out = value != 0;
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The stage

PrUsdGeomStageInfo prUsdGeomStageInfo(const PrUsdStage* stage) {
    PrUsdGeomStageInfo info{
        .upAxis = PrUsdUpAxis_Y,
        .metersPerUnit = 0.01,
        .timeCodesPerSecond = prUsdStageTimeCodesPerSecond(stage),
    };
    PrUsdValue value{};
    if (prUsdStageField(stage, "upAxis", &value) && value.type == PrUsdType_Token && !value.isArray &&
            *static_cast<const PrStringView*>(value.data) == "Z")
        info.upAxis = PrUsdUpAxis_Z;
    double number = 0;
    if (prUsdStageField(stage, "metersPerUnit", &value) && !value.isArray && numberAt(value, 0, &number) && number > 0)
        info.metersPerUnit = number;
    bool start = prUsdStageField(stage, "startTimeCode", &value) && !value.isArray && numberAt(value, 0, &number);
    if (start)
        info.startTimeCode = number;
    bool end = prUsdStageField(stage, "endTimeCode", &value) && !value.isArray && numberAt(value, 0, &number);
    if (end)
        info.endTimeCode = number;
    info.hasTimeRange = start && end;
    return info;
}

// ---------------------------------------------------------------------------------------------------------------------
// Prims by schema

PrUsdGeomKind prUsdGeomPrimKind(const PrUsdStage* stage, PrUsdPrimIndex prim) {
    PrStringView type = prUsdStagePrims(stage)[prim].typeName;
    uint32_t low = 0;
    uint32_t high = uint32_t(countOf(kKinds));
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        int order = prStringCompare(kKinds[middle].name, type);
        if (order == 0)
            return kKinds[middle].kind;
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return PrUsdGeomKind_Other;
}

bool prUsdGeomKindIsXformable(PrUsdGeomKind kind) {
    switch (kind) {
        case PrUsdGeomKind_Xform:
        case PrUsdGeomKind_Mesh:
        case PrUsdGeomKind_Shape:
        case PrUsdGeomKind_Gprim:
        case PrUsdGeomKind_Camera:
        case PrUsdGeomKind_PointInstancer:
        case PrUsdGeomKind_Light:
            return true;
        default:
            return false;
    }
}

PrUsdGeomPurpose prUsdGeomPurpose(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch, bool* authored) {
    PrStringView purpose{};
    *authored = readText(stage, prim, "purpose", kPrF64Nan, scratch, &purpose);
    if (purpose == "render")
        return PrUsdGeomPurpose_Render;
    if (purpose == "proxy")
        return PrUsdGeomPurpose_Proxy;
    if (purpose == "guide")
        return PrUsdGeomPurpose_Guide;
    return PrUsdGeomPurpose_Default;
}

bool prUsdGeomIsInvisible(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch) {
    PrStringView visibility{};
    return readText(stage, prim, "visibility", time, scratch, &visibility) && visibility == "invisible";
}

// ---------------------------------------------------------------------------------------------------------------------
// Transforms

bool prUsdGeomLocalTransform(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
        PrDMatrix4* out, bool* resetsStack) {
    *out = prDMatrix4Identity();
    RawArray<OrderItem> items{};
    bool ok = readOrder(stage, prim, scratch, &items, resetsStack);
    PrDMatrix4 local = prDMatrix4Identity();
    for (uint32_t i = 0; ok && i < items.count; ++i) {
        const OrderItem& item = items.data[i];
        PrUsdValue value{};
        PrDMatrix4 op{};
        ok = prUsdStageAttributeValue(stage, prim, item.property, time, scratch, &value) && opMatrix(item.kind, value, &op);
        if (ok && item.inverse) {
            PrDMatrix4 inverse{};
            // A singular operation has no inverse, and OpenUSD then applies the identity.
            op = prDMatrix4Invert(&op, &inverse) ? inverse : prDMatrix4Identity();
        }
        if (ok)
            local = times(local, op);
    }
    items.reset();
    if (ok)
        *out = local;
    return ok;
}

bool prUsdGeomTransformVaries(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch) {
    RawArray<OrderItem> items{};
    bool resets = false;
    bool varies = false;
    if (readOrder(stage, prim, scratch, &items, &resets)) {
        for (uint32_t i = 0; !varies && i < items.count; ++i)
            varies = prUsdStageAttributeVaries(stage, prim, items.data[i].property);
    }
    items.reset();
    return varies;
}

uint32_t prUsdGeomTransformTimes(const PrUsdStage* stage, PrUsdPrimIndex prim, double start, double end,
        PrUsdScratch* scratch, double* PR_NULLABLE times, uint32_t capacity) {
    RawArray<OrderItem> items{};
    RawArray<double> all{};
    bool resets = false;
    if (readOrder(stage, prim, scratch, &items, &resets)) {
        for (uint32_t i = 0; i < items.count; ++i) {
            const double* sampled = nullptr;
            uint32_t count = prUsdStageAttributeTimes(stage, prim, items.data[i].property, start, end, scratch, &sampled);
            // Merge the sorted times into the sorted union.
            RawArray<double> merged{};
            uint32_t a = 0;
            uint32_t b = 0;
            while (a < all.count || b < count) {
                double next = b >= count || (a < all.count && all.data[a] <= sampled[b]) ? all.data[a] : sampled[b];
                if (a < all.count && all.data[a] == next)
                    ++a;
                if (b < count && sampled[b] == next)
                    ++b;
                merged.push(next);
            }
            all.reset();
            all = merged;
        }
    }
    uint32_t total = all.count;
    for (uint32_t i = 0; times != nullptr && i < total && i < capacity; ++i)
        times[i] = all.data[i];
    all.reset();
    items.reset();
    return total;
}

// ---------------------------------------------------------------------------------------------------------------------
// Primitive variables

bool prUsdGeomPrimvarRead(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time,
        PrUsdScratch* scratch, PrUsdGeomPrimvar* out) {
    PrTextBuffer full = {};
    prTextBufferAppend(&full, "primvars:");
    prTextBufferAppend(&full, name);
    uint32_t property = prUsdStageFindProperty(stage, prim, prTextBufferView(&full));
    *out = PrUsdGeomPrimvar{.elementSize = 1};
    bool read = property != kPrUsdNone && prUsdStageProperties(stage)[property].form == PrUsdSpecForm_Attribute &&
        prUsdStageAttributeValue(stage, prim, property, time, scratch, &out->value);
    if (read) {
        PrUsdValue field{};
        if (prUsdStagePropertyField(stage, prim, property, "interpolation", scratch, &field) &&
                field.type == PrUsdType_Token && !field.isArray) {
            PrStringView interpolation = *static_cast<const PrStringView*>(field.data);
            if (interpolation == "uniform")
                out->interpolation = PrUsdGeomInterpolation_Uniform;
            else if (interpolation == "varying")
                out->interpolation = PrUsdGeomInterpolation_Varying;
            else if (interpolation == "vertex")
                out->interpolation = PrUsdGeomInterpolation_Vertex;
            else if (interpolation == "faceVarying")
                out->interpolation = PrUsdGeomInterpolation_FaceVarying;
        }
        double size = 1;
        if (prUsdStagePropertyField(stage, prim, property, "elementSize", scratch, &field) && !field.isArray &&
                numberAt(field, 0, &size) && size >= 1)
            out->elementSize = uint32_t(size);
        prTextBufferAppend(&full, ":indices");
        const int32_t* indices = nullptr;
        uint32_t indexCount = 0;
        PrUsdValue indexValue{};
        if (attributeValue(stage, prim, prTextBufferView(&full), time, scratch, &indexValue)) {
            read = indexValue.type == PrUsdType_Int && indexValue.isArray;
            indices = static_cast<const int32_t*>(indexValue.data);
            indexCount = indexValue.count;
            uint32_t elements = out->value.count / out->elementSize;
            for (uint32_t i = 0; read && i < indexCount; ++i)
                read = indices[i] >= 0 && uint32_t(indices[i]) < elements;
            out->indices = indexCount != 0 ? indices : nullptr;
            out->indexCount = indexCount;
        }
    }
    prTextBufferReset(&full);
    return read;
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

PrStatus prUsdGeomMeshRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
        PrUsdGeomMesh* out) {
    *out = PrUsdGeomMesh{.normals = {.interpolation = PrUsdGeomInterpolation_Vertex, .elementSize = 1}};
    PrUsdValue points{};
    if (!attributeValue(stage, prim, "points", time, scratch, &points))
        return PrStatus_NotFound;
    const float* floats = floatsOf(points, 3, scratch);
    if (floats == nullptr || !points.isArray)
        return PrStatus_InvalidData;
    out->points = reinterpret_cast<const PrVec3*>(floats);
    out->pointCount = points.count;
    if (!readInts(stage, prim, "faceVertexCounts", time, scratch, &out->faceVertexCounts, &out->faceCount) ||
            !readInts(stage, prim, "faceVertexIndices", time, scratch, &out->faceVertexIndices, &out->cornerCount)) {
        out->faceCount = 0;
        out->cornerCount = 0;
    }
    uint64_t corners = 0;
    for (uint32_t i = 0; i < out->faceCount; ++i) {
        if (out->faceVertexCounts[i] < 3)
            return PrStatus_InvalidData;
        corners += uint64_t(out->faceVertexCounts[i]);
    }
    if (corners != out->cornerCount)
        return PrStatus_InvalidData;
    for (uint32_t i = 0; i < out->cornerCount; ++i) {
        if (out->faceVertexIndices[i] < 0 || uint32_t(out->faceVertexIndices[i]) >= out->pointCount)
            return PrStatus_InvalidData;
    }
    if (readInts(stage, prim, "holeIndices", time, scratch, &out->holeIndices, &out->holeCount)) {
        for (uint32_t i = 0; i < out->holeCount; ++i) {
            if (out->holeIndices[i] < 0 || uint32_t(out->holeIndices[i]) >= out->faceCount)
                return PrStatus_InvalidData;
        }
    }
    PrStringView text{};
    out->leftHanded = readText(stage, prim, "orientation", kPrF64Nan, scratch, &text) && text == "leftHanded";
    readBool(stage, prim, "doubleSided", kPrF64Nan, scratch, &out->doubleSided);
    if (readText(stage, prim, "subdivisionScheme", kPrF64Nan, scratch, &text)) {
        if (text == "none")
            out->subdivision = PrUsdGeomSubdivision_None;
        else if (text == "loop")
            out->subdivision = PrUsdGeomSubdivision_Loop;
        else if (text == "bilinear")
            out->subdivision = PrUsdGeomSubdivision_Bilinear;
    }
    // A primitive variable of normals is stronger than the attribute (UsdGeomPointBased::GetNormalsInterpolation).
    PrUsdGeomPrimvar normals{};
    if (prUsdGeomPrimvarRead(stage, prim, "normals", time, scratch, &normals)) {
        out->normals = normals;
    } else {
        uint32_t property = prUsdStageFindProperty(stage, prim, "normals");
        PrUsdValue value{};
        if (property != kPrUsdNone && prUsdStageAttributeValue(stage, prim, property, time, scratch, &value)) {
            out->normals.value = value;
            PrUsdValue field{};
            if (prUsdStagePropertyField(stage, prim, property, "interpolation", scratch, &field) &&
                    field.type == PrUsdType_Token && !field.isArray) {
                PrStringView interpolation = *static_cast<const PrStringView*>(field.data);
                if (interpolation == "uniform")
                    out->normals.interpolation = PrUsdGeomInterpolation_Uniform;
                else if (interpolation == "varying")
                    out->normals.interpolation = PrUsdGeomInterpolation_Varying;
                else if (interpolation == "faceVarying")
                    out->normals.interpolation = PrUsdGeomInterpolation_FaceVarying;
                else if (interpolation == "constant")
                    out->normals.interpolation = PrUsdGeomInterpolation_Constant;
            }
        }
    }
    if (out->normals.value.count != 0) {
        const float* normalFloats = floatsOf(out->normals.value, 3, scratch);
        if (normalFloats == nullptr || !prUsdGeomPrimvarFits(out, &out->normals))
            return PrStatus_InvalidData;
        out->normals.value.type = PrUsdType_Float3;
        out->normals.value.data = normalFloats;
    }
    return PrStatus_Ok;
}

uint32_t prUsdGeomMeshElementCount(const PrUsdGeomMesh* mesh, PrUsdGeomInterpolation interpolation) {
    switch (interpolation) {
        case PrUsdGeomInterpolation_Constant:
            return 1;
        case PrUsdGeomInterpolation_Uniform:
            return mesh->faceCount;
        case PrUsdGeomInterpolation_FaceVarying:
            return mesh->cornerCount;
        default:
            return mesh->pointCount;
    }
}

bool prUsdGeomPrimvarFits(const PrUsdGeomMesh* mesh, const PrUsdGeomPrimvar* primvar) {
    uint32_t wanted = prUsdGeomMeshElementCount(mesh, primvar->interpolation);
    uint32_t has = primvar->indices != nullptr ? primvar->indexCount : primvar->value.count / primvar->elementSize;
    return has == wanted;
}

uint32_t prUsdGeomSubsets(const PrUsdStage* stage, PrUsdPrimIndex mesh, uint32_t faceCount, PrStringView family,
        double time, PrUsdScratch* scratch, PrUsdGeomSubset* PR_NULLABLE out, uint32_t capacity) {
    const PrUsdPrim* prims = prUsdStagePrims(stage);
    uint32_t count = 0;
    for (PrUsdPrimIndex child = prims[mesh].firstChild; child != kPrUsdNone; child = prims[child].nextSibling) {
        if (prUsdGeomPrimKind(stage, child) != PrUsdGeomKind_Subset)
            continue;
        PrStringView text{};
        if (!readText(stage, child, "familyName", kPrF64Nan, scratch, &text) || text != family)
            continue;
        if (readText(stage, child, "elementType", kPrF64Nan, scratch, &text) && text != "face")
            continue;
        PrUsdGeomSubset subset{.prim = child};
        if (!readInts(stage, child, "indices", time, scratch, &subset.indices, &subset.indexCount))
            subset.indexCount = 0;
        bool fits = true;
        for (uint32_t i = 0; fits && i < subset.indexCount; ++i)
            fits = subset.indices[i] >= 0 && uint32_t(subset.indices[i]) < faceCount;
        if (!fits)
            continue;
        if (out != nullptr && count < capacity)
            out[count] = subset;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

bool prUsdGeomShapeRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
        PrUsdGeomShape* out) {
    PrStringView type = prUsdStagePrims(stage)[prim].typeName;
    // The fallbacks of each schema.
    *out = PrUsdGeomShape{.axis = 2, .radius = 1, .radiusBottom = 1, .radiusTop = 1, .height = 2, .size = 2};
    if (type == "Sphere") {
        out->kind = PrUsdGeomShapeKind_Sphere;
    } else if (type == "Cube") {
        out->kind = PrUsdGeomShapeKind_Cube;
    } else if (type == "Cylinder" || type == "Cylinder_1") {
        out->kind = PrUsdGeomShapeKind_Cylinder;
    } else if (type == "Cone") {
        out->kind = PrUsdGeomShapeKind_Cone;
    } else if (type == "Capsule") {
        out->kind = PrUsdGeomShapeKind_Capsule;
        out->radius = 0.5;
        out->height = 1;
    } else if (type == "Plane") {
        out->kind = PrUsdGeomShapeKind_Plane;
        out->width = 2;
        out->length = 2;
        out->doubleSided = true;
    } else {
        return false;
    }
    readNumber(stage, prim, "radius", time, scratch, &out->radius);
    readNumber(stage, prim, "height", time, scratch, &out->height);
    readNumber(stage, prim, "size", time, scratch, &out->size);
    readNumber(stage, prim, "width", time, scratch, &out->width);
    readNumber(stage, prim, "length", time, scratch, &out->length);
    out->radiusBottom = out->radius;
    out->radiusTop = out->radius;
    if (type == "Cylinder_1") {
        readNumber(stage, prim, "radiusBottom", time, scratch, &out->radiusBottom);
        readNumber(stage, prim, "radiusTop", time, scratch, &out->radiusTop);
    }
    PrStringView text{};
    if (readText(stage, prim, "axis", kPrF64Nan, scratch, &text) && text == "X")
        out->axis = 0;
    else if (text == "Y")
        out->axis = 1;
    out->leftHanded = readText(stage, prim, "orientation", kPrF64Nan, scratch, &text) && text == "leftHanded";
    readBool(stage, prim, "doubleSided", kPrF64Nan, scratch, &out->doubleSided);
    const double dimensions[] = {
        out->radius, out->radiusBottom, out->radiusTop, out->height, out->size, out->width, out->length,
    };
    for (double dimension : dimensions) {
        if (!(dimension >= 0) || !prF64IsFinite(dimension))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cameras

bool prUsdGeomCameraRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
        PrUsdGeomCamera* out) {
    *out = PrUsdGeomCamera{
        .focalLength = 50,
        .horizontalAperture = 20.955f,
        .verticalAperture = 15.2908f,
        .nearClip = 1,
        .farClip = 1000000,
    };
    PrStringView projection{};
    out->orthographic = readText(stage, prim, "projection", time, scratch, &projection) && projection == "orthographic";
    readFloat(stage, prim, "focalLength", time, scratch, &out->focalLength);
    readFloat(stage, prim, "horizontalAperture", time, scratch, &out->horizontalAperture);
    readFloat(stage, prim, "verticalAperture", time, scratch, &out->verticalAperture);
    readFloat(stage, prim, "horizontalApertureOffset", time, scratch, &out->horizontalApertureOffset);
    readFloat(stage, prim, "verticalApertureOffset", time, scratch, &out->verticalApertureOffset);
    float range[2];
    if (readVector(stage, prim, "clippingRange", time, scratch, 2, range)) {
        out->nearClip = range[0];
        out->farClip = range[1];
    }
    return out->focalLength > 0 && out->horizontalAperture > 0 && out->verticalAperture > 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Point instancers

PrStatus prUsdGeomPointInstancerRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
        PrUsdGeomPointInstancer* out) {
    *out = PrUsdGeomPointInstancer{};
    if (!readInts(stage, prim, "protoIndices", time, scratch, &out->protoIndices, &out->instanceCount))
        return PrStatus_NotFound;
    const PrStringView* paths = nullptr;
    uint32_t count = 0;
    if (targetsOf(stage, prim, "prototypes", scratch, &paths, &count)) {
        PrUsdPrimIndex* prototypes =
            static_cast<PrUsdPrimIndex*>(prUsdScratchAlloc(scratch, count * sizeof(PrUsdPrimIndex),
                alignof(PrUsdPrimIndex)));
        for (uint32_t i = 0; i < count; ++i)
            prototypes[i] = primAt(stage, paths[i]);
        out->prototypes = prototypes;
        out->prototypeCount = count;
    }
    for (uint32_t i = 0; i < out->instanceCount; ++i) {
        if (out->protoIndices[i] < 0 || uint32_t(out->protoIndices[i]) >= out->prototypeCount)
            return PrStatus_InvalidData;
    }
    PrUsdValue value{};
    if (attributeValue(stage, prim, "positions", time, scratch, &value)) {
        const float* floats = floatsOf(value, 3, scratch);
        if (floats == nullptr || value.count != out->instanceCount)
            return PrStatus_InvalidData;
        out->positions = reinterpret_cast<const PrVec3*>(floats);
    }
    if (attributeValue(stage, prim, "scales", time, scratch, &value)) {
        const float* floats = floatsOf(value, 3, scratch);
        if (floats == nullptr || value.count != out->instanceCount)
            return PrStatus_InvalidData;
        out->scales = reinterpret_cast<const PrVec3*>(floats);
    }
    // `orientations` holds halves, and `orientationsf` floats, which a reader takes first.
    if (attributeValue(stage, prim, "orientationsf", time, scratch, &value) ||
            attributeValue(stage, prim, "orientations", time, scratch, &value)) {
        const float* floats = floatsOf(value, 4, scratch);
        if (floats == nullptr || value.count != out->instanceCount)
            return PrStatus_InvalidData;
        out->orientations = reinterpret_cast<const PrQuat*>(floats);
    }
    if (attributeValue(stage, prim, "ids", time, scratch, &value)) {
        if (value.type != PrUsdType_Int64 || value.count != out->instanceCount)
            return PrStatus_InvalidData;
        out->ids = static_cast<const int64_t*>(value.data);
    }
    if (attributeValue(stage, prim, "invisibleIds", time, scratch, &value) && value.type == PrUsdType_Int64) {
        out->invisibleIds = static_cast<const int64_t*>(value.data);
        out->invisibleIdCount = value.count;
    }
    return PrStatus_Ok;
}

PrDMatrix4 prUsdGeomInstanceTransform(const PrUsdGeomPointInstancer* instancer, uint32_t instance) {
    PrDMatrix4 m = prDMatrix4Identity();
    if (instancer->scales != nullptr) {
        PrVec3 s = instancer->scales[instance];
        double factors[3] = {double(s.x), double(s.y), double(s.z)};
        m = scaling(factors);
    }
    if (instancer->orientations != nullptr) {
        PrQuat q = instancer->orientations[instance];
        m = times(quaternionMatrix(double(q.x), double(q.y), double(q.z), double(q.w)), m);
    }
    if (instancer->positions != nullptr) {
        PrVec3 p = instancer->positions[instance];
        double offset[3] = {double(p.x), double(p.y), double(p.z)};
        m = times(translation(offset), m);
    }
    return m;
}

bool prUsdGeomInstanceIsVisible(const PrUsdGeomPointInstancer* instancer, uint32_t instance) {
    int64_t id = instancer->ids != nullptr ? instancer->ids[instance] : int64_t(instance);
    for (uint32_t i = 0; i < instancer->invisibleIdCount; ++i) {
        if (instancer->invisibleIds[i] == id)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Material bindings

PrUsdShadeBinding prUsdShadeDirectBinding(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch) {
    PrUsdShadeBinding binding{.material = kPrUsdNone};
    PrStringView names[] = {"material:binding:preview", "material:binding"};
    for (PrStringView name : names) {
        uint32_t property = prUsdStageFindProperty(stage, prim, name);
        if (property == kPrUsdNone || prUsdStageProperties(stage)[property].form != PrUsdSpecForm_Relationship)
            continue;
        PrUsdValue targets{};
        if (!prUsdStageTargets(stage, prim, property, false, scratch, &targets, nullptr))
            continue;
        binding.authored = true;
        if (targets.count != 0) {
            PrUsdPrimIndex material = primAt(stage, static_cast<const PrStringView*>(targets.data)[0]);
            if (material != kPrUsdNone && prUsdGeomPrimKind(stage, material) == PrUsdGeomKind_Material)
                binding.material = material;
        }
        PrUsdValue strength{};
        binding.strongerThanDescendants =
            prUsdStagePropertyField(stage, prim, property, "bindMaterialAs", scratch, &strength) &&
            strength.type == PrUsdType_Token && !strength.isArray &&
            *static_cast<const PrStringView*>(strength.data) == "strongerThanDescendants";
        return binding;
    }
    return binding;
}

PrUsdPrimIndex prUsdShadeBoundMaterial(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch) {
    const PrUsdPrim* prims = prUsdStagePrims(stage);
    PrUsdShadeBinding bound{.material = kPrUsdNone};
    for (PrUsdPrimIndex at = prim; at != kPrUsdNone && at != kPrUsdPseudoRoot; at = prims[at].parent) {
        PrUsdShadeBinding binding = prUsdShadeDirectBinding(stage, at, scratch);
        if (binding.authored && (!bound.authored || binding.strongerThanDescendants))
            bound = binding;
    }
    return bound.material;
}

// ---------------------------------------------------------------------------------------------------------------------
// The preview surface

bool prUsdPreviewSurfaceRead(const PrUsdStage* stage, PrUsdPrimIndex material, double time, PrUsdScratch* scratch,
        PrUsdPreviewSurface* out) {
    *out = PrUsdPreviewSurface{.shader = kPrUsdNone};
    for (uint32_t i = 0; i < PrUsdPreviewInput_Count; ++i) {
        for (uint32_t k = 0; k < 3; ++k)
            out->inputs[i].value[k] = kPreviewInputs[i].fallback[k];
    }
    Source surface{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (!follow(stage, material, "outputs:surface", scratch, &surface, &valuePrim, &valueName) ||
            shaderId(stage, surface.shader, scratch) != "UsdPreviewSurface")
        return false;
    PrUsdPrimIndex shader = surface.shader;
    out->shader = shader;
    float workflow = 0;
    if (inputNumbers(stage, shader, "inputs:useSpecularWorkflow", time, scratch, 1, &workflow))
        out->useSpecularWorkflow = workflow != 0;
    for (uint32_t i = 0; i < PrUsdPreviewInput_Count; ++i) {
        const PreviewInputInfo& info = kPreviewInputs[i];
        PrUsdPreviewInputValue& input = out->inputs[i];
        Source source{};
        if (follow(stage, shader, info.name, scratch, &source, &valuePrim, &valueName)) {
            input.textured = readTexture(stage, source.shader, source.output, time, scratch, &input.texture);
            if (!input.textured && prStringStartsWith(shaderId(stage, source.shader, scratch), "UsdPrimvarReader_")) {
                inputText(stage, source.shader, "inputs:varname", time, scratch, &input.primvar);
                inputNumbers(stage, source.shader, "inputs:fallback", time, scratch, info.width, input.value);
            }
            continue;
        }
        if (info.width == 1)
            readFloat(stage, valuePrim, valueName, time, scratch, input.value);
        else
            readVector(stage, valuePrim, valueName, time, scratch, info.width, input.value);
    }
    return true;
}

PrRawString prUsdTextureLocation(const PrUsdStage* stage, const PrUsdTexture* texture, double time) {
    if (texture->fileProperty == kPrUsdNone || texture->file.size == 0)
        return PrRawString{};
    return prUsdStageAttributeAssetResolve(stage, texture->shader, texture->fileProperty, time, texture->file);
}

// ---------------------------------------------------------------------------------------------------------------------
// MaterialX networks

PrStringView prUsdMtlxNodeCategory(PrStringView id) {
    if (!prStringStartsWith(id, "ND_"))
        return PrStringView{};
    PrStringView rest{id.data + 3, id.size - 3};
    for (size_t i = 0; i < rest.size; ++i) {
        if (rest.data[i] != '_')
            continue;
        PrStringView tail{rest.data + i + 1, rest.size - i - 1};
        for (PrStringView type : kMtlxTypes) {
            if (prStringStartsWith(tail, type))
                return PrStringView{rest.data, i};
        }
    }
    return rest;
}

PrUsdPrimIndex prUsdMtlxSurface(const PrUsdStage* stage, PrUsdPrimIndex material, PrUsdScratch* scratch, PrStringView* id) {
    *id = PrStringView{};
    Source surface{};
    PrUsdPrimIndex valuePrim = kPrUsdNone;
    PrStringView valueName{};
    if (!follow(stage, material, "outputs:mtlx:surface", scratch, &surface, &valuePrim, &valueName))
        return kPrUsdNone;
    *id = shaderId(stage, surface.shader, scratch);
    return surface.shader;
}

bool prUsdMtlxInputRead(const PrUsdStage* stage, PrUsdPrimIndex shader, PrStringView name, uint32_t width,
        const float* fallback, double time, PrUsdScratch* scratch, PrUsdMtlxInput* out) {
    *out = PrUsdMtlxInput{};
    for (uint32_t i = 0; i < width && i < 4; ++i)
        out->value[i] = fallback[i];
    char full[64] = "inputs:";
    if (width == 0 || width > 4 || name.size > sizeof(full) - 7)
        return false;
    for (size_t i = 0; i < name.size; ++i)
        full[7 + i] = name.data[i];
    PrStringView input{full, 7 + name.size};
    if (prUsdStageFindProperty(stage, shader, input) == kPrUsdNone)
        return false;
    Fold f{.stage = stage, .time = time, .scratch = scratch};
    Folded folded = resized(foldInput(f, shader, input, width, fallback), width);
    out->textured = folded.textured;
    out->normalMap = folded.normal;
    out->texture = folded.texture;
    out->primvar = folded.textured ? PrStringView{} : folded.primvar;
    out->tint = folded.tint;
    out->approximated = folded.approximated;
    for (uint32_t i = 0; i < 4; ++i) {
        out->primvarFallback[i] = folded.primvarFallback[i];
        out->tintFallback[i] = folded.tintFallback[i];
    }
    for (uint32_t i = 0; i < width; ++i) {
        out->offset[i] = folded.b[i];
        for (uint32_t j = 0; j < 4; ++j)
            out->weights[i][j] = folded.m[i][j];
        // Without a source that varies, the value is the input's. With one, it is the value where the surface lacks the
        // primitive variable.
        if (!folded.textured) {
            out->value[i] = folded.b[i];
            for (uint32_t j = 0; out->primvar.size != 0 && j < 4; ++j)
                out->value[i] += folded.m[i][j] * folded.primvarFallback[j];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lights

bool prUsdLuxLightRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch, PrUsdLuxLight* out) {
    if (prUsdGeomPrimKind(stage, prim) != PrUsdGeomKind_Light)
        return false;
    PrStringView type = prUsdStagePrims(stage)[prim].typeName;
    *out = PrUsdLuxLight{
        .kind = PrUsdLightKind_Other,
        .color = {1, 1, 1},
        .intensity = 1,
        .colorTemperature = 6500,
        .radius = 0.5f,
        .width = 1,
        .height = 1,
        .length = 1,
        .angle = 0.53f,
        .coneAngle = 90,
        .textureFileProperty = kPrUsdNone,
        .poleAxis = PrUsdLuxPoleAxis_Y,
    };
    if (type == "DistantLight")
        out->kind = PrUsdLightKind_Distant;
    else if (type == "SphereLight")
        out->kind = PrUsdLightKind_Sphere;
    else if (type == "DiskLight")
        out->kind = PrUsdLightKind_Disk;
    else if (type == "RectLight")
        out->kind = PrUsdLightKind_Rect;
    else if (type == "CylinderLight")
        out->kind = PrUsdLightKind_Cylinder;
    else if (type == "DomeLight" || type == "DomeLight_1")
        out->kind = PrUsdLightKind_Dome;
    // The schema gives a distant light's intensity a fallback of its own (UsdLuxDistantLight).
    if (out->kind == PrUsdLightKind_Distant)
        out->intensity = 50000;
    float color[3];
    if (lightFloats(stage, prim, "color", time, scratch, 3, color))
        out->color = PrVec3{color[0], color[1], color[2]};
    lightFloats(stage, prim, "intensity", time, scratch, 1, &out->intensity);
    lightFloats(stage, prim, "exposure", time, scratch, 1, &out->exposure);
    lightBool(stage, prim, "normalize", time, scratch, &out->normalize);
    lightBool(stage, prim, "enableColorTemperature", time, scratch, &out->enableColorTemperature);
    lightFloats(stage, prim, "colorTemperature", time, scratch, 1, &out->colorTemperature);
    lightFloats(stage, prim, "radius", time, scratch, 1, &out->radius);
    lightFloats(stage, prim, "width", time, scratch, 1, &out->width);
    lightFloats(stage, prim, "height", time, scratch, 1, &out->height);
    lightFloats(stage, prim, "length", time, scratch, 1, &out->length);
    lightFloats(stage, prim, "angle", time, scratch, 1, &out->angle);
    lightBool(stage, prim, "treatAsPoint", time, scratch, &out->treatAsPoint);
    lightFloats(stage, prim, "shaping:cone:angle", time, scratch, 1, &out->coneAngle);
    lightFloats(stage, prim, "shaping:cone:softness", time, scratch, 1, &out->coneSoftness);
    PrUsdValue file{};
    if (attributeValue(stage, prim, "inputs:texture:file", time, scratch, &file) && file.type == PrUsdType_Asset &&
            !file.isArray) {
        out->textureFile = *static_cast<const PrStringView*>(file.data);
        out->textureFileProperty = prUsdStageFindProperty(stage, prim, "inputs:texture:file");
    }
    PrStringView text{};
    if (readText(stage, prim, "inputs:texture:format", time, scratch, &text)) {
        if (text == "latlong")
            out->textureFormat = PrUsdLuxTextureFormat_Latlong;
        else if (text == "mirroredBall")
            out->textureFormat = PrUsdLuxTextureFormat_MirroredBall;
        else if (text == "angular")
            out->textureFormat = PrUsdLuxTextureFormat_Angular;
        else if (text == "cubeMapVerticalCross")
            out->textureFormat = PrUsdLuxTextureFormat_CubeMapVerticalCross;
    }
    // A DomeLight_1's pole follows the stage unless it says otherwise, and a DomeLight's is +y.
    if (type == "DomeLight_1") {
        out->poleAxis = PrUsdLuxPoleAxis_Scene;
        if (readText(stage, prim, "poleAxis", kPrF64Nan, scratch, &text) && text == "Y")
            out->poleAxis = PrUsdLuxPoleAxis_Y;
        else if (text == "Z")
            out->poleAxis = PrUsdLuxPoleAxis_Z;
    }
    return true;
}

PrRawString prUsdLuxTextureLocation(const PrUsdStage* stage, PrUsdPrimIndex prim, const PrUsdLuxLight* light, double time) {
    if (light->textureFileProperty == kPrUsdNone || light->textureFile.size == 0)
        return PrRawString{};
    return prUsdStageAttributeAssetResolve(stage, prim, light->textureFileProperty, time, light->textureFile);
}
