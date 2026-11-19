// OpenUSD layers: the public functions, over the layer that the readers of each format build, with packages, .usdz
// (usd-core/#package), and the locations of assets (usd-core/#resource-interface).

#include "pr/usd.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/blob.h"
#include "pr/file.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/zip.h"

#include "usd/src/usd_crate.h"
#include "usd/src/usd_layer.h"
#include "usd/src/usd_mtlx.h"
#include "usd/src/usd_text.h"

namespace pr_impl {

namespace {

// The alignment that the specification gives the bytes of each file of a package.
constexpr uint64_t kPackageAlignment = 64;

// The size of a ZIP archive's end record without a comment, and of the locator that precedes it in a ZIP64 archive.
constexpr size_t kZipEndSize = 22;
constexpr size_t kZip64LocatorSize = 20;

struct UsdPackage {
    PrBlob* file;
    PrZip* zip;
    RawArray<PrUsdPackageFile> files;
    bool conforming;
};

// Releases a file of a package: the reference that its blob holds to the package's bytes.
void releaseFile(void* PR_NULLABLE context, PrByteView) {
    prBlobRelease(static_cast<PrBlob*>(context));
}

// Returns the length of the URI scheme that begins `path`, with its colon, or zero. One letter is a drive of Windows
// and not a scheme.
size_t schemeSize(PrStringView path) {
    size_t at = 0;
    while (at < path.size && (prAsciiIsLetter(path.data[at]) ||
            (at != 0 && (prAsciiIsDigit(path.data[at]) || path.data[at] == '+' || path.data[at] == '-' || path.data[at] == '.'))))
        ++at;
    return at >= 2 && at < path.size && path.data[at] == ':' ? at + 1 : 0;
}

// What a ".." part does where no part above the floor is left to remove.
enum {
    ParentAtFloor_Drop,     // the floor is a root, whose parent is itself
    ParentAtFloor_Keep,     // the folder is relative, and its parent is outside what the location names
};
typedef uint8_t ParentAtFloor;

// Returns true if the folder in `out` ends in a ".." part above the floor.
bool endsInParent(const RawArray<char>& out, uint32_t floor) {
    PrStringView folder{out.data + floor, out.count - floor};
    while (prStringEndsWith(folder, "/"))
        folder.size -= 1;
    return folder == ".." || prStringEndsWith(folder, "/..");
}

// Appends the parts of `path` to the folder in `out`, which `floor` bytes of are fixed: a "." part is dropped, and a
// ".." part removes the part before it, down to the floor. Past the floor, `parent` decides.
void appendParts(RawArray<char>& out, uint32_t floor, PrStringView path, ParentAtFloor parent) {
    size_t at = 0;
    while (at <= path.size) {
        size_t end = at;
        while (end < path.size && path.data[end] != '/')
            ++end;
        PrStringView part{path.data + at, end - at};
        if (part == ".." && parent == ParentAtFloor_Keep && (out.count == floor || endsInParent(out, floor))) {
            if (out.count > floor && out.data[out.count - 1] != '/')
                out.push('/');
            out.append("..", 2);
        } else if (part == "..") {
            while (out.count > floor && out.data[out.count - 1] == '/')
                --out.count;
            while (out.count > floor && out.data[out.count - 1] != '/')
                --out.count;
        } else if (part.size != 0 && part != ".") {
            if (out.count > floor && out.data[out.count - 1] != '/')
                out.push('/');
            out.append(part.data, uint32_t(part.size));
        }
        at = end + 1;
    }
}

// Splits a location that names a file in a package, as "a/b.usdz[c/d.usda]", into the package and the file. Returns
// false for a location that names no file in a package.
bool splitPackage(PrStringView location, PrStringView* package, PrStringView* inner) {
    size_t open = prStringFindByte(location, '[');
    if (open == location.size || location.data[location.size - 1] != ']')
        return false;
    *package = PrStringView{location.data, open};
    *inner = PrStringView{location.data + open + 1, location.size - open - 2};
    return true;
}

bool isPackageName(PrStringView path) {
    return prAsciiEndsWithFold(path, ".usdz");
}

// A provider of a file in a package: the package's provider, the file's name, and once its first read found it, the
// range of the package that holds the file, which the reads after it read alone.
struct PackageEntry {
    const PrBlobSource* package;
    PrRawString name;
    PrUsdOptions options;
    Atomic<uint8_t> found;              // 0 before the first read finds the file, 1 after
    uint64_t offset;
    uint64_t size;
};

// A read of a package's file, and the provider that it reads.
struct PackageRead {
    PackageEntry* entry;
    PrBlobRead* read;
};

// Completes the read of a file from the package's bytes, finding the file in the package's directory.
void packageRead(void* PR_NULLABLE context, PrBlobRead* outer) {
    PackageRead pending = *static_cast<PackageRead*>(context);
    prMemoryFree(context);
    PrStatus status = prBlobReadStatus(outer);
    PrBlob* blob = prBlobReadBlob(outer);
    PrBlob* file = nullptr;
    if (status == PrStatus_Ok && pending.entry->found.load() != 0) {
        // The read was of the file's range.
        prBlobRetain(blob);
        file = blob;
    } else if (status == PrStatus_Ok) {
        PrUsdPackage* package = nullptr;
        status = prUsdPackageCreate(blob, &pending.entry->options, &package);
        uint32_t index = kPrUsdNone;
        if (status == PrStatus_Ok)
            index = prUsdPackageFind(package, prRawStringView(&pending.entry->name));
        if (status == PrStatus_Ok && index == kPrUsdNone)
            status = PrStatus_InvalidData;
        if (status == PrStatus_Ok) {
            PrByteView bytes = prUsdPackageFile(package, index).bytes;
            pending.entry->offset = uint64_t(bytes.data - prBlobBytes(blob).data);
            pending.entry->size = bytes.size;
            pending.entry->found.store(1);
            PrBlobRange range = prBlobReadRange(pending.read);
            PrBlob* whole = prUsdPackageOpen(package, index);
            file = prBlobSlice(whole, range.offset, range.size);
            prBlobRelease(whole);
        }
        prUsdPackageDestroy(package);
    }
    prBlobReadDestroy(outer);
    prBlobReadComplete(pending.read, status, file, 0);
}

void packageStart(void* PR_NULLABLE context, PrBlobRead* read) {
    PackageEntry* entry = static_cast<PackageEntry*>(context);
    PackageRead* pending = static_cast<PackageRead*>(prMemoryAlloc(sizeof(PackageRead)));
    *pending = PackageRead{entry, read};
    PrBlobReadDesc desc = prBlobReadDescDefault();
    if (entry->found.load() != 0) {
        // The file's range of the package is known, so the read maps to it.
        PrBlobRange range = prBlobReadRange(read);
        uint64_t offset = range.offset < entry->size ? range.offset : entry->size;
        uint64_t rest = entry->size - offset;
        desc.range = PrBlobRange{entry->offset + offset, range.size < rest ? range.size : rest};
    }
    desc.done = packageRead;
    desc.context = pending;
    prBlobReadCreate(entry->package, &desc);
}

void packageRelease(void* PR_NULLABLE context) {
    PackageEntry* entry = static_cast<PackageEntry*>(context);
    prBlobSourceRelease(entry->package);
    prRawStringReset(&entry->name);
    prMemoryFree(entry);
}

// Returns a provider of the file `name` in the package that `package` provides.
const PrBlobSource* packageEntrySource(const PrBlobSource* package, PrStringView name, const PrUsdOptions* PR_NULLABLE options) {
    PackageEntry* entry = allocZeroed<PackageEntry>();
    prBlobSourceRetain(package);
    entry->package = package;
    entry->name = prRawStringCopy(name);
    entry->options = options != nullptr ? *options : prUsdOptionsDefault();
    entry->found.store(0);
    PrBlobSourceDesc desc{
        .start = packageStart,
        .release = packageRelease,
        .context = entry,
        .size = kPrBlobSizeUnknown,
    };
    return prBlobSourceCreate(&desc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrUsdLayer : UsdLayer {};
struct PrUsdPackage : UsdPackage {};

PrStringView prUsdTypeName(PrUsdType type) {
    return usdTypeName(type);
}

uint32_t prUsdTypeSize(PrUsdType type) {
    return usdTypeSize(type);
}

bool prUsdTypeFromName(PrStringView typeName, PrUsdType* type, bool* isArray) {
    return usdTypeFromName(typeName, type, isArray);
}

PrUsdOptions prUsdOptionsDefault(void) {
    return PrUsdOptions{};
}

PrStatus prUsdLayerCreate(PrBlob* file, const PrUsdOptions* PR_NULLABLE options, PrUsdLayer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrUsdOptions defaults = prUsdOptionsDefault();
    const PrUsdOptions& o = options != nullptr ? *options : defaults;
    PrByteView bytes = prBlobBytes(file);
    PrUsdLayer* layer = allocZeroed<PrUsdLayer>();
    layer->arena.largestPage = kUsdArenaLargestPage;
    bool existed = false;
    usdLayerAddSpec(layer, PrUsdSpecForm_Layer, kPrUsdNone, "", &existed);
    PrStatus status = PrStatus_Ok;
    if (usdCrateMatches(bytes)) {
        layer->format = PrUsdFormat_Crate;
        prBlobRetain(file);
        layer->file = file;
        status = usdCrateRead(layer, bytes, o.codec, o.diagnostics);
    } else if (bytes.size >= 4 && bytes.data[0] == 'P' && bytes.data[1] == 'K' && bytes.data[2] == 3 && bytes.data[3] == 4) {
        usdDiagnose(o.diagnostics, "the file is a package, which prUsdPackageCreate reads");
        status = PrStatus_NotSupported;
    } else if (usdMtlxMatches(bytes)) {
        layer->format = PrUsdFormat_MaterialX;
        status = usdMtlxRead(layer, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size}, o.diagnostics);
    } else {
        layer->format = PrUsdFormat_Text;
        status = usdTextRead(layer, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size}, o.diagnostics);
    }
    if (status == PrStatus_Ok && !usdLayerFinish(layer, o.diagnostics))
        status = PrStatus_InvalidData;
    if (status != PrStatus_Ok) {
        usdLayerDestroy(layer);
        return status;
    }
    *out = layer;
    return PrStatus_Ok;
}

void prUsdLayerDestroy(PrUsdLayer* PR_NULLABLE layer) {
    usdLayerDestroy(layer);
}

PrUsdFormat prUsdLayerFormat(const PrUsdLayer* layer) {
    return layer->format;
}

PrStringView prUsdLayerVersion(const PrUsdLayer* layer) {
    return layer->version;
}

bool prUsdCrateVersionIsSupported(uint8_t major, uint8_t minor, uint8_t patch) {
    return usdCrateVersionIsSupported(major, minor, patch);
}

uint32_t prUsdLayerSpecCount(const PrUsdLayer* layer) {
    return layer->specs.count;
}

const PrUsdSpec* prUsdLayerSpecs(const PrUsdLayer* layer) {
    return layer->specs.data;
}

const PrUsdSpecIndex* PR_NULLABLE prUsdLayerList(const PrUsdLayer* layer, PrUsdList list) {
    PR_ASSERT(list.first + list.count <= layer->lists.count);
    return list.count != 0 ? layer->lists.data + list.first : nullptr;
}

PrUsdSpecIndex prUsdLayerFindSpec(const PrUsdLayer* layer, PrStringView path) {
    Atom atom = atomTableFind(&layer->strings, path);
    return atom != kAtomNone && atom < layer->specOfAtom.count ? layer->specOfAtom.data[atom] : kPrUsdNone;
}

PrUsdSpecIndex prUsdLayerFindChild(const PrUsdLayer* layer, PrUsdSpecIndex parent, PrUsdSpecForm form, PrStringView name) {
    PR_ASSERT(parent < layer->specs.count);
    const PrUsdSpec& spec = layer->specs.data[parent];
    PrUsdList list = spec.properties;
    if (form == PrUsdSpecForm_Prim)
        list = spec.prims;
    else if (form == PrUsdSpecForm_VariantSet)
        list = spec.variantSets;
    else if (form == PrUsdSpecForm_Variant)
        list = spec.variants;
    for (uint32_t i = 0; i < list.count; ++i) {
        PrUsdSpecIndex child = layer->lists.data[list.first + i];
        if (layer->specs.data[child].name == name)
            return child;
    }
    return kPrUsdNone;
}

const PrUsdField* PR_NULLABLE prUsdLayerFindField(const PrUsdLayer* layer, PrUsdSpecIndex spec, PrStringView name) {
    PR_ASSERT(spec < layer->specs.count);
    return usdLayerFindField(layer, spec, name);
}

PrUsdTextSpan prUsdLayerSpecSpan(const PrUsdLayer* layer, PrUsdSpecIndex spec) {
    PR_ASSERT(spec < layer->specs.count);
    return spec < layer->textSpans.count ? layer->textSpans.data[spec] : PrUsdTextSpan{};
}

PrUsdSpecIndex prUsdLayerSpecFromOffset(const PrUsdLayer* layer, uint32_t offset) {
    const RawArray<PrUsdSpecIndex>& order = layer->textOrder;
    const PrUsdTextSpan* spans = layer->textSpans.data;
    // The last spec whose text begins at or before the offset holds it, or is inside a spec that holds it, or ended
    // before it. Spans nest as specs do, so the innermost spec that holds the offset is that spec or an ancestor.
    uint32_t low = 0;
    uint32_t high = order.count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (spans[order.data[middle]].begin <= offset)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == 0)
        return kPrUsdNone;
    PrUsdSpecIndex at = order.data[low - 1];
    while (at != kPrUsdNone && (offset < spans[at].begin || offset >= spans[at].end))
        at = layer->specs.data[at].parent;
    return at;
}

uint32_t prUsdLayerSublayerCount(const PrUsdLayer* layer) {
    return layer->sublayers.count;
}

const PrUsdSublayer* PR_NULLABLE prUsdLayerSublayers(const PrUsdLayer* layer) {
    return layer->sublayers.data;
}

PrStatus prUsdPackageCreate(PrBlob* file, const PrUsdOptions* PR_NULLABLE options, PrUsdPackage* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrTextBuffer* diagnostics = options != nullptr ? options->diagnostics : nullptr;
    PrByteView bytes = prBlobBytes(file);
    PrZip* zip = prZipCreate(nullptr);
    PrStatus status = prZipOpen(zip, bytes);
    if (status != PrStatus_Ok) {
        usdDiagnose(diagnostics, "the file is not a ZIP archive");
        prZipDestroy(zip);
        return status;
    }
    PrUsdPackage* package = allocZeroed<PrUsdPackage>();
    package->zip = zip;
    // The rules that reading does not need: the archive ends with its end record, which has no comment and no ZIP64
    // form, and each file's bytes are aligned.
    package->conforming = bytes.size >= kZipEndSize && bytes.data[bytes.size - kZipEndSize] == 'P' &&
        bytes.data[bytes.size - kZipEndSize + 1] == 'K' && bytes.data[bytes.size - kZipEndSize + 2] == 5 &&
        bytes.data[bytes.size - kZipEndSize + 3] == 6;
    if (package->conforming && bytes.size >= kZipEndSize + kZip64LocatorSize) {
        const uint8_t* locator = bytes.data + bytes.size - kZipEndSize - kZip64LocatorSize;
        if (locator[0] == 'P' && locator[1] == 'K' && locator[2] == 6 && locator[3] == 7)
            package->conforming = false;
    }
    uint32_t count = prZipEntryCount(zip);
    if (count == 0) {
        usdDiagnose(diagnostics, "the package has no files");
        status = PrStatus_InvalidData;
    }
    for (uint32_t i = 0; status == PrStatus_Ok && i < count; ++i) {
        PrZipEntry entry = prZipEntry(zip, i);
        PrUsdPackageFile packed{};
        packed.path = entry.name;
        if (entry.encrypted || entry.method != PrZipMethod_Stored) {
            usdDiagnose(diagnostics, "the package compresses or encrypts a file");
            status = PrStatus_NotSupported;
        } else if (prZipEntryData(zip, i, &packed.bytes) != PrStatus_Ok || entry.size != entry.compressedSize) {
            usdDiagnose(diagnostics, "a file of the package lies outside the archive");
            status = PrStatus_InvalidData;
        } else {
            if (uint64_t(packed.bytes.data - bytes.data) % kPackageAlignment != 0)
                package->conforming = false;
            package->files.push(packed);
        }
    }
    if (status != PrStatus_Ok) {
        prUsdPackageDestroy(package);
        return status;
    }
    prBlobRetain(file);
    package->file = file;
    *out = package;
    return PrStatus_Ok;
}

void prUsdPackageDestroy(PrUsdPackage* PR_NULLABLE package) {
    if (package == nullptr)
        return;
    prZipDestroy(package->zip);
    prBlobRelease(package->file);
    package->files.reset();
    prMemoryFree(package);
}

uint32_t prUsdPackageFileCount(const PrUsdPackage* package) {
    return package->files.count;
}

PrUsdPackageFile prUsdPackageFile(const PrUsdPackage* package, uint32_t index) {
    PR_ASSERT(index < package->files.count);
    return package->files.data[index];
}

uint32_t prUsdPackageFind(const PrUsdPackage* package, PrStringView path) {
    for (uint32_t i = 0; i < package->files.count; ++i) {
        if (package->files.data[i].path == path)
            return i;
    }
    return kPrUsdNone;
}

PrBlob* prUsdPackageOpen(const PrUsdPackage* package, uint32_t index) {
    PR_ASSERT(index < package->files.count);
    prBlobRetain(package->file);
    return prBlobCreate(package->files.data[index].bytes, releaseFile, package->file);
}

bool prUsdPackageIsConforming(const PrUsdPackage* package) {
    return package->conforming;
}

const PrBlobSource* PR_NULLABLE prUsdFileOpen(void* PR_NULLABLE, PrStringView location) {
    return prBlobSourceCreateFile(location);
}

PrRawString prUsdAssetResolve(PrStringView anchor, PrStringView assetPath) {
    RawArray<char> out{};
    PrStringView package{};
    PrStringView inner{};
    if (schemeSize(assetPath) != 0 || prFilePathRootSize(assetPath) != 0) {
        out.append(assetPath.data, uint32_t(assetPath.size));
    } else if (splitPackage(anchor, &package, &inner) || isPackageName(anchor)) {
        // A path that a layer in a package authors names a file of the package, beside the layer.
        if (inner.data == nullptr)
            package = anchor;
        out.append(package.data, uint32_t(package.size));
        out.push('[');
        uint32_t floor = out.count;
        appendParts(out, floor, prFilePathParent(inner.data != nullptr ? inner : PrStringView{""}), ParentAtFloor_Drop);
        appendParts(out, floor, assetPath, ParentAtFloor_Drop);
        out.push(']');
    } else {
        // What follows a package's name in brackets names a file in the package, and is not a part of the path.
        size_t open = prStringFindByte(assetPath, '[');
        PrStringView folder = prFilePathParent(anchor);
        out.append(folder.data, uint32_t(folder.size));
        uint32_t floor = folder.size != 0 ? uint32_t(prFilePathRootSize(anchor)) : 0;
        if (schemeSize(anchor) != 0)
            floor = uint32_t(schemeSize(anchor));
        // A relative anchor's folder is relative to one that the host knows, so a ".." that leaves it stays.
        ParentAtFloor parent = ParentAtFloor_Keep;
        if (prFilePathRootSize(anchor) != 0 || schemeSize(anchor) != 0)
            parent = ParentAtFloor_Drop;
        appendParts(out, floor, PrStringView{assetPath.data, open}, parent);
        out.append(assetPath.data + open, uint32_t(assetPath.size - open));
    }
    PrRawString location = prRawStringCopy(PrStringView{out.data, out.count});
    out.reset();
    return location;
}

PrStatus prUsdResourceSource(PrStringView location, PrUsdOpenFn PR_NULLABLE open, void* PR_NULLABLE openContext,
        const PrUsdOptions* PR_NULLABLE options, const PrBlobSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrStringView outer = location;
    PrStringView inner{};
    bool packed = splitPackage(location, &outer, &inner);
    const PrBlobSource* source = open != nullptr ? open(openContext, outer) : prUsdFileOpen(nullptr, outer);
    if (source == nullptr)
        return PrStatus_Io;
    // Each package's file is a provider over the package's.
    while (packed) {
        PrStringView name = inner;
        PrStringView next{};
        packed = splitPackage(inner, &name, &next);
        const PrBlobSource* entry = packageEntrySource(source, name, options);
        prBlobSourceRelease(source);
        source = entry;
        inner = next;
    }
    *out = source;
    return PrStatus_Ok;
}

PrStatus prUsdLayerRead(PrStringView location, PrUsdOpenFn PR_NULLABLE open, void* PR_NULLABLE openContext,
        const PrUsdOptions* PR_NULLABLE options, PrUsdLayer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrTextBuffer* diagnostics = options != nullptr ? options->diagnostics : nullptr;
    PrStringView outer = location;
    PrStringView inner{};
    bool packed = splitPackage(location, &outer, &inner);
    const PrBlobSource* source = open != nullptr ? open(openContext, outer) : prUsdFileOpen(nullptr, outer);
    PrBlob* blob = nullptr;
    if (source != nullptr)
        prBlobSourceRead(source, prBlobRangeWhole(), &blob);
    prBlobSourceRelease(source);
    if (blob == nullptr) {
        usdDiagnose(diagnostics, "the layer's file cannot be read");
        return PrStatus_Io;
    }
    // A package holds the layer, in a package of its own if the location says so. A package that is named alone gives
    // its default layer.
    PrStatus status = PrStatus_Ok;
    for (;;) {
        PrByteView bytes = prBlobBytes(blob);
        bool isArchive = bytes.size >= 4 && bytes.data[0] == 'P' && bytes.data[1] == 'K' && bytes.data[2] == 3 &&
            bytes.data[3] == 4;
        if (!packed && !isArchive)
            break;
        PrUsdPackage* package = nullptr;
        status = prUsdPackageCreate(blob, options, &package);
        prBlobRelease(blob);
        blob = nullptr;
        if (status != PrStatus_Ok)
            break;
        PrStringView name = inner;
        PrStringView next{};
        packed = packed && splitPackage(inner, &name, &next);
        uint32_t index = inner.data != nullptr ? prUsdPackageFind(package, name) : 0;
        if (index == kPrUsdNone) {
            usdDiagnose(diagnostics, "the package has no file of the name that the location gives");
            status = PrStatus_InvalidData;
        } else {
            blob = prUsdPackageOpen(package, index);
        }
        prUsdPackageDestroy(package);
        if (status != PrStatus_Ok)
            break;
        inner = next;
    }
    if (status == PrStatus_Ok)
        status = prUsdLayerCreate(blob, options, out);
    prBlobRelease(blob);
    return status;
}
