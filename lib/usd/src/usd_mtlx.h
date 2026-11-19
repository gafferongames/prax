// MaterialX documents (.mtlx) as layers: a document's materials, shaders and node graphs translated into a UsdShade
// network in the layout of OpenUSD's UsdMtlx file format, so that a layer composes a document by reference as it
// composes any layer.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

#include "usd/src/usd_layer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `bytes` begin as an XML document does, after a byte order mark and whitespace: with `<`, which no
// text layer or crate file begins with.
bool usdMtlxMatches(PrByteView bytes);

// Reads the MaterialX document in `text` into `layer`, which holds the layer spec and nothing else. Returns
// InvalidData, with a sentence in `diagnostics`, for a document that lib/materialx refuses.
PrStatus usdMtlxRead(UsdLayer* layer, PrStringView text, PrTextBuffer* PR_NULLABLE diagnostics);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
