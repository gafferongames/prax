// Blobs: immutable bytes shared by reference, which whoever made them frees.
//
// A blob holds bytes and what frees them. Whatever makes bytes hands them out as a blob: a file that the platform maps
// (prFileOpen), a resource of the program (prResourceOpen), or a copy. Whatever keeps bytes retains the blob, rather
// than copying them or holding a mapping of its own, and releases it when it is done. The last release frees the bytes
// as their maker asked, so the holder never knows how they were made.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Bytes and what frees them, shared by reference.
typedef struct PrBlob PrBlob;

// Called with `context` and the bytes of a blob once its last reference is released, on the thread that releases it.
typedef void (*PrBlobReleaseFn)(void* PR_NULLABLE context, PrByteView bytes);

// Returns a blob of `bytes`, which it takes: they stay valid and unchanged until `release` is called with `context` and
// the bytes, once the blob's last reference is released. The caller holds the blob's one reference.
PrBlob* prBlobCreate(PrByteView bytes, PrBlobReleaseFn release, void* PR_NULLABLE context);

// Returns a blob of a copy of `bytes`. The caller holds the blob's one reference.
PrBlob* prBlobCopy(PrByteView bytes);

// Returns the blob's bytes, which stay valid while a reference to the blob is held.
PrByteView prBlobBytes(const PrBlob* blob);

// Takes a reference to the blob. May be called from any thread.
void prBlobRetain(PrBlob* blob);

// Releases a reference to the blob. The last one frees the blob and releases its bytes. May be called from any thread.
void prBlobRelease(PrBlob* PR_NULLABLE blob);

// Returns a blob of the `size` bytes of `blob` from `offset`, clamped to its bytes, which shares them and keeps `blob`
// alive. The caller holds the blob's one reference.
PrBlob* prBlobSlice(PrBlob* blob, uint64_t offset, uint64_t size);

// ---------------------------------------------------------------------------------------------------------------------
// Blob sources
//
// A source is a provider of one resource's bytes, which reads them again each time they are wanted. What holds a source
// keeps the means to read the bytes rather than the bytes themselves: a source of a file reads the file again, and a
// source made by the code that reads an archive's entry or fetches a network resource reads it again by that code's own
// policy, which may keep the bytes, consult a cache or fetch them again. Sources compose: a source of an archive's
// entry reads its range of the archive from the source of the archive, which may itself be an entry of another. A
// source is immutable and reference counted, and any thread may read it.
//
// A read is a request that completes later. It names a range of the bytes, the whole resource being the ordinary case,
// and receives them in a blob or in storage that the caller provides. It completes on whatever thread its provider
// completes it on, which may be the thread that starts it, during the call: a provider of bytes in memory or of a file
// on a platform where reading one is cheap completes at once, and one over the network or a page's event loop completes
// later. Nothing requires a provider to complete during the call. The caller learns of the completion through a
// callback, or polls the read, or waits for it, which blocks the thread or, in a program that suspends (JSPI), the
// program.

// A provider of a resource's bytes.
typedef struct PrBlobSource PrBlobSource;

// A read of a provider's bytes, which completes once.
typedef struct PrBlobRead PrBlobRead;

// A size that is not known, and as a range's size, the bytes to the resource's end.
PR_CONSTANT uint64_t kPrBlobSizeUnknown = 0xffffffffffffffffull;

// The `size` bytes of a resource from `offset`. A range that reaches past the resource's end holds the bytes to the
// end.
typedef struct PrBlobRange {
    uint64_t offset;
    uint64_t size;
} PrBlobRange;

// Returns the range of a whole resource.
PrBlobRange prBlobRangeWhole(void);

// Called once when a read completes, with the read, on the thread that completes it.
typedef void (*PrBlobReadDoneFn)(void* PR_NULLABLE context, PrBlobRead* read);

// Called with a source's `context` to begin `read`: the provider reads the range that the read names (prBlobReadRange),
// and completes it then or later, on any thread (prBlobReadComplete).
typedef void (*PrBlobSourceStartFn)(void* PR_NULLABLE context, PrBlobRead* read);

// Called with a source's `context` once the source's last reference is released, on the thread that releases it.
typedef void (*PrBlobSourceReleaseFn)(void* PR_NULLABLE context);

// What a provider is: its function, its context, and the size of its resource, or kPrBlobSizeUnknown.
typedef struct PrBlobSourceDesc {
    PrBlobSourceStartFn start;
    PrBlobSourceReleaseFn PR_NULLABLE release;
    void* PR_NULLABLE context;              // valid until `release` is called with it
    uint64_t size;
} PrBlobSourceDesc;

// Returns a source whose reads `desc->start` begins. The caller holds the source's one reference.
PrBlobSource* prBlobSourceCreate(const PrBlobSourceDesc* desc);

// Returns a source whose bytes are `blob`'s, which it retains until its last reference is released. Its bytes stay in
// memory for as long as the source lives, which suits bytes that nothing can read again. Its reads complete at once.
PrBlobSource* prBlobSourceCreateBlob(PrBlob* blob);

// Returns a source that reads the file at `path` (prFileOpen) each time it is read, and completes each read at once.
// Reading a file that cannot be opened completes with Io.
PrBlobSource* prBlobSourceCreateFile(PrStringView path);

// Returns a source of a range of `source`'s bytes, which it retains: each read of it is a read of the range that it
// maps to in `source`.
PrBlobSource* prBlobSourceCreateRange(const PrBlobSource* source, PrBlobRange range);

// Takes a reference to the source, or releases one. The last release releases the source's context.
void prBlobSourceRetain(const PrBlobSource* source);
void prBlobSourceRelease(const PrBlobSource* PR_NULLABLE source);

// Returns the size of the source's resource in bytes, or kPrBlobSizeUnknown if its provider does not know it.
uint64_t prBlobSourceSize(const PrBlobSource* source);

// What a read asks for: a range, and the storage that receives its bytes, or null for a blob. Storage holds the range's
// size, which is then known. `done`, if it is given, is called once the read completes.
typedef struct PrBlobReadDesc {
    PrBlobRange range;
    uint8_t* PR_NULLABLE storage;
    PrBlobReadDoneFn PR_NULLABLE done;
    void* PR_NULLABLE context;              // passed to `done`
} PrBlobReadDesc;

// Returns a description of a read of the whole resource into a blob, without a callback.
PrBlobReadDesc prBlobReadDescDefault(void);

// Starts a read of the source, which the caller destroys once it has completed (prBlobReadDestroy): in `done`, or after
// it.
PrBlobRead* prBlobReadCreate(const PrBlobSource* source, const PrBlobReadDesc* desc);

// Returns true if the read has completed. Any thread may poll it.
bool prBlobReadIsDone(const PrBlobRead* read);

// Returns once the read has completed: at once if it has, or after blocking the calling thread, or suspending a program
// that suspends, until its provider completes it.
void prBlobReadWait(PrBlobRead* read);

// Return how a completed read went: Ok, or Io for bytes that cannot be read, or the status of the code that reads them;
// its bytes as a blob, of a read without storage that succeeded, which stays valid while the read lives and which the
// caller retains to keep; and the number of its bytes, which a range past the resource's end makes fewer than the
// range's size.
PrStatus prBlobReadStatus(const PrBlobRead* read);
PrBlob* PR_NULLABLE prBlobReadBlob(const PrBlobRead* read);
uint64_t prBlobReadByteCount(const PrBlobRead* read);

// Frees a completed read, and releases its blob.
void prBlobReadDestroy(PrBlobRead* PR_NULLABLE read);

// Reads the range of the source into a blob at `*out`, which the caller releases, and waits for the read. Returns the
// read's status, and on failure, `*out` is null.
PrStatus prBlobSourceRead(const PrBlobSource* source, PrBlobRange range, PrBlob* PR_NULLABLE* PR_NONNULL out);

// For a provider: return the range that a read asks for, and the storage that receives it, or null for a blob.
PrBlobRange prBlobReadRange(const PrBlobRead* read);
uint8_t* PR_NULLABLE prBlobReadStorage(const PrBlobRead* read);

// For a provider: completes a read with `status`, and on success with the range's bytes in `bytes`, whose reference it
// takes. A read with storage receives a copy of them, or, with null `bytes`, the `byteCount` bytes that the provider
// wrote to its storage itself. The read's callback runs during the call. The read must not be completed twice.
void prBlobReadComplete(PrBlobRead* read, PrStatus status, PrBlob* PR_NULLABLE bytes, uint64_t byteCount);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
