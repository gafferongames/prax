// The text format of layers, .usda (usd-core/#text).
//
// The grammar gives a field's value no type of its own: a number is a float or an integer, and a string is a string, a
// token or a variant's name, according to the field that it is assigned to. An attribute's value takes its type from
// the attribute's declared type, a dictionary's entry from the type written before its key, and every other field from
// the module's table of fields (usdFieldTypeFromName). A field that the table does not hold is kept as its text.

#include "usd/src/usd_text.h"

#include "pr/float.h"
#include "pr/float16.h"
#include "pr/float_convert.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// The deepest nesting of prims, of variants, of dictionaries and of tuples that the reader follows.
constexpr uint32_t kMaxDepth = 256;

// The state of a read: a cursor over the text, and the first failure.
struct Text {
    UsdLayer* layer;
    const char* begin;
    const char* at;
    const char* end;
    PrStatus status;
    PrTextBuffer* PR_NULLABLE diagnostics;
    uint32_t depth;
    RawArray<PrUsdSpecIndex> declared;  // the variant sets that metadata declared and no statement has defined yet
};

// A number as the text writes it. An integer that fits 64 bits keeps its exact value.
struct Number {
    bool isInteger;
    bool isNegative;
    uint64_t magnitude;         // of an integer
    double value;
};

// Which of a list op's lists a statement assigns, with the list op's own Count for a statement without a keyword.
typedef uint8_t ListKeyword;
constexpr ListKeyword kNoListKeyword = PrUsdListOpList_Count;

// Records the first failure, with the line that the cursor is on, and returns false.
bool fail(Text& t, PrStatus status, PrStringView message) {
    if (t.status != PrStatus_Ok)
        return false;
    t.status = status;
    if (t.diagnostics != nullptr) {
        uint32_t line = 1;
        for (const char* p = t.begin; p < t.at; ++p) {
            if (*p == '\n' || (*p == '\r' && !(p + 1 < t.end && p[1] == '\n')))
                ++line;
        }
        prTextBufferAppend(t.diagnostics, "line ");
        prTextBufferAppendU64(t.diagnostics, line, 10);
        prTextBufferAppend(t.diagnostics, ": ");
        prTextBufferAppend(t.diagnostics, message);
    }
    return false;
}

bool syntax(Text& t, PrStringView message) {
    return fail(t, PrStatus_Syntax, message);
}

bool invalid(Text& t, PrStringView message) {
    return fail(t, PrStatus_InvalidData, message);
}

bool isNewline(char c) {
    return c == '\n' || c == '\r';
}

bool isNameStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || uint8_t(c) >= 0x80;
}

bool isNameContinue(char c) {
    return isNameStart(c) || (c >= '0' && c <= '9');
}

// Skips a comment at the cursor. Returns false if there is none. `lines` allows the comments that run to the end of a
// line, which padding inside a line does not.
bool skipComment(Text& t, bool lines) {
    if (t.at >= t.end)
        return false;
    if (*t.at == '/' && t.at + 1 < t.end && t.at[1] == '*') {
        const char* p = t.at + 2;
        while (p + 1 < t.end && !(p[0] == '*' && p[1] == '/'))
            ++p;
        if (p + 1 >= t.end) {
            syntax(t, "a comment does not end");
            t.at = t.end;
            return true;
        }
        t.at = p + 2;
        return true;
    }
    if (lines && (*t.at == '#' || (*t.at == '/' && t.at + 1 < t.end && t.at[1] == '/'))) {
        while (t.at < t.end && !isNewline(*t.at))
            ++t.at;
        return true;
    }
    return false;
}

// Skips spaces and block comments.
void skipInline(Text& t) {
    while (t.at < t.end && (*t.at == ' ' || *t.at == '\t' || skipComment(t, false))) {
        if (t.at < t.end && (*t.at == ' ' || *t.at == '\t'))
            ++t.at;
    }
}

// Skips spaces and comments, to the end of the line at most.
void skipLine(Text& t) {
    while (t.at < t.end) {
        if (*t.at == ' ' || *t.at == '\t')
            ++t.at;
        else if (!skipComment(t, true))
            break;
    }
}

// Skips spaces, comments and line ends.
void skipLines(Text& t) {
    while (t.at < t.end) {
        if (*t.at == ' ' || *t.at == '\t' || isNewline(*t.at))
            ++t.at;
        else if (!skipComment(t, true))
            break;
    }
}

// Skips what separates two statements: the end of a line, or a semicolon. Returns false if neither follows.
bool skipSeparator(Text& t) {
    skipLine(t);
    if (t.at < t.end && (*t.at == ';' || isNewline(*t.at))) {
        ++t.at;
        skipLines(t);
        return true;
    }
    return t.at >= t.end;
}

// Returns true, and skips it, if the character `c` is at the cursor.
bool take(Text& t, char c) {
    if (t.at < t.end && *t.at == c) {
        ++t.at;
        return true;
    }
    return false;
}

// Returns the identifier at the cursor without moving, or the empty view.
PrStringView peekIdentifier(const Text& t) {
    if (t.at >= t.end || !isNameStart(*t.at))
        return PrStringView{t.at, 0};
    const char* p = t.at + 1;
    while (p < t.end && isNameContinue(*p))
        ++p;
    return PrStringView{t.at, size_t(p - t.at)};
}

// Returns true, and skips it, if the whole identifier at the cursor is `word`.
bool takeWord(Text& t, PrStringView word) {
    if (peekIdentifier(t) != word)
        return false;
    t.at += word.size;
    return true;
}

// Reads a property's name: identifiers joined by colons.
bool readNamespacedName(Text& t, PrStringView* out) {
    const char* start = t.at;
    for (;;) {
        PrStringView part = peekIdentifier(t);
        if (part.size == 0)
            return syntax(t, "a name is expected");
        t.at += part.size;
        if (t.at + 1 < t.end && *t.at == ':' && isNameStart(t.at[1]))
            ++t.at;
        else
            break;
    }
    *out = PrStringView{start, size_t(t.at - start)};
    return true;
}

// Skips ` = `, with the padding that the grammar allows around it.
bool expectAssignment(Text& t) {
    skipInline(t);
    if (!take(t, '='))
        return syntax(t, "'=' is expected");
    skipInline(t);
    return true;
}

// Reads a string in either quote, on one line or between tripled quotes, and decodes its escapes into `out`.
bool readString(Text& t, RawArray<char>& out) {
    out.count = 0;
    if (t.at >= t.end || (*t.at != '"' && *t.at != '\''))
        return syntax(t, "a string is expected");
    char quote = *t.at;
    bool tripled = t.at + 2 < t.end && t.at[1] == quote && t.at[2] == quote;
    t.at += tripled ? 3 : 1;
    for (;;) {
        if (t.at >= t.end)
            return syntax(t, "a string does not end");
        char c = *t.at;
        if (c == quote) {
            if (!tripled) {
                ++t.at;
                return true;
            }
            if (t.at + 2 < t.end && t.at[1] == quote && t.at[2] == quote) {
                t.at += 3;
                return true;
            }
        } else if (isNewline(c) && !tripled) {
            return syntax(t, "a string does not end on its line");
        }
        ++t.at;
        if (c != '\\') {
            out.push(c);
            continue;
        }
        if (t.at >= t.end)
            return syntax(t, "a string does not end");
        char e = *t.at++;
        switch (e) {
            case 'a':
                out.push('\a');
                break;
            case 'b':
                out.push('\b');
                break;
            case 'f':
                out.push('\f');
                break;
            case 'n':
                out.push('\n');
                break;
            case 'r':
                out.push('\r');
                break;
            case 't':
                out.push('\t');
                break;
            case 'v':
                out.push('\v');
                break;
            case 'x': {
                uint32_t value = 0;
                uint32_t digits = 0;
                while (digits < 2 && t.at < t.end && prAsciiIsHexDigit(*t.at)) {
                    value = value * 16 + prAsciiHexValue(*t.at++);
                    ++digits;
                }
                if (digits == 0)
                    out.push('x');
                else
                    out.push(char(value));
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    uint32_t value = uint32_t(e - '0');
                    uint32_t digits = 1;
                    while (digits < 3 && t.at < t.end && *t.at >= '0' && *t.at <= '7') {
                        value = value * 8 + uint32_t(*t.at++ - '0');
                        ++digits;
                    }
                    out.push(char(value));
                } else {
                    // Any other character stands for itself, as a quote and a backslash do.
                    out.push(e);
                }
                break;
        }
    }
}

// Reads a string and returns it as a string of the layer.
bool readLayerString(Text& t, PrStringView* out) {
    RawArray<char> text{};
    bool ok = readString(t, text);
    if (ok)
        *out = usdLayerIntern(t.layer, PrStringView{text.data, text.count});
    text.reset();
    return ok;
}

// Reads an asset reference, between single or tripled at signs, and returns the asset path as a string of the layer.
bool readAsset(Text& t, PrStringView* out) {
    if (t.at >= t.end || *t.at != '@')
        return syntax(t, "an asset path is expected");
    bool tripled = t.at + 2 < t.end && t.at[1] == '@' && t.at[2] == '@';
    t.at += tripled ? 3 : 1;
    RawArray<char> text{};
    bool ok = false;
    while (t.at < t.end && !isNewline(*t.at)) {
        if (!tripled) {
            if (*t.at == '@') {
                ++t.at;
                ok = true;
                break;
            }
        } else if (*t.at == '@' && t.at + 2 < t.end && t.at[1] == '@' && t.at[2] == '@') {
            // More than three at signs end with the last three.
            if (t.at + 3 < t.end && t.at[3] == '@') {
                text.push(*t.at++);
                continue;
            }
            t.at += 3;
            ok = true;
            break;
        } else if (*t.at == '\\' && t.at + 3 < t.end && t.at[1] == '@' && t.at[2] == '@' && t.at[3] == '@') {
            // An escaped triple is the three at signs themselves.
            text.append("@@@", 3);
            t.at += 4;
            continue;
        }
        text.push(*t.at++);
    }
    if (ok)
        *out = usdLayerIntern(t.layer, PrStringView{text.data, text.count});
    text.reset();
    return ok || syntax(t, "an asset path does not end on its line");
}

// Reads a number: digits with an optional fraction and exponent, or inf, -inf or nan.
bool readNumber(Text& t, Number* out) {
    const char* start = t.at;
    *out = Number{};
    const char* p = t.at;
    if (p < t.end && *p == '-') {
        out->isNegative = true;
        ++p;
    }
    PrStringView rest{p, size_t(t.end - p)};
    if (prStringStartsWith(rest, "inf") && !(p + 3 < t.end && isNameContinue(p[3]))) {
        out->value = out->isNegative ? -kPrF64Infinity : kPrF64Infinity;
        t.at = p + 3;
        return true;
    }
    if (!out->isNegative && prStringStartsWith(rest, "nan") && !(p + 3 < t.end && isNameContinue(p[3]))) {
        out->value = kPrF64Nan;
        t.at = p + 3;
        return true;
    }
    const char* digits = p;
    bool overflow = false;
    while (p < t.end && *p >= '0' && *p <= '9') {
        uint64_t digit = uint64_t(*p - '0');
        if (out->magnitude > (0xffffffffffffffffull - digit) / 10)
            overflow = true;
        else
            out->magnitude = out->magnitude * 10 + digit;
        ++p;
    }
    bool whole = p > digits;
    bool fraction = false;
    if (p < t.end && *p == '.') {
        const char* q = p + 1;
        while (q < t.end && *q >= '0' && *q <= '9')
            ++q;
        if (whole || q > p + 1) {
            fraction = true;
            whole = whole || q > p + 1;
            p = q;
        }
    }
    if (!whole)
        return syntax(t, "a number is expected");
    bool exponent = false;
    if (p < t.end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q < t.end && (*q == '+' || *q == '-'))
            ++q;
        if (q < t.end && *q >= '0' && *q <= '9') {
            while (q < t.end && *q >= '0' && *q <= '9')
                ++q;
            exponent = true;
            p = q;
        }
    }
    out->isInteger = !fraction && !exponent && !overflow;
    // The decimal parser wants a digit before a fraction's point, which the grammar does not.
    char buffer[400];
    size_t size = 0;
    for (const char* c = start; c < p && size + 2 < sizeof(buffer); ++c) {
        if (*c == '.' && (c == start || c[-1] == '-'))
            buffer[size++] = '0';
        buffer[size++] = *c;
        if (*c == '.' && (c + 1 >= p || c[1] < '0' || c[1] > '9'))
            buffer[size++] = '0';
    }
    if (size_t(p - start) + 2 >= sizeof(buffer))
        return syntax(t, "a number is too long");
    double value = 0;
    if (prF64ParsePrefix(PrStringView{buffer, size}, &value) != size)
        return syntax(t, "a number is expected");
    out->value = value;
    if (out->isInteger && out->magnitude == 0)
        out->value = out->isNegative ? -0.0 : 0.0;
    t.at = p;
    return true;
}

// Stores a number as an element of an integer type. A number with a fraction loses it, and a number outside the type is
// refused.
bool storeInteger(Text& t, const Number& number, PrUsdType type, void* out) {
    bool isSigned = type == PrUsdType_Int || type == PrUsdType_Int64;
    uint64_t magnitude = number.magnitude;
    bool negative = number.isNegative;
    if (!number.isInteger) {
        double truncated = number.value < 0 ? -number.value : number.value;
        if (!(truncated < 18446744073709551616.0))
            return invalid(t, "a number is outside the range of its integer type");
        magnitude = uint64_t(truncated);
        negative = number.value < 0 && magnitude != 0;
    }
    if (magnitude == 0)
        negative = false;
    uint64_t most = 0;
    switch (type) {
        case PrUsdType_UChar:
            most = 0xff;
            break;
        case PrUsdType_Int:
            most = negative ? 0x80000000ull : 0x7fffffffull;
            break;
        case PrUsdType_UInt:
            most = 0xffffffffull;
            break;
        case PrUsdType_Int64:
            most = negative ? 0x8000000000000000ull : 0x7fffffffffffffffull;
            break;
        default:
            most = 0xffffffffffffffffull;
            break;
    }
    if (magnitude > most || (negative && !isSigned))
        return invalid(t, "a number is outside the range of its integer type");
    uint64_t bits = negative ? 0 - magnitude : magnitude;
    switch (type) {
        case PrUsdType_UChar:
            *static_cast<uint8_t*>(out) = uint8_t(bits);
            break;
        case PrUsdType_Int:
        case PrUsdType_UInt:
            *static_cast<uint32_t*>(out) = uint32_t(bits);
            break;
        default:
            *static_cast<uint64_t*>(out) = bits;
            break;
    }
    return true;
}

// The value of a number as a float. An integer converts directly, so that it rounds once.
float floatOf(const Number& number) {
    if (number.isInteger && number.magnitude != 0)
        return number.isNegative ? -float(number.magnitude) : float(number.magnitude);
    return float(number.value);
}

double doubleOf(const Number& number) {
    if (number.isInteger && number.magnitude != 0)
        return number.isNegative ? -double(number.magnitude) : double(number.magnitude);
    return number.value;
}

// The scalar that the components of a dimensioned type are made of, and how many there are in each of its rows.
struct Shape {
    PrUsdType scalar;
    uint8_t columns;
    uint8_t rows;               // zero for a type that is not a matrix
};

Shape shapeOf(PrUsdType type) {
    switch (type) {
        case PrUsdType_Int2:
            return {PrUsdType_Int, 2, 0};
        case PrUsdType_Int3:
            return {PrUsdType_Int, 3, 0};
        case PrUsdType_Int4:
            return {PrUsdType_Int, 4, 0};
        case PrUsdType_Half2:
            return {PrUsdType_Half, 2, 0};
        case PrUsdType_Half3:
            return {PrUsdType_Half, 3, 0};
        case PrUsdType_Half4:
        case PrUsdType_Quath:
            return {PrUsdType_Half, 4, 0};
        case PrUsdType_Float2:
            return {PrUsdType_Float, 2, 0};
        case PrUsdType_Float3:
            return {PrUsdType_Float, 3, 0};
        case PrUsdType_Float4:
        case PrUsdType_Quatf:
            return {PrUsdType_Float, 4, 0};
        case PrUsdType_Double2:
            return {PrUsdType_Double, 2, 0};
        case PrUsdType_Double3:
            return {PrUsdType_Double, 3, 0};
        case PrUsdType_Double4:
        case PrUsdType_Quatd:
            return {PrUsdType_Double, 4, 0};
        case PrUsdType_Matrix2d:
            return {PrUsdType_Double, 2, 2};
        case PrUsdType_Matrix3d:
            return {PrUsdType_Double, 3, 3};
        case PrUsdType_Matrix4d:
            return {PrUsdType_Double, 4, 4};
        default:
            return {type, 1, 0};
    }
}

bool storeScalar(Text& t, const Number& number, PrUsdType type, void* out) {
    switch (type) {
        case PrUsdType_Half:
            *static_cast<PrFloat16*>(out) = prFloat16FromFloat(floatOf(number));
            return true;
        case PrUsdType_Float:
            *static_cast<float*>(out) = floatOf(number);
            return true;
        case PrUsdType_Double:
        case PrUsdType_TimeCode:
            *static_cast<double*>(out) = doubleOf(number);
            return true;
        default:
            return storeInteger(t, number, type, out);
    }
}

// Reads a tuple of `count` numbers of the scalar type into `out`.
bool readTuple(Text& t, PrUsdType scalar, uint32_t count, void* out) {
    if (!take(t, '('))
        return syntax(t, "a tuple is expected");
    size_t size = usdTypeSize(scalar);
    for (uint32_t i = 0; i < count; ++i) {
        skipLines(t);
        Number number;
        if (!readNumber(t, &number) || !storeScalar(t, number, scalar, static_cast<uint8_t*>(out) + i * size))
            return false;
        skipLines(t);
        if (i + 1 < count && !take(t, ','))
            return invalid(t, "a tuple has fewer numbers than its type");
    }
    take(t, ',');
    skipLines(t);
    if (!take(t, ')'))
        return invalid(t, "a tuple has more numbers than its type");
    return true;
}

bool readPathReference(Text& t, PrStringView* raw);
bool resolvePath(Text& t, PrStringView anchor, PrStringView raw, PrStringView* out);

bool readTypedElement(Text& t, PrUsdType type, PrStringView anchor, void* out);

// Reads one element of `type` into `out`, which has room for it. Text that does not begin a value of the type at all,
// as a number where the type is a vector, is a value of another type, which the specification has a reader refuse
// (usd-core/#attribute-types-and-value-validation): it breaks a rule, where text that begins the value and goes wrong
// breaks the grammar.
bool readElement(Text& t, PrUsdType type, PrStringView anchor, void* out) {
    const char* start = t.at;
    bool ok = readTypedElement(t, type, anchor, out);
    if (!ok && t.status == PrStatus_Syntax && t.at == start)
        t.status = PrStatus_InvalidData;
    return ok;
}

bool readTypedElement(Text& t, PrUsdType type, PrStringView anchor, void* out) {
    switch (type) {
        case PrUsdType_Bool: {
            bool value = false;
            PrStringView word = peekIdentifier(t);
            RawArray<char> text{};
            if (t.at < t.end && (*t.at == '"' || *t.at == '\'')) {
                if (!readString(t, text))
                    return false;
                word = PrStringView{text.data, text.count};
            } else if (word.size != 0) {
                t.at += word.size;
            } else {
                Number number;
                if (!readNumber(t, &number))
                    return false;
                *static_cast<bool*>(out) = number.value != 0;
                return true;
            }
            bool known = true;
            if (prAsciiEqualsFold(word, "true") || prAsciiEqualsFold(word, "yes") || prAsciiEqualsFold(word, "on") ||
                    word == "1")
                value = true;
            else if (prAsciiEqualsFold(word, "false") || prAsciiEqualsFold(word, "no") ||
                    prAsciiEqualsFold(word, "off") || word == "0")
                value = false;
            else
                known = false;
            text.reset();
            if (!known)
                return invalid(t, "a boolean is expected");
            *static_cast<bool*>(out) = value;
            return true;
        }
        case PrUsdType_String:
        case PrUsdType_Token:
        case PrUsdType_PathExpression:
            return readLayerString(t, static_cast<PrStringView*>(out));
        case PrUsdType_Asset:
            return readAsset(t, static_cast<PrStringView*>(out));
        case PrUsdType_Path: {
            PrStringView raw;
            return readPathReference(t, &raw) && resolvePath(t, anchor, raw, static_cast<PrStringView*>(out));
        }
        default:
            break;
    }
    Shape shape = shapeOf(type);
    if (shape.rows != 0) {
        // A matrix is a tuple of its rows, and the element is its transpose.
        if (!take(t, '('))
            return syntax(t, "a matrix is expected");
        double rows[4][4];
        for (uint32_t r = 0; r < shape.rows; ++r) {
            skipLines(t);
            if (!readTuple(t, PrUsdType_Double, shape.columns, rows[r]))
                return false;
            skipLines(t);
            if (r + 1 < shape.rows && !take(t, ','))
                return invalid(t, "a matrix has fewer rows than its type");
        }
        take(t, ',');
        skipLines(t);
        if (!take(t, ')'))
            return invalid(t, "a matrix has more rows than its type");
        double* m = static_cast<double*>(out);
        for (uint32_t r = 0; r < shape.rows; ++r) {
            for (uint32_t c = 0; c < shape.columns; ++c)
                m[r * shape.columns + c] = rows[c][r];
        }
        return true;
    }
    if (shape.columns != 1) {
        if (type != PrUsdType_Quath && type != PrUsdType_Quatf && type != PrUsdType_Quatd)
            return readTuple(t, shape.scalar, shape.columns, out);
        // The text writes a quaternion's real part first, and the element holds it last.
        uint8_t parts[4 * sizeof(double)];
        if (!readTuple(t, shape.scalar, 4, parts))
            return false;
        size_t size = usdTypeSize(shape.scalar);
        prMemCopy(out, parts + size, 3 * size);
        prMemCopy(static_cast<uint8_t*>(out) + 3 * size, parts, size);
        return true;
    }
    Number number;
    return readNumber(t, &number) && storeScalar(t, number, type, out);
}

// Returns true, and skips it, if `None` is at the cursor.
bool takeNone(Text& t) {
    return takeWord(t, "None");
}

// Reads a value of `type`: one element, or an array of them between brackets.
bool readValue(Text& t, PrUsdType type, bool isArray, PrStringView anchor, PrUsdValue* out) {
    size_t size = usdTypeSize(type);
    if (!isArray) {
        if (t.at < t.end && *t.at == '[')
            return invalid(t, "an array is given for a type that is not an array");
        alignas(16) uint8_t element[sizeof(PrDMatrix4)];
        if (!readElement(t, type, anchor, element))
            return false;
        *out = usdLayerValue(t.layer, type, false, 1, element);
        return true;
    }
    if (!take(t, '['))
        return invalid(t, "an array's type is given a value that is not an array");
    RawArray<uint8_t> elements{};
    uint32_t count = 0;
    bool ok = true;
    for (;;) {
        skipLines(t);
        if (take(t, ']'))
            break;
        if (count != 0) {
            if (!take(t, ',')) {
                ok = syntax(t, "',' or ']' is expected in an array");
                break;
            }
            skipLines(t);
            if (take(t, ']'))
                break;
        }
        if (count == 0xffffffffu / 128) {
            ok = fail(t, PrStatus_Limit, "an array has too many elements");
            break;
        }
        elements.resize(uint32_t((count + 1) * size));
        // The elements are whole multiples of their alignment apart in storage that is aligned for any of them.
        if (!readElement(t, type, anchor, elements.data + count * size)) {
            ok = false;
            break;
        }
        ++count;
    }
    if (ok)
        *out = usdLayerValue(t.layer, type, true, count, elements.data);
    elements.reset();
    return ok;
}

bool readDictionary(Text& t, PrUsdValue* out);

// Skips a value of any shape that the grammar allows a field: an atom, a tuple, a list or a dictionary.
bool skipAnyValue(Text& t, uint32_t depth) {
    if (depth > kMaxDepth)
        return fail(t, PrStatus_Limit, "a value nests too deeply");
    if (t.at >= t.end)
        return syntax(t, "a value is expected");
    char c = *t.at;
    if (c == '"' || c == '\'') {
        RawArray<char> text{};
        bool ok = readString(t, text);
        text.reset();
        return ok;
    }
    if (c == '@') {
        PrStringView asset;
        return readAsset(t, &asset);
    }
    if (c == '<') {
        PrStringView raw;
        return readPathReference(t, &raw);
    }
    if (c == '(' || c == '[' || c == '{') {
        char close = c == '(' ? ')' : c == '[' ? ']' : '}';
        ++t.at;
        for (;;) {
            skipLines(t);
            while (take(t, ',') || take(t, ';'))
                skipLines(t);
            if (take(t, close))
                return true;
            if (t.at >= t.end)
                return syntax(t, "a value does not end");
            if (c == '{') {
                // An entry: a type, a key, and a value.
                PrStringView word = peekIdentifier(t);
                if (word.size == 0)
                    return syntax(t, "a dictionary's entry is expected");
                t.at += word.size;
                skipInline(t);
                if (take(t, '[') && !take(t, ']'))
                    return syntax(t, "']' is expected");
                skipInline(t);
                if (t.at < t.end && (*t.at == '"' || *t.at == '\'')) {
                    if (!skipAnyValue(t, depth + 1))
                        return false;
                } else {
                    PrStringView key = peekIdentifier(t);
                    if (key.size == 0)
                        return syntax(t, "a dictionary's key is expected");
                    t.at += key.size;
                }
                if (!expectAssignment(t))
                    return false;
            }
            if (!skipAnyValue(t, depth + 1))
                return false;
        }
    }
    const char* start = t.at;
    PrStringView word = peekIdentifier(t);
    if (word.size != 0) {
        t.at += word.size;
        // A type's name in the style of C++ has double colons.
        while (t.at + 2 < t.end && t.at[0] == ':' && t.at[1] == ':' && isNameStart(t.at[2])) {
            t.at += 2;
            t.at += peekIdentifier(t).size;
        }
        return true;
    }
    Number number;
    if (!readNumber(t, &number))
        return false;
    return t.at > start;
}

// Reads a value of any shape, as the value of a field whose type the module does not know: a dictionary is a
// dictionary, and anything else is its text, with the quotes of a string.
bool readUnregistered(Text& t, PrUsdValue* out) {
    if (t.at < t.end && *t.at == '{')
        return readDictionary(t, out);
    const char* start = t.at;
    if (!skipAnyValue(t, 0))
        return false;
    PrStringView text = usdLayerIntern(t.layer, PrStringView{start, size_t(t.at - start)});
    *out = usdLayerValue(t.layer, PrUsdType_Unregistered, false, 1, &text);
    return true;
}

// Reads the entries of a dictionary, after its opening brace, through its closing brace.
bool readDictionaryEntries(Text& t, RawArray<PrUsdDictionaryEntry>& entries) {
    for (;;) {
        skipLines(t);
        if (take(t, '}'))
            return true;
        PrUsdDictionaryEntry entry{};
        PrUsdType type = PrUsdType_Dictionary;
        bool isArray = false;
        PrStringView typeName = peekIdentifier(t);
        if (typeName.size == 0)
            return syntax(t, "a dictionary's entry begins with its type");
        t.at += typeName.size;
        bool isDictionary = typeName == "dictionary";
        if (!isDictionary) {
            skipInline(t);
            const char* bracket = t.at;
            if (take(t, '[')) {
                skipInline(t);
                if (!take(t, ']'))
                    return syntax(t, "']' is expected after '['");
                isArray = true;
            } else {
                t.at = bracket;
            }
            bool arrayOfName = false;
            if (!usdTypeFromName(typeName, &type, &arrayOfName))
                return invalid(t, "a dictionary's entry has a type that is not a type of the specification");
        }
        skipInline(t);
        if (t.at < t.end && (*t.at == '"' || *t.at == '\'')) {
            if (!readLayerString(t, &entry.key))
                return false;
        } else {
            PrStringView key = peekIdentifier(t);
            if (key.size == 0)
                return syntax(t, "a dictionary's key is expected");
            t.at += key.size;
            entry.key = usdLayerIntern(t.layer, key);
        }
        if (!expectAssignment(t))
            return false;
        if (isDictionary) {
            if (!readDictionary(t, &entry.value))
                return false;
        } else if (!readValue(t, type, isArray, "/", &entry.value)) {
            return false;
        }
        entries.push(entry);
        skipLine(t);
        if (t.at < t.end && *t.at == '}')
            continue;
        if (!skipSeparator(t))
            return syntax(t, "a new line or ';' is expected after a dictionary's entry");
    }
}

bool readDictionary(Text& t, PrUsdValue* out) {
    if (!take(t, '{'))
        return syntax(t, "a dictionary is expected");
    if (++t.depth > kMaxDepth)
        return fail(t, PrStatus_Limit, "a dictionary nests too deeply");
    RawArray<PrUsdDictionaryEntry> entries{};
    bool ok = readDictionaryEntries(t, entries);
    if (ok)
        *out = usdLayerDictionary(t.layer, entries);
    entries.reset();
    --t.depth;
    return ok;
}

// Reads a path between angle brackets, and returns the text between them, which may be empty.
bool readPathReference(Text& t, PrStringView* raw) {
    if (!take(t, '<'))
        return syntax(t, "a path is expected");
    const char* start = t.at;
    while (t.at < t.end && *t.at != '>' && !isNewline(*t.at))
        ++t.at;
    if (t.at >= t.end || *t.at != '>')
        return syntax(t, "a path does not end on its line");
    *raw = PrStringView{start, size_t(t.at - start)};
    ++t.at;
    return true;
}

// Removes the last element of a prim's path: a child's name, or a variant selection.
bool pathParent(RawArray<char>& path) {
    if (path.count <= 1)
        return false;
    if (path.data[path.count - 1] == '}') {
        while (path.count > 0 && path.data[path.count - 1] != '{')
            --path.count;
        if (path.count == 0)
            return false;
        --path.count;
        return true;
    }
    while (path.count > 0 && path.data[path.count - 1] != '/' && path.data[path.count - 1] != '}')
        --path.count;
    if (path.count > 1 && path.data[path.count - 1] == '/')
        --path.count;
    return path.count != 0;
}

// Makes the path `raw` absolute against the prim at `anchor`, checks that it is a path of the grammar, and returns it
// as a string of the layer.
bool resolvePath(Text& t, PrStringView anchor, PrStringView raw, PrStringView* out) {
    if (!usdPathIsValid(raw, UsdPathKind_Any) || raw == ".")
        return syntax(t, "a path is not a path of the grammar");
    if (raw.data[0] == '/') {
        *out = usdLayerIntern(t.layer, raw);
        return true;
    }
    RawArray<char> path{};
    path.append(anchor.data, uint32_t(anchor.size));
    size_t at = 0;
    bool ok = true;
    while (ok && at + 1 < raw.size && raw.data[at] == '.' && raw.data[at + 1] == '.') {
        ok = pathParent(path);
        at += 2;
        if (at < raw.size && raw.data[at] == '/')
            ++at;
    }
    if (ok && at < raw.size) {
        char last = path.data[path.count - 1];
        if (raw.data[at] != '.' && last != '/' && last != '}')
            path.push('/');
        path.append(raw.data + at, uint32_t(raw.size - at));
    }
    if (ok)
        *out = usdLayerIntern(t.layer, PrStringView{path.data, path.count});
    path.reset();
    return ok || invalid(t, "a relative path reaches above the root");
}

// Returns `path` without its variant selections, as a string of the layer.
PrStringView stripVariantSelections(Text& t, PrStringView path) {
    if (prStringFindByte(path, '{') == path.size)
        return path;
    RawArray<char> stripped{};
    for (size_t i = 0; i < path.size; ++i) {
        if (path.data[i] != '{') {
            stripped.push(path.data[i]);
            continue;
        }
        while (i < path.size && path.data[i] != '}')
            ++i;
        // A child of a variant follows its selection directly, and is a child of the prim.
        if (i + 1 < path.size && isNameStart(path.data[i + 1]))
            stripped.push('/');
    }
    PrStringView result = usdLayerIntern(t.layer, PrStringView{stripped.data, stripped.count});
    stripped.reset();
    return result;
}

bool hasVariantSelection(PrStringView path) {
    return prStringFindByte(path, '{') != path.size;
}

// How the paths of a list are held against the prim that authors them.
enum {
    PathUse_Target,             // a relationship's target or an inherited or specialized prim: no variant selection
    PathUse_Connection,         // an attribute's connection: variant selections are dropped
};
typedef uint8_t PathUse;

// Reads one path of a list and appends it to `paths`.
bool readListPath(Text& t, PrStringView anchor, PathUse use, RawArray<PrStringView>& paths) {
    PrStringView raw;
    PrStringView path;
    if (!readPathReference(t, &raw) || !resolvePath(t, anchor, raw, &path))
        return false;
    if (use == PathUse_Connection)
        path = stripVariantSelections(t, path);
    else if (hasVariantSelection(path))
        return invalid(t, "a path of a target, an inherit or a specialize has a variant selection");
    paths.push(path);
    return true;
}

// Reads the paths of a relationship, a connection, an inherit or a specialize: None, one path, or a list of paths. None
// and an empty list say that the explicit list is empty, and mean nothing for a list that edits.
bool readPathList(Text& t, PrStringView anchor, PathUse use, bool allowNone, PrUsdValue* out) {
    RawArray<PrStringView> paths{};
    bool ok = true;
    if (takeNone(t)) {
        if (!allowNone)
            ok = invalid(t, "None is given to a list op that edits");
    } else if (take(t, '[')) {
        for (;;) {
            skipLines(t);
            if (take(t, ']'))
                break;
            if (paths.count != 0) {
                if (!take(t, ',')) {
                    ok = syntax(t, "',' or ']' is expected in a list of paths");
                    break;
                }
                skipLines(t);
                if (take(t, ']'))
                    break;
            }
            if (!readListPath(t, anchor, use, paths)) {
                ok = false;
                break;
            }
        }
    } else {
        ok = readListPath(t, anchor, use, paths);
    }
    if (ok && paths.count == 0 && !allowNone)
        ok = invalid(t, "an empty list is given to a list op that edits");
    if (ok)
        *out = usdLayerValue(t.layer, PrUsdType_Path, true, paths.count, paths.data);
    paths.reset();
    return ok;
}

// Reads the offset and scale of a layer offset, and for a reference its custom data, between parentheses.
bool readArcParameters(Text& t, bool allowCustomData, PrUsdLayerOffset* offset,
        const PrUsdDictionary* PR_NULLABLE* PR_NULLABLE customData) {
    for (;;) {
        skipLines(t);
        if (take(t, ')'))
            return true;
        if (takeWord(t, "offset") || takeWord(t, "scale")) {
            bool isOffset = t.at[-1] == 't';
            Number number;
            if (!expectAssignment(t) || !readNumber(t, &number))
                return false;
            (isOffset ? offset->offset : offset->scale) = doubleOf(number);
        } else if (allowCustomData && takeWord(t, "customData")) {
            PrUsdValue value;
            if (!expectAssignment(t) || !readDictionary(t, &value))
                return false;
            *customData = static_cast<const PrUsdDictionary*>(value.data);
        } else {
            return syntax(t, "'offset' or 'scale' is expected");
        }
        skipLine(t);
        if (t.at < t.end && *t.at == ')')
            continue;
        if (!skipSeparator(t))
            return syntax(t, "a new line or ';' is expected");
    }
}

// Reads one reference or payload and appends it to `items`.
bool readArc(Text& t, bool isReference, RawArray<PrUsdReference>& items) {
    PrUsdReference item{};
    item.layerOffset = PrUsdLayerOffset{0, 1};
    item.assetPath = usdLayerIntern(t.layer, "");
    item.primPath = item.assetPath;
    bool hasAsset = t.at < t.end && *t.at == '@';
    if (hasAsset) {
        if (!readAsset(t, &item.assetPath))
            return false;
        if (item.assetPath.size == 0)
            return invalid(t, "a reference or a payload has an empty asset path");
        skipInline(t);
    }
    if (t.at < t.end && *t.at == '<') {
        PrStringView raw;
        if (!readPathReference(t, &raw))
            return false;
        if (raw.size != 0) {
            if (!usdPathIsValid(raw, UsdPathKind_Prim))
                return invalid(t, "a reference or a payload names a path that is not an absolute path to a prim");
            item.primPath = usdLayerIntern(t.layer, raw);
        }
    } else if (!hasAsset) {
        return syntax(t, "an asset path or a path is expected");
    }
    const char* before = t.at;
    skipInline(t);
    if (take(t, '(')) {
        if (!readArcParameters(t, isReference, &item.layerOffset, &item.customData))
            return false;
    } else {
        t.at = before;
    }
    items.push(item);
    return true;
}

// Reads the references or payloads of a prim: None, one, or a list.
bool readArcList(Text& t, bool isReference, bool allowNone, PrUsdValue* out) {
    RawArray<PrUsdReference> items{};
    bool ok = true;
    if (takeNone(t)) {
        if (!allowNone)
            ok = invalid(t, "None is given to a list op that edits");
    } else if (take(t, '[')) {
        for (;;) {
            skipLines(t);
            if (take(t, ']'))
                break;
            if (items.count != 0) {
                if (!take(t, ',')) {
                    ok = syntax(t, "',' or ']' is expected in a list");
                    break;
                }
                skipLines(t);
                if (take(t, ']'))
                    break;
            }
            if (!readArc(t, isReference, items)) {
                ok = false;
                break;
            }
        }
        if (ok && items.count == 0 && !allowNone)
            ok = invalid(t, "an empty list is given to a list op that edits");
    } else {
        ok = readArc(t, isReference, items);
    }
    if (ok)
        *out = usdLayerValue(t.layer, isReference ? PrUsdType_Reference : PrUsdType_Payload, true, items.count,
            items.data);
    items.reset();
    return ok;
}

// Reads the items of a list op whose items are scalars: None, or a list. A list of variant set names may also be one
// string.
bool readScalarList(Text& t, PrUsdType itemType, bool allowSingle, PrUsdValue* out) {
    if (takeNone(t)) {
        *out = PrUsdValue{itemType, true, 0, nullptr};
        return true;
    }
    if (itemType == PrUsdType_Unregistered) {
        // The items of a list whose type the module does not know are kept as their text.
        RawArray<PrStringView> items{};
        bool ok = take(t, '[') || syntax(t, "None or '[' is expected");
        while (ok) {
            skipLines(t);
            if (take(t, ']'))
                break;
            if (items.count != 0) {
                if (!take(t, ',')) {
                    ok = syntax(t, "',' or ']' is expected in a list");
                    break;
                }
                skipLines(t);
                if (take(t, ']'))
                    break;
            }
            const char* start = t.at;
            ok = skipAnyValue(t, 0);
            if (ok)
                items.push(usdLayerIntern(t.layer, PrStringView{start, size_t(t.at - start)}));
        }
        if (ok)
            *out = usdLayerValue(t.layer, itemType, true, items.count, items.data);
        items.reset();
        return ok;
    }
    if (allowSingle && t.at < t.end && *t.at != '[') {
        PrStringView name;
        if (!readLayerString(t, &name))
            return false;
        *out = usdLayerValue(t.layer, itemType, true, 1, &name);
        return true;
    }
    if (t.at >= t.end || *t.at != '[')
        return syntax(t, "None or '[' is expected");
    return readValue(t, itemType, true, "/", out);
}

// Assigns one list of the list op that is the field `name` of `spec`, making the list op if the spec has none. The
// lists that edit and the explicit list do not coexist: assigning one kind empties the other, so the statement that
// comes last decides which kind the list op is.
bool assignListOp(Text& t, PrUsdSpecIndex spec, PrStringView name, PrUsdType itemType, ListKeyword keyword, PrUsdValue items) {
    PrUsdListOpList list = keyword == kNoListKeyword ? PrUsdListOpList(PrUsdListOpList_Explicit) : keyword;
    uint32_t count = usdListRemoveRepeats(itemType, const_cast<void*>(items.data), items.count);
    if (count != items.count)
        return invalid(t, "a list of a list op names an item twice");
    PrUsdListOp* listOp = nullptr;
    PrUsdField* field = usdLayerBuildField(t.layer, spec, name);
    if (field != nullptr && field->value.type == PrUsdType_ListOp) {
        listOp = const_cast<PrUsdListOp*>(static_cast<const PrUsdListOp*>(field->value.data));
        if (listOp->itemType != itemType)
            listOp = nullptr;
    }
    if (listOp == nullptr) {
        listOp = t.layer->arena.allocZeroed<PrUsdListOp>();
        listOp->itemType = itemType;
        for (PrUsdValue& each : listOp->items)
            each = PrUsdValue{itemType, true, 0, nullptr};
        usdLayerSetField(t.layer, spec, name, PrUsdValue{PrUsdType_ListOp, false, 1, listOp});
    }
    bool isExplicit = list == PrUsdListOpList_Explicit;
    if (isExplicit != listOp->isExplicit) {
        for (PrUsdValue& each : listOp->items)
            each = PrUsdValue{itemType, true, 0, nullptr};
        listOp->isExplicit = isExplicit;
    }
    listOp->items[list] = items;
    return true;
}

// Reads the keyword of a list op, if one is at the cursor.
ListKeyword takeListKeyword(Text& t) {
    static const PrStringView kWords[] = {"add", "prepend", "append", "delete", "reorder"};
    static const PrUsdListOpList kLists[] = {
        PrUsdListOpList_Added, PrUsdListOpList_Prepended,
        PrUsdListOpList_Appended, PrUsdListOpList_Deleted, PrUsdListOpList_Ordered,
    };
    for (uint32_t i = 0; i < 5; ++i) {
        if (takeWord(t, kWords[i]))
            return kLists[i];
    }
    return kNoListKeyword;
}

void setString(Text& t, PrUsdSpecIndex spec, PrStringView name, PrUsdType type, PrStringView value) {
    usdLayerSetField(t.layer, spec, name, usdLayerValue(t.layer, type, false, 1, &value));
}

void setByte(Text& t, PrUsdSpecIndex spec, PrStringView name, PrUsdType type, uint8_t value) {
    usdLayerSetField(t.layer, spec, name, usdLayerValue(t.layer, type, false, 1, &value));
}

// The path of the prim that anchors the relative paths of a spec's fields: the spec's own for a prim or a variant, its
// parent's for a property, and the root for the layer.
PrStringView anchorOf(const Text& t, PrUsdSpecIndex spec) {
    const PrUsdSpec& s = t.layer->specs.data[spec];
    if (s.form == PrUsdSpecForm_Attribute || s.form == PrUsdSpecForm_Relationship)
        return t.layer->specs.data[s.parent].path;
    return s.path;
}

// Records that the text of `spec` runs from `begin` to the cursor.
void setSpan(Text& t, PrUsdSpecIndex spec, const char* begin) {
    RawArray<PrUsdTextSpan>& spans = t.layer->textSpans;
    while (spans.count < t.layer->specs.count)
        spans.push(PrUsdTextSpan{});
    spans.data[spec] = PrUsdTextSpan{uint32_t(begin - t.begin), uint32_t(t.at - t.begin)};
}

// Reads one item of a block of metadata, of the spec `spec`.
bool readMetadataItem(Text& t, PrUsdSpecIndex spec) {
    PrUsdSpecForm form = t.layer->specs.data[spec].form;
    bool isPrim = form == PrUsdSpecForm_Prim || form == PrUsdSpecForm_Variant;
    // A string alone is the spec's comment.
    if (t.at < t.end && (*t.at == '"' || *t.at == '\'')) {
        PrStringView comment;
        if (!readLayerString(t, &comment))
            return false;
        setString(t, spec, "comment", PrUsdType_String, comment);
        return true;
    }
    ListKeyword keyword = takeListKeyword(t);
    if (keyword != kNoListKeyword)
        skipInline(t);
    PrStringView name = peekIdentifier(t);
    if (name.size == 0)
        return syntax(t, "a field's name is expected");
    t.at += name.size;
    if (!expectAssignment(t))
        return false;
    PrStringView anchor = anchorOf(t, spec);
    PrUsdValue value{};

    if (name == "doc" && keyword == kNoListKeyword) {
        PrStringView text;
        if (!readLayerString(t, &text))
            return false;
        setString(t, spec, "documentation", PrUsdType_String, text);
        return true;
    }
    if (name == "kind" && keyword == kNoListKeyword && isPrim) {
        PrStringView text;
        if (!readLayerString(t, &text))
            return false;
        setString(t, spec, "kind", PrUsdType_Token, text);
        return true;
    }
    if (name == "permission" && keyword == kNoListKeyword) {
        if (takeWord(t, "public"))
            setByte(t, spec, name, PrUsdType_Permission, PrUsdPermission_Public);
        else if (takeWord(t, "private"))
            setByte(t, spec, name, PrUsdType_Permission, PrUsdPermission_Private);
        else
            return syntax(t, "'public' or 'private' is expected");
        return true;
    }
    if ((name == "symmetryFunction" || name == "displayUnit") && keyword == kNoListKeyword) {
        PrStringView word = peekIdentifier(t);
        t.at += word.size;
        setString(t, spec, name, PrUsdType_Token, usdLayerIntern(t.layer, word));
        return true;
    }
    if (isPrim && (name == "references" || name == "payload")) {
        bool isReference = name == "references";
        if (!readArcList(t, isReference, keyword == kNoListKeyword, &value))
            return false;
        return assignListOp(t, spec, name, isReference ? PrUsdType_Reference : PrUsdType_Payload, keyword, value);
    }
    if (isPrim && (name == "inherits" || name == "specializes")) {
        if (!readPathList(t, anchor, PathUse_Target, keyword == kNoListKeyword, &value))
            return false;
        return assignListOp(t, spec, name == "inherits" ? PrStringView("inheritPaths") : name, PrUsdType_Path, keyword, value);
    }
    if (isPrim && name == "variantSets") {
        if (!readScalarList(t, PrUsdType_String, true, &value))
            return false;
        const PrStringView* names = static_cast<const PrStringView*>(value.data);
        for (uint32_t i = 0; i < value.count; ++i) {
            if (!usdNameIsIdentifier(names[i]))
                return invalid(t, "a variant set's name is not an identifier");
        }
        if (!assignListOp(t, spec, "variantSetNames", PrUsdType_String, keyword, value))
            return false;
        // An explicit list, and a list of the deprecated `add`, declares its variant sets: each has a spec from here
        // on, in the list's order, whether or not the prim goes on to give it variants. The other lists declare none.
        bool declares = keyword == kNoListKeyword || keyword == PrUsdListOpList_Added;
        for (uint32_t i = 0; declares && i < value.count; ++i) {
            bool existed = false;
            PrUsdSpecIndex set = usdLayerAddSpec(t.layer, PrUsdSpecForm_VariantSet, spec, names[i], &existed);
            if (!existed)
                t.declared.push(set);
        }
        return true;
    }
    if (isPrim && name == "variants" && keyword == kNoListKeyword) {
        if (!readDictionary(t, &value))
            return false;
        const PrUsdDictionary* dictionary = static_cast<const PrUsdDictionary*>(value.data);
        RawArray<PrUsdVariantSelection> selections{};
        bool ok = true;
        for (uint32_t i = 0; i < dictionary->count && ok; ++i) {
            const PrUsdDictionaryEntry& entry = dictionary->entries[i];
            if (entry.value.type != PrUsdType_String || entry.value.isArray)
                ok = invalid(t, "a variant selection is not a string");
            else
                selections.push({entry.key, *static_cast<const PrStringView*>(entry.value.data)});
        }
        if (ok)
            usdLayerSetField(t.layer, spec, "variantSelection", usdLayerVariantSelections(t.layer, selections));
        selections.reset();
        return ok;
    }
    if (form == PrUsdSpecForm_Layer && name == "subLayers" && keyword == kNoListKeyword) {
        if (!take(t, '['))
            return syntax(t, "'[' is expected");
        RawArray<PrStringView> paths{};
        RawArray<PrUsdLayerOffset> offsets{};
        bool ok = true;
        for (;;) {
            skipLines(t);
            if (take(t, ']'))
                break;
            if (paths.count != 0) {
                if (!take(t, ',')) {
                    ok = syntax(t, "',' or ']' is expected in the sublayers");
                    break;
                }
                skipLines(t);
                if (take(t, ']'))
                    break;
            }
            PrStringView path;
            PrUsdLayerOffset offset{0, 1};
            if (!readAsset(t, &path)) {
                ok = false;
                break;
            }
            const char* before = t.at;
            skipInline(t);
            if (take(t, '(')) {
                if (!readArcParameters(t, false, &offset, nullptr)) {
                    ok = false;
                    break;
                }
            } else {
                t.at = before;
            }
            paths.push(path);
            offsets.push(offset);
        }
        if (ok) {
            usdLayerSetField(t.layer, spec, "subLayers",
                usdLayerValue(t.layer, PrUsdType_Asset, true, paths.count, paths.data));
            usdLayerSetField(t.layer, spec, "subLayerOffsets",
                usdLayerValue(t.layer, PrUsdType_LayerOffset, true, offsets.count, offsets.data));
        }
        paths.reset();
        offsets.reset();
        return ok;
    }
    if (form == PrUsdSpecForm_Layer && name == "relocates" && keyword == kNoListKeyword) {
        if (!take(t, '{'))
            return syntax(t, "'{' is expected");
        RawArray<PrUsdRelocate> relocates{};
        bool ok = true;
        for (;;) {
            skipLines(t);
            if (take(t, '}'))
                break;
            if (relocates.count != 0) {
                if (!take(t, ',')) {
                    ok = syntax(t, "',' or '}' is expected in the relocates");
                    break;
                }
                skipLines(t);
                if (take(t, '}'))
                    break;
            }
            PrStringView source;
            PrStringView target;
            PrUsdRelocate relocate{};
            ok = readPathReference(t, &source);
            skipInline(t);
            ok = ok && (take(t, ':') || syntax(t, "':' is expected between the paths of a relocate"));
            skipInline(t);
            ok = ok && readPathReference(t, &target);
            // A relative path is a path from the root.
            ok = ok && resolvePath(t, "/", source, &relocate.source);
            relocate.target = usdLayerIntern(t.layer, "");
            if (ok && target.size != 0)
                ok = resolvePath(t, "/", target, &relocate.target);
            if (ok && (!usdPathIsValid(relocate.source, UsdPathKind_Prim) ||
                    (target.size != 0 && !usdPathIsValid(relocate.target, UsdPathKind_Prim))))
                ok = invalid(t, "a relocate names a path that is not a path to a prim");
            if (!ok)
                break;
            relocates.push(relocate);
        }
        if (ok)
            usdLayerSetField(t.layer, spec, "layerRelocates",
                usdLayerValue(t.layer, PrUsdType_Relocate, true, relocates.count, relocates.data));
        relocates.reset();
        return ok;
    }
    if ((name == "prefixSubstitutions" || name == "suffixSubstitutions") && keyword == kNoListKeyword) {
        // A map from strings to strings, written with colons.
        if (!take(t, '{'))
            return syntax(t, "'{' is expected");
        RawArray<PrUsdDictionaryEntry> entries{};
        bool ok = true;
        for (;;) {
            skipLines(t);
            if (take(t, '}'))
                break;
            if (entries.count != 0) {
                if (!take(t, ',')) {
                    ok = syntax(t, "',' or '}' is expected");
                    break;
                }
                skipLines(t);
                if (take(t, '}'))
                    break;
            }
            PrUsdDictionaryEntry entry{};
            PrStringView text;
            ok = readLayerString(t, &entry.key);
            skipInline(t);
            ok = ok && (take(t, ':') || syntax(t, "':' is expected"));
            skipInline(t);
            ok = ok && readLayerString(t, &text);
            if (!ok)
                break;
            entry.value = usdLayerValue(t.layer, PrUsdType_String, false, 1, &text);
            entries.push(entry);
        }
        if (ok)
            usdLayerSetField(t.layer, spec, name, usdLayerDictionary(t.layer, entries));
        entries.reset();
        return ok;
    }

    // Every other field is written as its name and a value, which the table of fields gives a type.
    UsdFieldType type{};
    bool known = usdFieldTypeFromName(name, &type);
    if (usdFieldIsChildren(name) || name == "specifier" || name == "custom" || name == "variability" ||
            name == "default" || name == "timeSamples" || name == "targetPaths" || name == "connectionPaths" ||
            name == "inheritPaths" || name == "variantSetNames" || name == "variantSelection" ||
            name == "subLayerOffsets" || name == "layerRelocates" || name == "typeName")
        return invalid(t, "a field that the format writes in its own syntax is written as metadata");
    if (keyword != kNoListKeyword || (known && type.isListOp)) {
        PrUsdType itemType = PrUsdType_Unregistered;
        if (known) {
            if (!type.isListOp)
                return invalid(t, "a list op's keyword is given to a field that is not a list op");
            itemType = type.type;
        }
        if (keyword == kNoListKeyword && known && takeNone(t))
            return assignListOp(t, spec, name, itemType, keyword, PrUsdValue{itemType, true, 0, nullptr});
        if (!readScalarList(t, itemType, false, &value))
            return false;
        return assignListOp(t, spec, name, itemType, keyword, value);
    }
    if (!known) {
        if (!readUnregistered(t, &value))
            return false;
        usdLayerSetField(t.layer, spec, name, value);
        return true;
    }
    if (takeNone(t)) {
        // None clears a field, which a dictionary or an array reads as an empty one.
        if (type.type == PrUsdType_Dictionary) {
            RawArray<PrUsdDictionaryEntry> none{};
            value = usdLayerDictionary(t.layer, none);
        } else if (type.isArray) {
            value = PrUsdValue{type.type, true, 0, nullptr};
        } else {
            return invalid(t, "None is given to a field that is not a list or a dictionary");
        }
    } else if (type.type == PrUsdType_Dictionary) {
        if (!readDictionary(t, &value))
            return false;
    } else if (!readValue(t, type.type, type.isArray, anchor, &value)) {
        return false;
    }
    usdLayerSetField(t.layer, spec, name, value);
    return true;
}

// Reads a block of metadata between parentheses, if one is at the cursor.
bool readMetadata(Text& t, PrUsdSpecIndex spec) {
    if (!take(t, '('))
        return true;
    for (;;) {
        skipLines(t);
        if (take(t, ')'))
            return true;
        if (t.at >= t.end)
            return syntax(t, "a block of metadata does not end");
        if (!readMetadataItem(t, spec))
            return false;
        skipLine(t);
        if (t.at < t.end && *t.at == ')')
            continue;
        if (!skipSeparator(t))
            return syntax(t, "a new line, ';' or ')' is expected after a field");
    }
}

// Reads a list of names: one string, or strings between brackets.
bool readNameList(Text& t, PrUsdValue* out) {
    if (t.at < t.end && *t.at != '[') {
        PrStringView name;
        if (!readLayerString(t, &name))
            return false;
        *out = usdLayerValue(t.layer, PrUsdType_Token, true, 1, &name);
        return true;
    }
    return readValue(t, PrUsdType_Token, true, "/", out);
}

// Reads the samples of an attribute: times and values between braces.
bool readTimeSamples(Text& t, PrUsdType type, bool isArray, bool typed, PrStringView anchor, PrUsdValue* out) {
    if (!take(t, '{'))
        return syntax(t, "'{' is expected");
    RawArray<double> times{};
    RawArray<PrUsdValue> values{};
    bool ok = true;
    for (;;) {
        skipLines(t);
        if (take(t, '}'))
            break;
        if (times.count != 0) {
            if (!take(t, ',')) {
                ok = syntax(t, "',' or '}' is expected in the time samples");
                break;
            }
            skipLines(t);
            if (take(t, '}'))
                break;
        }
        Number time;
        PrUsdValue value{};
        ok = readNumber(t, &time);
        skipInline(t);
        ok = ok && (take(t, ':') || syntax(t, "':' is expected after a sample's time"));
        skipInline(t);
        if (!ok)
            break;
        if (time.value != time.value) {
            ok = invalid(t, "a sample's time is not a number");
            break;
        }
        if (takeNone(t))
            value = PrUsdValue{PrUsdType_Block, false, 0, nullptr};
        else if (!typed)
            ok = readUnregistered(t, &value);
        else
            ok = readValue(t, type, isArray, anchor, &value);
        if (!ok)
            break;
        times.push(doubleOf(time));
        values.push(value);
    }
    if (ok)
        *out = usdLayerTimeSamples(t.layer, times, values);
    times.reset();
    values.reset();
    return ok;
}

// Reads a property of the prim or variant `parent`, after the keyword of a list op if it has one, and writes its spec
// to `*out` once it has one.
bool readProperty(Text& t, PrUsdSpecIndex parent, ListKeyword keyword, PrUsdSpecIndex* out) {
    PrStringView anchor = t.layer->specs.data[parent].path;
    bool custom = takeWord(t, "custom");
    if (custom)
        skipInline(t);
    const char* afterCustom = t.at;
    bool varying = takeWord(t, "varying");
    if (varying)
        skipInline(t);
    bool existed = false;
    if (takeWord(t, "rel")) {
        skipInline(t);
        PrStringView name;
        if (!readNamespacedName(t, &name))
            return false;
        PrUsdSpecIndex spec = usdLayerAddSpec(t.layer, PrUsdSpecForm_Relationship, parent, name, &existed);
        *out = spec;
        if (existed && t.layer->specs.data[spec].form != PrUsdSpecForm_Relationship)
            return invalid(t, "a relationship has the name of an attribute of its prim");
        if (!existed || custom)
            setByte(t, spec, "custom", PrUsdType_Bool, custom);
        if (!existed || varying)
            setByte(t, spec, "variability", PrUsdType_Variability,
                varying ? PrUsdVariability_Varying : PrUsdVariability_Uniform);
        const char* before = t.at;
        skipInline(t);
        if (take(t, '.')) {
            if (!takeWord(t, "default") || keyword != kNoListKeyword)
                return syntax(t, "'default' is expected after a relationship's name");
            PrStringView raw;
            PrStringView path;
            if (!expectAssignment(t) || !readPathReference(t, &raw) || !resolvePath(t, anchor, raw, &path))
                return false;
            usdLayerSetField(t.layer, spec, "default", usdLayerValue(t.layer, PrUsdType_Path, false, 1, &path));
            return true;
        }
        if (take(t, '=')) {
            skipInline(t);
            PrUsdValue targets{};
            if (!readPathList(t, anchor, PathUse_Target, keyword == kNoListKeyword, &targets) ||
                    !assignListOp(t, spec, "targetPaths", PrUsdType_Path, keyword, targets))
                return false;
            before = t.at;
            skipInline(t);
        }
        if (t.at < t.end && *t.at == '(')
            return readMetadata(t, spec);
        t.at = before;
        return true;
    }
    if (varying)
        t.at = afterCustom;

    // An attribute: its variability, its type, its name, and what is assigned to it.
    bool uniform = takeWord(t, "uniform") || takeWord(t, "config");
    if (uniform)
        skipInline(t);
    const char* typeStart = t.at;
    PrStringView word = peekIdentifier(t);
    if (word.size == 0)
        return syntax(t, "a property or '}' is expected");
    t.at += word.size;
    while (t.at + 2 < t.end && t.at[0] == ':' && t.at[1] == ':' && isNameStart(t.at[2])) {
        t.at += 2;
        t.at += peekIdentifier(t).size;
    }
    PrStringView baseName{typeStart, size_t(t.at - typeStart)};
    const char* afterType = t.at;
    skipInline(t);
    bool isArray = false;
    if (take(t, '[')) {
        skipInline(t);
        if (!take(t, ']'))
            return syntax(t, "']' is expected after '['");
        isArray = true;
        afterType = t.at;
    }
    if (afterType == t.at && !isArray && t.at < t.end && !(*t.at == ' ' || *t.at == '\t'))
        return syntax(t, "a property's name is expected after its type");
    skipInline(t);
    PrStringView name;
    if (!readNamespacedName(t, &name))
        return false;
    RawArray<char> typeText{};
    typeText.append(baseName.data, uint32_t(baseName.size));
    if (isArray)
        typeText.append("[]", 2);
    PrStringView typeName = usdLayerIntern(t.layer, PrStringView{typeText.data, typeText.count});
    typeText.reset();
    PrUsdType type = PrUsdType_Block;
    bool arrayOfName = false;
    bool typed = usdTypeFromName(typeName, &type, &arrayOfName);

    PrUsdSpecIndex spec = usdLayerAddSpec(t.layer, PrUsdSpecForm_Attribute, parent, name, &existed);
    *out = spec;
    if (existed) {
        if (t.layer->specs.data[spec].form != PrUsdSpecForm_Attribute)
            return invalid(t, "an attribute has the name of a relationship of its prim");
        const PrUsdField* declared = usdLayerBuildField(t.layer, spec, "typeName");
        if (declared != nullptr && *static_cast<const PrStringView*>(declared->value.data) != typeName)
            return invalid(t, "an attribute is declared again with another type");
    }
    setString(t, spec, "typeName", PrUsdType_Token, typeName);
    if (!existed || custom)
        setByte(t, spec, "custom", PrUsdType_Bool, custom);
    if (!existed || uniform)
        setByte(t, spec, "variability", PrUsdType_Variability,
            uniform ? PrUsdVariability_Uniform : PrUsdVariability_Varying);

    const char* before = t.at;
    skipInline(t);
    if (take(t, '.')) {
        skipInline(t);
        if (takeWord(t, "connect")) {
            PrUsdValue paths{};
            return expectAssignment(t) && readPathList(t, anchor, PathUse_Connection, keyword == kNoListKeyword, &paths) &&
                assignListOp(t, spec, "connectionPaths", PrUsdType_Path, keyword, paths);
        }
        if (keyword != kNoListKeyword)
            return syntax(t, "'connect' is expected after the name of an attribute with a list op's keyword");
        if (takeWord(t, "timeSamples")) {
            PrUsdValue samples{};
            if (!expectAssignment(t) || !readTimeSamples(t, type, isArray, typed, anchor, &samples))
                return false;
            usdLayerSetField(t.layer, spec, "timeSamples", samples);
            return true;
        }
        if (takeWord(t, "spline"))
            return fail(t, PrStatus_NotSupported, "the layer has a spline, which the module does not read");
        return syntax(t, "'connect', 'timeSamples' or 'spline' is expected");
    }
    if (keyword != kNoListKeyword)
        return syntax(t, "a list op's keyword is given to an attribute without a connection");
    if (take(t, '=')) {
        skipInline(t);
        PrUsdValue value{};
        if (takeNone(t)) {
            value = PrUsdValue{PrUsdType_Block, false, 0, nullptr};
        } else if (typed) {
            if (!readValue(t, type, isArray, anchor, &value))
                return false;
        } else if (typeName == "opaque" || typeName == "group") {
            return invalid(t, "an opaque attribute is given a value");
        } else if (t.at < t.end && *t.at == '<') {
            // An attribute of a type that the module does not know keeps a path as a path, and anything else as text.
            PrStringView raw;
            PrStringView path;
            if (!readPathReference(t, &raw) || !resolvePath(t, anchor, raw, &path))
                return false;
            value = usdLayerValue(t.layer, PrUsdType_Path, false, 1, &path);
        } else if (!readUnregistered(t, &value)) {
            return false;
        }
        usdLayerSetField(t.layer, spec, "default", value);
        before = t.at;
        skipInline(t);
    }
    if (t.at < t.end && *t.at == '(')
        return readMetadata(t, spec);
    t.at = before;
    return true;
}

bool readPrim(Text& t, PrUsdSpecIndex parent, PrUsdSpecifier specifier, const char* begin);

// Reads what a prim or a variant holds, after its opening brace, through its closing brace.
bool readPrimContents(Text& t, PrUsdSpecIndex spec) {
    for (;;) {
        skipLines(t);
        if (take(t, '}'))
            return true;
        if (t.at >= t.end)
            return syntax(t, "a prim does not end");
        PrStringView word = peekIdentifier(t);
        bool isPrim = word == "def" || word == "over" || word == "class";
        if (isPrim || word == "variantSet") {
            const char* begin = t.at;
            if (isPrim) {
                t.at += word.size;
                PrUsdSpecifier specifier = PrUsdSpecifier_Def;
                if (word == "over")
                    specifier = PrUsdSpecifier_Over;
                else if (word == "class")
                    specifier = PrUsdSpecifier_Class;
                if (!readPrim(t, spec, specifier, begin))
                    return false;
            } else {
                t.at += word.size;
                skipInline(t);
                PrStringView name;
                if (!readLayerString(t, &name))
                    return false;
                if (!usdNameIsIdentifier(name))
                    return invalid(t, "a variant set's name is not an identifier");
                bool existed = false;
                PrUsdSpecIndex set = usdLayerAddSpec(t.layer, PrUsdSpecForm_VariantSet, spec, name, &existed);
                // A set that the prim's metadata declared has its spec already, and is given its variants once.
                if (existed && !t.declared.removeFirst(set))
                    return invalid(t, "a prim has two variant sets of one name");
                if (!expectAssignment(t))
                    return false;
                skipLines(t);
                if (!take(t, '{'))
                    return syntax(t, "'{' is expected after a variant set's name");
                uint32_t variants = 0;
                for (;;) {
                    skipLines(t);
                    if (take(t, '}'))
                        break;
                    const char* variantBegin = t.at;
                    PrStringView variantName;
                    if (!readLayerString(t, &variantName))
                        return false;
                    if (!usdNameIsVariant(variantName))
                        return invalid(t, "a variant's name has a character that the grammar does not allow it");
                    PrUsdSpecIndex variant = usdLayerAddSpec(t.layer, PrUsdSpecForm_Variant, set, variantName, &existed);
                    if (existed)
                        return invalid(t, "a variant set has two variants of one name");
                    setByte(t, variant, "specifier", PrUsdType_Specifier, PrUsdSpecifier_Over);
                    skipLines(t);
                    if (!readMetadata(t, variant))
                        return false;
                    skipLines(t);
                    if (!take(t, '{'))
                        return syntax(t, "'{' is expected after a variant's name");
                    if (++t.depth > kMaxDepth)
                        return fail(t, PrStatus_Limit, "prims and variants nest too deeply");
                    if (!readPrimContents(t, variant))
                        return false;
                    setSpan(t, variant, variantBegin);
                    --t.depth;
                    ++variants;
                }
                if (variants == 0)
                    return syntax(t, "a variant set has no variants");
                setSpan(t, set, begin);
            }
            // A prim and a variant set end their line.
            skipLine(t);
            if (t.at < t.end && !isNewline(*t.at) && *t.at != '}')
                return syntax(t, "a new line is expected after a prim or a variant set");
            continue;
        }
        const char* start = t.at;
        ListKeyword keyword = takeListKeyword(t);
        // A word that only begins with a keyword's letters, as `appendix`, is a type's name.
        if (keyword != kNoListKeyword && (t.at >= t.end || !(*t.at == ' ' || *t.at == '\t'))) {
            t.at = start;
            keyword = kNoListKeyword;
        }
        if (keyword == PrUsdListOpList_Ordered) {
            // The orders of a prim's children and properties are written with the keyword of a list op.
            skipInline(t);
            bool children = takeWord(t, "nameChildren");
            if (children || takeWord(t, "properties")) {
                PrUsdValue names{};
                if (!expectAssignment(t) || !readNameList(t, &names))
                    return false;
                usdLayerSetField(t.layer, spec, children ? "primOrder" : "propertyOrder", names);
                if (!skipSeparator(t))
                    return syntax(t, "a new line or ';' is expected after an order");
                continue;
            }
        }
        if (keyword != kNoListKeyword)
            skipInline(t);
        PrUsdSpecIndex property = kPrUsdNone;
        if (!readProperty(t, spec, keyword, &property))
            return false;
        setSpan(t, property, start);
        if (!skipSeparator(t))
            return syntax(t, "a new line or ';' is expected after a property");
    }
}

// Reads a prim, after its specifier, which begins at `begin`, as a child of `parent`.
bool readPrim(Text& t, PrUsdSpecIndex parent, PrUsdSpecifier specifier, const char* begin) {
    skipInline(t);
    PrStringView typeName{};
    if (t.at < t.end && *t.at != '"' && *t.at != '\'') {
        // A prim's type is an identifier, whose parts legacy content joins with dots or double colons.
        const char* start = t.at;
        for (;;) {
            PrStringView part = peekIdentifier(t);
            if (part.size == 0)
                return syntax(t, "a prim's type or its name is expected");
            t.at += part.size;
            if (t.at + 1 < t.end && *t.at == '.' && isNameStart(t.at[1]))
                ++t.at;
            else if (t.at + 2 < t.end && t.at[0] == ':' && t.at[1] == ':' && isNameStart(t.at[2]))
                t.at += 2;
            else
                break;
        }
        typeName = PrStringView{start, size_t(t.at - start)};
        skipInline(t);
    }
    PrStringView name;
    if (!readLayerString(t, &name))
        return false;
    if (!usdNameIsIdentifier(name))
        return invalid(t, "a prim's name is not an identifier");
    bool existed = false;
    PrUsdSpecIndex spec = usdLayerAddSpec(t.layer, PrUsdSpecForm_Prim, parent, name, &existed);
    if (existed)
        return invalid(t, "two prims have one path");
    setByte(t, spec, "specifier", PrUsdType_Specifier, specifier);
    if (typeName.size != 0)
        setString(t, spec, "typeName", PrUsdType_Token, usdLayerIntern(t.layer, typeName));
    skipLines(t);
    if (!readMetadata(t, spec))
        return false;
    skipLines(t);
    if (!take(t, '{'))
        return syntax(t, "'{' is expected after a prim's name");
    if (++t.depth > kMaxDepth)
        return fail(t, PrStatus_Limit, "prims and variants nest too deeply");
    if (!readPrimContents(t, spec))
        return false;
    setSpan(t, spec, begin);
    --t.depth;
    return true;
}

// Reads the layer: its header, its metadata and its root prims.
bool readLayer(Text& t) {
    PrStringView header{t.at, size_t(t.end - t.at)};
    if (!prStringStartsWith(header, "#usda "))
        return syntax(t, "the file does not begin with '#usda '");
    t.at += 6;
    while (t.at < t.end && (*t.at == ' ' || *t.at == '\t'))
        ++t.at;
    const char* version = t.at;
    while (t.at < t.end && ((*t.at >= '0' && *t.at <= '9') || *t.at == '.'))
        ++t.at;
    PrStringView stated{version, size_t(t.at - version)};
    if (stated.size == 0)
        return syntax(t, "the header states no version");
    t.layer->version = usdLayerIntern(t.layer, stated);
    // The versions that the specification defines: 1.0, and 1.1, which adds splines.
    bool supported = stated == "1.0" || stated == "1.1" || prStringStartsWith(stated, "1.0.") ||
        prStringStartsWith(stated, "1.1.");
    if (!supported)
        return fail(t, PrStatus_NotSupported, "the module does not read this version of the text format");
    skipLine(t);
    if (t.at < t.end && !isNewline(*t.at))
        return syntax(t, "a new line is expected after the header");
    skipLines(t);
    if (!readMetadata(t, kPrUsdLayerSpec))
        return false;
    for (;;) {
        skipLines(t);
        if (t.at >= t.end)
            return true;
        PrStringView word = peekIdentifier(t);
        const char* begin = t.at;
        t.at += word.size;
        if (word == "reorder") {
            skipInline(t);
            PrUsdValue names{};
            if (!takeWord(t, "rootPrims"))
                return syntax(t, "'rootPrims' is expected");
            if (!expectAssignment(t) || !readNameList(t, &names))
                return false;
            usdLayerSetField(t.layer, kPrUsdLayerSpec, "primOrder", names);
        } else if (word == "def") {
            if (!readPrim(t, kPrUsdLayerSpec, PrUsdSpecifier_Def, begin))
                return false;
        } else if (word == "over") {
            if (!readPrim(t, kPrUsdLayerSpec, PrUsdSpecifier_Over, begin))
                return false;
        } else if (word == "class") {
            if (!readPrim(t, kPrUsdLayerSpec, PrUsdSpecifier_Class, begin))
                return false;
        } else {
            return syntax(t, "a prim or the order of the root prims is expected");
        }
        skipLine(t);
        if (t.at < t.end && !isNewline(*t.at))
            return syntax(t, "a new line is expected after a prim");
    }
}

} // namespace

PrStatus usdTextRead(UsdLayer* layer, PrStringView text, PrTextBuffer* PR_NULLABLE diagnostics) {
    Text t{};
    t.layer = layer;
    t.begin = text.data;
    t.at = text.data;
    t.end = text.data + text.size;
    t.diagnostics = diagnostics;
    // The spans of specs count bytes in 32 bits.
    if (text.size > 0xffffffffu) {
        fail(t, PrStatus_Limit, "the text is longer than 4 GiB");
        return t.status;
    }
    if (prUtf8Validate(text) != text.size) {
        syntax(t, "the text is not UTF-8");
        return t.status;
    }
    readLayer(t);
    t.declared.reset();
    if (t.status != PrStatus_Ok)
        return t.status;
    // The layer spec's text is the file. The specs whose text is not empty are ordered by where it begins, a parent
    // before its first child.
    t.at = t.end;
    setSpan(t, kPrUsdLayerSpec, t.begin);
    const RawArray<PrUsdTextSpan>& spans = layer->textSpans;
    for (PrUsdSpecIndex i = 0; i < spans.count; ++i) {
        if (spans.data[i].end > spans.data[i].begin)
            layer->textOrder.push(i);
    }
    sortItems(layer->textOrder.span(), [&](PrUsdSpecIndex a, PrUsdSpecIndex b) {
        if (spans.data[a].begin != spans.data[b].begin)
            return spans.data[a].begin < spans.data[b].begin;
        return spans.data[a].end > spans.data[b].end;
    });
    return t.status;
}

} // namespace pr_impl
