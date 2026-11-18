// The selector for lib/gpu's backends. It reports which device backends the build includes, and creates a device of a
// given backend. Programs and composition libraries link it.

#pragma once

#include "pr/backend.h"
#include "pr/base.h"
#include "pr/gpu.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The device backends. Default is the backend that PR_BACKEND_GPU names (pr/backend.h). If the variable chooses
// nothing, Default is the first backend that the build includes, has not failed, and creates a device with the options.
// It tries the platform's own backend first and the memory backend last. A backend has failed once a creation by it
// fails with NotSupported, which only a creation finds out. If the failure is known when the creating call returns,
// Default moves on to the next backend at once. Otherwise the device is lost, and the application's recovery from the
// loss creates a device of the next backend.
enum {
    PrGpuBackend_Default,
    PrGpuBackend_Metal,
    PrGpuBackend_Metal4,
    PrGpuBackend_Vulkan,
    PrGpuBackend_D3d12,
    PrGpuBackend_D3d11,
    PrGpuBackend_Webgpu,
    PrGpuBackend_Memory,
    PrGpuBackend_Canvas2d,
    PrGpuBackend_Count,
};
typedef uint8_t PrGpuBackend;

// Returns the descriptor of the device interface.
const PrBackendInterface* prGpuSelectInterface(void);

// Creates a device of `backend` on the system's default adapter. Returns the device at once, while its creation may
// still run. Null options mean the defaults. A backend's own header creates a device over the application's native
// device. Returns null if the build does not include the backend, or if the backend creates no device with the options
// (PrGpuDeviceOptions::slotsOnly). For Default, it also returns null if PR_BACKEND_GPU is not valid
// (prBackendFromEnvironment).
//
// The device is ready once its creation is done (prGpuDeviceIsReady). Until then, it does no work, and the calls that
// make objects fail with InvalidState. If the creation fails, such as on a system without an adapter that the backend
// supports, the device is lost from the start. An application recovers from a failed creation as it recovers from a
// loss.
PrGpuDevice* PR_NULLABLE prGpuDeviceCreate(PrGpuBackend backend, const PrGpuDeviceOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
