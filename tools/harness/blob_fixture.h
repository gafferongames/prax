// Byte providers of the tests: a provider over bytes that a test reads into a blob.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Returns a provider of `blob`'s bytes, which takes the blob's reference, or null for no blob.
inline const PrBlobSource* PR_NULLABLE harnessBlobSource(PrBlob* PR_NULLABLE blob) {
    if (blob == nullptr)
        return nullptr;
    const PrBlobSource* source = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    return source;
}

} // namespace harness

PR_ASSUME_NONNULL_END
