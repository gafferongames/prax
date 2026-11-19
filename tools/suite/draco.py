#!/usr/bin/env python3
"""Decodes the Draco streams of Khronos's glTF sample assets with lib/draco and with the reference decoder.

The corpus is the `gltf_sample_assets` checkout. A case is one asset whose primitives have Draco streams
(KHR_draco_mesh_compression), and its key is its path under Models, as Duck/glTF-Draco/Duck.gltf. The runner writes
each stream of an asset to a file under `<build>/suites/draco`, and the draco_compare harness decodes each with
lib/draco and with the reference decoder, which is fetched for the development build alone
(tools/suite/draco/draco_compare.cpp).

A case passes if both decoders decode every stream of the asset and give the same geometry for each: the same faces,
the same points in the same order, and the same value of every attribute to the bit. The run fails on a case that
fails without an entry in the errata, a recorded case that passes, or an entry that names no case. `--time` also
prints what decoding takes with each decoder, for each asset and for its largest stream, and leaves the scoreboard
alone.
"""

import base64
import json
import os
import struct
import subprocess
import sys

from tools.suite import corpus
from tools.suite import gltf

EXTENSION = "KHR_draco_mesh_compression"


def streams(path):
    """Returns the bytes of each Draco stream of the asset at `path`, one for each buffer view that a primitive's
    extension names, in the order of the primitives."""
    with open(path, "rb") as f:
        data = f.read()
    chunk = None
    if data[:4] == b"glTF":
        length, = struct.unpack_from("<I", data, 12)
        document = json.loads(data[20:20 + length].decode("utf-8-sig"))
        if len(data) >= 28 + length:
            chunk = data[28 + length:]
    else:
        document = json.loads(data.decode("utf-8-sig"))
    buffers = {}

    def buffer(index):
        if index not in buffers:
            uri = document["buffers"][index].get("uri")
            if uri is None:
                buffers[index] = chunk
            elif uri.startswith("data:"):
                buffers[index] = base64.b64decode(uri.split(",", 1)[1])
            else:
                with open(os.path.join(os.path.dirname(path), uri), "rb") as f:
                    buffers[index] = f.read()
        return buffers[index]

    views = []
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            draco = primitive.get("extensions", {}).get(EXTENSION)
            if draco is not None and draco["bufferView"] not in views:
                views.append(draco["bufferView"])
    result = []
    for index in views:
        view = document["bufferViews"][index]
        start = view.get("byteOffset", 0)
        result.append(buffer(view["buffer"])[start:start + view["byteLength"]])
    return result


def main():
    ap = corpus.arguments(corpus.binary("draco_compare"),
                          os.path.join(corpus.ROOT, "third_party", "glTF-Sample-Assets", "Models"))
    ap.add_argument("--time", action="store_true", help="print what decoding takes with each decoder")
    args = ap.parse_args()
    if not os.path.isdir(args.tests):
        sys.exit("glTF corpus missing; run ./dev.sh fetch gltf_sample_assets")
    suite = corpus.Data("draco")
    keys = [key for key in gltf.cases(args.tests)
            if EXTENSION in gltf.required_extensions(os.path.join(args.tests, *key.split("/")))]
    chosen = [key for key in keys if suite.runs(key, args.match)]
    if not chosen:
        sys.exit("no draco cases selected")
    if args.list:
        return corpus.listing([("assets", chosen)])
    corpus.require(args.binary, "draco")

    out = os.path.join(os.path.dirname(os.path.abspath(args.binary)), "suites", "draco")
    os.makedirs(out, exist_ok=True)
    passed = 0
    failures = 0
    totals = {"streams": 0, "points": 0, "faces": 0}
    outcomes = {}
    for key in chosen:
        paths = []
        for i, stream in enumerate(streams(os.path.join(args.tests, *key.split("/")))):
            paths.append(os.path.join(out, "%s.%d.drc" % (key.replace("/", "."), i)))
            with open(paths[-1], "wb") as f:
                f.write(stream)
        done = subprocess.run([args.binary, *(["--time"] if args.time else []), *paths], stdout=subprocess.PIPE)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        results = [json.loads(line) for line in lines] if done.returncode == 0 and len(lines) == len(paths) else []
        ok = bool(results) and all(result["equal"] for result in results)
        for result in results:
            totals["streams"] += 1
            totals["points"] += result["points"]
            totals["faces"] += result["faces"]
        reason = suite.recorded(key)
        outcomes[key] = 1 if ok else 0
        if ok:
            passed += 1
            if reason is not None:
                failures += 1
                print("%s passes now; drop its erratum" % key)
        elif reason is not None:
            passed += 1
        else:
            failures += 1
            wrong = [json.dumps(result) for result in results if not result["equal"]]
            print("%s  %s" % (key, wrong[0] if wrong else "the harness ended"))
        if args.time and results:
            largest = max(results, key=lambda result: result["faces"])
            print("%-56s %4d streams  lib/draco %8.3f ms  reference %8.3f ms; largest, %d faces: %.3f ms and %.3f ms" %
                  (key, len(results), sum(result["time"]["prax"] for result in results),
                   sum(result["time"]["reference"] for result in results), largest["faces"],
                   largest["time"]["prax"], largest["time"]["reference"]))

    rows = [("reference decoder", passed, len(chosen))]
    print("reference decoder  %d/%d" % (passed, len(chosen)))
    notes = corpus.table(("Streams", "Points", "Faces"),
                         [(str(totals["streams"]), str(totals["points"]), str(totals["faces"]))])
    print("The streams of the cases counted: %d, which decode to %d points and %d faces." %
          (totals["streams"], totals["points"], totals["faces"]))
    print("\n%d/%d pass or are recorded; %d fail" % (passed, len(chosen), failures))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args) and not args.time:
        suite.write_scoreboard(rows, notes)
    stale = [] if args.match else suite.stale(set(keys))
    for kind, key in stale:
        print("%s names no case (%s)" % (key, kind))
    return int(bool(failures or stale))


if __name__ == "__main__":
    sys.exit(main())
