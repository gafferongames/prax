#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/raw_string.h"

#include "usd/tests/usd_fixture.h"

using namespace harness;

namespace pr_impl {

namespace {

constexpr PrStringView kRootLayer = "#usda 1.0\n(\n    subLayers = [@./sub/geo.usda@]\n)\ndef \"Root\" {}\n";
constexpr PrStringView kGeoLayer = "#usda 1.0\ndef \"Geo\" {}\n";

// A ZIP archive that a test writes, whose files are stored as they are.
struct ZipWriter {
    Array<uint8_t> bytes;
    Array<uint8_t> directory;
    uint16_t count = 0;

    void put16(Array<uint8_t>& out, uint32_t value) {
        out.push(uint8_t(value));
        out.push(uint8_t(value >> 8));
    }
    void put32(Array<uint8_t>& out, uint32_t value) {
        put16(out, value & 0xffff);
        put16(out, value >> 16);
    }

    // Adds a file whose bytes begin at a multiple of `alignment`, by padding the extra field of its header, as the
    // specification has a package's writer do. `method` is the method that the headers state.
    void add(PrStringView name, PrStringView content, uint32_t alignment = 64, uint16_t method = 0) {
        uint32_t header = bytes.count();
        uint32_t data = header + 30 + uint32_t(name.size);
        uint32_t extra = (alignment - data % alignment) % alignment;
        uint32_t crc = prCrc32Update(0, PrByteView{reinterpret_cast<const uint8_t*>(content.data), content.size});
        for (Array<uint8_t>* out : usdEach(&bytes, &directory)) {
            bool central = out == &directory;
            put32(*out, central ? 0x02014b50 : 0x04034b50);
            if (central)
                put16(*out, 20);
            put16(*out, 20);
            put16(*out, 0);
            put16(*out, method);
            put32(*out, 0);
            put32(*out, crc);
            put32(*out, uint32_t(content.size));
            put32(*out, uint32_t(content.size));
            put16(*out, uint32_t(name.size));
            put16(*out, central ? 0 : extra);
            if (central) {
                put16(*out, 0);
                put16(*out, 0);
                put16(*out, 0);
                put32(*out, 0);
                put32(*out, header);
            }
            out->append(reinterpret_cast<const uint8_t*>(name.data), uint32_t(name.size));
        }
        for (uint32_t i = 0; i < extra; ++i)
            bytes.push(0);
        bytes.append(reinterpret_cast<const uint8_t*>(content.data), uint32_t(content.size));
        ++count;
    }

    // Appends the directory and the end record, with a comment of `comment` bytes, and returns the archive.
    PrByteView finish(uint16_t comment = 0) {
        uint32_t start = bytes.count();
        bytes.append(directory.data(), directory.count());
        put32(bytes, 0x06054b50);
        put16(bytes, 0);
        put16(bytes, 0);
        put16(bytes, count);
        put16(bytes, count);
        put32(bytes, directory.count());
        put32(bytes, start);
        put16(bytes, comment);
        for (uint16_t i = 0; i < comment; ++i)
            bytes.push('c');
        return PrByteView{bytes.data(), bytes.count()};
    }
};

// A package read from a test's bytes, with the status of the read. It frees the package.
struct PackageRead {
    PrUsdPackage* PR_NULLABLE package = nullptr;
    PrStatus status = PrStatus_Ok;

    explicit PackageRead(PrByteView bytes) {
        PrBlob* file = prBlobCopy(bytes);
        status = prUsdPackageCreate(file, nullptr, &package);
        prBlobRelease(file);
    }
    ~PackageRead() {
        prUsdPackageDestroy(package);
    }
};

// The files that a reader gives a layer's reading, by location.
struct Files {
    PrByteView package;
    uint32_t reads = 0;
};

PrBlob* PR_NULLABLE readFile(void* PR_NULLABLE context, PrStringView location) {
    Files* files = static_cast<Files*>(context);
    ++files->reads;
    if (location == "folder/scene.usdz")
        return prBlobCopy(files->package);
    if (location == "folder/geo.usda")
        return prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(kGeoLayer.data), kGeoLayer.size});
    return nullptr;
}

const PrBlobSource* PR_NULLABLE openFile(void* PR_NULLABLE context, PrStringView location) {
    return harnessBlobSource(readFile(context, location));
}

// Reads the resource at `location` whole through the providers that prUsdResourceSource composes for it.
PrStatus readResource(PrStringView location, Files* files, PrBlob* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrBlobSource* source = nullptr;
    PrStatus status = prUsdResourceSource(location, openFile, files, nullptr, &source);
    if (status == PrStatus_Ok)
        status = prBlobSourceRead(source, prBlobRangeWhole(), out);
    prBlobSourceRelease(source);
    return status;
}

String resolved(PrStringView anchor, PrStringView assetPath) {
    PrRawString location = prUsdAssetResolve(anchor, assetPath);
    String text(prRawStringView(&location));
    prRawStringReset(&location);
    return text;
}

} // namespace

} // namespace pr_impl

using namespace harness;
using namespace pr_impl;

TEST_CASE("a package's files are read in place, and its first file is its default layer") {
    ZipWriter zip;
    zip.add("root.usda", kRootLayer);
    zip.add("sub/geo.usda", kGeoLayer);
    zip.add("textures/a.png", "not really an image");
    PackageRead read(zip.finish());
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prUsdPackageIsConforming(read.package));
    REQUIRE(prUsdPackageFileCount(read.package) == 3);
    PrUsdPackageFile root = prUsdPackageFile(read.package, 0);
    CHECK(root.path == "root.usda");
    CHECK(PrStringView{reinterpret_cast<const char*>(root.bytes.data), root.bytes.size} == kRootLayer);
    CHECK(prUsdPackageFind(read.package, "sub/geo.usda") == 1);
    CHECK(prUsdPackageFind(read.package, "sub/GEO.usda") == kPrUsdNone);
    CHECK(prUsdPackageFile(read.package, 2).bytes.size == 19);

    // A file's blob keeps the package's bytes alive after the package is destroyed.
    PrBlob* geo = prUsdPackageOpen(read.package, 1);
    prUsdPackageDestroy(read.package);
    read.package = nullptr;
    PrUsdLayer* layer = nullptr;
    REQUIRE(prUsdLayerCreate(geo, nullptr, &layer) == PrStatus_Ok);
    prBlobRelease(geo);
    CHECK(prUsdLayerFindSpec(layer, "/Geo") != kPrUsdNone);
    prUsdLayerDestroy(layer);
}

TEST_CASE("a package that breaks the specification's layout is read, and says so") {
    ZipWriter unaligned;
    unaligned.add("root.usda", kRootLayer, 1);
    unaligned.add("sub/geo.usda", kGeoLayer, 1);
    PackageRead first(unaligned.finish());
    REQUIRE(first.status == PrStatus_Ok);
    CHECK(!prUsdPackageIsConforming(first.package));
    CHECK(prUsdPackageFileCount(first.package) == 2);

    ZipWriter commented;
    commented.add("root.usda", kRootLayer);
    PackageRead second(commented.finish(3));
    REQUIRE(second.status == PrStatus_Ok);
    CHECK(!prUsdPackageIsConforming(second.package));
}

TEST_CASE("a file that is not a package of stored files is refused") {
    ZipWriter deflated;
    deflated.add("root.usda", kRootLayer, 64, 8);
    CHECK(PackageRead(deflated.finish()).status == PrStatus_NotSupported);

    ZipWriter empty;
    CHECK(PackageRead(empty.finish()).status == PrStatus_InvalidData);
    CHECK(PackageRead(PrByteView{reinterpret_cast<const uint8_t*>(kGeoLayer.data), kGeoLayer.size}).status ==
        PrStatus_InvalidData);

    // A file whose bytes lie outside the archive, and an archive cut short at every length.
    ZipWriter outside;
    outside.add("root.usda", kRootLayer);
    PrByteView archive = outside.finish();
    Array<uint8_t> changed;
    changed.append(archive.data, uint32_t(archive.size));
    uint32_t directory = uint32_t(archive.size) - 22 - outside.directory.count();
    changed[directory + 42] = 0xf0;
    changed[directory + 43] = 0xff;
    CHECK(PackageRead(PrByteView{changed.data(), changed.count()}).status == PrStatus_InvalidData);
    for (size_t size = 0; size < archive.size; ++size) {
        CAPTURE(size);
        CHECK(PackageRead(PrByteView{archive.data, size}).status != PrStatus_Ok);
    }

    // A package is not a layer.
    ZipWriter zip;
    zip.add("root.usda", kRootLayer);
    UsdRead layer = usdReadBytes(zip.finish());
    CHECK(layer.status == PrStatus_NotSupported);
    CHECK(layer.layer == nullptr);
}

TEST_CASE("an asset path resolves against the layer that authors it") {
    // Anchored paths, and the other relative paths, which the module resolves the same way.
    CHECK(resolved("/hello/world.usda", "./goodbye.txt") == "/hello/goodbye.txt");
    CHECK(resolved("/hello/world.usda", "goodbye.txt") == "/hello/goodbye.txt");
    CHECK(resolved("/a/b/c.usda", "../d/./e.usda") == "/a/d/e.usda");
    CHECK(resolved("/a/b/c.usda", "../../../../e.usda") == "/e.usda");
    CHECK(resolved("a/b.usda", "c.usda") == "a/c.usda");
    CHECK(resolved("b.usda", "./c.usda") == "c.usda");
    // A relative anchor's folder is relative to one that the host knows, so a path may leave it.
    CHECK(resolved("b.usda", "../c.usda") == "../c.usda");
    CHECK(resolved("a/b.usda", "../../c/d.usda") == "../c/d.usda");
    CHECK(resolved("../a/b.usda", "../../c.usda") == "../../c.usda");
    // An absolute path and a path with a scheme are their own locations.
    CHECK(resolved("/a/b.usda", "/x/y.usda") == "/x/y.usda");
    CHECK(resolved("/a/b.usda", "https://example.com/y.usda") == "https://example.com/y.usda");
    CHECK(resolved("https://example.com/a/b.usda", "../c.usda") == "https://example.com/c.usda");
    // A file in a package, from outside the package and from a layer inside it.
    CHECK(resolved("/a/b.usda", "pack.usdz[geo/c.usda]") == "/a/pack.usdz[geo/c.usda]");
    CHECK(resolved("/a/pack.usdz[layers/root.usda]", "./tex/a.png") == "/a/pack.usdz[layers/tex/a.png]");
    CHECK(resolved("/a/pack.usdz[layers/root.usda]", "../geo.usda") == "/a/pack.usdz[geo.usda]");
    CHECK(resolved("/a/pack.usdz[root.usda]", "../../geo.usda") == "/a/pack.usdz[geo.usda]");
    CHECK(resolved("/a/pack.usdz", "geo.usda") == "/a/pack.usdz[geo.usda]");
}

TEST_CASE("a layer is read from a location through the host's reader") {
    ZipWriter zip;
    zip.add("root.usda", kRootLayer);
    zip.add("sub/geo.usda", kGeoLayer);
    Files files;
    files.package = zip.finish();
    auto read = [&](PrStringView location, PrStringView path) {
        PrUsdLayer* layer = nullptr;
        PrStatus status = prUsdLayerRead(location, openFile, &files, nullptr, &layer);
        bool found = layer != nullptr && prUsdLayerFindSpec(layer, path) != kPrUsdNone;
        prUsdLayerDestroy(layer);
        CHECK((status == PrStatus_Ok) == found);
        return status;
    };
    CHECK(read("folder/geo.usda", "/Geo") == PrStatus_Ok);
    // A package alone gives its default layer, and a location in brackets names a file of it.
    CHECK(read("folder/scene.usdz", "/Root") == PrStatus_Ok);
    CHECK(read("folder/scene.usdz[sub/geo.usda]", "/Geo") == PrStatus_Ok);
    CHECK(read("folder/scene.usdz[missing.usda]", "/Geo") == PrStatus_InvalidData);
    CHECK(read("folder/missing.usda", "/Geo") == PrStatus_Io);
    CHECK(read("folder/geo.usda[inner.usda]", "/Geo") == PrStatus_InvalidData);

    // The sublayer of the package's default layer resolves to a file of the package.
    PrUsdLayer* root = nullptr;
    REQUIRE(prUsdLayerRead("folder/scene.usdz", openFile, &files, nullptr, &root) == PrStatus_Ok);
    REQUIRE(prUsdLayerSublayerCount(root) == 1);
    String sublayer = resolved("folder/scene.usdz[root.usda]", prUsdLayerSublayers(root)[0].assetPath);
    prUsdLayerDestroy(root);
    CHECK(sublayer == "folder/scene.usdz[sub/geo.usda]");
    CHECK(read(sublayer, "/Geo") == PrStatus_Ok);
}

TEST_CASE("a resource that a layer names is read through the packages of its location") {
    ZipWriter zip;
    zip.add("root.usda", kRootLayer);
    zip.add("textures/wood.png", "PNG bytes");
    Files files;
    files.package = zip.finish();
    PrBlob* blob = nullptr;
    REQUIRE(readResource("folder/scene.usdz[textures/wood.png]", &files, &blob) == PrStatus_Ok);
    PrByteView bytes = prBlobBytes(blob);
    CHECK(PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size} == "PNG bytes");
    prBlobRelease(blob);
    // A package named alone is the package's own bytes, which a resource is, where a layer would be its default layer.
    REQUIRE(readResource("folder/scene.usdz", &files, &blob) == PrStatus_Ok);
    CHECK(prBlobBytes(blob).size == files.package.size);
    prBlobRelease(blob);
    CHECK(readResource("folder/scene.usdz[missing.png]", &files, &blob) == PrStatus_InvalidData);
    CHECK(blob == nullptr);
    CHECK(readResource("folder/missing.png", &files, &blob) == PrStatus_Io);

    // A file's provider finds the file in the package at its first read, and then reads the file's range of the package
    // alone, as ranges of its own.
    const PrBlobSource* wood = nullptr;
    REQUIRE(prUsdResourceSource("folder/scene.usdz[textures/wood.png]", openFile, &files, nullptr, &wood) ==
        PrStatus_Ok);
    REQUIRE(prBlobSourceRead(wood, prBlobRangeWhole(), &blob) == PrStatus_Ok);
    prBlobRelease(blob);
    REQUIRE(prBlobSourceRead(wood, PrBlobRange{4, 3}, &blob) == PrStatus_Ok);
    bytes = prBlobBytes(blob);
    CHECK(PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size} == "byt");
    prBlobRelease(blob);
    uint8_t storage[3] = {};
    PrBlobReadDesc into{.range = {0, 3}, .storage = storage};
    PrBlobRead* read = prBlobReadCreate(wood, &into);
    prBlobReadWait(read);
    CHECK(prBlobReadStatus(read) == PrStatus_Ok);
    CHECK(PrStringView{reinterpret_cast<const char*>(storage), 3} == "PNG");
    prBlobReadDestroy(read);
    prBlobSourceRelease(wood);
}

TEST_CASE("without a reader, a layer is read from the file system") {
    String path = testFolder();
    PrTextBuffer joined = {};
    prTextBufferAppend(&joined, path);
    prFilePathAppend(&joined, "layer.usda");
    String file(prTextBufferView(&joined));
    prTextBufferReset(&joined);
    REQUIRE(prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>(kGeoLayer.data), kGeoLayer.size}));
    PrUsdLayer* layer = nullptr;
    REQUIRE(prUsdLayerRead(file, nullptr, nullptr, nullptr, &layer) == PrStatus_Ok);
    CHECK(prUsdLayerFindSpec(layer, "/Geo") != kPrUsdNone);
    prUsdLayerDestroy(layer);
    prFileRemove(file);
    CHECK(prUsdLayerRead(file, nullptr, nullptr, nullptr, &layer) == PrStatus_Io);
    CHECK(layer == nullptr);
}
