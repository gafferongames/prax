// The images of a folder.

#include "apps/pandite/folder.h"

#include "pr/file.h"
#include "pr/mime_sniff.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

namespace pandite {

namespace {

// The state of a listing: the codecs that must decode a name, and the names that they decode.
struct Listing {
    const PrImageCodecSet* codecs;
    Array<String>* names;
};

void collect(void* PR_NULLABLE context, const PrFileEntry* entry) {
    Listing& listing = *static_cast<Listing*>(context);
    if (entry->info.kind == PrFileKind_Regular && folderShows(listing.codecs, entry->name))
        listing.names->push(String(entry->name));
}

} // namespace

bool folderShows(const PrImageCodecSet* codecs, PrStringView name) {
    PrMimeFormat format = prMimeFormatOfFileName(name);
    PrMimeFormatInfo info = prMimeFormatInfo(format);
    return info.kind == PrMimeKind_Image && prImageCodecSetSupportsType(codecs, info.type);
}

bool folderList(PrStringView directory, const PrImageCodecSet* codecs, Folder* out) {
    out->directory = directory;
    out->names.clear();
    Listing listing{.codecs = codecs, .names = &out->names};
    if (!prFileEachEntry(directory, collect, &listing)) {
        out->names.clear();
        return false;
    }
    sortItems(out->names.span(), [](const String& a, const String& b) { return prStringCompareNatural(a, b) < 0; });
    return true;
}

uint32_t folderFind(const Folder& folder, PrStringView name) {
    for (uint32_t i = 0; i < folder.names.count(); ++i) {
        if (folder.names[i] == name)
            return i;
    }
    return kNoImage;
}

uint32_t folderNearest(const Folder& folder, PrStringView name) {
    uint32_t count = folder.names.count();
    if (count == 0)
        return kNoImage;
    for (uint32_t i = 0; i < count; ++i) {
        if (prStringCompareNatural(folder.names[i], name) >= 0)
            return i;
    }
    return count - 1;
}

String folderPath(const Folder& folder, uint32_t index) {
    PrTextBuffer path{};
    prTextBufferAppend(&path, folder.directory);
    prFilePathAppend(&path, folder.names[index]);
    return String::adopt(prTextBufferTake(&path));
}

} // namespace pandite
