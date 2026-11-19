#!/usr/bin/env python3
"""Classifies every file of the corpora that the other suites fetch with lib/mime's classifier.

The corpora are the `gltf_sample_assets` checkout's Models, the `usd_assets` checkout, and the `libwebp_test_data`
checkout: glTF and binary glTF assets with their buffers and images in PNG, JPEG, WebP and KTX 2.0, USD layers in text
and crate and USD packages, and WebP images. The mime_corpus harness classifies each file by its first bytes and its
name (lib/mime/tests/mime_corpus.cpp).

A case is one file whose extension names a format, and its key is its path under its corpus's part, as
glTF-Sample-Assets/Box/glTF-Binary/Box.glb. It passes if the classifier finds one of the formats that the extension
allows: a .png may be an animated PNG, and a .usd is text or crate. A file whose extension names no format, such as a
glTF asset's .bin buffer, is counted by what it classifies as, and is no case. The run fails on a case that fails
without an entry in the errata, a recorded case that passes, or an entry that names no case.
"""

import json
import os
import subprocess
import sys
import textwrap

from tools.suite import corpus

# The number of files that one run of the harness classifies.
BATCH = 256

# The parts of the suite: the name of each, and its corpus under third_party.
PARTS = (("glTF-Sample-Assets", os.path.join("glTF-Sample-Assets", "Models")),
         ("usd-assets", "usd-assets"),
         ("libwebp-test-data", "libwebp-test-data"))

# The formats that a file's extension allows, by the extension in lowercase.
EXPECTED = {
    "png": ("png", "apng"), "apng": ("apng",), "jpg": ("jpeg",), "jpeg": ("jpeg",), "gif": ("gif",),
    "webp": ("webp",), "bmp": ("bmp",), "ico": ("ico",), "tif": ("tiff",), "tiff": ("tiff",), "ktx": ("ktx",),
    "ktx2": ("ktx2",), "dds": ("dds",), "hdr": ("hdr",), "exr": ("exr",), "avif": ("avif",), "svg": ("svg",),
    "ttf": ("ttf",), "otf": ("otf",), "woff": ("woff",), "woff2": ("woff2",), "zip": ("zip", "usdz"),
    "gz": ("gzip",), "gltf": ("gltf",), "glb": ("glb",), "usda": ("usda",), "usdc": ("usdc",),
    "usd": ("usda", "usdc"), "usdz": ("usdz",), "drc": ("draco",), "ply": ("ply",), "pdf": ("pdf",),
    "html": ("html",), "htm": ("html",), "xml": ("xml", "svg"), "json": ("json", "gltf"), "md": ("markdown",),
    "txt": ("text", "json"), "wav": ("wav",), "mp3": ("mp3",), "ogg": ("ogg",), "mp4": ("mp4",),
    "mov": ("quicktime",), "webm": ("webm",), "pam": ("pnm",), "pgm": ("pnm",), "ppm": ("pnm",), "pbm": ("pnm",),
    "mtlx": ("xml",), "py": ("python",), "sh": ("shell",), "7z": ("7z",), "sbsar": ("7z",),
    # A glTF asset's buffer is no format of its own, unless it is a Draco stream.
    "bin": ("data", "draco"),
}


def cases():
    """Returns each file's key and path, by part, for the parts that are checked out."""
    found = []
    for part, directory in PARTS:
        root = os.path.join(corpus.ROOT, "third_party", directory)
        if not os.path.isdir(root):
            continue
        files = []
        for walk, folders, names in os.walk(root):
            folders[:] = sorted(folder for folder in folders if not folder.startswith("."))
            for name in names:
                if name.startswith("."):
                    continue
                path = os.path.join(walk, name)
                files.append((part + "/" + os.path.relpath(path, root).replace(os.sep, "/"), path))
        found.append((part, sorted(files)))
    return found


def answers(binary, paths):
    """Returns the harness's answer for the file at each of `paths`."""
    results = []
    for start in range(0, len(paths), BATCH):
        batch = paths[start:start + BATCH]
        done = subprocess.run([binary, *batch], stdout=subprocess.PIPE)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        if done.returncode != 0 or len(lines) != len(batch):
            sys.exit("%s exited %d on a batch from %s" % (binary, done.returncode, batch[0]))
        results += [json.loads(line) for line in lines]
    return results


def expected(key):
    """Returns the formats that the extension of the file at `key` allows, or None if it names no format."""
    name = key.rsplit("/", 1)[-1]
    if "." not in name:
        return None
    return EXPECTED.get(name.rsplit(".", 1)[1].lower())


def main():
    args = corpus.arguments(corpus.binary("mime_corpus"), None).parse_args()
    suite = corpus.Data("classify")
    parts = cases()
    if not parts:
        sys.exit("corpora missing; run ./dev.sh fetch gltf_sample_assets usd_assets libwebp_test_data")
    keys = [key for _, files in parts for key, _ in files if expected(key) is not None]
    chosen = [(part, [(key, path) for key, path in files if suite.runs(key, args.match)]) for part, files in parts]
    if args.list:
        return corpus.listing([(part, [f for f in files if expected(f[0]) is not None]) for part, files in chosen])
    corpus.require(args.binary, "classify")

    rows = []
    outcomes = {}
    failures = 0
    census = {}
    others = {}
    for part, files in chosen:
        results = answers(args.binary, [path for _, path in files])
        passed = 0
        count = 0
        for (key, _), result in zip(files, results):
            format = result["format"]
            census[format] = census.get(format, 0) + 1
            allowed = expected(key)
            if allowed is None:
                others[format] = others.get(format, 0) + 1
                continue
            count += 1
            ok = format in allowed
            reason = suite.recorded(key)
            outcomes[key] = 1 if ok else 0
            if ok:
                passed += 1
                if reason is not None:
                    failures += 1
                    print("%s passes now; drop its erratum" % key)
            elif reason is not None:
                passed += 1
                if args.verbose:
                    print("%s\n    %s" % (key, reason))
            else:
                failures += 1
                print("%s  %s, expected %s" % (key, format, " or ".join(allowed)))
            if args.match:
                print("%s  %s" % (key, json.dumps(result)))
        if count:
            rows.append((part, passed, count))
            print("%s  %d/%d" % (part, passed, count))

    def counts(table):
        return ", ".join("%s %d" % (name, table[name]) for name in sorted(table, key=lambda n: (-table[n], n)))

    total = sum(census.values())
    notes = textwrap.wrap("Of the %d files, by format: %s. Of the files whose extension names no format: %s." %
                          (total, counts(census), counts(others)), 120)
    print("\n".join(notes))
    print("\n%d/%d pass or are recorded; %d fail" % (sum(r[1] for r in rows), sum(r[2] for r in rows), failures))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args):
        suite.write_scoreboard(rows, notes)
    stale = [] if args.match else suite.stale(set(keys))
    for kind, key in stale:
        print("%s names no case (%s)" % (key, kind))
    return int(bool(failures or stale))


if __name__ == "__main__":
    sys.exit(main())
