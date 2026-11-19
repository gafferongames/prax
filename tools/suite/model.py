#!/usr/bin/env python3
"""Renders Khronos's glTF sample assets with lib/model.

The corpus is the `gltf_sample_assets` checkout, and the assets are those of the glTF and glTF-Binary variants of each
model, its core form with its files beside it or in one binary file, of the variants whose meshes are quantized
(KHR_mesh_quantization) or compressed (EXT_meshopt_compression, KHR_meshopt_compression, KHR_draco_mesh_compression),
of the variants whose textures are KTX 2.0 files of Basis Universal (KHR_texture_basisu), and of the variant whose
textures are WebP images (EXT_texture_webp). The model_corpus harness loads each asset as a model, renders it offscreen
on a device with handles and on one with the slots alone, and writes its image to `<build>/suites/model` for a person to
look at (lib/model/tests/model_corpus.cpp). No reference images exist, so the suite checks what a model and a renderer
owe each other, not that a picture is right.

A case is one check of one asset, and its key is the asset's path under Models and the check's number, from zero in
the order below, as Duck/glTF/Duck.gltf:2 for slots. The checks are the parts of the scoreboard:

- loads: the asset reads, and every image and mesh of the model loads.
- validation: the devices report no validation error. Metal's validation ends the harness instead, which fails every
  check of the asset as a crash.
- slots: the device with the slots alone draws the image that the device with handles draws, pixel for pixel to within
  the rounding of the tiers' two compiled libraries, a level of 255 (model_corpus.cpp's kTierLevels), unless
  the model reports that a material names more textures than that device samples: the asset is then counted under "Not
  drawn", and its two images differ by the layers whose textures the device reads as their factors alone.
- techniques: the visibility technique draws what the forward technique draws, away from geometric edges. A pixel is on
  an edge if four samples change it. Of the others, at most one in two hundred may differ.
- drawn: the model covers at least one hundredth of the image.
- textures: of a variant whose textures are KTX 2.0 or WebP files alone, the image is that of the model's glTF variant,
  whose textures are PNG and JPEG files, but for what the lossy compression of the textures changes: the channels of the
  two images differ by at most TEXTURE_TOLERANCE of 255 in the mean. The run prints the difference of each.

The run also prints each image whose file states the color of its values otherwise than a material samples them, as
sRGB values that a material reads as data or linear values that it reads as sRGB, which the model samples as the
material says, and the scoreboard counts them by what the file states. Several of Khronos's sample textures do so.

An asset that requires an extension that lib/gltf does not read is skipped, once lib/gltf has refused it as
NotSupported, and is in no part's count. The run fails on a case that fails without an entry in the errata or the
deferred list, a recorded case that passes, or an entry that names no case. `--gpu NAME` runs the suite on another of
lib/gpu's backends, and leaves the scoreboard alone.
"""

import json
import os
import struct
import subprocess
import sys
import zlib

from tools.suite import corpus
from tools.suite import gltf

# The number of assets that one run of the harness renders.
BATCH = 16

# The variants of a model that the suite renders: its core form, and the forms whose meshes a layer under the core
# stores otherwise, quantized or compressed with meshopt or with Draco, or whose textures it stores as KTX 2.0 or WebP
# files, which a model draws as it draws the core form.
VARIANTS = ("glTF", "glTF-Binary", "glTF-Quantized", "glTF-Meshopt", "glTF-Meshopt-EXT", "glTF-Draco",
            "glTF-KTX-BasisU", "glTF-KTX-BasisU-Draco", "glTF-Binary-KTX-ETC1S-Draco", "glTF-WEBP")

# The variants whose textures are compressed with loss, which the textures check compares with the glTF variant.
LOSSY = ("glTF-KTX-BasisU", "glTF-KTX-BasisU-Draco", "glTF-WEBP")

# The checks of an asset, in the order of the scoreboard. The last is of the LOSSY variants alone.
# The number of different textures that a device without bindless samples for one material
# (kPrRenderSlotMaterialTextures).
SLOT_TEXTURES = 5

CHECKS = ("loads", "validation", "slots", "techniques", "drawn", "textures")

# The most that the channels of the image of a LOSSY variant may differ from those of the glTF variant, in the mean,
# of 255.
TEXTURE_TOLERANCE = 4.0


def checks_of(asset):
    """Returns the checks of an asset: all of them for a LOSSY variant, and all but textures for any other."""
    return CHECKS if asset.split("/")[1] in LOSSY else CHECKS[:-1]


def pixels(path):
    """Returns the size and the bytes of the PNG at `path`, which the harness wrote: 8 bits a channel, not interlaced.
    Returns None if the file is not there."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    at = 8
    packed = b""
    while at < len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        chunk = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack_from(">IIBB", chunk)
        elif kind == b"IDAT":
            packed += chunk
        at += length + 12
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
    assert depth == 8
    raw = zlib.decompress(packed)
    stride = width * channels
    out = bytearray()
    before = bytearray(stride)
    for y in range(height):
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        kind = raw[y * (stride + 1)]
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = before[i]
            corner = before[i - channels] if i >= channels else 0
            if kind == 1:
                row[i] = (row[i] + left) & 255
            elif kind == 2:
                row[i] = (row[i] + up) & 255
            elif kind == 3:
                row[i] = (row[i] + (left + up) // 2) & 255
            elif kind == 4:
                guess = left + up - corner
                near = min((abs(guess - left), 0, left), (abs(guess - up), 1, up), (abs(guess - corner), 2, corner))
                row[i] = (row[i] + near[2]) & 255
        out += row
        before = row
    return (width, height, channels), bytes(out)


def difference(image, reference):
    """Returns the mean of the absolute differences of the channels of two images that the harness wrote, of 255, or
    None if either is missing or they differ in size."""
    a = pixels(image)
    b = pixels(reference)
    if a is None or b is None or a[0] != b[0]:
        return None
    return sum(abs(x - y) for x, y in zip(a[1], b[1])) / max(1, len(a[1]))

# What every check of an asset answers if the harness ends on it.
CRASH = {"status": "Crash", "diagnostics": "the harness ended"}


def answers(command, paths):
    """Returns the harness's answer for the asset at each of `paths`. An asset that ends the harness is answered with
    the status Crash."""
    results = []
    for start in range(0, len(paths), BATCH):
        batch = paths[start:start + BATCH]
        done = subprocess.run([*command, *batch], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        if done.returncode == 0 and len(lines) == len(batch):
            results += [json.loads(line) for line in lines]
            continue
        # One asset of the batch ended the harness, so each is rendered alone to find it.
        for path in batch:
            done = subprocess.run([*command, path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            lines = done.stdout.decode("utf-8", "replace").splitlines()
            if done.returncode == 0 and len(lines) == 1:
                results.append(json.loads(lines[0]))
            else:
                results.append(dict(CRASH, diagnostics="the harness exited %d" % done.returncode))
    return results


def passes(check, result):
    """Returns true if an asset's answer passes a check."""
    if result["status"] != "":
        return False
    if check == "loads":
        return result["failedImages"] == 0 and result["failedMeshes"] == 0
    if check == "validation":
        return result["validation"] == 0
    if check == "slots":
        return result["slots"] == 0 or result.get("unsampled", 0) > 0
    if check == "textures":
        return result.get("textures") is not None and result["textures"] <= TEXTURE_TOLERANCE
    if check == "techniques":
        return 0 <= result["techniques"] <= (result["pixels"] - result["edges"]) / 200
    return result["covered"] >= 0.01


def key_of(asset, check):
    """Returns the key of an asset's check."""
    return "%s:%d" % (asset, CHECKS.index(check))


def main():
    ap = corpus.arguments(corpus.binary("model_corpus"),
                          os.path.join(corpus.ROOT, "third_party", "glTF-Sample-Assets", "Models"))
    ap.add_argument("--gpu", metavar="NAME", help="the backend of lib/gpu to render with, its default unless named; a "
                                                  "run with one leaves the scoreboard alone")
    ap.add_argument("--out", default=os.path.join(corpus.BUILD, "suites", "model"),
                    help="the directory that the images are written to")
    args = ap.parse_args()
    if not os.path.isdir(args.tests):
        sys.exit("glTF corpus missing; run ./dev.sh fetch gltf_sample_assets")
    suite = corpus.Data("model")
    assets = [key for key in gltf.cases(args.tests) if key.split("/")[1] in VARIANTS]
    keys = [key_of(asset, check) for asset in assets for check in checks_of(asset)]
    chosen = [asset for asset in assets
              if any(suite.runs(key_of(asset, check), args.match) for check in checks_of(asset))]
    if not chosen:
        sys.exit("no model cases selected")
    if args.list:
        return corpus.listing([(check, [asset for asset in chosen if check in checks_of(asset)]) for check in CHECKS])
    corpus.require(args.binary, "model")

    paths = [os.path.join(args.tests, *asset.split("/")) for asset in chosen]
    required = [gltf.required_extensions(path) for path in paths]
    reads = gltf.supported(corpus.binary("gltf_corpus"), {name for names in required for name in names})
    os.makedirs(args.out, exist_ok=True)
    command = [args.binary, "--out", args.out]
    if args.gpu:
        command += ["--gpu", args.gpu]
    # Metal validates only if its layer is on when the device is made.
    os.environ.setdefault("MTL_DEBUG_LAYER", "1")
    # The glTF variant of a model is rendered beside each of its LOSSY variants, which are compared with it.
    extra = []
    for asset in chosen:
        model, variant, name = asset.split("/")
        base = "%s/glTF/%s" % (model, name)
        if variant in LOSSY and base in assets and base not in chosen and base not in extra:
            extra.append(base)
    results = answers(command, paths + [os.path.join(args.tests, *asset.split("/")) for asset in extra])
    images = {asset: result.get("image", "") for asset, result in zip(chosen + extra, results)}
    mismatched = {}
    for asset, result in zip(chosen, results):
        indices = [str(m["image"]) for m in result.get("mismatches", ()) if m["file"] == "srgb"]
        if indices:
            files = "the file of image %s states" if len(indices) == 1 else "the files of images %s state"
            print("%s: %s sRGB values, which a material samples as data" % (asset, files % ", ".join(indices)))
            row = mismatched.setdefault("srgb", [0, 0])
            row[0] += len(indices)
            row[1] += 1
        model, variant, name = asset.split("/")
        if variant in LOSSY and result.get("image"):
            result["textures"] = difference(result["image"], images.get("%s/glTF/%s" % (model, name), ""))
            if result["textures"] is not None:
                print("%s differs from its glTF variant by %.3f of 255 in the mean" % (asset, result["textures"]))

    parts = {check: [0, 0] for check in CHECKS}
    skipped = {}
    skips = 0
    unsupported = {}
    outcomes = {}
    failures = 0
    deferred = 0
    for asset, needs, result in zip(chosen, required, results):
        unread = [name for name in needs if name not in reads]
        if unread and result["status"] == "NotSupported":
            skips += 1
            for name in unread:
                skipped[name] = skipped.get(name, 0) + 1
            continue
        for name in result.get("unsupported", ()):
            unsupported[name] = unsupported.get(name, 0) + 1
        if result.get("unsampled", 0) > 0:
            name = "textures past the %d that a device with slots samples for a material" % SLOT_TEXTURES
            unsupported[name] = unsupported.get(name, 0) + 1
        for check in checks_of(asset):
            key = key_of(asset, check)
            if not suite.runs(key, args.match):
                deferred += suite.left_out(key) is not None
                continue
            passed = passes(check, result)
            reason = suite.recorded(key)
            outcomes[key] = 1 if passed else 0
            parts[check][1] += 1
            if passed:
                parts[check][0] += 1
                if reason is not None:
                    failures += 1
                    print("%s passes now; drop its erratum" % key)
            elif reason is not None:
                parts[check][0] += 1
                if args.verbose:
                    print("%s\n    %s" % (key, reason))
            else:
                failures += 1
                print("%s (%s)  %s" % (key, check, json.dumps({name: value for name, value in result.items()
                                                               if name != "image"}, ensure_ascii=False)))
        if args.match:
            print("%s  %s" % (asset, json.dumps(result, ensure_ascii=False)))

    rows = [(check, parts[check][0], parts[check][1]) for check in CHECKS]
    for check, passed, count in rows:
        print("%s  %d/%d" % (check.ljust(max(len(name) for name in CHECKS)), passed, count))
    notes = []
    if deferred:
        notes += ["Left out, and in no part's count: the %d cases that the suite's lists cut, defer or skip." % deferred,
                  ""]
    if skipped:
        notes += ["Skipped, and in no part's count: the %d assets that require an extension that lib/gltf does not" % skips,
                  "read. An asset that requires several of them counts under each.", ""]
        notes += corpus.table(("Extension", "Assets"), [(name, str(skipped[name])) for name in sorted(skipped)])
    if mismatched:
        notes += ([""] if notes and notes[-1] != "" else []) + [
            "Sampled otherwise than their files state, as the materials ask: the images whose files state sRGB values",
            "that a material reads as data.", ""]
        names = {"srgb": "sRGB, read as data"}
        notes += corpus.table(("File states", "Images", "Assets"),
                              [(names[stated], str(mismatched[stated][0]), str(mismatched[stated][1]))
                               for stated in sorted(mismatched)])
    if unsupported:
        # A note before this one ends in a blank line, or ends in a table that one separates.
        notes += ([""] if notes and notes[-1] != "" else []) + [
            "Drawn without what lib/model does not draw yet, and counted above as they are drawn:", ""]
        notes += corpus.table(("Not drawn", "Assets"), [(name, str(unsupported[name])) for name in sorted(unsupported)])
    print("\n%d/%d pass or are recorded; %d fail; %d cases left out; %d assets skipped for a required extension; "
          "images in %s" % (sum(row[1] for row in rows), sum(row[2] for row in rows), failures, deferred, skips,
                            os.path.relpath(args.out, corpus.ROOT)))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args) and not args.gpu:
        suite.write_scoreboard(rows, notes)
    stale = [] if args.match else suite.stale(set(keys))
    for kind, key in stale:
        print("%s names no case (%s)" % (key, kind))
    return int(bool(failures or stale))


if __name__ == "__main__":
    sys.exit(main())
