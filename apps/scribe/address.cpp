// Scribe's resource addresses: a URL whose fragment holds a chain of steps into nested resources, and the resolver that
// follows them through the handlers of formats.

#include "apps/scribe/address.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/construct.h"
#include "pr/file.h"
#include "pr/string_ops.h"
#include "pr/url.h"
#include "pr/url_file.h"
#include "pr/url_percent.h"

namespace scribe {

// An outermost resource that the resolver read, and its stamp when it read it.
struct AddressRoot {
    String url;
    uint64_t stamp = 0;
};

struct AddressNode {
    uint32_t references = 1;
    AddressResolver* resolver = nullptr;
    AddressNode* PR_NULLABLE parent = nullptr;  // a reference, or null for the outermost resource
    Address address;
    String name;
    AddressNodeKind kind = AddressNodeKind_Resource;
    PrMimeFormat format = PrMimeFormat_None;
    bool stale = false;                         // true once the node has left its resolver's table
    bool opened = false;                        // true once an open was tried, whose status is `openStatus`
    PrStatus openStatus = PrStatus_Ok;
    PrBlob* PR_NULLABLE bytes = nullptr;           // a reference, or null for a location
    AddressHandler* PR_NULLABLE handler = nullptr; // the handler of `format`, or null if none opens it
    void* PR_NULLABLE state = nullptr;             // what `handler` opened

    // The bytes that the steps of the node's chain decoded and that its relative reads read, which the budget bounds.
    uint64_t chainBytes = 0;

    // The outermost resources that the node depends on itself, by index in its resolver's roots: its own if it is
    // outermost, and those of what it read relatively. A node also depends on what its ancestors depend on.
    Array<uint32_t> roots;
};

struct AddressResolver {
    Array<AddressHandler*> handlers;
    AddressReadFn read = nullptr;
    AddressStampFn PR_NULLABLE stamp = nullptr;
    void* PR_NULLABLE context = nullptr;
    AddressLimits limits;
    uint64_t generation = 0;

    // The held nodes that are not stale, which resolving searches for the longest prefix of an address.
    Array<AddressNode*> nodes;

    // The outermost resources that the resolver read, each once.
    Array<AddressRoot> roots;
};

namespace {

constexpr char kHex[] = "0123456789ABCDEF";

// Returns true if a step's byte is percent-encoded: what the fragment percent-encode set holds
// (url/#fragment-percent-encode-set), and `%`, `!` and `#`, which the parser reads as an escape, a separator and the
// start of the fragment.
bool encodes(uint8_t b) {
    return b < 0x20 || b > 0x7e || b == ' ' || b == '"' || b == '<' || b == '>' || b == '`' || b == '%' || b == '!' || b == '#';
}

void appendEncoded(String& out, PrStringView step) {
    for (size_t i = 0; i < step.size; ++i) {
        uint8_t b = uint8_t(step.data[i]);
        if (!encodes(b)) {
            out.append(char(b));
            continue;
        }
        out.append('%');
        out.append(kHex[b >> 4]);
        out.append(kHex[b & 15]);
    }
}

// Returns `encoded`, percent-decoded.
String decoded(PrStringView encoded) {
    Array<char> bytes;
    bytes.resize(uint32_t(encoded.size));
    size_t size = encoded.size != 0 ? prUrlPercentDecode(encoded, bytes.data(), bytes.count()) : 0;
    return String(PrStringView{bytes.data(), size});
}

// Returns true if `node`'s address is the first `count` steps of `address`.
bool holdsPrefix(const AddressNode* node, const Address& address, uint32_t count) {
    const Address& held = node->address;
    if (held.steps.count() != count || held.url != address.url)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (held.steps[i] != address.steps[i])
            return false;
    }
    return true;
}

// Returns a copy of the first `count` steps of `address`.
Address prefixOf(const Address& address, uint32_t count) {
    Address prefix = address;
    prefix.steps.resize(count);
    return prefix;
}

// Returns the index of the root of `url`, recording it with `stamp` if the resolver has none, or setting its stamp if
// it has.
uint32_t rootOf(AddressResolver* resolver, PrStringView url, uint64_t stamp) {
    for (uint32_t i = 0; i < resolver->roots.count(); ++i) {
        if (resolver->roots[i].url == url) {
            resolver->roots[i].stamp = stamp;
            return i;
        }
    }
    resolver->roots.push(AddressRoot{.url = url, .stamp = stamp});
    return resolver->roots.count() - 1;
}

// Returns the handler that opens `format`, or null.
AddressHandler* PR_NULLABLE handlerOf(const AddressResolver* resolver, PrMimeFormat format) {
    for (AddressHandler* handler : resolver->handlers) {
        if (handler->opensFn != nullptr && handler->opensFn(handler, format))
            return handler;
    }
    return nullptr;
}

// Returns the last part of `path`, after its last '/'.
PrStringView lastPart(PrStringView path) {
    size_t slash = prStringFindLastByte(path, '/');
    return slash == path.size ? path : prStringFrom(path, slash + 1);
}

// Makes a node of `address` with a reference for the caller, and puts it in the resolver's table.
AddressNode* nodeCreate(AddressResolver* resolver, AddressNode* PR_NULLABLE parent, Address address,
        AddressNodeKind kind, PrBlob* PR_NULLABLE bytes, PrStringView name) {
    AddressNode* node = create<AddressNode>();
    node->resolver = resolver;
    node->parent = parent;
    if (parent != nullptr) {
        addressNodeRetain(parent);
        node->chainBytes = parent->chainBytes;
    }
    node->address = static_cast<Address&&>(address);
    node->name = name;
    node->kind = kind;
    node->bytes = bytes;
    if (kind == AddressNodeKind_Resource) {
        node->format = prMimeSniff(prBlobBytes(bytes), name);
        node->handler = handlerOf(resolver, node->format);
    }
    resolver->nodes.push(node);
    return node;
}

// Returns true if the node or an ancestor depends on a root marked in `changed`, by root index.
bool dependsOn(const AddressNode* node, const Array<bool>& changed) {
    for (const AddressNode* at = node; at != nullptr; at = at->parent) {
        for (uint32_t root : at->roots) {
            if (changed[root])
                return true;
        }
    }
    return false;
}

// Makes each node of the table that depends on a root marked in `changed` stale. Returns true if any became stale.
bool invalidate(AddressResolver* resolver, const Array<bool>& changed) {
    uint32_t removed = resolver->nodes.removeWhere([&](AddressNode* node) {
        if (!dependsOn(node, changed))
            return false;
        node->stale = true;
        return true;
    });
    if (removed != 0)
        ++resolver->generation;
    return removed != 0;
}

// Takes step `index` of `address` from `node`, a node of its first `index` steps, and writes a node of one more step.
PrStatus stepFrom(AddressNode* node, const Address& address, uint32_t index, AddressNode** out) {
    *out = nullptr;
    if (node->kind != AddressNodeKind_Resource)
        return PrStatus_NotFound;
    PrStatus status = addressNodeOpen(node);
    if (status != PrStatus_Ok)
        return status;
    AddressHandler* handler = node->handler;
    if (handler->stepFn == nullptr)
        return PrStatus_NotSupported;
    AddressResolver* resolver = node->resolver;
    uint64_t budget = resolver->limits.bytes > node->chainBytes ? resolver->limits.bytes - node->chainBytes : 0;
    PrStringView step = address.steps[index];
    AddressStepResult result;
    status = handler->stepFn(handler, node, node->state, step, budget, &result);
    if (status == PrStatus_Ok && result.decoded > budget)
        status = PrStatus_Limit;
    if (status != PrStatus_Ok) {
        prBlobRelease(result.bytes);
        return status;
    }
    PR_ASSERT((result.kind == AddressNodeKind_Resource) == (result.bytes != nullptr));
    PrStringView name = result.name.isEmpty() ? lastPart(step) : PrStringView(result.name);
    *out = nodeCreate(resolver, node, prefixOf(address, index + 1), result.kind, result.bytes, name);
    (*out)->chainBytes += result.decoded;
    return PrStatus_Ok;
}

// Returns the last part of the path of the URL `url`, percent-decoded.
String urlName(PrStringView url) {
    PrUrl* parsed = nullptr;
    if (prUrlCreate(url, nullptr, PrCharset_Utf8, &parsed) != PrStatus_Ok)
        return String();
    String name = decoded(lastPart(prUrlPath(parsed)));
    prUrlDestroy(parsed);
    return name;
}

void releaseShare(void* PR_NULLABLE context, PrByteView) {
    addressNodeRelease(static_cast<AddressNode*>(context));
}

// Returns true if `reference` is absolute: a path from the root, or a URL with a scheme (url/#url-scheme-string).
bool isAbsolute(PrStringView reference) {
    if (prStringStartsWith(reference, "/"))
        return true;
    size_t at = 0;
    while (at < reference.size && (prAsciiIsLetter(reference.data[at]) || prAsciiIsDigit(reference.data[at]) ||
            reference.data[at] == '+' || reference.data[at] == '-' || reference.data[at] == '.'))
        ++at;
    // A one-letter scheme is a Windows drive, which is a path.
    return at > 1 && at < reference.size && reference.data[at] == ':' && prAsciiIsLetter(reference.data[0]);
}

// Appends `path` to `out` as a relative URL: with `%`, and what a URL's path escapes, percent-encoded.
void appendPathUrl(String& out, PrStringView path) {
    for (size_t i = 0; i < path.size; ++i) {
        uint8_t b = uint8_t(path.data[i]);
        bool escaped = b <= 0x20 || b > 0x7e || b == '"' || b == '#' || b == '<' || b == '>' || b == '?' || b == '^' ||
            b == '`' || b == '{' || b == '}' || b == '%';
        if (!escaped) {
            out.append(char(b));
            continue;
        }
        out.append('%');
        out.append(kHex[b >> 4]);
        out.append(kHex[b & 15]);
    }
}

// Writes the address that `reference` reaches from `node` to `*out`, or returns false if it reaches nothing.
bool targetOf(const AddressNode* node, PrStringView reference, Address* out) {
    const AddressNode* parent = node->parent;
    if (parent == nullptr || isAbsolute(reference)) {
        PrUrl* base = nullptr;
        if (prUrlCreate(node->address.url, nullptr, PrCharset_Utf8, &base) != PrStatus_Ok)
            return false;
        String text;
        if (isAbsolute(reference) && !prStringStartsWith(reference, "/"))
            text = reference;
        else
            appendPathUrl(text, reference);
        PrUrl* url = nullptr;
        PrStatus status = prUrlCreate(text, base, PrCharset_Utf8, &url);
        prUrlDestroy(base);
        if (status != PrStatus_Ok)
            return false;
        String serialized = String::adopt(prUrlSerialize(url, PrUrlSerialization_ExcludeFragment));
        prUrlDestroy(url);
        return addressParse(serialized, out) == PrStatus_Ok;
    }
    AddressHandler* handler = parent->handler;
    if (handler == nullptr || handler->siblingFn == nullptr || !parent->opened || parent->openStatus != PrStatus_Ok)
        return false;
    PrStringView step = node->address.steps.last();
    PrTextBuffer sibling = {};
    bool found = handler->siblingFn(handler, parent->state, step, reference, &sibling);
    if (found) {
        *out = parent->address;
        out->steps.push(String(prTextBufferView(&sibling)));
    }
    prTextBufferReset(&sibling);
    return found;
}

// Adds each root that `from` and its ancestors depend on to the roots of `node`.
void addRoots(AddressNode* node, const AddressNode* from) {
    for (const AddressNode* at = from; at != nullptr; at = at->parent) {
        for (uint32_t root : at->roots) {
            if (!node->roots.contains(root))
                node->roots.push(root);
        }
    }
}

} // namespace

PrStatus addressParse(PrStringView text, Address* out) {
    size_t hash = prStringFindByte(text, '#');
    PrUrl* url = nullptr;
    PrStatus status = prUrlCreate(prStringHead(text, hash), nullptr, PrCharset_Utf8, &url);
    if (status != PrStatus_Ok)
        return status;
    Address address;
    address.url = String::adopt(prUrlSerialize(url, PrUrlSerialization_ExcludeFragment));
    prUrlDestroy(url);
    if (hash + 1 < text.size) {
        PrStringView fragment = prStringFrom(text, hash + 1);
        for (;;) {
            size_t bang = prStringFindByte(fragment, '!');
            address.steps.push(decoded(prStringHead(fragment, bang)));
            if (bang == fragment.size)
                break;
            fragment = prStringFrom(fragment, bang + 1);
        }
    }
    *out = static_cast<Address&&>(address);
    return PrStatus_Ok;
}

String addressText(const Address& address) {
    String text = address.url;
    for (uint32_t i = 0; i < address.steps.count(); ++i) {
        text.append(i == 0 ? '#' : '!');
        appendEncoded(text, address.steps[i]);
    }
    return text;
}

String addressWithStep(PrStringView text, PrStringView step) {
    Address address;
    if (addressParse(text, &address) != PrStatus_Ok)
        return String();
    address.steps.push(String(step));
    return addressText(address);
}

PrBlob* PR_NULLABLE addressFileRead(void*, PrStringView url) {
    PrRawString path = prRawStringNull();
    if (!prUrlToFilePath(url, &path))
        return nullptr;
    PrBlob* blob = prFileOpen(prRawStringView(&path));
    prRawStringReset(&path);
    return blob;
}

uint64_t addressFileStamp(void*, PrStringView url) {
    PrRawString path = prRawStringNull();
    if (!prUrlToFilePath(url, &path))
        return 0;
    PrFileInfo info{};
    bool found = prFileInfo(prRawStringView(&path), &info);
    prRawStringReset(&path);
    if (!found || info.kind != PrFileKind_Regular)
        return 0;
    // A modification time is never zero, so neither is the stamp of a file that exists.
    return (info.modified.ticks ^ (info.size * 0x9e3779b97f4a7c15ull)) | 1;
}

AddressResolver* addressResolverCreate(const AddressResolverDesc& desc) {
    PR_ASSERT(desc.read != nullptr);
    AddressResolver* resolver = create<AddressResolver>();
    for (AddressHandler* handler : desc.handlers)
        resolver->handlers.push(handler);
    resolver->read = desc.read;
    resolver->stamp = desc.stamp;
    resolver->context = desc.context;
    resolver->limits = desc.limits;
    return resolver;
}

void addressResolverDestroy(AddressResolver* PR_NULLABLE resolver) {
    if (resolver == nullptr)
        return;
    PR_ASSERT(resolver->nodes.isEmpty());
    destroy(resolver);
}

PrStatus addressResolve(AddressResolver* resolver, const Address& address, AddressNode* PR_NULLABLE* PR_NONNULL out,
        uint32_t* PR_NULLABLE failedStep) {
    *out = nullptr;
    uint32_t failed = kAddressStepNone;
    if (failedStep == nullptr)
        failedStep = &failed;
    *failedStep = kAddressStepNone;
    uint32_t count = address.steps.count();
    if (count > resolver->limits.steps) {
        *failedStep = resolver->limits.steps;
        return PrStatus_Limit;
    }
    // The longest prefix that the resolver holds.
    AddressNode* node = nullptr;
    uint32_t done = 0;
    for (uint32_t k = count + 1; k-- > 0 && node == nullptr;) {
        for (AddressNode* held : resolver->nodes) {
            if (holdsPrefix(held, address, k)) {
                node = held;
                done = k;
                break;
            }
        }
    }
    if (node != nullptr) {
        addressNodeRetain(node);
    } else {
        PrBlob* bytes = resolver->read(resolver->context, address.url);
        if (bytes == nullptr)
            return PrStatus_Io;
        uint64_t stamp = resolver->stamp != nullptr ? resolver->stamp(resolver->context, address.url) : 0;
        String name = urlName(address.url);
        node = nodeCreate(resolver, nullptr, prefixOf(address, 0), AddressNodeKind_Resource, bytes, name);
        node->roots.push(rootOf(resolver, address.url, stamp));
    }
    for (uint32_t i = done; i < count; ++i) {
        AddressNode* child = nullptr;
        PrStatus status = stepFrom(node, address, i, &child);
        addressNodeRelease(node);
        if (status != PrStatus_Ok) {
            *failedStep = i;
            return status;
        }
        node = child;
    }
    *out = node;
    return PrStatus_Ok;
}

PrStatus addressResolveText(AddressResolver* resolver, PrStringView text, AddressNode* PR_NULLABLE* PR_NONNULL out,
        uint32_t* PR_NULLABLE failedStep) {
    *out = nullptr;
    if (failedStep != nullptr)
        *failedStep = kAddressStepNone;
    Address address;
    PrStatus status = addressParse(text, &address);
    if (status != PrStatus_Ok)
        return status;
    return addressResolve(resolver, address, out, failedStep);
}

bool addressResolverPoll(AddressResolver* resolver) {
    if (resolver->stamp == nullptr)
        return false;
    // Only the roots that a held node depends on are read.
    Array<bool> used(resolver->roots.count(), false);
    for (const AddressNode* node : resolver->nodes) {
        for (uint32_t root : node->roots)
            used[root] = true;
    }
    Array<bool> changed(resolver->roots.count(), false);
    for (uint32_t i = 0; i < resolver->roots.count(); ++i) {
        if (!used[i])
            continue;
        AddressRoot& root = resolver->roots[i];
        changed[i] = resolver->stamp(resolver->context, root.url) != root.stamp;
    }
    return invalidate(resolver, changed);
}

void addressResolverInvalidate(AddressResolver* resolver, PrStringView url) {
    Address address;
    if (addressParse(url, &address) != PrStatus_Ok)
        return;
    Array<bool> changed(resolver->roots.count(), false);
    for (uint32_t i = 0; i < resolver->roots.count(); ++i)
        changed[i] = resolver->roots[i].url == address.url;
    invalidate(resolver, changed);
}

uint64_t addressResolverGeneration(const AddressResolver* resolver) {
    return resolver->generation;
}

void addressNodeRetain(AddressNode* node) {
    ++node->references;
}

void addressNodeRelease(AddressNode* PR_NULLABLE node) {
    while (node != nullptr) {
        PR_ASSERT(node->references > 0);
        if (--node->references != 0)
            return;
        if (!node->stale)
            node->resolver->nodes.removeFirst(node);
        if (node->handler != nullptr && node->handler->closeFn != nullptr && node->opened &&
                node->openStatus == PrStatus_Ok)
            node->handler->closeFn(node->handler, node->state);
        prBlobRelease(node->bytes);
        AddressNode* parent = node->parent;
        destroy(node);
        node = parent;
    }
}

AddressNodeKind addressNodeKind(const AddressNode* node) {
    return node->kind;
}

const Address& addressNodeAddress(const AddressNode* node) {
    return node->address;
}

AddressNode* PR_NULLABLE addressNodeParent(const AddressNode* node) {
    return node->parent;
}

PrStringView addressNodeName(const AddressNode* node) {
    return node->name;
}

PrBlob* PR_NULLABLE addressNodeBytes(const AddressNode* node) {
    return node->bytes;
}

PrMimeFormat addressNodeFormat(const AddressNode* node) {
    return node->format;
}

bool addressNodeIsStale(const AddressNode* node) {
    return node->stale;
}

PrStatus addressNodeOpen(AddressNode* node) {
    if (node->opened)
        return node->openStatus;
    node->opened = true;
    if (node->kind != AddressNodeKind_Resource || node->handler == nullptr) {
        node->openStatus = PrStatus_NotSupported;
    } else if (node->handler->openFn != nullptr) {
        void* state = nullptr;
        node->openStatus = node->handler->openFn(node->handler, node, &state);
        node->state = node->openStatus == PrStatus_Ok ? state : nullptr;
    }
    return node->openStatus;
}

uint32_t addressNodeChildCount(const AddressNode* node) {
    if (!node->opened || node->openStatus != PrStatus_Ok || node->handler->childCountFn == nullptr)
        return 0;
    return node->handler->childCountFn(node->handler, node->state);
}

AddressChild addressNodeChild(const AddressNode* node, uint32_t index) {
    PR_ASSERT(index < addressNodeChildCount(node));
    return node->handler->childFn(node->handler, node->state, index);
}

void addressHandlerDestroy(AddressHandler* PR_NULLABLE handler) {
    if (handler != nullptr && handler->destroyFn != nullptr)
        handler->destroyFn(handler);
}

const AddressHandler* PR_NULLABLE addressNodeHandler(const AddressNode* node) {
    return node->handler;
}

void* PR_NULLABLE addressNodeState(const AddressNode* node, const AddressHandler* handler) {
    if (node->handler != handler || !node->opened || node->openStatus != PrStatus_Ok)
        return nullptr;
    return node->state;
}

PrBlob* addressNodeShare(AddressNode* node, PrByteView bytes) {
    addressNodeRetain(node);
    return prBlobCreate(bytes, releaseShare, node);
}

PrStatus addressNodeReadRelative(AddressNode* node, PrStringView reference, PrBlob* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    Address target;
    if (!targetOf(node, reference, &target))
        return PrStatus_NotFound;
    AddressNode* found = nullptr;
    PrStatus status = addressResolve(node->resolver, target, &found);
    if (status == PrStatus_Io)
        return PrStatus_NotFound;
    if (status != PrStatus_Ok)
        return status;
    if (found->kind != AddressNodeKind_Resource || found == node) {
        addressNodeRelease(found);
        return PrStatus_NotFound;
    }
    // A sibling costs what its step decoded, and a resource beside the outermost one costs what was read.
    uint64_t cost = prBlobBytes(found->bytes).size;
    if (found->parent != nullptr)
        cost = found->chainBytes - found->parent->chainBytes;
    uint64_t limit = node->resolver->limits.bytes;
    if (node->chainBytes > limit || cost > limit - node->chainBytes) {
        addressNodeRelease(found);
        return PrStatus_Limit;
    }
    node->chainBytes += cost;
    addRoots(node, found);
    *out = found->bytes;
    prBlobRetain(found->bytes);
    addressNodeRelease(found);
    return PrStatus_Ok;
}

bool addressPathJoin(PrStringView from, PrStringView reference, PrTextBuffer* out) {
    // The parts of the result, as views of `from` and `reference`.
    Array<PrStringView> parts;
    size_t slash = prStringFindLastByte(from, '/');
    PrStringView directory = slash == from.size ? PrStringView{""} : prStringHead(from, slash);
    PrStringView texts[] = {directory, reference};
    for (PrStringView text : texts) {
        while (text.size != 0) {
            size_t end = prStringFindByte(text, '/');
            PrStringView part = prStringHead(text, end);
            text = end == text.size ? PrStringView{""} : prStringFrom(text, end + 1);
            if (part.size == 0 || part == ".")
                continue;
            if (part == "..") {
                if (parts.isEmpty())
                    return false;
                parts.pop();
                continue;
            }
            parts.push(part);
        }
    }
    for (uint32_t i = 0; i < parts.count(); ++i) {
        if (i != 0)
            prTextBufferAppendChar(out, '/');
        prTextBufferAppend(out, parts[i]);
    }
    return true;
}

} // namespace scribe
