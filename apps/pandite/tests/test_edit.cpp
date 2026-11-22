// The viewer's edits: what each does to pixels, the extents of a resize, the stack's undo, and crop mode's selection.

#include "pr/compress_select.h"
#include "pr/format.h"
#include "pr/image_select.h"

#include "apps/pandite/edit.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace pandite;
using namespace pr;

namespace {

// Returns a bitmap of extent `extent`. The red channel numbers the pixels from 1 in reading order.
Bitmap numbered(PrUVec2 extent) {
    Array<uint8_t> pixels(size_t(extent.x) * extent.y * 4);
    for (uint32_t i = 0; i < extent.x * extent.y; ++i) {
        pixels[i * 4] = uint8_t(i + 1);
        pixels[i * 4 + 3] = 255;
    }
    return bitmapOf(static_cast<Array<uint8_t>&&>(pixels), extent);
}

// Returns the numbers of a bitmap's pixels in reading order.
Array<uint8_t> numbersOf(const Bitmap& bitmap) {
    Array<uint8_t> numbers;
    PrPixelView view = bitmapView(bitmap);
    for (uint32_t i = 0; i < bitmap.extent.x * bitmap.extent.y; ++i)
        numbers.push(view.data[i * 4]);
    return numbers;
}

Edit editOf(EditKind kind) {
    return Edit{.kind = kind};
}

// Returns the selection that a drag from `from` to `to` makes in an image of extent `extent`.
CropSelection dragged(PrUVec2 extent, PrVec2 from, PrVec2 to, bool square = false) {
    CropSelection crop{.image = extent};
    cropBegin(crop, from, 4);
    cropDragTo(crop, to, square);
    cropEnd(crop);
    return crop;
}

} // namespace

TEST_CASE("an edit rotates, flips, crops or resizes the pixels of the image") {
    // 1 2 3 4 5 6
    Bitmap image = numbered({3, 2});
    Bitmap right = editApply(editOf(EditKind_RotateRight), image);
    CHECK(right.extent == PrUVec2{2, 3});
    CHECK(numbersOf(right) == Array<uint8_t>({4, 1, 5, 2, 6, 3}));
    Bitmap left = editApply(editOf(EditKind_RotateLeft), image);
    CHECK(numbersOf(left) == Array<uint8_t>({3, 6, 2, 5, 1, 4}));
    CHECK(numbersOf(editApply(editOf(EditKind_FlipHorizontal), image)) == Array<uint8_t>({3, 2, 1, 6, 5, 4}));
    CHECK(numbersOf(editApply(editOf(EditKind_FlipVertical), image)) == Array<uint8_t>({4, 5, 6, 1, 2, 3}));

    Edit crop{.kind = EditKind_Crop, .crop = PrIRect{1, 0, 3, 2}};
    Bitmap cropped = editApply(crop, image);
    CHECK(cropped.extent == PrUVec2{2, 2});
    CHECK(numbersOf(cropped) == Array<uint8_t>({2, 3, 5, 6}));
    // A crop that reaches past the image does not apply.
    crop.crop = PrIRect{2, 0, 4, 2};
    CHECK(editApply(crop, image).bytes.get() == nullptr);

    Edit resize{.kind = EditKind_Resize, .resize = {.mode = ResizeMode_Exact, .size = {6, 4}}};
    CHECK(editApply(resize, image).extent == PrUVec2{6, 4});
}

TEST_CASE("a bitmap written as a PNG decodes to the same pixels") {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    Bitmap image = numbered({5, 3});
    String path = format("{}/written.png", testFolder());
    REQUIRE(bitmapWritePng(images, image, path) == PrStatus_Ok);
    Bitmap read{};
    REQUIRE(bitmapDecode(images, path, &read) == PrStatus_Ok);
    CHECK(read.extent == image.extent);
    CHECK(numbersOf(read) == numbersOf(image));
    CHECK(bitmapDecode(images, format("{}/missing.png", testFolder()), &read) == PrStatus_NotFound);
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
}

TEST_CASE("a resize gives a percentage of the image's extent, the largest extent that fits a box, or an exact extent") {
    CHECK(resizeExtent({.mode = ResizeMode_Percent, .percent = 50}, {101, 50}) == PrUVec2{51, 25});
    CHECK(resizeExtent({.mode = ResizeMode_Percent, .percent = 0.01f}, {100, 50}) == PrUVec2{1, 1});
    CHECK(resizeExtent({.mode = ResizeMode_Percent, .percent = 0}, {100, 50}) == PrUVec2{});

    // A fit keeps the aspect, and does not enlarge unless asked.
    ResizeSpec fit{.mode = ResizeMode_Fit, .size = {1920, 1080}};
    CHECK(resizeExtent(fit, {4000, 3000}) == PrUVec2{1440, 1080});
    CHECK(resizeExtent(fit, {800, 600}) == PrUVec2{800, 600});
    fit.enlarge = true;
    CHECK(resizeExtent(fit, {800, 600}) == PrUVec2{1440, 1080});

    CHECK(resizeExtent({.mode = ResizeMode_Exact, .size = {10, 700}}, {100, 50}) == PrUVec2{10, 700});
    CHECK(resizeExtent({.mode = ResizeMode_Exact, .size = {0, 700}}, {100, 50}) == PrUVec2{});
    CHECK(resizeExtent({.mode = ResizeMode_Percent, .percent = 1000}, {10000, 10}) == PrUVec2{});
}

TEST_CASE("the stack runs one edit at a time, and undo removes a queued edit, the running edit, then an applied edit") {
    Bitmap base = numbered({3, 2});
    EditStack stack{};
    CHECK(!editIsEdited(stack));
    editPush(stack, editOf(EditKind_RotateRight));
    editPush(stack, editOf(EditKind_FlipHorizontal));
    CHECK(editIsEdited(stack));

    Edit job{};
    REQUIRE(editNextJob(stack, &job));
    CHECK(job.kind == EditKind_RotateRight);
    // One edit runs at a time.
    Edit other{};
    CHECK(!editNextJob(stack, &other));
    CHECK(editFinish(stack, editApply(job, editCurrent(stack, base))));
    CHECK(editCurrent(stack, base).extent == PrUVec2{2, 3});

    REQUIRE(editNextJob(stack, &job));
    CHECK(job.kind == EditKind_FlipHorizontal);
    editPush(stack, editOf(EditKind_FlipVertical));
    CHECK(editUndo(stack) == EditUndo_Dequeued);
    CHECK(editUndo(stack) == EditUndo_DroppedRunning);
    // The result of the edit that was undone while it ran is dropped.
    CHECK(!editFinish(stack, editApply(job, editCurrent(stack, base))));
    CHECK(stack.applied.count() == 1);
    CHECK(editUndo(stack) == EditUndo_Reverted);
    CHECK(&editCurrent(stack, base) == &base);
    CHECK(editUndo(stack) == EditUndo_Nothing);
    CHECK(!editIsEdited(stack));

    // A failed edit is dropped, and a clear forgets every edit.
    editPush(stack, Edit{.kind = EditKind_Crop, .crop = PrIRect{0, 0, 9, 9}});
    REQUIRE(editNextJob(stack, &job));
    CHECK(!editFinish(stack, editApply(job, editCurrent(stack, base))));
    editPush(stack, editOf(EditKind_RotateLeft));
    REQUIRE(editNextJob(stack, &job));
    editPush(stack, editOf(EditKind_RotateLeft));
    editClear(stack);
    CHECK(!editFinish(stack, editApply(job, base)));
    CHECK(!editIsEdited(stack));
}

TEST_CASE("a crop selection is drawn, moved and resized within the image, and rounds to whole pixels") {
    PrUVec2 image{100, 80};
    CropSelection crop = dragged(image, {60, 50}, {10, 20});
    REQUIRE(crop.selected);
    CHECK(crop.rect == PrRect{10, 20, 60, 50});
    // A square drag takes the larger axis, within the image's room.
    CHECK(dragged(image, {10, 10}, {40, 20}, true).rect == PrRect{10, 10, 40, 40});
    CHECK(dragged(image, {90, 10}, {40, 70}, true).rect == PrRect{30, 10, 90, 70});
    CHECK(dragged(image, {90, 10}, {40, 90}, true).rect == PrRect{20, 10, 90, 80});
    // A drag past the image stops at its edge, and a click selects nothing.
    CHECK(dragged(image, {50, 50}, {150, -20}).rect == PrRect{50, 0, 100, 50});
    CHECK(!dragged(image, {50, 50}, {50.5f, 50}).selected);

    // A press inside moves the selection, which stays within the image.
    cropBegin(crop, {30, 30}, 4);
    CHECK(crop.grabbed == CropHit_Inside);
    cropDragTo(crop, {200, 30}, false);
    cropEnd(crop);
    CHECK(crop.rect == PrRect{50, 20, 100, 50});

    // A corner's handle wins over its sides, and a side's handle spans its side.
    CropHandle handle = CropHandle_Count;
    CHECK(cropHit(crop, {51, 21}, 4, &handle) == CropHit_Handle);
    CHECK(handle == CropHandle_TopLeft);
    CHECK(cropHit(crop, {70, 49}, 4, &handle) == CropHit_Handle);
    CHECK(handle == CropHandle_Bottom);
    CHECK(cropHit(crop, {20, 30}, 4, &handle) == CropHit_Outside);
    cropBegin(crop, {50, 35}, 4);
    cropDragTo(crop, {120, 35}, false);
    cropEnd(crop);
    // Dragging the left edge past the right edge turns the selection over.
    CHECK(crop.rect == PrRect{100, 20, 100, 50});
    CHECK(!crop.selected);

    cropSelectAll(crop);
    crop.rect = PrRect{10.4f, 10.6f, 20.5f, 30.2f};
    PrIRect pixels{};
    REQUIRE(cropPixels(crop, &pixels));
    CHECK(pixels == PrIRect{10, 11, 21, 30});
    cropNudge(crop, {-50, 100});
    CHECK(crop.rect.left == 0);
    CHECK(crop.rect.bottom == 80);
}
