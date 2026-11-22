// The viewer's edits of the image on screen.

#include "apps/pandite/edit.h"

#include "pr/blob.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/image.h"
#include "pr/image_process.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"

namespace pandite {

namespace {

// Returns `value` clamped to [low, high].
float clampTo(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

// Returns the rectangle of the corners `a` and `b`, in any order.
PrRect rectOfCorners(PrVec2 a, PrVec2 b) {
    return PrRect{prF32Min(a.x, b.x), prF32Min(a.y, b.y), prF32Max(a.x, b.x), prF32Max(a.y, b.y)};
}

// Returns `rect` clamped to an image of extent `extent`.
PrRect clampRect(PrRect rect, PrUVec2 extent) {
    float w = float(extent.x);
    float h = float(extent.y);
    return PrRect{
        clampTo(rect.left, 0, w),
        clampTo(rect.top, 0, h),
        clampTo(rect.right, 0, w),
        clampTo(rect.bottom, 0, h),
    };
}

// Returns true if `handle` is on a corner.
bool isCorner(CropHandle handle) {
    return handle % 2 == 0;
}

// Returns the far corner of a square that runs from `anchor` toward `at`. The side of the square is the larger of the
// distances from `anchor` to `at` on the two axes. It is no larger than the room that the image has in that direction.
PrVec2 squareEnd(PrVec2 anchor, PrVec2 at, PrUVec2 extent) {
    float dx = at.x - anchor.x;
    float dy = at.y - anchor.y;
    float side = prF32Max(prF32Abs(dx), prF32Abs(dy));
    float roomX = dx >= 0 ? float(extent.x) - anchor.x : anchor.x;
    float roomY = dy >= 0 ? float(extent.y) - anchor.y : anchor.y;
    side = prF32Min(side, prF32Min(roomX, roomY));
    return PrVec2{anchor.x + (dx >= 0 ? side : -side), anchor.y + (dy >= 0 ? side : -side)};
}

// Returns the orientation that applies the turn or the mirror of `kind`.
PrImageOrientation orientationOf(EditKind kind) {
    switch (kind) {
        case EditKind_RotateLeft: return PrImageOrientation_Rotate270;
        case EditKind_RotateRight: return PrImageOrientation_Rotate90;
        case EditKind_FlipHorizontal: return PrImageOrientation_FlipHorizontal;
        default: return PrImageOrientation_FlipVertical;
    }
}

} // namespace

Bitmap bitmapOf(Array<uint8_t>&& pixels, PrUVec2 extent) {
    RawArray<uint8_t> taken = pixels.take();
    PrByteView bytes{taken.data, taken.count};
    PrBlob* blob = prBlobCreate(bytes, [](void* PR_NULLABLE, PrByteView freed) {
        prMemoryFree(const_cast<uint8_t*>(freed.data));
    }, nullptr);
    return Bitmap{.bytes = BlobRef::adopt(blob), .extent = extent};
}

PrPixelView bitmapView(const Bitmap& bitmap) {
    return PrPixelView{bitmap.bytes.bytes().data, size_t(bitmap.extent.x) * 4, bitmap.extent};
}

PrStatus bitmapDecode(const PrImageCodecSet* codecs, PrStringView path, Bitmap* out) {
    *out = Bitmap{};
    BlobRef file = BlobRef::adopt(prFileOpen(path));
    if (file.get() == nullptr)
        return PrStatus_NotFound;
    PrImageDecoder* decoder = prImageDecoderCreate(codecs);
    PrImageSource source{};
    source.memory = file.bytes();
    PrStatus status = prImageDecoderOpen(decoder, source, nullptr);
    if (status == PrStatus_Ok || status == PrStatus_Truncated) {
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        bool swaps = prImageOrientationSwapsAxes(info.orientation);
        PrUVec2 extent = swaps ? PrUVec2{info.extent.y, info.extent.x} : info.extent;
        Array<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
        PrPixels output{pixels.data(), size_t(extent.x) * 4, extent};
        status = prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, info.orientation, output);
        if (status == PrStatus_Ok || status == PrStatus_Truncated)
            *out = bitmapOf(static_cast<Array<uint8_t>&&>(pixels), extent);
    }
    prImageDecoderDestroy(decoder);
    return status;
}

PrStatus bitmapWritePng(const PrImageCodecSet* codecs, const Bitmap& bitmap, PrStringView path) {
    PrPixelView pixels = bitmapView(bitmap);
    size_t size = 0;
    PrStatus status = prImageCodecSetEncode(codecs, "image/png", pixels, nullptr, 0, &size);
    if (status != PrStatus_Ok && status != PrStatus_Truncated)
        return status;
    Array<uint8_t> encoded(size);
    status = prImageCodecSetEncode(codecs, "image/png", pixels, encoded.data(), size, &size);
    if (status != PrStatus_Ok)
        return status;
    return prFileWrite(path, PrByteView{encoded.data(), size}) ? PrStatus_Ok : PrStatus_Io;
}

PrUVec2 resizeExtent(const ResizeSpec& spec, PrUVec2 from) {
    if (from.x == 0 || from.y == 0)
        return PrUVec2{};
    double w = 0;
    double h = 0;
    if (spec.mode == ResizeMode_Percent) {
        if (!(spec.percent > 0))
            return PrUVec2{};
        w = double(from.x) * double(spec.percent) / 100;
        h = double(from.y) * double(spec.percent) / 100;
    } else if (spec.mode == ResizeMode_Fit) {
        if (spec.size.x == 0 || spec.size.y == 0)
            return PrUVec2{};
        double scale = prF64Min(double(spec.size.x) / from.x, double(spec.size.y) / from.y);
        if (!spec.enlarge && scale > 1)
            scale = 1;
        w = double(from.x) * scale;
        h = double(from.y) * scale;
    } else {
        if (spec.size.x == 0 || spec.size.y == 0)
            return PrUVec2{};
        w = spec.size.x;
        h = spec.size.y;
    }
    w = prF64Max(prF64Round(w), 1);
    h = prF64Max(prF64Round(h), 1);
    if (w > kMaxSide || h > kMaxSide)
        return PrUVec2{};
    return PrUVec2{uint32_t(w), uint32_t(h)};
}

PrUVec2 editExtent(const Edit& edit, PrUVec2 from) {
    switch (edit.kind) {
        case EditKind_RotateLeft:
        case EditKind_RotateRight: return PrUVec2{from.y, from.x};
        case EditKind_FlipHorizontal:
        case EditKind_FlipVertical: return from;
        case EditKind_Crop: {
            const PrIRect& r = edit.crop;
            if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top || uint32_t(r.right) > from.x ||
                    uint32_t(r.bottom) > from.y)
                return PrUVec2{};
            return PrUVec2{uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
        }
        default: return resizeExtent(edit.resize, from);
    }
}

Bitmap editApply(const Edit& edit, const Bitmap& image) {
    PrUVec2 extent = editExtent(edit, image.extent);
    if (extent.x == 0 || extent.y == 0 || image.bytes.get() == nullptr)
        return Bitmap{};
    Array<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
    PrPixels out{pixels.data(), size_t(extent.x) * 4, extent};
    PrPixelView in = bitmapView(image);
    if (edit.kind == EditKind_Crop) {
        for (uint32_t y = 0; y < extent.y; ++y) {
            const uint8_t* from = in.data + size_t(uint32_t(edit.crop.top) + y) * in.stride + size_t(edit.crop.left) * 4;
            prMemCopy(out.data + size_t(y) * out.stride, from, size_t(extent.x) * 4);
        }
    } else if (edit.kind == EditKind_Resize) {
        prImageResize(in, out);
    } else {
        prImageOrient(in, 4, orientationOf(edit.kind), out);
    }
    return bitmapOf(static_cast<Array<uint8_t>&&>(pixels), extent);
}

const char* editLabel(EditKind kind) {
    switch (kind) {
        case EditKind_RotateLeft:
        case EditKind_RotateRight: return "Rotate";
        case EditKind_FlipHorizontal:
        case EditKind_FlipVertical: return "Flip";
        case EditKind_Crop: return "Crop";
        default: return "Resize";
    }
}

const Bitmap& editCurrent(const EditStack& stack, const Bitmap& base) {
    return stack.applied.count() != 0 ? stack.applied.last().result : base;
}

bool editIsEdited(const EditStack& stack) {
    return stack.applied.count() != 0 || editIsBusy(stack);
}

bool editIsBusy(const EditStack& stack) {
    return stack.running || stack.queued.count() != 0;
}

void editPush(EditStack& stack, const Edit& edit) {
    stack.queued.push(edit);
}

bool editNextJob(EditStack& stack, Edit* out) {
    if (stack.running || stack.queued.count() == 0)
        return false;
    stack.runningEdit = stack.queued[0];
    stack.queued.erase(0);
    stack.running = true;
    stack.discardRunning = false;
    *out = stack.runningEdit;
    return true;
}

bool editFinish(EditStack& stack, Bitmap result) {
    if (!stack.running)
        return false;
    stack.running = false;
    bool discard = stack.discardRunning;
    stack.discardRunning = false;
    if (discard || result.bytes.get() == nullptr)
        return false;
    stack.applied.push(AppliedEdit{.edit = stack.runningEdit, .result = static_cast<Bitmap&&>(result)});
    return true;
}

EditUndo editUndo(EditStack& stack) {
    if (stack.queued.count() != 0) {
        stack.queued.pop();
        return EditUndo_Dequeued;
    }
    if (stack.running && !stack.discardRunning) {
        stack.discardRunning = true;
        return EditUndo_DroppedRunning;
    }
    if (stack.applied.count() != 0) {
        stack.applied.pop();
        return EditUndo_Reverted;
    }
    return EditUndo_Nothing;
}

void editClear(EditStack& stack) {
    stack.applied.clear();
    stack.queued.clear();
    if (stack.running)
        stack.discardRunning = true;
}

PrVec2 cropHandleCenter(CropHandle handle, PrRect rect) {
    float midX = (rect.left + rect.right) / 2;
    float midY = (rect.top + rect.bottom) / 2;
    switch (handle) {
        case CropHandle_TopLeft: return PrVec2{rect.left, rect.top};
        case CropHandle_Top: return PrVec2{midX, rect.top};
        case CropHandle_TopRight: return PrVec2{rect.right, rect.top};
        case CropHandle_Right: return PrVec2{rect.right, midY};
        case CropHandle_BottomRight: return PrVec2{rect.right, rect.bottom};
        case CropHandle_Bottom: return PrVec2{midX, rect.bottom};
        case CropHandle_BottomLeft: return PrVec2{rect.left, rect.bottom};
        default: return PrVec2{rect.left, midY};
    }
}

CropHit cropHit(const CropSelection& crop, PrVec2 at, float reach, CropHandle* handle) {
    if (!crop.selected)
        return CropHit_Outside;
    const PrRect& r = crop.rect;
    for (CropHandle h = 0; h < CropHandle_Count; h += 2) {
        PrVec2 center = cropHandleCenter(h, r);
        if (prF32Abs(at.x - center.x) <= reach && prF32Abs(at.y - center.y) <= reach) {
            *handle = h;
            return CropHit_Handle;
        }
    }
    // A side's handle spans the side, not only its square.
    bool withinX = at.x >= r.left && at.x <= r.right;
    bool withinY = at.y >= r.top && at.y <= r.bottom;
    struct Side {
        bool within;
        float distance;
        CropHandle handle;
    };
    const Side sides[] = {
        {withinX, prF32Abs(at.y - r.top), CropHandle_Top},
        {withinX, prF32Abs(at.y - r.bottom), CropHandle_Bottom},
        {withinY, prF32Abs(at.x - r.left), CropHandle_Left},
        {withinY, prF32Abs(at.x - r.right), CropHandle_Right},
    };
    for (const Side& side : sides) {
        if (side.within && side.distance <= reach) {
            *handle = side.handle;
            return CropHit_Handle;
        }
    }
    return withinX && withinY ? CropHit_Inside : CropHit_Outside;
}

void cropBegin(CropSelection& crop, PrVec2 at, float reach) {
    at = PrVec2{clampTo(at.x, 0, float(crop.image.x)), clampTo(at.y, 0, float(crop.image.y))};
    CropHandle handle = CropHandle_TopLeft;
    crop.grabbed = cropHit(crop, at, reach, &handle);
    crop.handle = handle;
    crop.dragging = true;
    crop.anchor = at;
    crop.start = crop.rect;
}

void cropDragTo(CropSelection& crop, PrVec2 at, bool square) {
    if (!crop.dragging)
        return;
    at = PrVec2{clampTo(at.x, 0, float(crop.image.x)), clampTo(at.y, 0, float(crop.image.y))};
    PrRect rect{};
    if (crop.grabbed == CropHit_Outside) {
        PrVec2 end = square ? squareEnd(crop.anchor, at, crop.image) : at;
        rect = rectOfCorners(crop.anchor, end);
        crop.selected = true;
    } else if (crop.grabbed == CropHit_Inside) {
        const PrRect& s = crop.start;
        float w = s.right - s.left;
        float h = s.bottom - s.top;
        float left = clampTo(s.left + at.x - crop.anchor.x, 0, prF32Max(float(crop.image.x) - w, 0));
        float top = clampTo(s.top + at.y - crop.anchor.y, 0, prF32Max(float(crop.image.y) - h, 0));
        rect = PrRect{left, top, left + w, top + h};
    } else {
        CropHandle h = crop.handle;
        bool left = h == CropHandle_TopLeft || h == CropHandle_BottomLeft || h == CropHandle_Left;
        bool right = h == CropHandle_TopRight || h == CropHandle_BottomRight || h == CropHandle_Right;
        bool top = h == CropHandle_TopLeft || h == CropHandle_TopRight || h == CropHandle_Top;
        bool bottom = h == CropHandle_BottomLeft || h == CropHandle_BottomRight || h == CropHandle_Bottom;
        PrRect s = crop.start;
        if (square && isCorner(h)) {
            PrVec2 anchor{left ? s.right : s.left, top ? s.bottom : s.top};
            rect = rectOfCorners(anchor, squareEnd(anchor, at, crop.image));
        } else {
            s.left = left ? at.x : s.left;
            s.right = right ? at.x : s.right;
            s.top = top ? at.y : s.top;
            s.bottom = bottom ? at.y : s.bottom;
            rect = rectOfCorners(PrVec2{s.left, s.top}, PrVec2{s.right, s.bottom});
        }
    }
    crop.rect = clampRect(rect, crop.image);
}

void cropEnd(CropSelection& crop) {
    crop.dragging = false;
    if (crop.selected && (crop.rect.right - crop.rect.left < 1 || crop.rect.bottom - crop.rect.top < 1))
        crop.selected = false;
}

void cropSelectAll(CropSelection& crop) {
    crop.selected = true;
    crop.rect = PrRect{0, 0, float(crop.image.x), float(crop.image.y)};
}

void cropNudge(CropSelection& crop, PrVec2 delta) {
    if (!crop.selected)
        return;
    PrRect& r = crop.rect;
    float w = r.right - r.left;
    float h = r.bottom - r.top;
    r.left = clampTo(r.left + delta.x, 0, prF32Max(float(crop.image.x) - w, 0));
    r.top = clampTo(r.top + delta.y, 0, prF32Max(float(crop.image.y) - h, 0));
    r.right = r.left + w;
    r.bottom = r.top + h;
}

bool cropPixels(const CropSelection& crop, PrIRect* out) {
    if (!crop.selected || crop.image.x == 0 || crop.image.y == 0)
        return false;
    int32_t w = int32_t(crop.image.x);
    int32_t h = int32_t(crop.image.y);
    int32_t x0 = int32_t(prF32Round(prF32Max(crop.rect.left, 0)));
    int32_t y0 = int32_t(prF32Round(prF32Max(crop.rect.top, 0)));
    int32_t x1 = int32_t(prF32Round(prF32Max(crop.rect.right, 0)));
    int32_t y1 = int32_t(prF32Round(prF32Max(crop.rect.bottom, 0)));
    x0 = x0 < w - 1 ? x0 : w - 1;
    y0 = y0 < h - 1 ? y0 : h - 1;
    x1 = x1 < w ? x1 : w;
    y1 = y1 < h ? y1 : h;
    if (x1 <= x0 || y1 <= y0)
        return false;
    *out = PrIRect{x0, y0, x1, y1};
    return true;
}

} // namespace pandite
