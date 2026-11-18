// Backend choices, read from the program's environment.

#include "pr/backend.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/environment.h"
#include "pr/file.h"
#include "pr/raw_string.h"

namespace pr_impl {

namespace {

// Appends the names of the backends of `interface` that the build includes, separated by commas.
void appendBuilt(const PrBackendInterface& interface, PrTextBuffer* out) {
    bool first = true;
    for (uint32_t b = 1; b < interface.count; ++b) {
        if (!prBackendIsBuilt(&interface, b))
            continue;
        if (!first)
            prTextBufferAppend(out, ", ");
        prTextBufferAppend(out, interface.names[b]);
        first = false;
    }
    if (first)
        prTextBufferAppend(out, "none");
}

// Reads `name` as the backend of `interface`, and writes its value to `*out`. `variable` names the environment variable
// in an error message.
PrStatus readBackend(const PrBackendInterface& interface, PrStringView name, PrStringView variable, uint32_t* out,
        PrTextBuffer* message) {
    uint32_t backend = prBackendFromName(&interface, name);
    if (backend == interface.count) {
        prTextBufferAppend(message, variable);
        prTextBufferAppend(message, ": ");
        prTextBufferAppend(message, interface.name);
        prTextBufferAppend(message, " has no backend named ");
        prTextBufferAppend(message, name);
        prTextBufferAppend(message, "\n");
        return PrStatus_NotFound;
    }
    if (!prBackendIsBuilt(&interface, backend)) {
        prTextBufferAppend(message, variable);
        prTextBufferAppend(message, ": the build does not include the ");
        prTextBufferAppend(message, name);
        prTextBufferAppend(message, " backend of ");
        prTextBufferAppend(message, interface.name);
        prTextBufferAppend(message, ". It includes ");
        appendBuilt(interface, message);
        prTextBufferAppend(message, "\n");
        return PrStatus_NotSupported;
    }
    *out = backend;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prBackendFromName(const PrBackendInterface* interface, PrStringView name) {
    for (uint32_t b = 0; b < interface->count; ++b) {
        if (prAsciiEqualsFold(interface->names[b], name))
            return b;
    }
    return interface->count;
}

PrStringView prBackendName(const PrBackendInterface* interface, uint32_t backend) {
    return backend < interface->count ? interface->names[backend] : PrStringView{};
}

bool prBackendIsBuilt(const PrBackendInterface* interface, uint32_t backend) {
    if (backend >= interface->count || backend >= kPrBackendMaxBackends)
        return false;
    if (backend == 0)
        return (interface->built & ~uint64_t(1)) != 0;
    return (interface->built >> backend & 1) != 0;
}

PrStatus prBackendFromEnvironment(const PrBackendInterface* interface, uint32_t* out, PrTextBuffer* PR_NULLABLE message) {
    PR_ASSERT(interface->count <= kPrBackendMaxBackends);
    *out = 0;
    PrTextBuffer variable{};
    prTextBufferAppend(&variable, "PR_BACKEND_");
    for (size_t c = 0; c < interface->name.size; ++c)
        prTextBufferAppendChar(&variable, prAsciiUpper(interface->name.data[c]));
    PrRawString value = prEnvironmentVariable(prTextBufferView(&variable));
    PrStatus status = PrStatus_Ok;
    if (prRawStringView(&value).size != 0) {
        PrTextBuffer written{};
        PrTextBuffer* to = message != nullptr ? message : &written;
        status = readBackend(*interface, prRawStringView(&value), prTextBufferView(&variable), out, to);
        if (message == nullptr && written.size != 0) {
            PrStringView text = prTextBufferView(&written);
            prFileStreamWrite(PrFileStream_Error, PrByteView(reinterpret_cast<const uint8_t*>(text.data), text.size));
        }
        prTextBufferReset(&written);
    }
    prRawStringReset(&value);
    prTextBufferReset(&variable);
    return status;
}
