// The classification harness, which tools/suite/classify.py drives.
//
// `mime_corpus <path>...` classifies the file at each path by its first bytes and its name (prMimeSniff), and writes a
// line of JSON for each: {"format": the format's name, "type": its media type}, with the format "unreadable" for a file
// that cannot be read.

#include "pr/assert.h"
#include "pr/file.h"
#include "pr/mime_sniff.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

using namespace harness;

namespace {

void run(const char* path) {
    PrBlob* blob = prFileOpen(path);
    PrTextBuffer out = {};
    prTextBufferAppend(&out, "{\"format\":");
    if (blob == nullptr) {
        harnessWriteString(&out, "unreadable");
        prTextBufferAppend(&out, ",\"type\":\"\"}");
    } else {
        PrMimeFormatInfo info = prMimeFormatInfo(prMimeSniff(prBlobBytes(blob), path));
        harnessWriteString(&out, info.name);
        prTextBufferAppend(&out, ",\"type\":");
        harnessWriteString(&out, info.type);
        prTextBufferAppendChar(&out, '}');
    }
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
    prBlobRelease(blob);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    for (int i = 1; i < argc; ++i)
        run(argv[i]);
    return 0;
}
