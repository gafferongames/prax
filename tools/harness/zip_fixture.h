// ZIP archives that tests build: stored or deflated files, each stored file's bytes aligned to 64 bytes as a USD
// package's are (usd-core/#package).

#pragma once

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/checksum.h"
#include "pr/compress.h"
#include "pr/mem.h"
#include "pr/span.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// A file of an archive that a test builds.
struct ZipFixtureFile {
    PrStringView name;
    PrStringView bytes;
    bool deflate = false;           // compressed with Deflate, through the codec that builds the archive
};

namespace zip_fixture {

// Append a little-endian integer of 16 or 32 bits.
inline void put16(String& out, uint32_t value) {
    out.append(1, char(value & 0xff));
    out.append(1, char((value >> 8) & 0xff));
}
inline void put32(String& out, uint32_t value) {
    put16(out, value & 0xffff);
    put16(out, value >> 16);
}

} // namespace zip_fixture

// Returns the bytes of an archive of `files`, in order. A deflated file is compressed with `codec`, which must encode
// Deflate.
inline String zipFixture(Span<const ZipFixtureFile> files, PrCompressCodecSet* PR_NULLABLE codec = nullptr) {
    using namespace zip_fixture;
    String out;
    String directory;
    for (const ZipFixtureFile& file : files) {
        PrByteView input{reinterpret_cast<const uint8_t*>(file.bytes.data), file.bytes.size};
        uint32_t crc = prCrc32Update(0, input);
        String data;
        uint16_t method = 0;
        if (file.deflate) {
            PR_ASSERT(codec != nullptr);
            Array<uint8_t> encoded(uint32_t(prCompressCodecSetEncodeBound(codec, PrCompressFormat_Deflate,
                input.size)));
            size_t written = 0;
            PrStatus status = prCompressCodecSetEncode(codec, PrCompressFormat_Deflate, PrCompressLevel_Default, input,
                encoded.data(), &written);
            PR_ASSERT(status == PrStatus_Ok);
            (void)status;
            data = PrStringView{reinterpret_cast<const char*>(encoded.data()), written};
            method = 8;
        } else {
            data = file.bytes;
        }
        uint32_t offset = out.count();
        // An extra field pads a stored file's bytes to a multiple of 64.
        uint32_t extra = 0;
        if (!file.deflate) {
            uint32_t start = offset + 30 + uint32_t(file.name.size);
            extra = (64 - start % 64) % 64;
            if (extra != 0 && extra < 4)
                extra += 64;
        }
        String* records[] = {&out, &directory};
        for (String* record : records) {
            bool central = record == &directory;
            put32(*record, central ? 0x02014b50 : 0x04034b50);
            if (central)
                put16(*record, 20);
            put16(*record, 20);
            put16(*record, 0);
            put16(*record, method);
            put32(*record, 0);
            put32(*record, crc);
            put32(*record, data.count());
            put32(*record, uint32_t(file.bytes.size));
            put16(*record, uint32_t(file.name.size));
            put16(*record, central ? 0 : extra);
            if (central) {
                put16(*record, 0);
                put16(*record, 0);
                put16(*record, 0);
                put32(*record, 0);
                put32(*record, offset);
            }
            record->append(file.name);
            if (!central && extra != 0) {
                put16(*record, 0xcafe);
                put16(*record, extra - 4);
                record->append(extra - 4, '\0');
            }
        }
        out.append(data);
    }
    uint32_t start = out.count();
    out.append(directory);
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, uint32_t(files.count()));
    put16(out, uint32_t(files.count()));
    put32(out, directory.count());
    put32(out, start);
    put16(out, 0);
    return out;
}

} // namespace harness

PR_ASSUME_NONNULL_END
