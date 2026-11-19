// Undoing the prediction of an attribute's integers.

#pragma once

#include "draco/src/draco_decoder.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Reads what prediction scheme `method` needs, with transform `transform`, and undoes the prediction: `values` holds
// the corrections of the attribute's values, each of its portable components, and becomes the values.
bool dracoDecodePrediction(Draco* draco, DracoAttributeState* attribute, int method, int transform, int32_t* values);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
