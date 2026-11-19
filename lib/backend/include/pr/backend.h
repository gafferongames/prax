// Backend choices: the descriptor of an interface whose backend is chosen, and the choices that the environment makes.
//
// Each module with backends has a selector library, lib/<module>/select, which programs and composition libraries link.
// A selector's Default reads the environment variable PR_BACKEND_<NAME> of its interface:
//
//     PR_BACKEND_GPU=vulkan
//     PR_BACKEND_COMPRESS_DEFLATE=apple
//
// An unset or empty variable chooses nothing, and Default then takes the build's own default. Choosing a backend that
// the build does not include is an error, and the message lists the backends that the build includes. The default is
// never a fallback.

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The maximum number of backends of an interface, including its default.
enum { kPrBackendMaxBackends = 64 };

// An interface whose backend a program chooses.
typedef struct PrBackendInterface {
    // The interface's name, as PR_BACKEND_<NAME> spells it in uppercase: lowercase letters, digits and underscores.
    PrStringView name;

    // The name of each backend, indexed by its value, and the number of backends. Value 0 is the interface's default,
    // named "default".
    const PrStringView* names;
    uint32_t count;

    uint64_t built;     // bit b is set if the build includes backend b
} PrBackendInterface;

// Returns the value of the backend of `interface` named `name`, ignoring case. Returns the interface's count if no
// backend has that name.
uint32_t prBackendFromName(const PrBackendInterface* interface, PrStringView name);

// Returns the lowercase name of `backend`, as the environment spells it. Returns the null view if `backend` is not a
// backend of `interface`.
PrStringView prBackendName(const PrBackendInterface* interface, uint32_t backend);

// Returns true if the build includes `backend`. The build includes the default if it includes any backend.
bool prBackendIsBuilt(const PrBackendInterface* interface, uint32_t backend);

// Reads the backend of `interface` that the environment variable PR_BACKEND_<NAME> chooses, and writes its value to
// `*out`. An unset or empty variable, or one that names the default, writes 0, the default.
//
// On failure, `*out` is 0, and the call writes what went wrong to `message`, or to standard error if `message` is null.
// Returns NotFound if no backend has the name. Returns NotSupported if the build does not include the backend, and the
// message then lists the backends that the build includes.
PrStatus prBackendFromEnvironment(const PrBackendInterface* interface, uint32_t* out, PrTextBuffer* PR_NULLABLE message);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
