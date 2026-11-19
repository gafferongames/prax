# Textures

`lib/texture` makes what a shader samples of an image, and keeps one copy of it on each device for every consumer that
samples it. It has three parts: sources, which produce a texture's bits on demand; format sets, which make sources of
encoded bytes without depending on any format; and the texture cache, a store of what sources realize on one device,
which lib/draw's renderers, lib/render and Studio's views share. How a consumer samples a texture, such as which
channel an input reads, whether as color or as data, and with which sampler, is the consumer's, never the texture's.
The headers state each call's contract: `pr/texture.h` for sources, format sets and uploads, and `pr/texture_cache.h`
for the cache. This file covers what the library is for, how its parts relate, and what it promises.

## Dependencies

```
lib/core (PrBlobSource)   lib/color   lib/gpu   lib/mime   lib/task
          \                   |          |        /           /
           +------------- lib/texture ---+-------+-----------+
                          /        \
          backends/ktx (lib/ktx,    backends/image          lib/model         lib/draw, lib/render, Studio
                        lib/basis)  (PrImageCodecSet)       (takes a          (sample what the cache realizes)
                                                            PrTextureFormatSet)
```

- **Only the backends reach a format.** `backends/ktx`
  adapts `lib/ktx`, whose transcoding comes from `lib/basis`, into the sources of KTX 2.0 files, and `backends/image`
  adapts an image codec set into the sources of every image that it decodes. The rest of the library includes no
  format's header (`format-dependency`), and `lib/ktx` and `lib/image` do not link `lib/texture`.
- **A library that loads textures takes a format set.** `lib/model` reaches no texture format. The program builds the
  set (`PrTextureFormatSet`) from the formats that it wants, or takes the selector's, and hands it over
  (`PrModelOptions::textures`).
- **A producer of drawings names sources, never textures.** A layout tree, an SVG drawing, a Lottie frame, a canvas
  and Kit's widgets put sources in their draw lists, and none of them sees a cache or a device
  ([draw.md](draw.md#images)).

## Sources

A source (`PrTextureSource`) is a texture's bits, produced on demand. It is immutable and reference counted.

- **The simplest source is bits that something has** (`prTextureSourceCreate`): a description (`PrTextureDesc`) and
  a function that produces the bits of the levels and layers that a read asks for. The description is the texture's
  kind (2D, array, cube, cube array or 3D), extent, depth, layers and levels, one GPU format, its alpha and its
  orientation. A KTX 2.0 file of the blocks that a GPU samples is such a source, and so is an application's texture,
  which fills a level from pixels that it keeps, procedurally, or by rendering it again.
- **A texture's color is its transfer, and sampling chooses it.** The only property of color that a texture has is
  whether a sampler decodes sRGB's transfer function when a shader reads it, which is the choice between a format's
  Srgb form and its plain one (Rgba8Srgb or Rgba8, Bc7Srgb or Bc7). A texture carries no primaries, and the working
  color space of an asset is not handled per texture. A source is of the format that its bits are in: a KTX 2.0 file
  whose descriptor states sRGB's transfer is of the Srgb form, and an image that cannot say whether its values are
  color, as a PNG that may hold a normal map, is of the plain one. A consumer does not relabel a source for its use: a
  realized texture is sampled through either form ([Format views](gpu.md#format-views)), and the consumer takes the
  one that its use reads.
- **A texture keeps its alpha.** A source is opaque, straight or premultiplied as its bits are, and every texture
  made of it reports the same (`PrTexture::alpha`). A consumer that needs another relation converts where it samples.
- **A texture keeps its values.** Floats stay linear, and 8-bit values stay in the encoding that they were stored in.
  A consumer that wants another encoding or another color space converts where it samples, or wraps the source in a
  conversion of its own.
- **Every other source wraps a source, and changes its format alone.**
  - *Decode*: an image's source decodes its encoded bytes to the pixels of R8, Rgba8, Rgba16F or Rgba32F.
  - *Transcode*: a KTX 2.0 file's source transcodes Basis Universal's ETC1S and UASTC to the blocks that `lib/basis`
    makes of them (BC7, ASTC, BC1, BC4, ETC2 and the others), or decodes them to pixels.
  - *Convert* (`prTextureSourceCreateConvert`): pixels to another plain format, channels moved or dropped, straight
    and premultiplied alpha, and the swizzled forms of lib/gpu (`PrGpuSwizzle`). Values keep their encoding between
    formats of 8 bits, and convert as a sampler reads them between 8 bits and floats: a Srgb form's values become
    linear floats, and floats become a Srgb form's encoding. A conversion that changes only a format's Srgb form or its
    swizzle reads the source's bits as they are, into the read's own storage, for blocks too.
  - *Mips* (`prTextureSourceCreateMips`): the mip levels that a source lacks, each the average of two by two texels
    of the level before, in linear light for a Srgb form.

  A format's source offers the forms that it decodes or transcodes to as formats of its own
  (`prTextureSourceFormats`), and makes the source of one of them (`prTextureSourceReformat`), so a consumer that knows
  no format chooses among them. Convert and mips are this library's, for every format.

## Reads

A read (`PrTextureRead`) asks a source for a range of its levels and layers, and completes once.

- **Reads are asynchronous.** A read completes during the call or later, on the thread that its innermost provider
  completes it on. A source of a format reads its bytes again from their provider (`PrBlobSource`, `pr/blob.h`) for
  each read, which may complete later, and a source that wraps another reads that one. The caller learns of the
  completion through a callback, by polling, or by waiting.
- **A read's storage is the caller's or its own.** The levels follow one another, each its layers in order, each its
  rows of texels or blocks with nothing between them, as lib/gpu's uploads take them
  (`prTextureReadSize`, `prTextureReadOffset`).
- **A source keeps nothing between reads.** A decode reads and decodes the bytes again, a conversion converts again,
  and a source of mip levels makes them again.
- **A CPU consumer reads sources directly.** A writer that embeds an image's texels, or an environment that a renderer
  prefilters on the CPU, reads the source, and no device is involved.

## Residency

A texture keeps the means to produce its data, never the data.

- **A source holds its byte provider**, the code that reads the encoded bytes again. A file's provider reads the file
  again (`prBlobSourceCreateFile`). An archive entry's or a package file's reads it again from the provider of its
  container. A network resource's keeps its bytes, consults a cache or fetches them again, by its own policy. A
  provider over a blob (`prBlobSourceCreateBlob`) keeps the bytes in memory, for bytes that nothing can read again, as
  a glTF document's own images.
- **An upload reads a chain once, and frees what it read** (`prTextureUpload`). It reads every level and layer of the
  chain, creates a sampled GPU texture of the chain's format, uploads the bits, and makes the rest of a mip chain on
  the device where it is asked to and the device renders to the format (`PrTextureUploadFlag_MakesLevels`). Nothing of
  the bits outlives the upload. It is the cache's primitive; a consumer that samples a source asks the cache.
- **After a device is lost, the cache realizes its sources again** on the new device, which reads the bits again from
  their providers. No path depends on this library having kept data.

## Discovery

A format set (`PrTextureFormatSet`) is an ordered list of formats (`PrTextureFormat`), as an image codec set is for
images.

- **Opening is asynchronous at the bytes** (`prTextureFormatSetOpen`). The set reads the resource, classifies its
  first bytes and its name with lib/mime, and gives the bytes to the first format that reads that media type and does
  not refuse them, which makes the format's source. `prTextureFormatSetRead` waits, for a caller that may block.
- **KTX 2.0 goes before the image codec set**, which decodes KTX 2.0's pixels too. The selector's `builtin` preset is:

```c
PrTextureFormat formats[] = {prKtxTextureFormat(compressCodecs), prImageTextureFormat(imageCodecs)};
PrTextureFormatSet* textures = prTextureFormatSetCreate(formats, 2);
```

| format | adapter | what it produces |
| --- | --- | --- |
| KTX 2.0 | `backends/ktx`, `prKtxTextureSourceOpen`, `prKtxTextureFormat` | its blocks of BC, ETC2, EAC and ASTC; Basis Universal's ETC1S and UASTC transcoded to the blocks that `lib/basis` makes; pixels of its levels, where they decode; levels, layers and cube faces; the Srgb forms of each where its descriptor states sRGB's transfer; supercompression through the compression codec set that it is handed |
| any image that a `PrImageCodecSet` decodes | `backends/image`, `prImageTextureSourceOpen`, `prImageTextureFormat` | pixels of 8 and 16 bits as Rgba8, floats and 32-bit integers as Rgba32F, and either as the others or as R8 alone, all of the plain forms; mip levels that halve as a texture's do; a cube's faces and layers; the image's alpha and EXIF orientation |
| an application's | `lib/texture`, `prTextureSourceCreate` | the levels that its function fills, in its one format |

OpenEXR, Radiance HDR and WebP have no sources of their own. Each reaches a texture through the image codec set, which
reports what a texture needs of them in `PrImageInfo`: OpenEXR's mip levels and its floats, Radiance HDR's floats, and
WebP's pixels and alpha. The chromaticities that OpenEXR states stay metadata of the image, which no texture acts on.
None of them stores blocks that a GPU samples, so a source of its own would give nothing that the codec's does not. A
rip map of OpenEXR is its first level: its levels do not halve as a texture's do.

## Threads

Sources, reads, format sets and byte providers follow the library's default: their const operations are safe on a shared
object, so `lib/model` opens sources in its loading jobs on the pool's threads, and the cache realizes them on its
executor's. A step of a job runs on the thread that completes its read. A provider over code that one thread must call,
as Scribe's address resolver is, reads its bytes on that thread when it is opened, and any thread reads the provider
after.

## The texture cache

A texture cache (`pr/texture_cache.h`) is a store of the textures that sources realize on one device. A host makes one
for each device, and gives it to every consumer that samples on that device: lib/draw's renderer
(`PrDrawRendererOptions::textures`), lib/render (`PrRenderOptions::textures`) and Studio's views. One image that a
material samples, a UI shows and a thumbnail draws is one texture.

- **A consumer asks for a source each time it samples it** (`prTextureCacheRealize`), and the first ask starts the
  realization. An ask states what the consumer reads of the texture (`PrTextureCacheAsk`): the channels, whether it
  needs every mip level, and whether it reads the values as color or as data, which decides how levels that the cache
  makes are filtered. Nothing else of a use reaches the cache.
- **Levels of color are filtered in linear light, whichever form a consumer samples.** A color ask's values are
  encoded with sRGB's transfer function even where the consumer reads them as stored, as lib/draw does. So the cache
  makes each level by decoding the level before to linear light, averaging, and encoding again: on the device through
  `prGpuCommandsGenerateMipmaps` for encoded color (`PrGpuMipFilter_Srgb`), or on the CPU through a source of mips over
  the source relabeled to its Srgb form, where the device does not make that format's levels. A data ask's levels
  average the values as stored. Levels that a source has of its own are what it made them.
- **The cache chooses what it realizes, from the source, the ask and the device**, never from the consumer. Blocks
  stay blocks where the device samples them, unless an ask needs levels that the texture lacks and its blocks decode,
  when its pixels are kept so that the device makes its mip chain. Basis Universal's formats transcode to the first of
  ASTC 4x4, BC7, EAC R11, BC4, EAC RG11, BC5, ETC2 RGBA8, BC3, ETC2 RGB8 and BC1 that the device samples, with those of one
  channel first for an ask that reads one. Floats realize as half floats of the same values. An ask that reads one
  channel of data realizes as R8 with that channel in red. On a device of swizzled formats, a gray image realizes in
  one channel of a swizzled form, which samples as the texture of four would, so no consumer knows. A color ask
  realizes in one channel only where that channel's format has a Srgb form on the device (R8Srgb, Rg8Srgb). A host
  that wants another choice gives the cache its own (`PrTextureCacheDesc::choose`), and `prTextureCacheChooseDefault`
  is this one.
- **Asks that agree share a realization.** An ask without levels is served by a realization with levels once it is
  ready. A color ask and a data ask share one where neither needs levels that the cache makes, or the source has its
  own: only made levels are filtered by what the values are. An ask of one channel that the cache would realize in
  fewer channels than another ask's is a realization of its own.
- **A realization is sampled through either form.** Its region (`PrTextureRegion`) gives the texture through its Srgb
  form, which a shader reads in linear light, and its plain form, which a shader reads as stored
  (`PrTexture::srgb`, `PrTexture::plain`); a format of one form gives the same texture for both. The cache creates
  each texture that has both forms with the views it needs (`PrGpuTextureUsageFlag_SrgbViews`), and the consumer takes
  the form that its use reads.
- **It owns no threads.** The host gives it an executor (`PrTextureCacheDesc::tasks`), and a realization reads the
  source's bits and uploads them there. Without an executor, a realization runs in the ask that starts it, so a host
  that renders once, as a test or a capture, draws every image at once.
- **It reports arrivals.** It calls the host's `arrived` as a realization that an ask found pending arrives or fails,
  so that the host draws again. A consumer that must sample an image whole waits for it (`prTextureCacheWait`).
- **It owns placement.** A region is a texture of its own or a slot of a page that small sources share. The cache
  packs on every device that draws through a GPU, on either binding tier: a texture of its own costs a renderer a
  binding, and a draw where the renderer batches by texture. It does not pack on a memory device, which reads texels in
  place, where a page would only add a copy. A slot holds a source of 8 bits of at most 256 texels a side, of one
  level, for an ask without levels. The first page of a format is 512 texels square, and each next one twice the side
  of the last, up to 2048 or the device's largest. A slot whose source a realization with levels serves is freed once
  that realization arrives.
- **It keeps what others hold.** The cache holds a reference to each source that it realized, and keeps its region
  while anything else holds the source, since a drawing that keeps the source may draw the region. A trim at the end of
  a frame (`prTextureCacheTrim`) evicts, least recently asked for first, the realizations whose sources only it holds
  and that no ask since the last trim asked for, until they fit its budget, and forgets their failures.
- **It survives a lost device.** `prTextureCacheSetDevice` frees what the cache made on the old device and moves it to
  the new one, where every source realizes again as it is asked for. The consumers on the new device ask again as they
  sample, so a producer's drawings and a model's sources stay as they were.
