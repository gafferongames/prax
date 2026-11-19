// The draco harness, which tools/suite/draco.py drives: lib/draco's decoder beside the reference decoder, over Draco
// streams in files.
//
// `draco_compare [--time] <path>...` decodes the stream at each path with both decoders and writes a line of JSON for
// each:
//
//   {"status": the name of lib/draco's status, or "" for Ok, "reference": whether the reference decoder decodes it,
//    "equal": whether the two give the same geometry, "points": its points, "faces": its faces,
//    "attributes": its attributes, "time": {"prax": milliseconds, "reference": milliseconds}}
//
// The geometries are the same if they have the same faces, the same points in the same order, and the same value of
// every attribute to the bit. "time" is present with --time: what one decoding of the stream takes with each decoder,
// measured as the harness measures a benchmark (harnessMeasure).

#include "pr/draco.h"
#include "pr/status.h"
#include "pr/text_buffer.h"

#include "draco/compression/decode.h"
#include "tools/harness/harness.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace harness;

namespace {

// Returns true if lib/draco's geometry is the reference decoder's.
bool sameGeometry(const PrDraco* draco, const draco::Mesh& mesh) {
    if (prDracoPointCount(draco) != mesh.num_points() || prDracoFaceCount(draco) != mesh.num_faces() ||
            prDracoAttributeCount(draco) != uint32_t(mesh.num_attributes())) {
        return false;
    }
    const uint32_t* faces = prDracoFaces(draco);
    for (uint32_t f = 0; f < mesh.num_faces(); ++f) {
        const draco::Mesh::Face& face = mesh.face(draco::FaceIndex(f));
        for (uint32_t c = 0; c < 3; ++c) {
            if (faces[f * 3 + c] != face[c].value())
                return false;
        }
    }
    for (uint32_t a = 0; a < prDracoAttributeCount(draco); ++a) {
        const draco::PointAttribute* attribute = mesh.attribute(int(a));
        const PrDracoAttribute& info = prDracoAttributes(draco)[a];
        size_t elementSize = size_t(attribute->byte_stride());
        PrByteView values = prDracoAttributeValues(draco, a);
        if (info.id != attribute->unique_id() || int(info.componentType) + 1 != int(attribute->data_type()) ||
                info.componentCount != attribute->num_components() || values.size != elementSize * mesh.num_points()) {
            return false;
        }
        std::vector<uint8_t> value(elementSize);
        for (uint32_t p = 0; p < mesh.num_points(); ++p) {
            attribute->GetMappedValue(draco::PointIndex(p), value.data());
            if (std::memcmp(value.data(), values.data + p * elementSize, elementSize) != 0)
                return false;
        }
    }
    return true;
}

void run(const char* path, bool times) {
    PrTextBuffer file = {};
    PrTextBuffer out = {};
    if (!harnessReadFile(&file, path)) {
        harnessWriteLine("{\"status\":\"Io\",\"reference\":false,\"equal\":false,\"points\":0,\"faces\":0,\"attributes\":0}");
        return;
    }
    PrByteView stream = {reinterpret_cast<const uint8_t*>(file.data), file.size};
    PrDracoLimits limits = {.maxPoints = 1u << 26, .maxFaces = 1u << 26};
    PrDraco* draco = nullptr;
    PrStatus status = prDracoCreate(stream, &limits, &draco);

    draco::DecoderBuffer buffer;
    buffer.Init(file.data, file.size);
    draco::Decoder decoder;
    draco::StatusOr<std::unique_ptr<draco::Mesh>> decoded = decoder.DecodeMeshFromBuffer(&buffer);
    bool equal = draco != nullptr && decoded.ok() && sameGeometry(draco, *decoded.value());

    harnessAppendFormat(&out, "{\"status\":\"%s\",\"reference\":%s,\"equal\":%s,\"points\":%u,\"faces\":%u,"
        "\"attributes\":%u", prStatusName(status).data, decoded.ok() ? "true" : "false", equal ? "true" : "false",
        draco != nullptr ? prDracoPointCount(draco) : 0, draco != nullptr ? prDracoFaceCount(draco) : 0,
        draco != nullptr ? prDracoAttributeCount(draco) : 0);
    prDracoDestroy(draco);
    if (times) {
        double prax = harnessMeasure([&] {
            PrDraco* again = nullptr;
            prDracoCreate(stream, &limits, &again);
            prDracoDestroy(again);
        }).millis;
        double reference = harnessMeasure([&] {
            draco::DecoderBuffer input;
            input.Init(file.data, file.size);
            draco::Decoder again;
            again.DecodeMeshFromBuffer(&input);
        }).millis;
        harnessAppendFormat(&out, ",\"time\":{\"prax\":%.6g,\"reference\":%.6g}", prax, reference);
    }
    prTextBufferAppendChar(&out, '}');
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
    prTextBufferReset(&file);
}

} // namespace

int main(int argc, char** argv) {
    bool times = false;
    int first = 1;
    if (first < argc && std::strcmp(argv[first], "--time") == 0) {
        times = true;
        ++first;
    }
    for (int i = first; i < argc; ++i)
        run(argv[i], times);
    return 0;
}
