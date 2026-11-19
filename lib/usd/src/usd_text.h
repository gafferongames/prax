// The text format of layers, .usda (usd-core/#text).

#pragma once

#include "pr/base.h"
#include "pr/status.h"

#include "usd/src/usd_layer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Reads the text layer in `text` into `layer`, which holds the layer spec and nothing else. Returns Syntax for text
// that the grammar does not accept, InvalidData for text that parses and breaks another rule of the specification, and
// NotSupported for a version of the format, or a part of it, that the module does not read. A failure writes a sentence
// that begins with the line to `diagnostics`.
PrStatus usdTextRead(UsdLayer* layer, PrStringView text, PrTextBuffer* PR_NULLABLE diagnostics);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
