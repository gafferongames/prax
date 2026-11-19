// A resolver of addresses (apps/scribe/address.h) over files in memory, which the tests of the formats' handlers share.

#pragma once

#include "pr/array.h"
#include "pr/map.h"
#include "pr/span.h"
#include "pr/string.h"

#include "apps/scribe/address.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

// Files by URL, and a resolver that reads them with the handlers that a test gives. The fixture destroys the handlers.
struct AddressFixture {
    Map<String, String> files;
    Map<String, uint64_t> stamps;
    Array<AddressHandler*> handlers;
    AddressResolver* PR_NULLABLE resolver = nullptr;
    uint32_t reads = 0;

    explicit AddressFixture(Span<AddressHandler* const> used, AddressLimits limits = {}) {
        for (AddressHandler* handler : used)
            handlers.push(handler);
        AddressResolverDesc desc{
            .handlers = handlers.span(),
            .read = [](void* PR_NULLABLE context, PrStringView url) -> PrBlob* PR_NULLABLE {
                AddressFixture& fixture = *static_cast<AddressFixture*>(context);
                if (!fixture.files.contains(url))
                    return nullptr;
                ++fixture.reads;
                PrStringView text = fixture.files[url];
                return prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
            },
            .stamp = [](void* PR_NULLABLE context, PrStringView url) -> uint64_t {
                AddressFixture& fixture = *static_cast<AddressFixture*>(context);
                return fixture.stamps.contains(url) ? fixture.stamps[url] : 0;
            },
            .context = this,
            .limits = limits,
        };
        resolver = addressResolverCreate(desc);
    }

    AddressFixture(const AddressFixture&) = delete;
    AddressFixture& operator=(const AddressFixture&) = delete;

    ~AddressFixture() {
        addressResolverDestroy(resolver);
        for (AddressHandler* handler : handlers)
            addressHandlerDestroy(handler);
    }

    // Sets the file at `url`, and changes its stamp.
    void set(PrStringView url, PrStringView bytes) {
        files[url] = bytes;
        stamps[url] = stamps.contains(url) ? stamps[url] + 1 : 1;
    }

    // Resolves the address `text`, and writes the index of the step that failed to `*failed`.
    PrStatus resolve(PrStringView text, AddressNode* PR_NULLABLE* PR_NONNULL out, uint32_t* PR_NULLABLE failed = nullptr) {
        return addressResolveText(resolver, text, out, failed);
    }

    // Returns the status of resolving `text`, and releases what it resolved to.
    PrStatus status(PrStringView text, uint32_t* PR_NULLABLE failed = nullptr) {
        AddressNode* node = nullptr;
        PrStatus result = resolve(text, &node, failed);
        addressNodeRelease(node);
        return result;
    }
};

// Returns the bytes of a resource's node as text.
inline String addressNodeText(const AddressNode* node) {
    PrByteView bytes = prBlobBytes(addressNodeBytes(node));
    return String(PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size});
}

} // namespace scribe

PR_ASSUME_NONNULL_END
