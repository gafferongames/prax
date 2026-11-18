// The selector for lib/app's backends.

#include "pr/app_select.h"

#if defined(PR_APP_HAS_APPKIT)
#include "pr/app_appkit.h"
#endif
#if defined(PR_APP_HAS_UIKIT)
#include "pr/app_uikit.h"
#endif
#if defined(PR_APP_HAS_WIN32)
#include "pr/app_win32.h"
#endif
#if defined(PR_APP_HAS_WEB)
#include "pr/app_web.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrAppBackend_Count] = {"default", "appkit", "uikit", "win32", "web"};

// The backends that the build includes, in order of preference. Default ends the list, so that a build without backends
// creates no application.
constexpr PrAppBackend kBuilt[] = {
#if defined(PR_APP_HAS_APPKIT)
    PrAppBackend_AppKit,
#endif
#if defined(PR_APP_HAS_UIKIT)
    PrAppBackend_UiKit,
#endif
#if defined(PR_APP_HAS_WIN32)
    PrAppBackend_Win32,
#endif
#if defined(PR_APP_HAS_WEB)
    PrAppBackend_Web,
#endif
    PrAppBackend_Default,
};

constexpr uint64_t builtMask() {
    uint64_t mask = 0;
    for (PrAppBackend backend : kBuilt)
        mask |= uint64_t(1) << backend;
    return mask & ~uint64_t(1);
}

constexpr PrBackendInterface kInterface{
    .name = "app",
    .names = kBackendNames,
    .count = PrAppBackend_Count,
    .built = builtMask(),
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrBackendInterface* prAppSelectInterface(void) {
    return &kInterface;
}

PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppOptions* PR_NULLABLE options) {
    PrAppOptions chosen = options != nullptr ? *options : prAppOptionsDefault();
    // A build without a backend has no use for the options.
    (void)chosen;
    if (backend == PrAppBackend_Default) {
        uint32_t environment = 0;
        if (prBackendFromEnvironment(&kInterface, &environment, nullptr) != PrStatus_Ok)
            return nullptr;
        backend = environment != PrAppBackend_Default ? PrAppBackend(environment) : kBuilt[0];
    }
    switch (backend) {
#if defined(PR_APP_HAS_APPKIT)
        case PrAppBackend_AppKit: return prAppAppKitCreate(&chosen);
#endif
#if defined(PR_APP_HAS_UIKIT)
        case PrAppBackend_UiKit: return prAppUiKitCreate(&chosen);
#endif
#if defined(PR_APP_HAS_WIN32)
        case PrAppBackend_Win32: return prAppWin32Create(&chosen);
#endif
#if defined(PR_APP_HAS_WEB)
        case PrAppBackend_Web: return prAppWebCreate(&chosen, nullptr);
#endif
        default: return nullptr;
    }
}
