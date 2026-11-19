// The address handler of USD layers and packages.

#include "apps/scribe/usd_address.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/construct.h"
#include "pr/string_ops.h"

namespace scribe {

namespace {

struct UsdHandler : AddressHandler {
    const PrCompressCodecSet* PR_NULLABLE codec = nullptr;
};

// An opened layer or package: the stage that it composes, its files if it is a package, and the location that the stage
// knows it by.
struct UsdState {
    PrUsdStage* PR_NULLABLE stage = nullptr;
    PrUsdPackage* PR_NULLABLE package = nullptr;
    String root;
};

// What the stage's reader reads with while a node opens.
struct UsdRead {
    AddressNode* node;
    PrStringView root;
};

// Opens a resource that composition reaches: the node's own bytes for the root location, and otherwise a resource
// relative to the node. A location in a package, `root.usdz[file]`, opens the package, which lib/usd reads itself. The
// node's resolver reads the resource while it is opened, on the resolver's thread, so the provider holds its bytes and
// any thread reads them.
const PrBlobSource* PR_NULLABLE usdOpenResource(void* PR_NULLABLE context, PrStringView location) {
    const UsdRead* read = static_cast<const UsdRead*>(context);
    if (location == read->root)
        return prBlobSourceCreateBlob(addressNodeBytes(read->node));
    PrBlob* blob = nullptr;
    addressNodeReadRelative(read->node, location, &blob);
    if (blob == nullptr)
        return nullptr;
    const PrBlobSource* source = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    return source;
}

bool usdOpens(const AddressHandler*, PrMimeFormat format) {
    return format == PrMimeFormat_Usda || format == PrMimeFormat_Usdc || format == PrMimeFormat_Usdz;
}

void usdClose(AddressHandler*, void* PR_NULLABLE state) {
    UsdState* usd = static_cast<UsdState*>(state);
    prUsdStageDestroy(usd->stage);
    prUsdPackageDestroy(usd->package);
    destroy(usd);
}

PrStatus usdOpen(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE* PR_NONNULL state) {
    PrUsdOptions layer = prUsdOptionsDefault();
    layer.codec = static_cast<UsdHandler*>(handler)->codec;
    bool packaged = addressNodeFormat(node) == PrMimeFormat_Usdz;
    PrUsdPackage* package = nullptr;
    if (packaged) {
        PrStatus status = prUsdPackageCreate(addressNodeBytes(node), &layer, &package);
        if (status != PrStatus_Ok)
            return status;
    }
    UsdState* usd = create<UsdState>();
    usd->package = package;
    // The stage knows the root layer by the node's name, which anchors what the layer names relative to the node's own
    // folder. Brackets would read as a package's syntax, and a package is known by its extension.
    PrStringView name = addressNodeName(node);
    for (size_t i = 0; i < name.size; ++i) {
        char c = name.data[i];
        usd->root.append(c == '[' || c == ']' ? '_' : c);
    }
    if (usd->root.isEmpty())
        usd->root = "root";
    if (packaged && !prAsciiEndsWithFold(usd->root, ".usdz"))
        usd->root.append(".usdz");
    UsdRead read{.node = node, .root = usd->root};
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer = layer;
    options.open = usdOpenResource;
    options.openContext = &read;
    PrStatus status = prUsdStageCreate(usd->root, &options, &usd->stage);
    if (status != PrStatus_Ok) {
        usdClose(handler, usd);
        return status;
    }
    *state = usd;
    return PrStatus_Ok;
}

PrStatus usdStep(AddressHandler*, AddressNode*, void* PR_NULLABLE state, PrStringView step, uint64_t, AddressStepResult* out) {
    const UsdState* usd = static_cast<const UsdState*>(state);
    if (prStringStartsWith(step, "/")) {
        PrUsdPrimIndex prim = kPrUsdNone;
        PrStatus status = prUsdStageFindPath(usd->stage, step, &prim);
        if (status == PrStatus_Ok)
            out->kind = AddressNodeKind_Location;
        return status;
    }
    if (usd->package == nullptr)
        return PrStatus_Syntax;
    uint32_t index = prUsdPackageFind(usd->package, step);
    if (index == kPrUsdNone)
        return PrStatus_NotFound;
    out->kind = AddressNodeKind_Resource;
    // A package's files are stored, and the blob shares the package's bytes.
    out->bytes = prUsdPackageOpen(usd->package, index);
    size_t slash = prStringFindLastByte(step, '/');
    out->name = slash == step.size ? step : prStringFrom(step, slash + 1);
    return PrStatus_Ok;
}

uint32_t usdChildCount(const AddressHandler*, const void* PR_NULLABLE state) {
    const UsdState* usd = static_cast<const UsdState*>(state);
    return usd->package != nullptr ? prUsdPackageFileCount(usd->package) : 0;
}

AddressChild usdChild(const AddressHandler*, const void* PR_NULLABLE state, uint32_t index) {
    PrUsdPackageFile file = prUsdPackageFile(static_cast<const UsdState*>(state)->package, index);
    return AddressChild{.step = file.path, .path = file.path, .size = file.bytes.size};
}

bool usdSibling(const AddressHandler*, const void* PR_NULLABLE state, PrStringView step, PrStringView reference,
        PrTextBuffer* out) {
    // Only a package holds files, and its steps are their paths.
    if (static_cast<const UsdState*>(state)->package == nullptr)
        return false;
    return addressPathJoin(step, reference, out);
}

void usdDestroy(AddressHandler* handler) {
    UsdHandler* usd = static_cast<UsdHandler*>(handler);
    destroy(usd);
}

// Returns the state of a node that the USD handler opens, opening it if it is not open, or null.
const UsdState* PR_NULLABLE stateOf(AddressNode* node) {
    const AddressHandler* handler = addressNodeHandler(node);
    if (handler == nullptr || handler->openFn != usdOpen || addressNodeOpen(node) != PrStatus_Ok)
        return nullptr;
    return static_cast<const UsdState*>(addressNodeState(node, handler));
}

} // namespace

AddressHandler* usdAddressHandlerCreate(const PrCompressCodecSet* PR_NULLABLE codec) {
    UsdHandler* handler = create<UsdHandler>();
    handler->destroyFn = usdDestroy;
    handler->opensFn = usdOpens;
    handler->openFn = usdOpen;
    handler->closeFn = usdClose;
    handler->stepFn = usdStep;
    handler->childCountFn = usdChildCount;
    handler->childFn = usdChild;
    handler->siblingFn = usdSibling;
    handler->codec = codec;
    return handler;
}

const PrUsdStage* PR_NULLABLE usdAddressStage(AddressNode* node) {
    const UsdState* usd = stateOf(node);
    return usd != nullptr ? usd->stage : nullptr;
}

const PrBlobSource* PR_NULLABLE usdAddressOpen(void* PR_NULLABLE context, PrStringView location) {
    AddressNode* node = static_cast<AddressNode*>(context);
    const UsdState* usd = stateOf(node);
    if (usd == nullptr)
        return nullptr;
    UsdRead read{.node = node, .root = usd->root};
    return usdOpenResource(&read, location);
}

} // namespace scribe
