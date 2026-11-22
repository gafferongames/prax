// The facts of an image file that the information panel shows, and the rows that it shows them in.
//
// The facts come from the file system and the header of the image. No pixel is decoded. A fact that is not known has no
// row, and no row has an empty value.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/image.h"
#include "pr/image_exif.h"
#include "pr/string.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The facts of an image file. The file system gives its name and folder, its size, and the time when it last changed.
// The header, once it opens, gives the rest: the extent of the image as it displays, its format and the meaning of its
// pixels, its color space or profile, its frames and how long they play, and how the photograph was taken. `opened` is
// false for a file that does not open as an image.
struct ImageFacts {
    String name;
    String folder;
    bool known;
    uint64_t bytes;
    PrTimeSystem modified;

    bool opened;
    PrUVec2 extent;
    String format;
    PrImageFormat pixels;
    String color;
    uint32_t frames;
    uint32_t playCount;
    double durationMs;
    PrImageCapture capture;
};

// Reads the facts of the image file at `path` with `codecs` and writes them to `*out`. Runs on any thread. Blocks on
// the disk.
void factsRead(const PrImageCodecSet* codecs, PrStringView path, ImageFacts* out);

// A row of the information panel.
struct InfoRow {
    String label;
    String value;
};

// Writes the rows of `facts` to `*out`, in the order that the panel shows them.
void factsRows(const ImageFacts& facts, Array<InfoRow>* out);

// Returns a byte count as text, in the largest unit that keeps the number at 1 or more. A unit is 1024 of the unit
// below it. Examples: "512 bytes", "2.4 MB".
String infoBytes(uint64_t bytes);

// Returns a count of megapixels as text, with the precision that a small image needs: "24.2 MP", "0.31 MP".
String infoMegapixels(PrUVec2 extent);

// Returns an aspect ratio as text. If the reduced terms are small, the text gives them: "3:2 (1.50:1)". Otherwise the
// text is a decimal ratio: "1.47:1".
String infoAspect(PrUVec2 extent);

// Returns an exposure time as text, from a fraction of seconds: "1/250 s" below one second, and "2.5 s" from one second
// up. Returns empty text if there is no exposure time.
String infoExposure(uint32_t numerator, uint32_t denominator);

} // namespace pandite

PR_ASSUME_NONNULL_END
