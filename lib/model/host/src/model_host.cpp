// The model host: the default formats and services of models.

#include "pr/model_host.h"

#include "pr/alloc.h"
#include "pr/gltf_model.h"
#include "pr/image_texture_source.h"
#include "pr/ktx_texture_source.h"
#include "pr/memory.h"
#include "pr/usd_model.h"

namespace pr_impl {

namespace {

// The host's formats, what USD's composes and reifies with, its opener and its pool.
struct ModelHost {
    PrModelFormat formats[2];
    PrUsdModelFormatDesc usd;
    uint32_t formatCount;
    PrTextureOpener* opener;
    PrTaskPool* PR_NULLABLE pool;
};

const PrBlobSource* PR_NULLABLE resolveFile(void* PR_NULLABLE, PrStringView location) {
    return prBlobSourceCreateFile(location);
}

constexpr PrModelResolver kFileResolver{.resolve = resolveFile};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrModelHost : ModelHost {};

PrModelHost* prModelHostCreate(const PrModelHostDesc* desc) {
    PrModelHost* host = allocZeroed<PrModelHost>();
    host->formats[0] = prGltfModelFormat();
    host->usd = PrUsdModelFormatDesc{.codec = desc->compress};
    host->formats[1] = prUsdModelFormat(&host->usd);
    host->formatCount = 2;
    PrTextureFormat textures[2];
    uint32_t count = 0;
    textures[count++] = prKtxTextureFormat(desc->compress);
    if (desc->images != nullptr)
        textures[count++] = prImageTextureFormat(desc->images);
    host->opener = prTextureOpenerCreate(textures, count);
    host->pool = desc->pool;
    return host;
}

void prModelHostDestroy(PrModelHost* PR_NULLABLE host) {
    if (host == nullptr)
        return;
    prTextureOpenerDestroy(host->opener);
    prMemoryFree(host);
}

uint32_t prModelHostFormatCount(const PrModelHost* host) {
    return host->formatCount;
}

const PrModelFormat* prModelHostFormats(const PrModelHost* host) {
    return host->formats;
}

const PrTextureOpener* prModelHostTextureOpener(const PrModelHost* host) {
    return host->opener;
}

PrModelOptions prModelHostModelOptions(const PrModelHost* host) {
    PrModelOptions options = prModelOptionsDefault();
    options.textures = host->opener;
    options.pool = host->pool;
    return options;
}

const PrModelResolver* prModelHostFileResolver(void) {
    return &kFileResolver;
}

PrModel* prModelHostOpen(const PrModelHost* host, const PrBlobSource* bytes, PrStringView location,
        const PrModelResolver* PR_NULLABLE resolver) {
    PrModelOptions options = prModelHostModelOptions(host);
    return prModelOpen(bytes, location, resolver != nullptr ? resolver : &kFileResolver, host->formats,
        host->formatCount, &options);
}

PrModel* prModelHostOpenFile(const PrModelHost* host, PrStringView path) {
    PrBlobSource* bytes = prBlobSourceCreateFile(path);
    PrModel* model = prModelHostOpen(host, bytes, path, nullptr);
    prBlobSourceRelease(bytes);
    return model;
}
