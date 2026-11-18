// Draw writers: the interface functions, which enforce the contract around each backend's operations.

#include "pr/draw_writer.h"

#include "pr/assert.h"
#include "pr/draw_writer_backend.h"
#include "pr/float.h"

PrDrawWriterOptions prDrawWriterOptionsDefault(void) {
    PrDrawWriterOptions options{};
    options.rasterScale = 2;
    return options;
}

void prDrawWriterDestroy(PrDrawWriter* PR_NULLABLE writer) {
    if (writer != nullptr && writer->destroyFn != nullptr)
        writer->destroyFn(writer);
}

void prDrawWriterBegin(PrDrawWriter* writer, const PrDrawDocumentDesc* desc) {
    PR_ASSERT(desc->write != nullptr);
    writer->beginFn(writer, desc);
}

void prDrawWriterAddPage(PrDrawWriter* writer, const PrDrawPage* page) {
    PR_ASSERT(prDrawListDepth(page->list) == 0);
    writer->addPageFn(writer, page);
}

void prDrawWriterEnd(PrDrawWriter* writer) {
    writer->endFn(writer);
}

void prDrawWriterWrite(PrDrawWriter* writer, const PrDrawDocumentDesc* desc, const PrDrawPage* PR_NULLABLE pages,
        uint32_t pageCount) {
    prDrawWriterBegin(writer, desc);
    for (uint32_t i = 0; i < pageCount; ++i)
        prDrawWriterAddPage(writer, &pages[i]);
    prDrawWriterEnd(writer);
}
