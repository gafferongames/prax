// The address handler of USD layers and packages.
//
// A layer, text or crate, composes a stage with itself as the root layer, and a package composes one from its default
// layer. A step that begins with `/` is a scene path of that stage (prUsdStageFindPath), which is a location: a prim, a
// property, a variant set or a variant selection. In a package, any other step is the name of one of its files, which
// reaches the file's bytes. What a layer names, its sublayers, references and payloads, is read relative to its
// address: beside it in its container or its folder, or in its package.

#pragma once

#include "pr/compress.h"
#include "pr/usd_stage.h"

#include "apps/scribe/address.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

// Creates the handler of USD layers and packages, which decodes crate files' LZ4 blocks with `codec`. Without a codec,
// a crate file does not open. `codec` outlives the handler. The caller frees it with addressHandlerDestroy.
AddressHandler* usdAddressHandlerCreate(const PrCompressCodecSet* PR_NULLABLE codec);

// Returns the stage that a node of the USD handler composes, opening the node if it is not open, or null if the node is
// not a USD layer or package or its root layer does not read. The stage is valid while the node is held.
const PrUsdStage* PR_NULLABLE usdAddressStage(AddressNode* node);

// Opens a resource that the stage of a node of the USD handler names, as a PrUsdOpenFn whose context is the node: the
// node's own bytes for its stage's root layer, and otherwise what the location names relative to the node, as the stage
// read its layers. Returns null if the node is not a USD layer or package, or the resource does not read. The node's
// resolver is used by one thread, so the opener is called on that thread alone, and it reads the resource as it opens
// it: the provider holds the bytes, and any thread reads it.
const PrBlobSource* PR_NULLABLE usdAddressOpen(void* PR_NULLABLE context, PrStringView location);

} // namespace scribe

PR_ASSUME_NONNULL_END
