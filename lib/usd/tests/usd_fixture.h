// The fixture of the USD tests: layers written in a test's own text, and what they read as.

#pragma once

#include "pr/compress_select.h"
#include "pr/string.h"
#include "pr/text_buffer.h"
#include "pr/usd.h"

#include "tools/harness/blob_fixture.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace pr;

// A layer read from a test's bytes, with the status and the sentence of the read. It frees the layer.
struct UsdRead {
    PrUsdLayer* PR_NULLABLE layer = nullptr;
    PrStatus status = PrStatus_Ok;
    String diagnostics;

    UsdRead() = default;
    UsdRead(UsdRead&& other) : layer(other.layer), status(other.status), diagnostics(move(other.diagnostics)) {
        other.layer = nullptr;
    }
    UsdRead& operator=(UsdRead&& other) {
        prUsdLayerDestroy(layer);
        layer = other.layer;
        status = other.status;
        diagnostics = move(other.diagnostics);
        other.layer = nullptr;
        return *this;
    }
    ~UsdRead() {
        prUsdLayerDestroy(layer);
    }

    // The spec at `path`, which the layer must have.
    const PrUsdSpec& spec(PrStringView path) const {
        PrUsdSpecIndex index = prUsdLayerFindSpec(layer, path);
        REQUIRE(index != kPrUsdNone);
        return prUsdLayerSpecs(layer)[index];
    }

    // The value of the field `name` of the spec at `path`, both of which the layer must have.
    const PrUsdValue& value(PrStringView path, PrStringView name) const {
        const PrUsdField* field = prUsdLayerFindField(layer, prUsdLayerFindSpec(layer, path), name);
        REQUIRE(field != nullptr);
        return field->value;
    }

    // Returns true if the spec at `path` authors the field `name`.
    bool has(PrStringView path, PrStringView name) const {
        return prUsdLayerFindField(layer, prUsdLayerFindSpec(layer, path), name) != nullptr;
    }
};

// A few values to loop over, which a braced list cannot be without the standard library.
template <typename T, uint32_t N>
struct UsdEach {
    T items[N];

    const T* PR_NONNULL begin() const {
        return items;
    }
    const T* PR_NONNULL end() const {
        return items + N;
    }
};

// Returns the values given, each as the type of the first, to loop over.
template <typename T, typename... Rest>
UsdEach<T, 1 + sizeof...(Rest)> usdEach(T first, Rest... rest) {
    return {{first, T(rest)...}};
}

// Reads the layer in `bytes`, with the built-in codec.
inline UsdRead usdReadBytes(PrByteView bytes) {
    UsdRead read;
    PrTextBuffer diagnostics = {};
    PrUsdOptions options = prUsdOptionsDefault();
    options.codec = prCompressCodecSetCreateDefault();
    options.diagnostics = &diagnostics;
    PrBlob* file = prBlobCopy(bytes);
    read.status = prUsdLayerCreate(file, &options, &read.layer);
    prBlobRelease(file);
    read.diagnostics = String(prTextBufferView(&diagnostics));
    prTextBufferReset(&diagnostics);
    return read;
}

// Reads the text layer that `body` makes after the header of version 1.0.
inline UsdRead usdReadText(PrStringView body) {
    String text("#usda 1.0\n");
    text.append(body);
    return usdReadBytes(PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.count()});
}

// The one element of a value that is not an array, as a T.
template <typename T>
const T& usdScalar(const PrUsdValue& value) {
    REQUIRE(!value.isArray);
    REQUIRE(value.count == 1);
    return *static_cast<const T*>(value.data);
}

// The elements of an array, as Ts.
template <typename T>
const T* PR_NULLABLE usdElements(const PrUsdValue& value, uint32_t count) {
    REQUIRE(value.isArray);
    REQUIRE(value.count == count);
    return static_cast<const T*>(value.data);
}

// The list op that a value holds.
inline const PrUsdListOp& usdListOp(const PrUsdValue& value) {
    REQUIRE(value.type == PrUsdType_ListOp);
    return *static_cast<const PrUsdListOp*>(value.data);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
