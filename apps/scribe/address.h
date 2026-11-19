// Scribe's resource addresses: a URL whose fragment holds a chain of steps into nested resources, and the resolver that
// follows them through the handlers of formats.
//
// An address is `url#step!step!...`. The URL names the outermost resource, and each step is interpreted by the format
// of the resource that the steps before it reached: an entry of an archive, a JSON Pointer into a glTF asset, a scene
// path of a USD stage. A step resolves to a nested resource, whose bytes are classified (pr/mime_sniff.h) and may take
// another step, or to a location inside the resource, which ends the chain. Each step is percent-encoded, with `%`, `!`
// and `#` among the bytes that it encodes, so a step holds any byte.
//
// A resolver, its nodes and its addresses are used by one thread.

#pragma once

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/byte_view.h"
#include "pr/mime_sniff.h"
#include "pr/span.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

using namespace pr;

// ---------------------------------------------------------------------------------------------------------------------
// Addresses

// A URL and the steps after it.
struct Address {
    String url;                     // the outermost resource's URL, serialized without a fragment
    Array<String> steps;            // decoded: any bytes

    friend bool operator==(const Address& a, const Address& b) { return a.url == b.url && a.steps == b.steps; }
};

// The index of no step, which names the outermost resource.
constexpr uint32_t kAddressStepNone = 0xffffffffu;

// Parses `text` as an address into `*out`: a URL (url/#concept-basic-url-parser) and the steps of its fragment, each
// percent-decoded. A URL without a fragment, or with an empty one, has no steps. Returns Syntax if the URL does not
// parse, and leaves `*out` unchanged in that case.
PrStatus addressParse(PrStringView text, Address* out);

// Returns the address as text: the URL, and `#` and the steps, each percent-encoded and separated by `!`, if it has
// any. Parsing the text gives the same address.
String addressText(const Address& address);

// Returns the address `text` with `step` appended, or empty if `text` is no address.
String addressWithStep(PrStringView text, PrStringView step);

// ---------------------------------------------------------------------------------------------------------------------
// Resolvers and nodes

// The handler of one or more formats (AddressHandler, below).
struct AddressHandler;

// The resources and locations that addresses resolved to, kept while anything holds them, and the handlers that opened
// them.
struct AddressResolver;

// A resolved address: a resource, with its bytes, or a location inside one. A node is reference counted, and holds a
// reference to its parent, so a container stays open while anything under it is held.
struct AddressNode;

// Reads the bytes of the outermost resource at `url`. Returns a blob that the caller releases, or null if the resource
// cannot be read.
typedef PrBlob* PR_NULLABLE (*AddressReadFn)(void* PR_NULLABLE context, PrStringView url);

// Returns a stamp of the outermost resource at `url`, which changes when its bytes change, or zero if it cannot be
// read.
typedef uint64_t (*AddressStampFn)(void* PR_NULLABLE context, PrStringView url);

// The reader and the stamp of resources whose URLs are file: URLs (pr/url_file.h). The stamp combines the file's
// modification time and size.
PrBlob* PR_NULLABLE addressFileRead(void* PR_NULLABLE context, PrStringView url);
uint64_t addressFileStamp(void* PR_NULLABLE context, PrStringView url);

// The limits of resolving one address, which bound what a hostile resource can make the resolver do.
struct AddressLimits {
    uint32_t steps = 16;                    // the most steps in an address
    uint64_t bytes = uint64_t(1) << 30;     // the most bytes that a chain's steps and its relative reads decode or read
};

// What a resolver reads with and opens with.
struct AddressResolverDesc {
    // The handlers, which outlive the resolver. The first that opens a format opens each resource of that format.
    Span<AddressHandler* const> handlers;

    // Reads outermost resources. A null `stamp` gives every resource the same stamp, so polling finds no change.
    AddressReadFn read = addressFileRead;
    AddressStampFn PR_NULLABLE stamp = addressFileStamp;
    void* PR_NULLABLE context = nullptr;

    AddressLimits limits;
};

// Creates a resolver that holds nothing, or frees one. Every node must be released before its resolver is freed.
AddressResolver* addressResolverCreate(const AddressResolverDesc& desc);
void addressResolverDestroy(AddressResolver* PR_NULLABLE resolver);

// Resolves `address` and writes a node that the caller releases to `*out`. Reuses the nodes of the longest prefix of
// the address that the resolver holds, and reads, opens and steps through the rest.
//
// Returns Io if the outermost resource cannot be read, NotFound for a step that names nothing or that follows a
// location, Syntax for a step that its format's grammar does not accept, NotSupported for a step on a resource that no
// handler opens, InvalidData for a resource whose bytes do not parse, and Limit for an address past the limits. On
// failure, `*out` is null, and `*failedStep` is the index of the step that failed, or kAddressStepNone if the outermost
// resource did.
PrStatus addressResolve(AddressResolver* resolver, const Address& address, AddressNode* PR_NULLABLE* PR_NONNULL out,
    uint32_t* PR_NULLABLE failedStep = nullptr);

// Resolves the address `text` as addressResolve does. Returns Syntax, with no failed step, for text that is no address.
PrStatus addressResolveText(AddressResolver* resolver, PrStringView text, AddressNode* PR_NULLABLE* PR_NONNULL out,
    uint32_t* PR_NULLABLE failedStep = nullptr);

// Reads the stamp of each outermost resource that a held node depends on. Each node under one whose stamp changed, and
// each node that read one relatively, becomes stale and leaves the resolver, so that resolving its address again reads
// it again. Returns true, and advances the generation, if any node became stale.
bool addressResolverPoll(AddressResolver* resolver);

// Makes every held node that depends on the outermost resource at `url` stale, as a poll does when its stamp changes.
void addressResolverInvalidate(AddressResolver* resolver, PrStringView url);

// A count that advances each time nodes become stale. A holder that saw another generation resolves its address again.
uint64_t addressResolverGeneration(const AddressResolver* resolver);

// What a node is.
enum {
    AddressNodeKind_Resource,       // bytes, which may take further steps
    AddressNodeKind_Location,       // a place inside its parent, which ends the chain
};
typedef uint8_t AddressNodeKind;

// A nested resource that a container holds.
struct AddressChild {
    PrStringView step;              // the step from the container that reaches it
    PrStringView path;              // where it sits in a tree of the container's resources, with '/' between folders
    uint64_t size = 0;              // its size in bytes, as the container declares it
};

// Adds or releases a reference. The last release closes the node, and releases its parent.
void addressNodeRetain(AddressNode* node);
void addressNodeRelease(AddressNode* PR_NULLABLE node);

// The node's kind, its address, and its parent, which is null for the outermost resource.
AddressNodeKind addressNodeKind(const AddressNode* node);
const Address& addressNodeAddress(const AddressNode* node);
AddressNode* PR_NULLABLE addressNodeParent(const AddressNode* node);

// The name that the node's bytes are classified by: the last part of the outermost URL's path, or the name that its
// container gives a nested resource, or else the step. The view is valid while the node is held.
PrStringView addressNodeName(const AddressNode* node);

// The bytes of a resource, or null for a location. The blob is valid while the node is held, and a caller that keeps it
// longer retains it.
PrBlob* PR_NULLABLE addressNodeBytes(const AddressNode* node);

// The format of a resource's bytes, or None for a location.
PrMimeFormat addressNodeFormat(const AddressNode* node);

// Returns true if the node left its resolver because what it was read from changed.
bool addressNodeIsStale(const AddressNode* node);

// Opens a resource with the handler of its format, which taking a step does too. Returns the status of the first open,
// which is kept: Ok, NotSupported if no handler opens the format or the node is a location, or the handler's failure.
PrStatus addressNodeOpen(AddressNode* node);

// The nested resources of an opened container, in the container's order. A node that is not open has none. The views
// are valid while the node is held.
uint32_t addressNodeChildCount(const AddressNode* node);
AddressChild addressNodeChild(const AddressNode* node, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Handlers

// What a step reached.
struct AddressStepResult {
    AddressNodeKind kind = AddressNodeKind_Resource;

    // The bytes of a nested resource, a reference that the resolver takes. Null for a location.
    PrBlob* PR_NULLABLE bytes = nullptr;

    // The bytes that the step decoded or copied, which count against the chain's budget. Bytes that the blob shares
    // with the container cost nothing.
    uint64_t decoded = 0;

    // The name that a nested resource is classified by, such as an entry's file name, or empty for the step's last
    // part.
    String name;
};

// The operations of a handler. A handler's state derives from this struct. A null member is an operation that the
// handler does not support. The resolver calls a handler from the thread that uses the resolver.
struct AddressHandler {
    // Frees the handler.
    void (*destroyFn)(AddressHandler* handler) = nullptr;

    // Returns true if the handler opens resources of `format`.
    bool (*opensFn)(const AddressHandler* handler, PrMimeFormat format) = nullptr;

    // Opens the resource of `node`, whose bytes are addressNodeBytes, and writes the state that its steps read to
    // `*state`. It may read the resources that the resource names (addressNodeReadRelative). Returns InvalidData for
    // bytes that do not parse.
    PrStatus (*openFn)(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE* PR_NONNULL state) = nullptr;

    // Frees the state that openFn made.
    void (*closeFn)(AddressHandler* handler, void* PR_NULLABLE state) = nullptr;

    // Takes `step` on the opened resource of `node`. A nested resource may decode at most `budget` bytes, and a step
    // that would decode more fails with Limit before it decodes any. Returns NotFound for a step that names nothing,
    // and Syntax for one that the format's grammar does not accept.
    PrStatus (*stepFn)(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE state, PrStringView step,
        uint64_t budget, AddressStepResult* out) = nullptr;

    // The nested resources of the opened resource, as addressNodeChildCount and addressNodeChild report them.
    uint32_t (*childCountFn)(const AddressHandler* handler, const void* PR_NULLABLE state) = nullptr;
    AddressChild (*childFn)(const AddressHandler* handler, const void* PR_NULLABLE state, uint32_t index) = nullptr;

    // Appends to `out` the step of the resource that `reference`, a relative path that the nested resource at `step`
    // names, reaches in the same container. Returns false if the container has no such sibling, as for a path that
    // leaves its root. A handler whose steps are not paths leaves it null.
    bool (*siblingFn)(const AddressHandler* handler, const void* PR_NULLABLE state, PrStringView step,
        PrStringView reference, PrTextBuffer* out) = nullptr;
};

// Frees a handler. The resolvers that use it must be destroyed first.
void addressHandlerDestroy(AddressHandler* PR_NULLABLE handler);

// Returns the handler of the node's format, or null if no handler opens it or the node is a location.
const AddressHandler* PR_NULLABLE addressNodeHandler(const AddressNode* node);

// Returns the state that `handler` opened the node with, or null if another handler opened it or it is not open. A
// format's handler reaches its own state this way.
void* PR_NULLABLE addressNodeState(const AddressNode* node, const AddressHandler* handler);

// Returns a blob of `bytes`, which the node's bytes or its state hold, that keeps the node open while it is held. A
// step gives a nested resource that it reads in place this way. The caller holds the blob's one reference.
PrBlob* addressNodeShare(AddressNode* node, PrByteView bytes);

// Reads the resource that `reference`, a relative or absolute path that the node's resource names, reaches: a sibling
// in the node's container, joined with the directory of the node's step, or a URL resolved against the outermost URL if
// the node is outermost or the path is absolute. The node then depends on what it read, and becomes stale with it. The
// read counts against the budget of the node's chain. Returns NotFound if nothing is there, Limit past the budget, and
// the status of resolving the address otherwise. On failure, `*out` is null.
PrStatus addressNodeReadRelative(AddressNode* node, PrStringView reference, PrBlob* PR_NULLABLE* PR_NONNULL out);

// Appends `reference`, a relative path, joined with the directory of `from`, a path whose parts '/' separates, to
// `out`, with each `.` part removed and each `..` part removing the part before it. Returns false, and appends nothing,
// if a `..` would leave the root. A handler whose steps are paths implements siblingFn with it.
bool addressPathJoin(PrStringView from, PrStringView reference, PrTextBuffer* out);

} // namespace scribe

PR_ASSUME_NONNULL_END
