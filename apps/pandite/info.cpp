// The facts of an image file that the information panel shows.

#include "apps/pandite/info.h"

#include "pr/ascii.h"
#include "pr/blob_ref.h"
#include "pr/color_profile.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/date_time.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/format.h"
#include "pr/mem.h"
#include "pr/mime_sniff.h"
#include "pr/string_ops.h"

namespace pandite {

namespace {

// The names of formats as people write them, keyed by the short ids that sniffing gives. A format that is not listed
// shows as its id in capitals.
struct FormatName {
    const char* id;
    const char* name;
};
constexpr FormatName kFormatNames[] = {
    {"jpeg", "JPEG"},
    {"webp", "WebP"},
    {"exr", "OpenEXR"},
    {"hdr", "Radiance HDR"},
    {"ico", "Windows icon"},
};

// The names of the color spaces that images name, as people write them.
struct SpaceName {
    PrColorSpace space;
    const char* name;
};
constexpr SpaceName kSpaceNames[] = {
    {PrColorSpace_Srgb, "sRGB"},
    {PrColorSpace_SrgbLinear, "Linear sRGB"},
    {PrColorSpace_DisplayP3, "Display P3"},
    {PrColorSpace_DisplayP3Linear, "Linear Display P3"},
    {PrColorSpace_A98Rgb, "Adobe RGB (1998)"},
    {PrColorSpace_ProPhotoRgb, "ProPhoto RGB"},
    {PrColorSpace_Rec2020, "Rec. 2020"},
    {PrColorSpace_Rec2100Pq, "Rec. 2100 PQ"},
    {PrColorSpace_Rec2100Hlg, "Rec. 2100 HLG"},
};

// Returns the name of the format whose short id is `id`.
String formatName(PrStringView id) {
    for (const FormatName& f : kFormatNames) {
        if (id == PrStringView(f.id))
            return String(f.name);
    }
    char upper[32];
    size_t size = id.size < sizeof upper ? id.size : sizeof upper;
    prMemCopy(upper, id.data, size);
    prAsciiUpperText(upper, size);
    return String(PrStringView{upper, size});
}

// Returns the name that the panel shows for the color of `color`, or the null string for uncharacterized color.
String colorName(const PrImageColor& color) {
    if (color.encoding == PrImageColorEncoding_Named) {
        for (const SpaceName& s : kSpaceNames) {
            if (s.space == color.space)
                return String(s.name);
        }
        return String(prColorSpaceName(color.space));
    }
    if (color.encoding == PrImageColorEncoding_Icc) {
        char description[96];
        size_t size = prColorProfileDescription(color.profile, description, sizeof description);
        return size != 0 ? String(PrStringView{description, size}) : String("ICC profile");
    }
    if (color.encoding == PrImageColorEncoding_Chromaticities)
        return String("Custom primaries");
    return String();
}

// Returns the name that the panel shows for the channels of `format`, and writes the number of channels to `*count`.
PrStringView channelsOf(const PrImageFormat& format, uint32_t* count) {
    bool alpha = format.alpha != PrImageAlpha_None;
    switch (format.model) {
        case PrColorModel_Gray: *count = alpha ? 2 : 1; return alpha ? "Gray and alpha" : "Gray";
        case PrColorModel_Rgb: *count = alpha ? 4 : 3; return alpha ? "RGBA" : "RGB";
        case PrColorModel_Cmyk: *count = alpha ? 5 : 4; return alpha ? "CMYK and alpha" : "CMYK";
        default: *count = 0; return "";
    }
}

// Returns the number of bits in a channel of `sample`.
uint32_t bitsOf(PrImageSample sample) {
    switch (sample) {
        case PrImageSample_U8: return 8;
        case PrImageSample_U16:
        case PrImageSample_I16:
        case PrImageSample_F16: return 16;
        default: return 32;
    }
}

// Returns `value` with one decimal, without a trailing ".0".
String decimal(double value) {
    String text = format("{:.1f}", value);
    if (prStringEndsWith(text, ".0"))
        text = String(prStringHead(text, text.count() - 2));
    return text;
}

// Returns `n` with a comma between each group of three digits.
String grouped(uint64_t n) {
    String digits = format("{}", n);
    String text;
    uint32_t count = digits.count();
    for (uint32_t i = 0; i < count; ++i) {
        if (i > 0 && (count - i) % 3 == 0)
            text.append(",");
        text.append(prStringSlice(digits, i, 1));
    }
    return text;
}

uint32_t gcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

// The rows under construction. A row with an empty value is dropped.
struct Rows {
    Array<InfoRow>* out;

    void push(const char* label, String value) {
        if (!value.isEmpty())
            out->push(InfoRow{.label = String(label), .value = move(value)});
    }
};

} // namespace

void factsRead(const PrImageCodecSet* codecs, PrStringView path, ImageFacts* out) {
    *out = ImageFacts{};
    out->name = String(prFilePathName(path));
    out->folder = String(prFilePathParent(path));
    PrFileInfo file{};
    if (prFileInfo(path, &file)) {
        out->known = true;
        out->bytes = file.size;
        out->modified = file.modified;
    }
    BlobRef blob = BlobRef::adopt(prFileOpen(path));
    if (blob.get() == nullptr)
        return;
    PrByteView bytes = blob.bytes();
    PrByteView head{bytes.data, bytes.size < kPrMimeSniffBytes ? bytes.size : size_t(kPrMimeSniffBytes)};
    PrMimeFormat sniffed = prMimeSniff(head, out->name);
    if (sniffed != PrMimeFormat_None)
        out->format = formatName(prMimeFormatInfo(sniffed).name);
    PrImageDecoder* decoder = prImageDecoderCreate(codecs);
    PrImageSource source{};
    source.memory = bytes;
    PrStatus status = prImageDecoderOpen(decoder, source, nullptr);
    if (status == PrStatus_Ok || status == PrStatus_Truncated) {
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        out->opened = true;
        bool swaps = prImageOrientationSwapsAxes(info.orientation);
        out->extent = swaps ? PrUVec2{info.extent.y, info.extent.x} : info.extent;
        out->pixels = info.format;
        out->color = colorName(info.format.color);
        out->frames = info.frameCount;
        out->playCount = info.playCount;
        for (uint32_t frame = 0; frame < info.frameCount && info.frameCount > 1; ++frame) {
            PrImageFrameInfo timing{};
            if (prImageDecoderFrameInfo(decoder, frame, &timing) == PrStatus_Ok)
                out->durationMs += timing.durationMs;
        }
        prImageExifCapture(info.exif, &out->capture);
    }
    prImageDecoderDestroy(decoder);
}

void factsRows(const ImageFacts& facts, Array<InfoRow>* out) {
    Rows rows{out};
    rows.push("File", facts.name);
    rows.push("Folder", facts.folder);
    if (facts.known) {
        uint64_t n = facts.bytes;
        rows.push("File size", n < 1024 ? infoBytes(n) : format("{} ({} bytes)", infoBytes(n), grouped(n)));
        PrDateTime t = prDateTimeFromSystem(facts.modified, 0);
        if (t.year >= 1970) {
            rows.push("Modified", format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", t.year, t.month, t.day, t.hour,
                t.minute, t.second));
        }
    }
    if (!facts.opened)
        return;
    PrUVec2 e = facts.extent;
    bool sized = e.x > 0 && e.y > 0;
    if (sized) {
        rows.push("Pixels", format("{} x {}", e.x, e.y));
        rows.push("Megapixels", infoMegapixels(e));
        rows.push("Aspect ratio", infoAspect(e));
    }
    rows.push("Format", facts.format);
    uint32_t channels = 0;
    PrStringView named = channelsOf(facts.pixels, &channels);
    uint32_t bits = bitsOf(facts.pixels.sample);
    if (channels != 0) {
        rows.push("Channels", format("{} ({})", named, channels));
        rows.push("Bits per channel", format("{}", bits));
        rows.push("Bits per pixel", format("{}", bits * channels));
    }
    rows.push("Color space", facts.color);
    uint32_t frames = facts.frames > 1 ? facts.frames : 1;
    if (sized) {
        double area = double(e.x) * double(e.y) * frames;
        if (channels != 0) {
            double raw = prF64Ceil(area * bits * channels / 8);
            rows.push("Uncompressed", infoBytes(uint64_t(raw)));
            if (facts.known && facts.bytes > 0)
                rows.push("Compression ratio", format("{}:1", decimal(raw / double(facts.bytes))));
        }
        if (facts.known)
            rows.push("Bits per pixel on disk", format("{:.2f}", double(facts.bytes) * 8 / area));
    }
    if (facts.frames > 1) {
        rows.push("Frames", format("{}", facts.frames));
        if (facts.durationMs > 0) {
            double seconds = facts.durationMs / 1000;
            rows.push("Duration", format("{:.2f} s", seconds));
            rows.push("Frame rate", format("{:.1f} fps", facts.frames / seconds));
        }
        if (facts.playCount == 0)
            rows.push("Loop count", String("Forever"));
        else
            rows.push("Loop count", facts.playCount == 1 ? String("1 play") : format("{} plays", facts.playCount));
    }

    // The camera's maker and model. Many models begin with the maker's name, and such a model is shown alone.
    const PrImageCapture& c = facts.capture;
    PrStringView make(c.make);
    PrStringView model(c.model);
    String camera;
    if (!make.isEmpty() && !model.isEmpty()) {
        bool leads = model.size >= make.size && prAsciiEqualsFold(prStringHead(model, make.size), make);
        camera = leads ? String(model) : format("{} {}", make, model);
    } else {
        camera = String(make.isEmpty() ? model : make);
    }
    rows.push("Camera", camera);
    rows.push("Lens", String(PrStringView(c.lens)));
    rows.push("Exposure", infoExposure(c.exposure[0], c.exposure[1]));
    if (c.fNumber > 0)
        rows.push("Aperture", format("f/{}", decimal(double(c.fNumber))));
    if (c.iso != 0)
        rows.push("ISO", format("{}", c.iso));
    if (c.focalLength > 0)
        rows.push("Focal length", format("{} mm", decimal(double(c.focalLength))));
    rows.push("Taken", String(PrStringView(c.taken)));
}

String infoBytes(uint64_t bytes) {
    if (bytes < 1024)
        return format("{} bytes", bytes);
    const char* const units[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = double(bytes) / 1024;
    uint32_t unit = 0;
    while (value >= 1024 && unit + 1 < countOf(units)) {
        value /= 1024;
        ++unit;
    }
    return value >= 100 ? format("{:.0f} {}", value, units[unit]) : format("{:.1f} {}", value, units[unit]);
}

String infoMegapixels(PrUVec2 extent) {
    double megapixels = double(extent.x) * double(extent.y) / 1000000;
    if (megapixels >= 1)
        return format("{:.1f} MP", megapixels);
    if (megapixels >= 0.01)
        return format("{:.2f} MP", megapixels);
    return String("less than 0.01 MP");
}

String infoAspect(PrUVec2 extent) {
    uint32_t g = gcd(extent.x, extent.y);
    g = g > 0 ? g : 1;
    uint32_t a = extent.x / g;
    uint32_t b = extent.y / g;
    double ratio = double(extent.x) / double(extent.y);
    if (a <= 32 && b <= 32)
        return b == 1 ? format("{}:1", a) : format("{}:{} ({:.2f}:1)", a, b, ratio);
    return format("{:.2f}:1", ratio);
}

String infoExposure(uint32_t numerator, uint32_t denominator) {
    if (numerator == 0 || denominator == 0)
        return String();
    uint32_t g = gcd(numerator, denominator);
    uint32_t n = numerator / g;
    uint32_t d = denominator / g;
    if (n >= d)
        return format("{} s", decimal(double(n) / double(d)));
    if (n == 1)
        return format("1/{} s", d);
    return format("1/{:.0f} s", prF64Round(double(d) / double(n)));
}

} // namespace pandite
