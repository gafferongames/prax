// The binary format of layers, crate, .usdc (usd-core/#binary).

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/status.h"

#include "usd/src/usd_layer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the module reads a crate file of the version, as prUsdCrateVersionIsSupported documents.
bool usdCrateVersionIsSupported(uint8_t major, uint8_t minor, uint8_t patch);

// Returns true if `bytes` begin as a crate file does.
bool usdCrateMatches(PrByteView bytes);

// Reads the crate file in `bytes` into `layer`, which holds the layer spec and nothing else, and keeps `bytes` alive
// for as long as it lives: the values of uncompressed arrays point into them. Returns InvalidData for a file that
// breaks a rule of the format, NotSupported for a version that the module does not read, for a value of a type that it
// does not read, and for a missing codec, and Limit for a compressed array that decodes to more than the module allows.
// A failure writes a sentence to `diagnostics`.
PrStatus usdCrateRead(UsdLayer* layer, PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE codec,
    PrTextBuffer* PR_NULLABLE diagnostics);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
