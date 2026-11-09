// Rendering (mermaid_render.h). A label's source is decoded into its characters and a list of styled ranges, which
// become the font spans of a text layout and the colors and decorations that a draw adds.

#include "mermaid/src/mermaid_render.h"

#include "pr/ascii.h"
#include "pr/color_css.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/geom.h"
#include "pr/html_entity.h"
#include "pr/markdown.h"
#include "pr/mem.h"
#include "pr/path_polyline.h"
#include "pr/path_stroke.h"
#include "pr/string_ops.h"
#include "pr/text_font.h"
#include "pr/text_shaping.h"
#include "pr/utf8.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// By role: Mermaid's default theme, as mermaid.js 12.3 computes its variables, and the engine's dark theme, which is
// its counterpart: the default's indigo nodes and olive subgraphs, darkened, with light text and lines.
constexpr PrColor8 kPalettes[2][MermaidRole_Count] = {
    {
        {0, 0, 0, 0},
        {0xff, 0xff, 0xff, 0xff},       // Background
        {0x33, 0x33, 0x33, 0xff},       // Text
        {0x33, 0x33, 0x33, 0xff},       // Title
        {0x33, 0x33, 0x33, 0xff},       // Line
        {0xec, 0xec, 0xff, 0xff},       // NodeFill
        {0x93, 0x70, 0xdb, 0xff},       // NodeStroke
        {0x33, 0x33, 0x33, 0xff},       // NodeText
        {0xff, 0xff, 0xde, 0xff},       // ClusterFill
        {0xaa, 0xaa, 0x33, 0xff},       // ClusterStroke
        {0xe8, 0xe8, 0xe8, 0xcc},       // EdgeLabelBackground
        {0x55, 0x22, 0x22, 0xff},       // ErrorIcon
        {0x55, 0x22, 0x22, 0xff},       // ErrorText
        {0x00, 0x00, 0x00, 0xff},       // StrongText
        {0xff, 0xf5, 0xad, 0xff},       // NoteFill
        {0xaa, 0xaa, 0x33, 0xff},       // NoteStroke
        {0xf4, 0xf4, 0xf4, 0xff},       // ActivationFill
        {0x66, 0x66, 0x66, 0xff},       // ActivationStroke
        {0xff, 0xff, 0xff, 0xff},       // SequenceNumberText
        {0xf0, 0xf0, 0xf0, 0xff},       // AltBackground
        {0xff, 0xff, 0xff, 0xff},       // RowOdd
        {0xf8, 0xf8, 0xff, 0xff},       // RowEven
        {0x66, 0x66, 0xff, 0x19},       // GanttBandA
        {0xff, 0xf4, 0x00, 0x33},       // GanttBandB
        {0xd3, 0xd3, 0xd3, 0xcc},       // GanttGrid
        {0xee, 0xee, 0xee, 0xff},       // GanttExclude
        {0x8a, 0x90, 0xdd, 0xff},       // GanttTaskFill
        {0x53, 0x4f, 0xbc, 0xff},       // GanttTaskStroke
        {0xff, 0xff, 0xff, 0xff},       // GanttTaskText
        {0xbf, 0xc7, 0xff, 0xff},       // GanttActiveFill
        {0xd3, 0xd3, 0xd3, 0xff},       // GanttDoneFill
        {0x80, 0x80, 0x80, 0xff},       // GanttDoneStroke
        {0xff, 0x00, 0x00, 0xff},       // GanttCritFill
        {0xff, 0x88, 0x88, 0xff},       // GanttCritStroke
        {0x00, 0x00, 0x80, 0xff},       // GanttVertLine
        {0xec, 0xec, 0xff, 0xff},       // Pie1
        {0xff, 0xff, 0xde, 0xff},       // Pie2
        {0xb5, 0xff, 0x20, 0xff},       // Pie3
        {0xb9, 0xb9, 0xff, 0xff},       // Pie4
        {0xff, 0xff, 0x45, 0xff},       // Pie5
        {0xd7, 0xff, 0x86, 0xff},       // Pie6
        {0xff, 0x86, 0xff, 0xff},       // Pie7
        {0x20, 0xff, 0xff, 0xff},       // Pie8
        {0xff, 0x20, 0x20, 0xff},       // Pie9
        {0xff, 0x20, 0xff, 0xff},       // Pie10
        {0x20, 0xff, 0x90, 0xff},       // Pie11
        {0xff, 0x53, 0x53, 0xff},       // Pie12
    },
    {
        {0, 0, 0, 0},
        {0x1e, 0x1f, 0x26, 0xff},       // Background
        {0xe2, 0xe2, 0xea, 0xff},       // Text
        {0xf2, 0xf2, 0xf8, 0xff},       // Title
        {0xb4, 0xb4, 0xc4, 0xff},       // Line
        {0x2e, 0x2a, 0x4e, 0xff},       // NodeFill
        {0xa6, 0x8b, 0xe6, 0xff},       // NodeStroke
        {0xe8, 0xe6, 0xf5, 0xff},       // NodeText
        {0x33, 0x32, 0x1e, 0xff},       // ClusterFill
        {0xb8, 0xb8, 0x52, 0xff},       // ClusterStroke
        {0x3c, 0x3c, 0x4a, 0xd9},       // EdgeLabelBackground
        {0xd0, 0x5a, 0x5a, 0xff},       // ErrorIcon
        {0xe2, 0xe2, 0xea, 0xff},       // ErrorText
        {0xf2, 0xf2, 0xf8, 0xff},       // StrongText
        {0x3d, 0x3a, 0x1e, 0xff},       // NoteFill
        {0xb8, 0xb8, 0x52, 0xff},       // NoteStroke
        {0x3a, 0x3a, 0x48, 0xff},       // ActivationFill
        {0x9a, 0x9a, 0xaa, 0xff},       // ActivationStroke
        {0x1e, 0x1f, 0x26, 0xff},       // SequenceNumberText
        {0x2a, 0x2b, 0x34, 0xff},       // AltBackground
        {0x24, 0x22, 0x3c, 0xff},       // RowOdd
        {0x29, 0x26, 0x45, 0xff},       // RowEven
        {0x66, 0x66, 0xff, 0x22},       // GanttBandA
        {0xff, 0xf4, 0x00, 0x14},       // GanttBandB
        {0x4a, 0x4a, 0x58, 0xff},       // GanttGrid
        {0x2c, 0x2d, 0x36, 0xff},       // GanttExclude
        {0x6b, 0x70, 0xc4, 0xff},       // GanttTaskFill
        {0x9a, 0x96, 0xe6, 0xff},       // GanttTaskStroke
        {0xff, 0xff, 0xff, 0xff},       // GanttTaskText
        {0x3e, 0x44, 0x70, 0xff},       // GanttActiveFill
        {0x55, 0x55, 0x5f, 0xff},       // GanttDoneFill
        {0x8a, 0x8a, 0x96, 0xff},       // GanttDoneStroke
        {0xc0, 0x39, 0x2b, 0xff},       // GanttCritFill
        {0xff, 0x88, 0x88, 0xff},       // GanttCritStroke
        {0x00, 0xbf, 0xff, 0xff},       // GanttVertLine, mermaid.js's dark theme's
        {0x4b, 0x4a, 0x8a, 0xff},       // Pie1
        {0x6b, 0x6a, 0x2e, 0xff},       // Pie2
        {0x4f, 0x7a, 0x12, 0xff},       // Pie3
        {0x5a, 0x5a, 0xb8, 0xff},       // Pie4
        {0x8a, 0x8a, 0x1c, 0xff},       // Pie5
        {0x5f, 0x8a, 0x2a, 0xff},       // Pie6
        {0x8a, 0x3a, 0x8a, 0xff},       // Pie7
        {0x1c, 0x7a, 0x7a, 0xff},       // Pie8
        {0x8a, 0x24, 0x24, 0xff},       // Pie9
        {0xa0, 0x1c, 0xa0, 0xff},       // Pie10
        {0x14, 0x8a, 0x50, 0xff},       // Pie11
        {0xa0, 0x38, 0x38, 0xff},       // Pie12
    },
};

// ---------------------------------------------------------------------------------------------------------------------
// Decoding

// The style of a range of decoded text.
struct TextRange {
    uint32_t start;
    uint32_t end;
    bool bold;
    bool italic;
    bool monospace;
    bool underline;
    bool strike;
    bool hasColor;
    PrColor8 color;
};

// Decoded text: its characters, and the styled ranges, in order.
struct Decoded {
    RawArray<char> text;
    RawArray<TextRange> ranges;
    TextRange current;              // the style that the next characters take
};

void appendCodePoint(RawArray<char>& out, uint32_t codePoint) {
    char bytes[4];
    out.append(bytes, prUtf8Encode(codePoint, bytes));
}

// Appends `text` in the current style.
void emit(Decoded& d, PrStringView text) {
    if (text.size == 0)
        return;
    uint32_t start = d.text.count;
    d.text.append(text.data, uint32_t(text.size));
    TextRange range = d.current;
    range.start = start;
    range.end = d.text.count;
    bool styled = range.bold || range.italic || range.monospace || range.underline || range.strike || range.hasColor;
    if (!styled)
        return;
    // A range that continues the last one in the same style extends it.
    if (d.ranges.count > 0) {
        TextRange& last = d.ranges.last();
        bool same = last.end == start && last.bold == range.bold && last.italic == range.italic &&
            last.monospace == range.monospace && last.underline == range.underline && last.strike == range.strike &&
            last.hasColor == range.hasColor && prMemEquals(&last.color, &range.color, sizeof(PrColor8));
        if (same) {
            last.end = range.end;
            return;
        }
    }
    d.ranges.push(range);
}

// Returns the size of a line break written as <br>, <br/> or <br /> at the start of `text`, or zero.
size_t lineBreakAt(PrStringView text) {
    static constexpr PrStringView kForms[] = {"<br>", "<br/>", "<br />"};
    for (PrStringView form : kForms) {
        if (text.size >= form.size && prAsciiEqualsFold(PrStringView{text.data, form.size}, form))
            return form.size;
    }
    return 0;
}

// Returns the size of one of Mermaid's entity codes at the start of `text`, such as #quot; or #9829;, and appends what
// it stands for, or returns zero.
size_t mermaidEntityAt(PrStringView text, RawArray<char>& out) {
    if (text.size < 3 || text.data[0] != '#')
        return 0;
    size_t end = 1;
    while (end < text.size && end < 12 && text.data[end] != ';')
        ++end;
    if (end >= text.size || text.data[end] != ';')
        return 0;
    PrStringView name{text.data + 1, end - 1};
    if (name.size > 0 && prAsciiIsDigit(name.data[0])) {
        uint32_t codePoint = 0;
        for (size_t k = 0; k < name.size; ++k) {
            if (!prAsciiIsDigit(name.data[k]))
                return 0;
            codePoint = codePoint * 10 + uint32_t(name.data[k] - '0');
        }
        if (codePoint == 0 || codePoint >= 0x110000)
            return 0;
        appendCodePoint(out, codePoint);
        return end + 1;
    }
    PrStringView expansion = prHtmlEntityMatch(PrStringView{text.data + 1, end});
    if (expansion.data == nullptr)
        return 0;
    out.append(expansion.data, uint32_t(expansion.size));
    return end + 1;
}

// Returns the size of an HTML character reference at the start of `text`, such as &amp; or &#x2192;, and appends what
// it stands for, or returns zero.
size_t htmlReferenceAt(PrStringView text, RawArray<char>& out) {
    if (text.size < 3 || text.data[0] != '&')
        return 0;
    size_t end = 1;
    while (end < text.size && end <= kPrHtmlEntityMaxLength && text.data[end] != ';')
        ++end;
    if (end >= text.size || text.data[end] != ';')
        return 0;
    if (text.data[1] == '#') {
        bool hex = end > 2 && (text.data[2] == 'x' || text.data[2] == 'X');
        uint32_t codePoint = 0;
        for (size_t k = hex ? 3 : 2; k < end; ++k) {
            char c = text.data[k];
            if (hex ? !prAsciiIsHexDigit(c) : !prAsciiIsDigit(c))
                return 0;
            codePoint = codePoint * (hex ? 16 : 10) + uint32_t(prAsciiHexValue(c));
            if (codePoint >= 0x110000)
                return 0;
        }
        appendCodePoint(out, codePoint == 0 ? 0xfffd : codePoint);
        return end + 1;
    }
    PrStringView expansion = prHtmlEntityMatch(PrStringView{text.data + 1, end});
    if (expansion.data == nullptr)
        return 0;
    out.append(expansion.data, uint32_t(expansion.size));
    return end + 1;
}

// Decodes characters, line breaks and references, in the current style. HTML's references are read only if `html`.
void emitText(Decoded& d, PrStringView text, bool html) {
    RawArray<char> chars{};
    for (size_t i = 0; i < text.size;) {
        PrStringView rest{text.data + i, text.size - i};
        size_t taken = lineBreakAt(rest);
        if (taken > 0) {
            chars.push('\n');
        } else if ((taken = mermaidEntityAt(rest, chars)) == 0 && html) {
            taken = htmlReferenceAt(rest, chars);
        }
        if (taken == 0) {
            chars.push(text.data[i]);
            taken = 1;
        }
        i += taken;
    }
    emit(d, PrStringView{chars.data, chars.count});
    chars.reset();
}

// Reads the value of attribute `name` from the attributes of a start tag, or returns the null view.
PrStringView attributeOf(PrStringView attributes, PrStringView name) {
    for (size_t i = 0; i + name.size < attributes.size; ++i) {
        bool starts = i == 0 || prAsciiIsSpace(attributes.data[i - 1]);
        if (!starts || !prAsciiEqualsFold(PrStringView{attributes.data + i, name.size}, name))
            continue;
        size_t at = i + name.size;
        while (at < attributes.size && prAsciiIsSpace(attributes.data[at]))
            ++at;
        if (at >= attributes.size || attributes.data[at] != '=')
            continue;
        ++at;
        while (at < attributes.size && prAsciiIsSpace(attributes.data[at]))
            ++at;
        char quote = 0;
        if (at < attributes.size && (attributes.data[at] == '"' || attributes.data[at] == '\''))
            quote = attributes.data[at];
        size_t start = quote != 0 ? at + 1 : at;
        size_t end = start;
        while (end < attributes.size) {
            char c = attributes.data[end];
            if (quote != 0 ? c == quote : prAsciiIsSpace(c))
                break;
            ++end;
        }
        return PrStringView{attributes.data + start, end - start};
    }
    return PrStringView{};
}

// Applies the declarations of a style attribute that text takes: color, font-weight, font-style and text-decoration.
void applyStyleAttribute(TextRange& style, PrStringView declarations) {
    MermaidPaint paint{};
    mermaidPaintParse(declarations, paint);
    if ((paint.flags & MermaidPaintFlag_Color) != 0) {
        style.hasColor = true;
        style.color = paint.color.color;
    }
    if ((paint.flags & MermaidPaintFlag_FontWeight) != 0)
        style.bold = paint.fontWeight >= 600;
    if ((paint.flags & MermaidPaintFlag_FontStyle) != 0)
        style.italic = paint.italic;
    if (prStringContains(declarations, "underline"))
        style.underline = true;
    if (prStringContains(declarations, "line-through"))
        style.strike = true;
}

// Decodes HTML: text, references, and the tags that style text or break lines. A tag that the decoder does not know is
// dropped and its content kept, and an end tag restores the style from before its start tag.
void decodeHtml(Decoded& d, PrStringView text) {
    RawArray<TextRange> stack{};
    size_t textStart = 0;
    for (size_t i = 0; i <= text.size; ++i) {
        if (i < text.size && text.data[i] != '<')
            continue;
        size_t close = i < text.size ? prStringFindByte(PrStringView{text.data + i, text.size - i}, '>') : 0;
        bool isTag = i < text.size && close < text.size - i && i + 1 < text.size &&
            (prAsciiIsLetter(text.data[i + 1]) || text.data[i + 1] == '/');
        if (i < text.size && !isTag)
            continue;
        emitText(d, PrStringView{text.data + textStart, i - textStart}, true);
        if (i == text.size)
            break;
        PrStringView tag{text.data + i + 1, close - 1};
        i += close;
        textStart = i + 1;
        bool end = tag.size > 0 && tag.data[0] == '/';
        if (end)
            tag = PrStringView{tag.data + 1, tag.size - 1};
        size_t nameEnd = 0;
        while (nameEnd < tag.size && (prAsciiIsLetter(tag.data[nameEnd]) || prAsciiIsDigit(tag.data[nameEnd])))
            ++nameEnd;
        PrStringView name{tag.data, nameEnd};
        PrStringView attributes{tag.data + nameEnd, tag.size - nameEnd};
        if (prAsciiEqualsFold(name, "br")) {
            emit(d, "\n");
            continue;
        }
        bool block = prAsciiEqualsFold(name, "p") || prAsciiEqualsFold(name, "div");
        if (block && d.text.count > 0 && d.text.last() != '\n')
            emit(d, "\n");
        if (end) {
            if (stack.count > 0) {
                d.current = stack.last();
                stack.pop();
            }
            continue;
        }
        bool selfClosing = attributes.size > 0 && attributes.data[attributes.size - 1] == '/';
        if (selfClosing || prAsciiEqualsFold(name, "img") || prAsciiEqualsFold(name, "hr"))
            continue;
        stack.push(d.current);
        TextRange& style = d.current;
        if (prAsciiEqualsFold(name, "b") || prAsciiEqualsFold(name, "strong") || prAsciiEqualsFold(name, "th"))
            style.bold = true;
        else if (prAsciiEqualsFold(name, "i") || prAsciiEqualsFold(name, "em") || prAsciiEqualsFold(name, "cite"))
            style.italic = true;
        else if (prAsciiEqualsFold(name, "u") || prAsciiEqualsFold(name, "ins") || prAsciiEqualsFold(name, "a"))
            style.underline = true;
        else if (prAsciiEqualsFold(name, "s") || prAsciiEqualsFold(name, "del") || prAsciiEqualsFold(name, "strike"))
            style.strike = true;
        else if (prAsciiEqualsFold(name, "code") || prAsciiEqualsFold(name, "kbd") || prAsciiEqualsFold(name, "tt") ||
                prAsciiEqualsFold(name, "samp"))
            style.monospace = true;
        PrColor8 color{};
        PrStringView colorAttribute = attributeOf(attributes, "color");
        if (colorAttribute.data != nullptr && prColor8FromCss(colorAttribute, &color)) {
            style.hasColor = true;
            style.color = color;
        }
        PrStringView styleAttribute = attributeOf(attributes, "style");
        if (styleAttribute.data != nullptr)
            applyStyleAttribute(style, styleAttribute);
    }
    stack.reset();
}

// Decodes a Markdown string with lib/markdown: strong and emphasis style their text, code is set in the monospace
// family, and each line break of the source breaks a line. Raw HTML is decoded as HTML.
void decodeMarkdown(Decoded& d, PrStringView text) {
    // Each line loses its indentation first, so that an indented line does not read as a code block.
    RawArray<char> lines{};
    bool lineStart = true;
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (lineStart && (c == ' ' || c == '\t'))
            continue;
        lineStart = c == '\n';
        lines.push(c);
    }
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.extensions = PrMarkdownExtensionFlag_Strikethrough;
    PrMarkdown* document = prMarkdownCreate(&options);
    prMarkdownParse(document, PrStringView{lines.data, lines.count});
    struct Walk {
        Decoded* d;
        RawArray<TextRange> stack;
        uint32_t blocks;
    };
    Walk walk{.d = &d};
    prMarkdownWalk(document, 0, prMarkdownBlockCount(document), [](void* PR_NULLABLE context, const PrMarkdownStep* step) {
        Walk& w = *static_cast<Walk*>(context);
        Decoded& out = *w.d;
        bool enter = step->kind == PrMarkdownStepKind_Enter;
        switch (step->node) {
            case PrMarkdownNodeKind_Paragraph:
            case PrMarkdownNodeKind_Heading:
                if (enter && w.blocks++ > 0)
                    emit(out, "\n");
                if (step->node == PrMarkdownNodeKind_Heading) {
                    if (enter) {
                        w.stack.push(out.current);
                        out.current.bold = true;
                    } else {
                        out.current = w.stack.last();
                        w.stack.pop();
                    }
                }
                return;
            case PrMarkdownNodeKind_SoftBreak:
            case PrMarkdownNodeKind_HardBreak:
                if (enter)
                    emit(out, "\n");
                return;
            case PrMarkdownNodeKind_Strong:
            case PrMarkdownNodeKind_Emphasis:
            case PrMarkdownNodeKind_Code:
            case PrMarkdownNodeKind_Strikethrough:
                if (enter) {
                    w.stack.push(out.current);
                    if (step->node == PrMarkdownNodeKind_Strong)
                        out.current.bold = true;
                    else if (step->node == PrMarkdownNodeKind_Emphasis)
                        out.current.italic = true;
                    else if (step->node == PrMarkdownNodeKind_Code)
                        out.current.monospace = true;
                    else
                        out.current.strike = true;
                } else if (step->kind == PrMarkdownStepKind_Leave) {
                    out.current = w.stack.last();
                    w.stack.pop();
                }
                return;
            case PrMarkdownNodeKind_Text:
                if (step->kind != PrMarkdownStepKind_Text)
                    return;
                if (step->textKind == PrMarkdownTextKind_Html)
                    decodeHtml(out, step->text);
                else
                    emitText(out, step->text, false);
                return;
            default:
                return;
        }
    }, &walk);
    walk.stack.reset();
    prMarkdownDestroy(document);
    lines.reset();
}

void decode(Decoded& d, PrStringView text, MermaidTextFormat format) {
    switch (format) {
        case MermaidTextFormat_Html:
            decodeHtml(d, text);
            return;
        case MermaidTextFormat_Markdown:
            decodeMarkdown(d, text);
            return;
        default:
            emitText(d, text, false);
            return;
    }
}

void resetDecoded(Decoded& d) {
    d.text.reset();
    d.ranges.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// Parses a CSS length or number, ignoring its unit.
bool parseNumber(PrStringView value, float* out) {
    double number = 0;
    if (mermaidNumberParse(value, &number) == 0)
        return false;
    *out = float(number);
    return true;
}

} // namespace

MermaidTextStyle mermaidTextStyle(float size) {
    return MermaidTextStyle{.size = size, .weight = 400, .align = PrTextAlign_Center};
}

MermaidInk mermaidInk(MermaidRole role) {
    return MermaidInk{.role = role};
}

MermaidInk mermaidInkOfColor(PrColor8 color) {
    return MermaidInk{.role = MermaidRole_None, .color = color};
}

MermaidInk mermaidInkOfColors(PrColor8 light, PrColor8 dark) {
    return MermaidInk{.role = MermaidRole_None, .paired = true, .color = light, .dark = dark};
}

PrColor8 mermaidInkColor(MermaidInk ink, PrColorScheme scheme) {
    if (ink.role == MermaidRole_None)
        return ink.paired && scheme == PrColorScheme_Dark ? ink.dark : ink.color;
    return kPalettes[scheme == PrColorScheme_Dark ? 1 : 0][ink.role];
}

void mermaidPaintParse(PrStringView declarations, MermaidPaint& paint) {
    size_t start = 0;
    uint32_t depth = 0;
    for (size_t i = 0; i <= declarations.size; ++i) {
        if (i < declarations.size && declarations.data[i] == '(')
            ++depth;
        if (i < declarations.size && declarations.data[i] == ')' && depth > 0)
            --depth;
        bool separator = i == declarations.size || (depth == 0 && (declarations.data[i] == ',' || declarations.data[i] == ';'));
        if (!separator)
            continue;
        PrStringView declaration = prStringTrim(PrStringView{declarations.data + start, i - start});
        start = i + 1;
        size_t colon = prStringFindByte(declaration, ':');
        if (colon >= declaration.size)
            continue;
        PrStringView name = prStringTrim(PrStringView{declaration.data, colon});
        PrStringView value = prStringTrim(PrStringView{declaration.data + colon + 1, declaration.size - colon - 1});
        if (value.size > 10 && prAsciiEqualsFold(PrStringView{value.data + value.size - 10, 10}, "!important"))
            value = prStringTrim(PrStringView{value.data, value.size - 10});
        PrColor8 color{};
        float number = 0;
        if (prAsciiEqualsFold(name, "fill") && prColor8FromCss(value, &color)) {
            paint.fill = mermaidInkOfColor(color);
            paint.flags |= MermaidPaintFlag_Fill;
        } else if ((prAsciiEqualsFold(name, "stroke") || prAsciiEqualsFold(name, "border-color")) &&
                prColor8FromCss(value, &color)) {
            paint.stroke = mermaidInkOfColor(color);
            paint.flags |= MermaidPaintFlag_Stroke;
        } else if (prAsciiEqualsFold(name, "background") || prAsciiEqualsFold(name, "background-color")) {
            if (prColor8FromCss(value, &color)) {
                paint.fill = mermaidInkOfColor(color);
                paint.flags |= MermaidPaintFlag_Fill;
            }
        } else if (prAsciiEqualsFold(name, "color") && prColor8FromCss(value, &color)) {
            paint.color = mermaidInkOfColor(color);
            paint.flags |= MermaidPaintFlag_Color;
        } else if (prAsciiEqualsFold(name, "stroke-width") && parseNumber(value, &number)) {
            paint.strokeWidth = number;
            paint.flags |= MermaidPaintFlag_StrokeWidth;
        } else if (prAsciiEqualsFold(name, "font-weight")) {
            paint.fontWeight = 400;
            if (prAsciiEqualsFold(value, "bold") || prAsciiEqualsFold(value, "bolder"))
                paint.fontWeight = 700;
            else if (parseNumber(value, &number))
                paint.fontWeight = number;
            paint.flags |= MermaidPaintFlag_FontWeight;
        } else if (prAsciiEqualsFold(name, "font-style")) {
            paint.italic = prAsciiEqualsFold(value, "italic") || prAsciiEqualsFold(value, "oblique");
            paint.flags |= MermaidPaintFlag_FontStyle;
        } else if (prAsciiEqualsFold(name, "font-size") && parseNumber(value, &number)) {
            paint.fontSize = number;
            paint.flags |= MermaidPaintFlag_FontSize;
        } else if ((prAsciiEqualsFold(name, "opacity") || prAsciiEqualsFold(name, "fill-opacity")) &&
                parseNumber(value, &number)) {
            paint.opacity = number;
            paint.flags |= MermaidPaintFlag_Opacity;
        } else if (prAsciiEqualsFold(name, "stroke-dasharray")) {
            float dash[2] = {0, 0};
            size_t at = 0;
            uint32_t found = 0;
            while (found < 2 && at < value.size) {
                while (at < value.size && (prAsciiIsSpace(value.data[at]) || value.data[at] == ','))
                    ++at;
                double parsed = 0;
                size_t taken = mermaidNumberParse(PrStringView{value.data + at, value.size - at}, &parsed);
                if (taken == 0)
                    break;
                dash[found++] = float(parsed);
                at += taken;
                while (at < value.size && prAsciiIsLetter(value.data[at]))
                    ++at;
            }
            if (found > 0) {
                paint.dash[0] = dash[0];
                paint.dash[1] = found > 1 ? dash[1] : dash[0];
                paint.flags |= MermaidPaintFlag_Dash;
            }
        }
    }
}

void mermaidPaintMerge(MermaidPaint& into, const MermaidPaint& from) {
    if ((from.flags & MermaidPaintFlag_Fill) != 0)
        into.fill = from.fill;
    if ((from.flags & MermaidPaintFlag_Stroke) != 0)
        into.stroke = from.stroke;
    if ((from.flags & MermaidPaintFlag_Color) != 0)
        into.color = from.color;
    if ((from.flags & MermaidPaintFlag_StrokeWidth) != 0)
        into.strokeWidth = from.strokeWidth;
    if ((from.flags & MermaidPaintFlag_Dash) != 0) {
        into.dash[0] = from.dash[0];
        into.dash[1] = from.dash[1];
    }
    if ((from.flags & MermaidPaintFlag_FontWeight) != 0)
        into.fontWeight = from.fontWeight;
    if ((from.flags & MermaidPaintFlag_FontStyle) != 0)
        into.italic = from.italic;
    if ((from.flags & MermaidPaintFlag_FontSize) != 0)
        into.fontSize = from.fontSize;
    if ((from.flags & MermaidPaintFlag_Opacity) != 0)
        into.opacity = from.opacity;
    into.flags |= from.flags;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

void mermaidSceneDraw(const MermaidScene& scene, PrDrawList* list, PrColorScheme scheme) {
    for (uint32_t i = 0; i < scene.commands.count; ++i) {
        const MermaidCommand& c = scene.commands.data[i];
        switch (c.kind) {
            case MermaidCommandKind_Path: {
                if (c.filled) {
                    PrColor fill = prColorFromColor8(mermaidInkColor(c.fill, scheme));
                    fill.a *= c.opacity;
                    prDrawListPath(list, c.path, PrPathFillRule_NonZero,
                        PrDrawPaint{.color = prColor16FromColor(fill)});
                }
                if (c.stroked) {
                    PrDrawStroke stroke = prDrawStrokeDefault();
                    stroke.shape.width = c.strokeWidth;
                    stroke.shape.join = PrPathJoin_Round;
                    if (c.dashed) {
                        stroke.shape.dashes = c.dash;
                        stroke.shape.dashCount = 2;
                    }
                    PrColor16 color = prColor16FromColor(prColorFromColor8(mermaidInkColor(c.stroke, scheme)));
                    prDrawListPath(list, c.path, PrPathFillRule_NonZero, PrDrawPaint{.color = color, .stroke = &stroke});
                }
                break;
            }
            case MermaidCommandKind_Glyphs: {
                const MermaidGlyphRun& r = scene.runs.data[c.run];
                PrFontGlyphRun run{
                    .font = r.font,
                    .size = r.size,
                    .glyphs = scene.glyphs.data + r.start,
                    .glyphCount = r.count,
                    .transforms = r.scaled ? scene.transforms.data + r.start : nullptr,
                };
                PrColor16 color = prColor16FromColor(prColorFromColor8(mermaidInkColor(c.fill, scheme)));
                prDrawListGlyphs(list, &run, r.origin, PrDrawPaint{.color = color});
                break;
            }
            case MermaidCommandKind_PushTransform:
                prDrawListPushTransformAffine2(list, c.transform);
                break;
            case MermaidCommandKind_PopTransform:
                prDrawListPopTransform(list);
                break;
            default:
                break;
        }
    }
}

void mermaidSceneReset(MermaidScene& scene) {
    for (uint32_t i = 0; i < scene.commands.count; ++i)
        prPathDestroy(scene.commands.data[i].path);
    for (uint32_t i = 0; i < scene.runs.count; ++i)
        prFontRelease(scene.runs.data[i].font);
    scene.commands.reset();
    scene.runs.reset();
    scene.glyphs.reset();
    scene.transforms.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Renders

void mermaidRenderInit(MermaidRender& render, const PrMermaidDesc& desc, const PrMermaidOptions& options) {
    render.fonts = desc.fonts;
    render.shaper = desc.shaper;
    render.options = options;
}

void mermaidRenderReset(MermaidRender& render) {
    for (uint32_t i = 0; i < render.fontCache.count; ++i)
        prTextFontRelease(render.fontCache.data[i].font);
    render.fontCache.reset();
    for (uint32_t i = 0; i < render.labels.count; ++i)
        prTextLayoutDestroy(render.labels.data[i].layout);
    render.labels.reset();
    render.labelSpans.reset();
    render.labelText.reset();
    mermaidSceneReset(render.scene);
    render.items.reset();
    render.itemText.reset();
    render.itemPoints.reset();
}

PrTextFont* mermaidFont(MermaidRender& render, const MermaidTextStyle& style) {
    for (uint32_t i = 0; i < render.fontCache.count; ++i) {
        const MermaidFont& f = render.fontCache.data[i];
        if (f.size == style.size && f.weight == style.weight && f.italic == style.italic &&
                f.monospace == style.monospace)
            return f.font;
    }
    // Mermaid's font list, after the monospace family for code, so that code falls back to it as other text does.
    PrFontFamily families[] = {
        prFontFamilyGeneric(PrFontGeneric_Monospace),
        prFontFamilyFromName("trebuchet ms"),
        prFontFamilyFromName("verdana"),
        prFontFamilyFromName("arial"),
        prFontFamilyGeneric(PrFontGeneric_SansSerif),
    };
    PrFontStyle fontStyle = prFontStyleDefault();
    fontStyle.size = style.size;
    fontStyle.weight = style.weight;
    if (style.italic)
        fontStyle.slope = PrFontSlope_Italic;
    PrTextFontDesc desc{
        .set = render.fonts,
        .families = style.monospace ? families : families + 1,
        .familyCount = style.monospace ? 5u : 4u,
        .style = fontStyle,
    };
    PrTextFont* font = prTextFontCreate(&desc);
    render.fontCache.push(MermaidFont{style.size, style.weight, style.italic, style.monospace, font});
    return font;
}

MermaidLabelIndex mermaidLabelCreate(MermaidRender& render, PrStringView text, MermaidTextFormat format,
        const MermaidTextStyle& style) {
    Decoded d{};
    decode(d, text, format);
    // The text without the line breaks and spaces that surround it.
    uint32_t start = 0;
    uint32_t end = d.text.count;
    while (start < end && prAsciiIsSpace(d.text.data[start]))
        ++start;
    while (end > start && prAsciiIsSpace(d.text.data[end - 1]))
        --end;
    MermaidLabel label{.spanStart = render.labelSpans.count};
    if (end > start) {
        RawArray<PrTextLayoutSpan> spans{};
        for (uint32_t i = 0; i < d.ranges.count; ++i) {
            // A range clipped to the text without its surrounding space, which may leave nothing of it.
            const TextRange& range = d.ranges.data[i];
            uint32_t clippedStart = range.start > start ? range.start : start;
            uint32_t clippedEnd = range.end < end ? range.end : end;
            if (clippedEnd <= clippedStart)
                continue;
            uint32_t from = clippedStart - start;
            uint32_t to = clippedEnd - start;
            if (range.bold || range.italic || range.monospace) {
                MermaidTextStyle spanStyle = style;
                spanStyle.weight = range.bold ? 700 : style.weight;
                spanStyle.italic = style.italic || range.italic;
                spanStyle.monospace = style.monospace || range.monospace;
                spans.push(PrTextLayoutSpan{from, to, mermaidFont(render, spanStyle)});
            }
            if (range.underline || range.strike || range.hasColor) {
                render.labelSpans.push(MermaidLabelSpan{
                    from, to, range.underline, range.strike, range.hasColor, range.color,
                });
            }
        }
        label.text = MermaidSpan{render.labelText.count, end - start};
        render.labelText.append(d.text.data + start, end - start);
        label.layout = prTextLayoutCreate();
        PrTextLayoutDesc desc{
            .text = PrStringView{d.text.data + start, end - start},
            .font = mermaidFont(render, style),
            .spans = spans.data,
            .spanCount = spans.count,
            .shaper = render.shaper,
            .align = style.align,
            .width = style.wrapWidth,
            .lineHeight = style.lineHeight,
        };
        prTextLayoutSet(label.layout, &desc);
        // A wrapped label is as wide as its widest line, not as the width that it wraps in.
        if (style.wrapWidth > 0) {
            float widest = 0;
            for (uint32_t i = 0; i < prTextLayoutLineCount(label.layout); ++i) {
                float width = prTextLayoutLine(label.layout, i).width;
                widest = width > widest ? width : widest;
            }
            if (widest < style.wrapWidth) {
                desc.width = widest + kPrTextLineFitTolerance;
                prTextLayoutSet(label.layout, &desc);
            }
        }
        label.extent = prTextLayoutExtent(label.layout);
        spans.reset();
    }
    label.spanCount = render.labelSpans.count - label.spanStart;
    resetDecoded(d);
    render.labels.push(label);
    return render.labels.count - 1;
}

PrVec2 mermaidLabelExtent(const MermaidRender& render, MermaidLabelIndex label) {
    if (label == kMermaidNone)
        return PrVec2{0, 0};
    return render.labels.data[label].extent;
}

uint32_t mermaidLabelLineCount(const MermaidRender& render, MermaidLabelIndex label) {
    if (label == kMermaidNone)
        return 1;
    const PrTextLayout* layout = render.labels.data[label].layout;
    return layout != nullptr ? prTextLayoutLineCount(layout) : 1;
}

void mermaidLabelDraw(MermaidRender& render, MermaidLabelIndex label, PrVec2 origin, MermaidInk ink) {
    if (label == kMermaidNone)
        return;
    const MermaidLabel& placed = render.labels.data[label];
    if (placed.layout == nullptr)
        return;
    MermaidScene& scene = render.scene;
    const MermaidLabelSpan* spans = render.labelSpans.data + placed.spanStart;
    for (uint32_t i = 0; i < prTextLayoutRunCount(placed.layout); ++i) {
        PrFontGlyphRun run = prTextLayoutRun(placed.layout, i);
        uint32_t byte = prTextLayoutRunStart(placed.layout, i);
        MermaidInk runInk = ink;
        for (uint32_t k = 0; k < placed.spanCount; ++k) {
            if (spans[k].hasColor && byte >= spans[k].start && byte < spans[k].end)
                runInk = mermaidInkOfColor(spans[k].color);
        }
        prFontRetain(run.font);
        scene.runs.push(MermaidGlyphRun{
            .font = run.font,
            .size = run.size,
            .start = scene.glyphs.count,
            .count = run.glyphCount,
            .scaled = run.transforms != nullptr,
            .origin = origin,
        });
        scene.glyphs.append(run.glyphs, run.glyphCount);
        // The transforms stay parallel to the glyphs, with zeros for a run without its own.
        uint32_t transformStart = scene.transforms.count;
        scene.transforms.resize(scene.glyphs.count);
        if (run.transforms != nullptr)
            prMemCopy(scene.transforms.data + transformStart, run.transforms, sizeof(PrAffine2) * run.glyphCount);
        else
            prMemZero(scene.transforms.data + transformStart, sizeof(PrAffine2) * run.glyphCount);
        scene.commands.push(MermaidCommand{
            .kind = MermaidCommandKind_Glyphs,
            .fill = runInk,
            .run = scene.runs.count - 1,
        });
    }
    // Underlines and strikes run along each line that their spans cover, from caret to caret.
    for (uint32_t k = 0; k < placed.spanCount; ++k) {
        const MermaidLabelSpan& span = spans[k];
        if (!span.underline && !span.strike)
            continue;
        MermaidInk lineInk = span.hasColor ? mermaidInkOfColor(span.color) : ink;
        for (uint32_t i = 0; i < prTextLayoutLineCount(placed.layout); ++i) {
            PrTextLine line = prTextLayoutLine(placed.layout, i);
            // The line's content, without the white space and line feed that end it, which a decoration stops short of.
            uint32_t content = line.end;
            while (content > line.start && prAsciiIsSpace(render.labelText.data[placed.text.start + content - 1]))
                --content;
            uint32_t from = span.start > line.start ? span.start : line.start;
            uint32_t to = span.end < content ? span.end : content;
            if (to <= from)
                continue;
            PrVec2 a = prTextLayoutCaret(placed.layout, from);
            PrVec2 b = to == content ? PrVec2{line.left + line.width, a.y} : prTextLayoutCaret(placed.layout, to);
            float size = line.height * 0.8f;
            for (int pass = 0; pass < 2; ++pass) {
                bool under = pass == 0;
                if (under ? !span.underline : !span.strike)
                    continue;
                float y = under ? line.baseline + size * 0.1f : line.baseline - size * 0.3f;
                mermaidDrawLine(render, origin + PrVec2{a.x, y}, origin + PrVec2{b.x, y}, lineInk, 1, nullptr);
            }
        }
    }
}

void mermaidLabelDrawCentered(MermaidRender& render, MermaidLabelIndex label, PrVec2 center, MermaidInk ink) {
    PrVec2 extent = mermaidLabelExtent(render, label);
    mermaidLabelDraw(render, label, center - extent * 0.5f, ink);
}

void mermaidTextDecode(PrStringView text, MermaidTextFormat format, RawArray<char>& out) {
    Decoded d{};
    decode(d, text, format);
    out.append(d.text.data, d.text.count);
    resetDecoded(d);
}

void mermaidDrawPath(MermaidRender& render, const PrPath* path, const MermaidPaint& paint) {
    bool filled = (paint.flags & MermaidPaintFlag_Fill) != 0;
    bool stroked = (paint.flags & MermaidPaintFlag_Stroke) != 0;
    float width = (paint.flags & MermaidPaintFlag_StrokeWidth) != 0 ? paint.strokeWidth : 1;
    stroked = stroked && width > 0;
    if (!filled && !stroked)
        return;
    bool dashed = (paint.flags & MermaidPaintFlag_Dash) != 0 && paint.dash[0] + paint.dash[1] > 0;
    render.scene.commands.push(MermaidCommand{
        .kind = MermaidCommandKind_Path,
        .filled = filled,
        .stroked = stroked,
        .dashed = dashed,
        .fill = paint.fill,
        .stroke = paint.stroke,
        .strokeWidth = width,
        .dash = {paint.dash[0], paint.dash[1]},
        .opacity = (paint.flags & MermaidPaintFlag_Opacity) != 0 ? paint.opacity : 1,
        .path = prPathClone(path),
    });
}

void mermaidStrokePath(MermaidRender& render, const PrPath* path, MermaidInk ink, float width, const float* PR_NULLABLE dash) {
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .stroke = ink,
        .strokeWidth = width,
    };
    if (dash != nullptr) {
        paint.flags |= MermaidPaintFlag_Dash;
        paint.dash[0] = dash[0];
        paint.dash[1] = dash[1];
    }
    mermaidDrawPath(render, path, paint);
}

void mermaidFillPath(MermaidRender& render, const PrPath* path, MermaidInk ink) {
    mermaidDrawPath(render, path, MermaidPaint{.flags = MermaidPaintFlag_Fill, .fill = ink});
}

void mermaidFillRect(MermaidRender& render, PrRect rect, MermaidInk ink) {
    PrPath* path = prPathCreate();
    prPathAddRect(path, rect);
    mermaidFillPath(render, path, ink);
    prPathDestroy(path);
}

void mermaidDrawLine(MermaidRender& render, PrVec2 from, PrVec2 to, MermaidInk ink, float width,
        const float* PR_NULLABLE dash) {
    PrPath* path = prPathCreate();
    prPathMoveTo(path, from);
    prPathLineTo(path, to);
    mermaidStrokePath(render, path, ink, width, dash);
    prPathDestroy(path);
}

float mermaidDrawMarker(MermaidRender& render, MermaidMarker marker, PrVec2 tip, PrVec2 direction, MermaidInk ink, float size) {
    if (marker == MermaidMarker_None)
        return 0;
    MermaidInk background = mermaidInk(MermaidRole_Background);
    PrVec2 across{-direction.y, direction.x};
    PrPath* path = prPathCreate();
    float back = 0;
    MermaidPaint paint{.flags = MermaidPaintFlag_Fill, .fill = ink, .stroke = ink};
    MermaidPaint hollow{.flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke, .fill = background, .stroke = ink};
    switch (marker) {
        case MermaidMarker_Arrow:
        case MermaidMarker_Triangle: {
            PrVec2 base = tip - direction * size;
            prPathMoveTo(path, tip);
            prPathLineTo(path, base + across * (size * 0.5f));
            prPathLineTo(path, base - across * (size * 0.5f));
            prPathClose(path);
            if (marker == MermaidMarker_Triangle)
                paint = hollow;
            back = marker == MermaidMarker_Arrow ? size - 1 : size;
            break;
        }
        case MermaidMarker_OpenArrow: {
            PrVec2 base = tip - direction * size;
            prPathMoveTo(path, base + across * (size * 0.5f));
            prPathLineTo(path, tip);
            prPathLineTo(path, base - across * (size * 0.5f));
            paint = MermaidPaint{.flags = MermaidPaintFlag_Stroke, .stroke = ink};
            break;
        }
        case MermaidMarker_Circle:
        case MermaidMarker_Lollipop: {
            float radius = size * 0.5f;
            prPathAddEllipse(path, tip - direction * radius, PrVec2{radius, radius}, 0);
            if (marker == MermaidMarker_Lollipop)
                paint = hollow;
            back = size;
            break;
        }
        case MermaidMarker_Cross: {
            float half = size * 0.5f;
            PrVec2 center = tip - direction * half;
            PrVec2 a = (direction + across) * (half * 0.7071f);
            PrVec2 b = (direction - across) * (half * 0.7071f);
            prPathMoveTo(path, center - a);
            prPathLineTo(path, center + a);
            prPathMoveTo(path, center - b);
            prPathLineTo(path, center + b);
            paint = MermaidPaint{
                .flags = MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
                .stroke = ink,
                .strokeWidth = 1.5f,
            };
            back = half;
            break;
        }
        case MermaidMarker_Diamond:
        case MermaidMarker_OpenDiamond: {
            PrVec2 middle = tip - direction * size;
            prPathMoveTo(path, tip);
            prPathLineTo(path, middle + across * (size * 0.4f));
            prPathLineTo(path, tip - direction * (size * 2));
            prPathLineTo(path, middle - across * (size * 0.4f));
            prPathClose(path);
            paint = hollow;
            if (marker == MermaidMarker_Diamond)
                paint.fill = ink;
            back = size * 2;
            break;
        }
        default:
            break;
    }
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);
    return back;
}

void mermaidPushTransform(MermaidRender& render, PrAffine2 transform) {
    render.scene.commands.push(MermaidCommand{.kind = MermaidCommandKind_PushTransform, .transform = transform});
}

void mermaidPopTransform(MermaidRender& render) {
    render.scene.commands.push(MermaidCommand{.kind = MermaidCommandKind_PopTransform});
}

void mermaidItemAdd(MermaidRender& render, PrMermaidItemKind kind, PrRect rect, PrStringView id, PrStringView link) {
    MermaidItemRecord item{.kind = kind, .rect = rect};
    item.id = MermaidSpan{render.itemText.count, uint32_t(id.size)};
    render.itemText.append(id.data, uint32_t(id.size));
    item.link = MermaidSpan{render.itemText.count, uint32_t(link.size)};
    render.itemText.append(link.data, uint32_t(link.size));
    render.items.push(item);
}

void mermaidItemAddEdge(MermaidRender& render, const PrVec2* points, uint32_t count, PrStringView id) {
    if (count == 0)
        return;
    PrRect bounds{points[0].x, points[0].y, points[0].x, points[0].y};
    for (uint32_t i = 1; i < count; ++i) {
        bounds.left = prF32Min(bounds.left, points[i].x);
        bounds.top = prF32Min(bounds.top, points[i].y);
        bounds.right = prF32Max(bounds.right, points[i].x);
        bounds.bottom = prF32Max(bounds.bottom, points[i].y);
    }
    mermaidItemAdd(render, PrMermaidItemKind_Edge, bounds, id, PrStringView{});
    MermaidItemRecord& item = render.items.last();
    item.pointStart = render.itemPoints.count;
    item.pointCount = count;
    render.itemPoints.append(points, count);
}

void mermaidItemAddPath(MermaidRender& render, const PrPath* path, PrStringView id) {
    PrPathPolyline polyline{};
    prPathFlatten(path, prAffine2Identity(), 0.25f, PrPathFlattening_Inside, &polyline);
    mermaidItemAddEdge(render, polyline.points, polyline.pointCount, id);
    prPathPolylineReset(&polyline);
}

PrVec2 mermaidRouteEndDirection(const PrVec2* points, uint32_t count, bool atStart) {
    PrVec2 end = points[atStart ? 0 : count - 1];
    for (uint32_t k = 1; k < count; ++k) {
        PrVec2 direction = end - points[atStart ? k : count - 1 - k];
        float length = prF32Sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length > 0.01f)
            return direction * (1 / length);
    }
    return PrVec2{0, 1};
}

void mermaidRouteMoveEnd(PrVec2* points, uint32_t count, bool atStart, PrVec2 delta) {
    uint32_t end = atStart ? 0 : count - 1;
    uint32_t beside = atStart ? 1 : count - 2;
    points[end] = points[end] + delta;
    if (count >= 4)
        points[beside] = points[beside] + delta;
}

void mermaidRouteAppend(PrPath* path, const PrVec2* points, uint32_t count) {
    prPathMoveTo(path, points[0]);
    for (uint32_t i = 1; i + 2 < count; i += 3) {
        PrVec2 from = points[i - 1];
        PrVec2 to = points[i + 2];
        PrVec2 chord = to - from;
        PrVec2 a = points[i] - from;
        PrVec2 b = points[i + 1] - from;
        float scale = chord.x * chord.x + chord.y * chord.y;
        bool straight = prF32Abs(chord.x * a.y - chord.y * a.x) <= 1e-3f * scale &&
            prF32Abs(chord.x * b.y - chord.y * b.x) <= 1e-3f * scale;
        if (straight)
            prPathLineTo(path, to);
        else
            prPathCubicTo(path, points[i], points[i + 1], to);
    }
}

PrGraphEdge mermaidGraphAddEdge(PrGraph* graph, PrGraphNode source, PrGraphNode target, MermaidGraphPortFlags ports) {
    PrGraphEdgeDesc desc = prGraphEdgeDescDefault(source, target);
    if (source != target && (ports & MermaidGraphPortFlag_Source) != 0) {
        PrGraphPortDesc port{.node = source, .constraint = PrGraphPortConstraint_Free};
        desc.sourcePort = prGraphAddPort(graph, &port);
    }
    if (source != target && (ports & MermaidGraphPortFlag_Target) != 0) {
        PrGraphPortDesc port{.node = target, .constraint = PrGraphPortConstraint_Free};
        desc.targetPort = prGraphAddPort(graph, &port);
    }
    return prGraphAddEdge(graph, &desc);
}

PrVec2 mermaidGraphPlace(MermaidRender& render, const PrGraph* graph, PrStringView title,
        const MermaidTextStyle& titleStyle, float titleGap, MermaidInk titleInk) {
    constexpr float kMargin = 8;
    MermaidLabelIndex label = mermaidLabelCreate(render, title, MermaidTextFormat_Plain, titleStyle);
    PrVec2 titleExtent = mermaidLabelExtent(render, label);
    float titleSpace = titleExtent.y > 0 ? titleExtent.y + titleGap : 0;
    PrRect bounds = prGraphBounds(graph);
    float width = prF32Max(bounds.right, titleExtent.x);
    render.extent = PrVec2{width + 2 * kMargin, bounds.bottom + titleSpace + 2 * kMargin};
    mermaidLabelDraw(render, label, PrVec2{(render.extent.x - titleExtent.x) * 0.5f, kMargin}, titleInk);
    return PrVec2{kMargin + (width - bounds.right) * 0.5f, kMargin + titleSpace};
}

bool mermaidGraphRouteAppend(const PrGraph* graph, PrGraphEdge edge, PrVec2 offset, RawArray<PrVec2>& points) {
    PrGraphRoute route = prGraphEdgeRoute(graph, edge);
    for (uint32_t i = 0; i < route.pointCount; ++i)
        points.push(route.points[i] + offset);
    return route.pointCount >= 2;
}

void mermaidGraphLabelDraw(MermaidRender& render, const PrGraph* graph, PrGraphLabel place, PrVec2 offset,
        MermaidLabelIndex label, MermaidInk ink, const MermaidInk* PR_NULLABLE background) {
    PrRect rect = prRectOffset(prGraphLabelRect(graph, place), offset);
    if (background != nullptr)
        mermaidFillRect(render, rect, *background);
    mermaidLabelDrawCentered(render, label, prRectCenter(rect), ink);
    mermaidItemAdd(render, PrMermaidItemKind_Label, rect, PrStringView{}, PrStringView{});
}

} // namespace pr_impl
