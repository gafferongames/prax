// The model host: the wiring that opens a model of a file in one call, for programs that choose it.
//
// lib/model depends on no format: it opens a model with the formats that it is handed (prModelOpen). The host is the
// companion library that a program links to hand it the default ones, as the backend strategy's hosts hand a program
// the backends of an interface: the model formats of glTF and USD, the texture formats of KTX 2.0 and of the images
// that an image codec decodes, and a resolver of the file system. It chooses within lib/model's formats alone. The
// image and compression codecs are the program's choice, which it hands the host, as `image_host` is handed a
// compression codec set.
//
//     PrModelHost* host = prModelHostCreate(&desc);
//     PrModel* helmet = prModelHostOpenFile(host, "helmet.glb");
//
// Threads: a host's const operations are safe on a shared host, as lib/model's sources are.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/compress.h"
#include "pr/image.h"
#include "pr/model.h"
#include "pr/model_source.h"
#include "pr/task.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The default formats and services of models.
typedef struct PrModelHost PrModelHost;

// What a host builds its formats with: the image codec that decodes the image formats of textures, or null for none,
// the compression codec that decodes KTX 2.0's supercompression and USD's crate files, or null for none, and the pool
// that its models load on, or null to load one job at each prModelUpdate. Each outlives the host.
typedef struct PrModelHostDesc {
    const PrImageCodecSet* PR_NULLABLE images;
    const PrCompressCodecSet* PR_NULLABLE compress;
    PrTaskPool* PR_NULLABLE pool;
} PrModelHostDesc;

// Creates a host, or frees one. The models that a host opened outlive it only once their opening has completed.
PrModelHost* prModelHostCreate(const PrModelHostDesc* desc);
void prModelHostDestroy(PrModelHost* PR_NULLABLE host);

// Return the host's model formats, glTF's and then USD's, its texture opener, KTX 2.0 and then the image codec's, and
// the options of a model that it opens: its opener and its pool.
uint32_t prModelHostFormatCount(const PrModelHost* host);
const PrModelFormat* prModelHostFormats(const PrModelHost* host);
const PrTextureOpener* prModelHostTextureOpener(const PrModelHost* host);
PrModelOptions prModelHostModelOptions(const PrModelHost* host);

// Returns the resolver of the file system, which opens a location as a file's path.
const PrModelResolver* prModelHostFileResolver(void);

// Opens a model of the resource that `bytes` provides at `location` with the host's formats and options, and what it
// names through `resolver`, or the file system's for null, which outlives the opening (prModelOpen).
PrModel* prModelHostOpen(const PrModelHost* host, const PrBlobSource* bytes, PrStringView location,
    const PrModelResolver* PR_NULLABLE resolver);

// Opens a model of the file at `path`, and of the files beside it that it names.
PrModel* prModelHostOpenFile(const PrModelHost* host, PrStringView path);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
