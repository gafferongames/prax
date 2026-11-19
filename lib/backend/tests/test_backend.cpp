#include "pr/backend.h"
#include "pr/environment.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

extern "C" int prTestBackendCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// An interface of one backend at a time, whose build does not include its third backend.
constexpr PrStringView kGpuNames[] = {"default", "metal", "vulkan", "d3d12"};
constexpr PrBackendInterface kGpu{.name = "gpu", .names = kGpuNames, .count = 4, .built = 1u << 1 | 1u << 2};

// The result of reading an interface's choice with its variable set to `value`.
struct Read {
    PrStatus status;
    uint32_t backend;
    String message;

    bool says(PrStringView text) const { return prStringFind(message, text) != message.count(); }
};

Read read(const PrBackendInterface& interface, PrStringView variable, PrStringView value) {
    REQUIRE(prEnvironmentSetVariable(variable, value));
    Read out{};
    PrTextBuffer message{};
    out.status = prBackendFromEnvironment(&interface, &out.backend, &message);
    out.message = String(prTextBufferView(&message));
    prTextBufferReset(&message);
    REQUIRE(prEnvironmentRemoveVariable(variable));
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a backend is found by its name, ignoring case, and the default is built if any backend is") {
    CHECK(prBackendFromName(&kGpu, "Vulkan") == 2);
    CHECK(prBackendFromName(&kGpu, "opengl") == kGpu.count);
    CHECK(prBackendName(&kGpu, 2) == "vulkan");
    CHECK(prBackendName(&kGpu, 9).data == nullptr);
    CHECK(prBackendIsBuilt(&kGpu, 0));
    CHECK(prBackendIsBuilt(&kGpu, 1));
    CHECK(!prBackendIsBuilt(&kGpu, 3));
    CHECK(!prBackendIsBuilt(&kGpu, 9));
}

TEST_CASE("the environment chooses a backend") {
    Read gpu = read(kGpu, "PR_BACKEND_GPU", "Vulkan");
    REQUIRE(gpu.status == PrStatus_Ok);
    CHECK(gpu.backend == 2);
}

TEST_CASE("an unset or empty variable, or one that names the default, chooses nothing") {
    uint32_t backend = 1;
    CHECK(prBackendFromEnvironment(&kGpu, &backend, nullptr) == PrStatus_Ok);
    CHECK(backend == 0);
    CHECK(read(kGpu, "PR_BACKEND_GPU", "").backend == 0);
    Read named = read(kGpu, "PR_BACKEND_GPU", "default");
    CHECK(named.status == PrStatus_Ok);
    CHECK(named.backend == 0);
}

TEST_CASE("a backend that the build does not include is an error that lists what it includes") {
    Read r = read(kGpu, "PR_BACKEND_GPU", "d3d12");
    CHECK(r.status == PrStatus_NotSupported);
    CHECK(r.says("metal, vulkan"));
    CHECK(r.backend == 0);
}

TEST_CASE("a name that no backend has is an error, as is a list of names") {
    CHECK(read(kGpu, "PR_BACKEND_GPU", "opengl").status == PrStatus_NotFound);
    CHECK(read(kGpu, "PR_BACKEND_GPU", "metal,vulkan").status == PrStatus_NotFound);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestBackendCApi() == 0);
}
