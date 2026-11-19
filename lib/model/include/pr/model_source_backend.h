// The interface that model sources implement: a format's adapter, as pr/gltf_model.h and pr/usd_model.h are. Models use
// the functions of pr/model_source.h, which call these. Only sources include this header.

#pragma once

#include "pr/base.h"
#include "pr/model_source.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a source, at the start of its state. `infoFn` returns the description, which does not change.
// `meshReadFn` implements prModelSourceMeshRead and `imageOpenFn` prModelSourceImageOpen, which any thread calls at
// once. `nodeAddressFn` and `nodeFromAddressFn` implement the addresses of nodes. `destroyFn` frees the source.
struct PrModelSource {
    void (*destroyFn)(PrModelSource* source);
    const PrModelSourceInfo* PR_NONNULL (*PR_NONNULL infoFn)(const PrModelSource* source);
    PrStatus (*meshReadFn)(const PrModelSource* source, PrModelMeshIndex mesh, const PrModelMeshRequest* request,
        PrModelMeshData* out);
    PrStatus (*imageOpenFn)(const PrModelSource* source, PrModelImageIndex image, PrModelImageData* out);
    void (*nodeAddressFn)(const PrModelSource* source, PrModelNodeIndex node, PrTextBuffer* out);
    PrModelNodeIndex (*nodeFromAddressFn)(const PrModelSource* source, PrStringView address);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
