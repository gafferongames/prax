// Scribe's resource addresses: their syntax, and the resolver's steps, limits, caching and changes, through a handler
// of a toy container.

#include "pr/alloc.h"
#include "pr/construct.h"
#include "pr/file.h"
#include "pr/format.h"
#include "pr/string_ops.h"
#include "pr/url_file.h"

#include "apps/scribe/tests/address_fixture.h"

#include "tools/harness/test.h"

using namespace scribe;

namespace {

// A toy container of named records, `name=<size>:<bytes>` one after another, which a handler of text opens. A record
// whose name begins with `~` is decoded by its step: its bytes are copied, and count against the budget. A record named
// `needs` holds a relative path, which opening reads, as a layer reads its sublayers.
struct Records : AddressHandler {
    uint32_t opens = 0;
    uint32_t closes = 0;
};

struct Record {
    PrStringView name;
    PrByteView bytes;
};

// The records of an opened resource.
struct RecordsState {
    Array<Record> records;
    PrBlob* PR_NULLABLE needed = nullptr;
};

bool parse(PrByteView bytes, Array<Record>& out) {
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    size_t at = 0;
    while (at < text.size) {
        size_t equals = prStringFindByte(prStringFrom(text, at), '=') + at;
        size_t colon = prStringFindByte(prStringFrom(text, at), ':') + at;
        if (equals >= text.size || colon >= text.size || colon < equals)
            return false;
        uint64_t size = 0;
        for (size_t i = equals + 1; i < colon; ++i)
            size = size * 10 + uint64_t(text.data[i] - '0');
        if (size > text.size - colon - 1)
            return false;
        out.push(Record{prStringSlice(text, at, equals - at), PrByteView{bytes.data + colon + 1, size_t(size)}});
        at = colon + 1 + size_t(size);
    }
    return true;
}

void freeRecords(RecordsState* records) {
    prBlobRelease(records->needed);
    destroy(records);
}

PrStatus recordsOpen(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE* PR_NONNULL state) {
    RecordsState* records = create<RecordsState>();
    if (!parse(prBlobBytes(addressNodeBytes(node)), records->records)) {
        freeRecords(records);
        return PrStatus_InvalidData;
    }
    for (const Record& record : records->records) {
        if (record.name != "needs")
            continue;
        PrStringView path{reinterpret_cast<const char*>(record.bytes.data), record.bytes.size};
        PrStatus status = addressNodeReadRelative(node, path, &records->needed);
        if (status != PrStatus_Ok) {
            freeRecords(records);
            return status;
        }
    }
    ++static_cast<Records*>(handler)->opens;
    *state = records;
    return PrStatus_Ok;
}

void recordsClose(AddressHandler* handler, void* PR_NULLABLE state) {
    freeRecords(static_cast<RecordsState*>(state));
    ++static_cast<Records*>(handler)->closes;
}

PrStatus recordsStep(AddressHandler*, AddressNode* node, void* PR_NULLABLE state, PrStringView step, uint64_t budget,
        AddressStepResult* out) {
    RecordsState* records = static_cast<RecordsState*>(state);
    bool location = prStringStartsWith(step, "@");
    PrStringView name = location ? prStringFrom(step, 1) : step;
    if (name.isEmpty())
        return PrStatus_Syntax;
    for (const Record& record : records->records) {
        if (record.name != name)
            continue;
        if (location) {
            out->kind = AddressNodeKind_Location;
        } else if (prStringStartsWith(name, "~")) {
            if (record.bytes.size > budget)
                return PrStatus_Limit;
            out->bytes = prBlobCopy(record.bytes);
            out->decoded = record.bytes.size;
        } else {
            out->bytes = addressNodeShare(node, record.bytes);
        }
        return PrStatus_Ok;
    }
    return PrStatus_NotFound;
}

uint32_t recordsChildCount(const AddressHandler*, const void* PR_NULLABLE state) {
    return static_cast<const RecordsState*>(state)->records.count();
}

AddressChild recordsChild(const AddressHandler*, const void* PR_NULLABLE state, uint32_t index) {
    const Record& record = static_cast<const RecordsState*>(state)->records[index];
    return AddressChild{.step = record.name, .path = record.name, .size = record.bytes.size};
}

bool recordsSibling(const AddressHandler*, const void* PR_NULLABLE, PrStringView step, PrStringView reference,
        PrTextBuffer* out) {
    return addressPathJoin(step, reference, out);
}

// The handler of records, which the fixture's resolver opens text with.
Records* recordsHandler() {
    Records* handler = create<Records>();
    handler->opensFn = [](const AddressHandler*, PrMimeFormat format) { return format == PrMimeFormat_Text; };
    handler->openFn = recordsOpen;
    handler->closeFn = recordsClose;
    handler->stepFn = recordsStep;
    handler->childCountFn = recordsChildCount;
    handler->childFn = recordsChild;
    handler->siblingFn = recordsSibling;
    handler->destroyFn = [](AddressHandler* records) {
        destroy(static_cast<Records*>(records));
    };
    return handler;
}

// A resolver over files with the records handler.
struct Fixture {
    Records* handler = recordsHandler();
    AddressHandler* handlers[1] = {handler};
    AddressFixture f;

    explicit Fixture(AddressLimits limits = {}) : f(handlers, limits) {}
};

// The bytes of `size` and `text` as a record named `name`.
String record(PrStringView name, PrStringView text) {
    return format("{}={}:{}", name, text.size, text);
}

// The two texts one after the other.
String joined(PrStringView first, PrStringView second) {
    String text(first);
    text.append(second);
    return text;
}

// Returns the address `text` as it parses.
Address parsed(PrStringView text) {
    Address address;
    REQUIRE(addressParse(text, &address) == PrStatus_Ok);
    return address;
}

} // namespace

TEST_CASE("an address is a URL and the steps of its fragment") {
    Address address = parsed("file:///assets/outer.zip#inner.zip!scene.usdz!geom.usdc!/World/Car{color=red}");
    CHECK(address.url == "file:///assets/outer.zip");
    REQUIRE(address.steps.count() == 4);
    CHECK(address.steps[0] == "inner.zip");
    CHECK(address.steps[3] == "/World/Car{color=red}");
    CHECK(addressText(address) == "file:///assets/outer.zip#inner.zip!scene.usdz!geom.usdc!/World/Car{color=red}");

    // No fragment, and an empty one, have no steps.
    CHECK(parsed("file:///a/b.png").steps.isEmpty());
    address = parsed("file:///a/b.png#");
    CHECK(address.steps.isEmpty());
    CHECK(addressText(address) == "file:///a/b.png");

    Address unchanged = address;
    CHECK(addressParse("not a url", &address) == PrStatus_Syntax);
    CHECK(address == unchanged);
}

TEST_CASE("a step is percent-encoded, and holds any byte") {
    Address address = parsed("file:///a.zip");
    address.steps.push("wow!such #name 100%.png");
    address.steps.push("/images/0");
    String text = addressText(address);
    CHECK(text == "file:///a.zip#wow%21such%20%23name%20100%25.png!/images/0");
    Address again = parsed(text);
    CHECK(again == address);
    CHECK(again.steps[0] == "wow!such #name 100%.png");
    again.steps.pop();
    CHECK(!(again == address));
    CHECK(addressWithStep("file:///a.zip#wow%21such%20%23name%20100%25.png", "/images/0") == text);
    CHECK(addressWithStep("not a url", "x").isEmpty());
}

TEST_CASE("resolving follows each step and reports the one that fails") {
    Fixture x;
    AddressFixture& f = x.f;
    String inner = joined(record("leaf", "hello"), record("other", "x"));
    f.set("file:///box", joined(record("inner", inner), record("note", "plain")));
    AddressNode* node = nullptr;
    REQUIRE(f.resolve("file:///box#inner!leaf", &node) == PrStatus_Ok);
    CHECK(addressNodeKind(node) == AddressNodeKind_Resource);
    CHECK(addressNodeText(node) == "hello");
    CHECK(addressNodeName(node) == "leaf");
    CHECK(addressNodeFormat(node) == PrMimeFormat_Text);
    CHECK(addressNodeAddress(node).steps.count() == 2);
    AddressNode* parent = addressNodeParent(node);
    REQUIRE(parent != nullptr);
    CHECK(addressNodeName(parent) == "inner");
    REQUIRE(addressNodeParent(parent) != nullptr);
    CHECK(addressNodeName(addressNodeParent(parent)) == "box");
    addressNodeRelease(node);

    uint32_t failed = 0;
    CHECK(f.resolve("file:///box#inner!missing", &node, &failed) == PrStatus_NotFound);
    CHECK(node == nullptr);
    CHECK(failed == 1);
    CHECK(f.resolve("file:///box#inner!@", &node, &failed) == PrStatus_Syntax);
    CHECK(failed == 1);
    CHECK(f.resolve("file:///nothing#inner", &node, &failed) == PrStatus_Io);
    CHECK(failed == kAddressStepNone);
    CHECK(f.resolve("not a url", &node, &failed) == PrStatus_Syntax);
    CHECK(failed == kAddressStepNone);

    // A location ends the chain.
    REQUIRE(f.resolve("file:///box#inner!@leaf", &node) == PrStatus_Ok);
    CHECK(addressNodeKind(node) == AddressNodeKind_Location);
    CHECK(addressNodeBytes(node) == nullptr);
    CHECK(addressNodeOpen(node) == PrStatus_NotSupported);
    addressNodeRelease(node);
    CHECK(f.resolve("file:///box#inner!@leaf!more", &node, &failed) == PrStatus_NotFound);
    CHECK(failed == 2);

    // Bytes that no handler opens take no step.
    f.set("file:///binary", {"\x01\x02\x03", 3});
    CHECK(f.resolve("file:///binary#x", &node, &failed) == PrStatus_NotSupported);
    CHECK(failed == 0);
    // Bytes that do not parse.
    f.set("file:///broken", "nonsense");
    CHECK(f.resolve("file:///broken#x", &node, &failed) == PrStatus_InvalidData);
}

TEST_CASE("an opened container stays open while anything under it is held") {
    Fixture x;
    AddressFixture& f = x.f;
    f.set("file:///box", joined(record("a", "1"), record("b", "2")));
    AddressNode* a = nullptr;
    AddressNode* b = nullptr;
    REQUIRE(f.resolve("file:///box#a", &a) == PrStatus_Ok);
    REQUIRE(f.resolve("file:///box#b", &b) == PrStatus_Ok);
    // One read and one open serve both.
    CHECK(f.reads == 1);
    CHECK(x.handler->opens == 1);
    CHECK(addressNodeParent(a) == addressNodeParent(b));

    // The same address resolves to the same node.
    AddressNode* again = nullptr;
    REQUIRE(f.resolve("file:///box#a", &again) == PrStatus_Ok);
    CHECK(again == a);
    addressNodeRelease(again);

    // A nested resource's blob keeps its container open after its node is released.
    PrBlob* blob = addressNodeBytes(a);
    prBlobRetain(blob);
    addressNodeRelease(a);
    addressNodeRelease(b);
    CHECK(x.handler->closes == 0);
    prBlobRelease(blob);
    CHECK(x.handler->closes == 1);

    // With nothing held, the container is read again.
    REQUIRE(f.resolve("file:///box#a", &a) == PrStatus_Ok);
    CHECK(f.reads == 2);
    addressNodeRelease(a);
}

TEST_CASE("a container lists its nested resources once it is open") {
    Fixture x;
    AddressFixture& f = x.f;
    f.set("file:///box", joined(record("a", "1"), record("bb", "22")));
    AddressNode* box = nullptr;
    REQUIRE(f.resolve("file:///box", &box) == PrStatus_Ok);
    CHECK(addressNodeChildCount(box) == 0);
    REQUIRE(addressNodeOpen(box) == PrStatus_Ok);
    REQUIRE(addressNodeChildCount(box) == 2);
    CHECK(addressNodeChild(box, 1).step == "bb");
    CHECK(addressNodeChild(box, 1).size == 2);
    CHECK(addressNodeState(box, x.handler) != nullptr);
    addressNodeRelease(box);
}

TEST_CASE("an address has at most the limit of steps") {
    Fixture x(AddressLimits{.steps = 3});
    AddressFixture& f = x.f;
    // Records nested four deep.
    String text = record("d", "leaf");
    const char* names[] = {"c", "b", "a"};
    for (const char* name : names)
        text = record(name, text);
    f.set("file:///deep", text);
    CHECK(f.status("file:///deep#a!b!c") == PrStatus_Ok);
    uint32_t failed = 0;
    CHECK(f.status("file:///deep#a!b!c!d", &failed) == PrStatus_Limit);
    CHECK(failed == 3);
}

TEST_CASE("the bytes that a chain decodes count against one budget") {
    Fixture x(AddressLimits{.bytes = 10});
    AddressFixture& f = x.f;
    // Each decoded record copies its bytes, and shared records cost nothing.
    PrStringView six = "xxxxxx";
    String decoded = record("~a", record("~b", six));
    f.set("file:///bomb", joined(decoded, joined(record("~c", "12345678901"), record("s", "shared"))));
    uint32_t failed = 0;
    // The first step decodes 13 bytes.
    CHECK(f.status("file:///bomb#~a", &failed) == PrStatus_Limit);
    CHECK(failed == 0);
    CHECK(f.status("file:///bomb#~c") == PrStatus_Limit);
    CHECK(f.status("file:///bomb#s") == PrStatus_Ok);

    Fixture y(AddressLimits{.bytes = 16});
    AddressFixture& g = y.f;
    g.set("file:///bomb", record("~a", record("~b", six)));
    // 13 bytes, then 6 more: the second step is past the budget that the first left.
    CHECK(g.status("file:///bomb#~a") == PrStatus_Ok);
    CHECK(g.status("file:///bomb#~a!~b", &failed) == PrStatus_Limit);
    CHECK(failed == 1);
}

TEST_CASE("a resource reads what it names relatively, beside it in its container") {
    Fixture x;
    AddressFixture& f = x.f;
    String layer = record("needs", "../lib/base");
    f.set("file:///dir/pack", joined(record("scene/main", layer), record("lib/base", "B")));
    CHECK(f.status("file:///dir/pack#scene/main!@needs") == PrStatus_Ok);
    // A path that leaves the container's root finds nothing.
    f.set("file:///dir/escape", record("main", record("needs", "../../x")));
    uint32_t failed = 0;
    CHECK(f.status("file:///dir/escape#main!@needs", &failed) == PrStatus_NotFound);
    CHECK(failed == 1);
    // At the outermost level, the path resolves against the URL.
    f.set("file:///dir/top", record("needs", "sub/other file"));
    f.set("file:///dir/sub/other%20file", "O");
    CHECK(f.status("file:///dir/top#@needs") == PrStatus_Ok);
}

TEST_CASE("a relative path joins the directory of a step") {
    auto joinedPath = [](PrStringView from, PrStringView reference) {
        PrTextBuffer out = {};
        String text = addressPathJoin(from, reference, &out) ? String(prTextBufferView(&out)) : String("-");
        prTextBufferReset(&out);
        return text;
    };
    CHECK(joinedPath("a/b/c.usda", "./d/../e.usda") == "a/b/e.usda");
    CHECK(joinedPath("a/b/c.usda", "../x.png") == "a/x.png");
    CHECK(joinedPath("c.usda", "textures/x.png") == "textures/x.png");
    CHECK(joinedPath("a/c.usda", "../../x.png") == "-");
}

TEST_CASE("a change to the outermost resource makes everything under it stale") {
    Fixture x;
    AddressFixture& f = x.f;
    f.set("file:///box", record("a", record("x", "1")));
    AddressNode* node = nullptr;
    REQUIRE(f.resolve("file:///box#a!x", &node) == PrStatus_Ok);
    uint64_t generation = addressResolverGeneration(f.resolver);
    CHECK(!addressResolverPoll(f.resolver));
    CHECK(!addressNodeIsStale(node));

    f.set("file:///box", record("a", record("y", "2")));
    CHECK(addressResolverPoll(f.resolver));
    CHECK(addressResolverGeneration(f.resolver) == generation + 1);
    CHECK(addressNodeIsStale(node));
    CHECK(addressNodeIsStale(addressNodeParent(node)));
    // The stale node keeps its bytes for its holder.
    CHECK(addressNodeText(node) == "1");
    addressNodeRelease(node);

    // Resolving again reads the new bytes, and a step that no longer exists fails where it did.
    uint32_t failed = 0;
    CHECK(f.resolve("file:///box#a!x", &node, &failed) == PrStatus_NotFound);
    CHECK(failed == 1);
    REQUIRE(f.resolve("file:///box#a!y", &node) == PrStatus_Ok);
    CHECK(addressNodeText(node) == "2");

    addressResolverInvalidate(f.resolver, "file:///box");
    CHECK(addressNodeIsStale(node));
    addressNodeRelease(node);
}

TEST_CASE("a resource that read another relatively follows its changes") {
    Fixture x;
    AddressFixture& f = x.f;
    f.set("file:///dir/layer", record("needs", "base"));
    f.set("file:///dir/base", "B");
    AddressNode* node = nullptr;
    REQUIRE(f.resolve("file:///dir/layer#@needs", &node) == PrStatus_Ok);
    CHECK(!addressResolverPoll(f.resolver));
    f.set("file:///dir/base", "C");
    CHECK(addressResolverPoll(f.resolver));
    CHECK(addressNodeIsStale(node));
    addressNodeRelease(node);
}

TEST_CASE("the file reader reads file: URLs, and its stamp follows the file") {
    String directory = String::adopt(prFileUserDirectory(PrFileUserDirectory_Temporary));
    PrTextBuffer path = {};
    prTextBufferAppend(&path, directory);
    prFilePathAppend(&path, "prax_scribe_address_test.txt");
    PrStringView file = prTextBufferView(&path);
    REQUIRE(prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>("one"), 3}));
    String url = String::adopt(prUrlFromFilePath(file));
    PrBlob* blob = addressFileRead(nullptr, url);
    REQUIRE(blob != nullptr);
    CHECK(prBlobBytes(blob).size == 3);
    prBlobRelease(blob);
    uint64_t stamp = addressFileStamp(nullptr, url);
    CHECK(stamp != 0);
    REQUIRE(prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>("three"), 5}));
    CHECK(addressFileStamp(nullptr, url) != stamp);
    CHECK(prFileRemove(file));
    CHECK(addressFileStamp(nullptr, url) == 0);
    CHECK(addressFileRead(nullptr, url) == nullptr);
    CHECK(addressFileRead(nullptr, "https://example.com/a.png") == nullptr);
    prTextBufferReset(&path);
}
