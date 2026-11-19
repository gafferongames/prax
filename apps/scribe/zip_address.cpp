// The address handler of ZIP archives: a step is the name of an entry as the archive gives it, with '/' separators, and
// reaches the entry's bytes.

#include "apps/scribe/zip_address.h"

#include "pr/alloc.h"
#include "pr/construct.h"
#include "pr/string_ops.h"
#include "pr/zip.h"

namespace scribe {

namespace {

struct ZipHandler : AddressHandler {
    const PrCompressCodecSet* PR_NULLABLE codec = nullptr;
};

// An opened archive, and the indices of its entries that are files rather than folders.
struct ZipState {
    PrZip* archive = nullptr;
    Array<uint32_t> files;
};

void freeBytes(void* PR_NULLABLE, PrByteView bytes) {
    prMemoryFree(const_cast<uint8_t*>(bytes.data));
}

bool zipOpens(const AddressHandler*, PrMimeFormat format) {
    return format == PrMimeFormat_Zip;
}

PrStatus zipOpen(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE* PR_NONNULL state) {
    PrZip* archive = prZipCreate(static_cast<ZipHandler*>(handler)->codec);
    // The node holds its bytes for as long as the state lives.
    if (prZipOpen(archive, prBlobBytes(addressNodeBytes(node))) != PrStatus_Ok) {
        prZipDestroy(archive);
        return PrStatus_InvalidData;
    }
    ZipState* zip = create<ZipState>();
    zip->archive = archive;
    for (uint32_t i = 0; i < prZipEntryCount(archive); ++i) {
        if (!prStringEndsWith(prZipEntry(archive, i).name, "/"))
            zip->files.push(i);
    }
    *state = zip;
    return PrStatus_Ok;
}

void zipClose(AddressHandler*, void* PR_NULLABLE state) {
    ZipState* zip = static_cast<ZipState*>(state);
    prZipDestroy(zip->archive);
    destroy(zip);
}

PrStatus zipStep(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE state, PrStringView step,
        uint64_t budget, AddressStepResult* out) {
    const ZipState* zip = static_cast<const ZipState*>(state);
    PrZip* archive = zip->archive;
    uint32_t index = prZipFind(archive, step);
    if (index == prZipEntryCount(archive) || prStringEndsWith(step, "/"))
        return PrStatus_NotFound;
    PrZipEntry entry = prZipEntry(archive, index);
    size_t slash = prStringFindLastByte(step, '/');
    out->name = slash == step.size ? step : prStringFrom(step, slash + 1);
    out->kind = AddressNodeKind_Resource;
    if (entry.encrypted)
        return PrStatus_NotSupported;
    if (entry.method == PrZipMethod_Stored) {
        PrByteView bytes{};
        if (prZipEntryData(archive, index, &bytes) != PrStatus_Ok || entry.compressedSize != entry.size)
            return PrStatus_InvalidData;
        out->bytes = addressNodeShare(node, bytes);
        return PrStatus_Ok;
    }
    if (entry.method != PrZipMethod_Deflated || static_cast<ZipHandler*>(handler)->codec == nullptr)
        return PrStatus_NotSupported;
    // The declared size bounds what decoding writes, so an entry that claims less than it holds fails as corrupt.
    if (entry.size > budget || uint64_t(size_t(entry.size)) != entry.size)
        return PrStatus_Limit;
    size_t size = size_t(entry.size);
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(size != 0 ? size : 1));
    PrStatus status = prZipRead(archive, index, bytes, size);
    if (status != PrStatus_Ok) {
        prMemoryFree(bytes);
        return status;
    }
    out->bytes = prBlobCreate(PrByteView{bytes, size}, freeBytes, nullptr);
    out->decoded = size;
    return PrStatus_Ok;
}

uint32_t zipChildCount(const AddressHandler*, const void* PR_NULLABLE state) {
    return static_cast<const ZipState*>(state)->files.count();
}

AddressChild zipChild(const AddressHandler*, const void* PR_NULLABLE state, uint32_t index) {
    const ZipState* zip = static_cast<const ZipState*>(state);
    PrZipEntry entry = prZipEntry(zip->archive, zip->files[index]);
    return AddressChild{.step = entry.name, .path = entry.name, .size = entry.size};
}

bool zipSibling(const AddressHandler*, const void* PR_NULLABLE, PrStringView step, PrStringView reference, PrTextBuffer* out) {
    return addressPathJoin(step, reference, out);
}

void zipDestroy(AddressHandler* handler) {
    ZipHandler* zip = static_cast<ZipHandler*>(handler);
    destroy(zip);
}

} // namespace

AddressHandler* zipAddressHandlerCreate(const PrCompressCodecSet* PR_NULLABLE codec) {
    ZipHandler* handler = create<ZipHandler>();
    handler->destroyFn = zipDestroy;
    handler->opensFn = zipOpens;
    handler->openFn = zipOpen;
    handler->closeFn = zipClose;
    handler->stepFn = zipStep;
    handler->childCountFn = zipChildCount;
    handler->childFn = zipChild;
    handler->siblingFn = zipSibling;
    handler->codec = codec;
    return handler;
}

} // namespace scribe
