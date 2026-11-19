// Decoding the connectivity of a mesh that Edgebreaker codes.

#pragma once

#include "draco/src/draco_decoder.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Decodes the connectivity of a mesh that Edgebreaker codes, with the seams of its attributes, into the corner table,
// the seam tables and the faces (the bitstream's EdgeBreaker Decoder).
bool dracoDecodeEdgebreaker(Draco* draco);

// Reads how the stream's attributes are grouped and sequenced over an Edgebreaker mesh, one record for each group, and
// makes each group's sequence by its traversal.
bool dracoReadEdgebreakerSequences(Draco* draco, uint32_t count);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
