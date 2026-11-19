// The address handler of ZIP archives: a step is the name of an entry as the archive gives it, with '/' separators, and
// reaches the entry's bytes.

#pragma once

#include "pr/compress.h"

#include "apps/scribe/address.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

// Creates the handler of ZIP archives, which decompresses deflated entries with `codec`. Without a codec, a step to a
// deflated entry is NotSupported. A stored entry's bytes are read in place, and a deflated entry's are decoded once its
// declared size fits the budget, and checked against its checksum. `codec` outlives the handler. The caller frees the
// handler with addressHandlerDestroy.
AddressHandler* zipAddressHandlerCreate(const PrCompressCodecSet* PR_NULLABLE codec);

} // namespace scribe

PR_ASSUME_NONNULL_END
