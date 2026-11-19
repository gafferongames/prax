// The lookup tables of a renderer's shader set (pr/render_shaders.h): one texture that holds every table of the set,
// which the set's shaders sample (pr/render_table.slang).

#pragma once

#include "pr/base.h"
#include "pr/status.h"

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Makes the texture of the tables of the renderer's shader set, and fills each table by its function. Returns Limit if
// the device has no memory or handle for the texture, or if the tables are larger than a texture of the device may be,
// and Lost if the device is lost.
PrStatus renderTablesCreate(Render* render);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
