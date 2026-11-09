// CSS colors without a style sheet (color_css.h).

#include "pr/color_css.h"

#include "pr/ascii.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/float_convert.h"

#include "color/src/color_names.h"

namespace pr_impl {

namespace {

// A cursor over the text of a color function's arguments.
struct Reader {
    PrStringView text;
    size_t at;
};

void skipSpace(Reader& reader) {
    while (reader.at < reader.text.size && prAsciiIsSpace(reader.text.data[reader.at]))
        ++reader.at;
}

bool consume(Reader& reader, char c) {
    skipSpace(reader);
    if (reader.at < reader.text.size && reader.text.data[reader.at] == c) {
        ++reader.at;
        return true;
    }
    return false;
}

// Reads a CSS number, and whether a percent sign follows it.
bool readNumber(Reader& reader, double* value, bool* percent) {
    skipSpace(reader);
    const char* data = reader.text.data;
    size_t size = reader.text.size;
    size_t at = reader.at;
    size_t taken = prF64ParsePrefix(PrStringView{data + at, size - at}, value);
    if (taken == 0)
        return false;
    at += taken;
    *percent = at < size && data[at] == '%';
    if (*percent)
        ++at;
    // Skips a unit such as deg. Only a hue takes a unit, and degrees are its default.
    while (at < size && prAsciiIsLetter(data[at]))
        ++at;
    reader.at = at;
    return true;
}

// Parses the arguments of rgb() or hsl(), from after the opening parenthesis: three components and an optional alpha,
// separated by commas or by spaces with a slash before the alpha.
bool parseFunction(Reader& reader, bool hsl, PrColor* out) {
    double values[4] = {0, 0, 0, 1};
    bool percents[4] = {false, false, false, false};
    bool commas = false;
    for (uint32_t i = 0; i < 3; ++i) {
        if (i > 0) {
            bool comma = consume(reader, ',');
            if (i == 1)
                commas = comma;
            else if (comma != commas)
                return false;
        }
        if (!readNumber(reader, &values[i], &percents[i]))
            return false;
    }
    bool hasAlpha = commas ? consume(reader, ',') : consume(reader, '/');
    if (hasAlpha && !readNumber(reader, &values[3], &percents[3]))
        return false;
    if (!consume(reader, ')'))
        return false;
    skipSpace(reader);
    if (reader.at != reader.text.size)
        return false;
    double alpha = percents[3] ? values[3] / 100 : values[3];
    alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    if (hsl) {
        double saturation = values[1] < 0 ? 0 : values[1] > 100 ? 100 : values[1];
        double lightness = values[2] < 0 ? 0 : values[2] > 100 ? 100 : values[2];
        *out = prColorFromHsl(prColorHue(values[0]), saturation, lightness, alpha);
    } else {
        double rgb[3];
        for (uint32_t i = 0; i < 3; ++i) {
            double value = percents[i] ? values[i] / 100 : values[i] / 255;
            rgb[i] = value < 0 ? 0 : value > 1 ? 1 : value;
        }
        *out = PrColor{float(rgb[0]), float(rgb[1]), float(rgb[2]), float(alpha)};
    }
    return true;
}

bool parseHex(PrStringView digits, PrColor8* out) {
    for (size_t i = 0; i < digits.size; ++i) {
        if (!prAsciiIsHexDigit(digits.data[i]))
            return false;
    }
    auto hex = [&](size_t i) { return uint8_t(prAsciiHexValue(digits.data[i])); };
    switch (digits.size) {
        case 3:
        case 4: {
            uint8_t a = digits.size == 4 ? uint8_t(hex(3) * 17) : uint8_t(255);
            *out = PrColor8{uint8_t(hex(0) * 17), uint8_t(hex(1) * 17), uint8_t(hex(2) * 17), a};
            return true;
        }
        case 6:
        case 8: {
            uint8_t a = digits.size == 8 ? uint8_t(hex(6) * 16 + hex(7)) : uint8_t(255);
            *out = PrColor8{uint8_t(hex(0) * 16 + hex(1)), uint8_t(hex(2) * 16 + hex(3)), uint8_t(hex(4) * 16 + hex(5)), a};
            return true;
        }
        default:
            return false;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prColorFromCss(PrStringView text, PrColor* out) {
    size_t start = 0;
    size_t end = text.size;
    while (start < end && prAsciiIsSpace(text.data[start]))
        ++start;
    while (end > start && prAsciiIsSpace(text.data[end - 1]))
        --end;
    PrStringView trimmed{text.data + start, end - start};
    if (trimmed.size == 0)
        return false;
    size_t open = 0;
    while (open < trimmed.size && trimmed.data[open] != '(')
        ++open;
    if (trimmed.data[0] != '#' && open < trimmed.size) {
        PrStringView name{trimmed.data, open};
        Reader reader{trimmed, open + 1};
        if (prAsciiEqualsFold(name, "rgb") || prAsciiEqualsFold(name, "rgba"))
            return parseFunction(reader, false, out);
        if (prAsciiEqualsFold(name, "hsl") || prAsciiEqualsFold(name, "hsla"))
            return parseFunction(reader, true, out);
        return false;
    }
    PrColor8 color{};
    bool parsed;
    if (trimmed.data[0] == '#')
        parsed = parseHex(PrStringView{trimmed.data + 1, trimmed.size - 1}, &color);
    else if (prAsciiEqualsFold(trimmed, "transparent"))
        parsed = true;
    else
        parsed = colorNamed(trimmed, &color);
    if (parsed)
        *out = prColorFromColor8(color);
    return parsed;
}

bool prColor8FromCss(PrStringView text, PrColor8* out) {
    PrColor color{};
    if (!prColorFromCss(text, &color))
        return false;
    auto channel = [](float value) { return uint8_t(prF32Round(prF32Clamp(value, 0, 1) * 255)); };
    *out = PrColor8{channel(color.r), channel(color.g), channel(color.b), channel(color.a)};
    return true;
}

bool prColor8FromCssName(PrStringView name, PrColor8* out) {
    return colorNamed(name, out);
}
