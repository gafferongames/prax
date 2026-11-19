#!/usr/bin/env python3
"""Reads Khronos's glTF sample assets with lib/gltf.

A case is one file of one variant of one model of the `gltf_sample_assets` checkout: every .gltf and .glb file in a
directory of a model, which is all the variants that the corpus offers, such as glTF, glTF-Binary and glTF-Embedded.
Its key is its path under Models, as Box/glTF-Binary/Box.glb. The gltf_corpus harness reads each asset with the
buffers and images beside it, and then reads everything that the document offers (lib/gltf/tests/gltf_corpus.cpp).

A case passes if the asset reads, every buffer and image has its bytes, and reading the document finds nothing wrong.
A case whose asset requires an extension that lib/gltf does not read is skipped rather than failed, once lib/gltf has
refused it as NotSupported: the run counts those cases by extension, and they are in no part's count. The run fails
on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names no case.
"""

import json
import os
import struct
import subprocess
import sys

from tools.suite import corpus

# The number of assets that one run of the harness reads.
BATCH = 32


def cases(models):
    """Returns the key of each case under `models`, sorted: the path of each asset in a directory of a model."""
    keys = []
    for model in sorted(os.listdir(models)):
        if not os.path.isdir(os.path.join(models, model)):
            continue
        for variant in sorted(os.listdir(os.path.join(models, model))):
            directory = os.path.join(models, model, variant)
            if not os.path.isdir(directory):
                continue
            keys += ["%s/%s/%s" % (model, variant, name) for name in sorted(os.listdir(directory))
                     if name.endswith((".gltf", ".glb"))]
    return keys


def required_extensions(path):
    """Returns the extensions that the asset at `path` requires, or none if its JSON cannot be read, which is for
    lib/gltf to report."""
    try:
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] == b"glTF":
            length, = struct.unpack_from("<I", data, 12)
            data = data[20:20 + length]
        return list(json.loads(data.decode("utf-8-sig")).get("extensionsRequired", []))
    except (OSError, ValueError, struct.error, AttributeError):
        return []


def supported(binary, names):
    """Returns the extensions among `names` that lib/gltf reads, as the harness reports them."""
    if not names:
        return set()
    done = subprocess.run([binary, "--supports", *sorted(names)], stdout=subprocess.PIPE)
    if done.returncode != 0:
        sys.exit("%s --supports exited %d" % (binary, done.returncode))
    answer = json.loads(done.stdout.decode("utf-8"))
    return {name for name in names if answer.get(name)}


def answers(binary, paths):
    """Returns the harness's answer for the asset at each of `paths`. An asset that ends the harness is answered with
    the status Crash."""
    results = []
    for start in range(0, len(paths), BATCH):
        batch = paths[start:start + BATCH]
        done = subprocess.run([binary, *batch], stdout=subprocess.PIPE)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        if done.returncode == 0 and len(lines) == len(batch):
            results += [json.loads(line) for line in lines]
            continue
        # One asset of the batch ended the harness, so each is read alone to find it.
        for path in batch:
            done = subprocess.run([binary, path], stdout=subprocess.PIPE)
            lines = done.stdout.decode("utf-8", "replace").splitlines()
            if done.returncode == 0 and len(lines) == 1:
                results.append(json.loads(lines[0]))
            else:
                results.append({"status": "Crash", "diagnostics": "the harness exited %d" % done.returncode,
                                "absent": 0, "problem": ""})
    return results


def main():
    args = corpus.arguments(corpus.binary("gltf_corpus"),
                            os.path.join(corpus.ROOT, "third_party", "glTF-Sample-Assets", "Models")).parse_args()
    if not os.path.isdir(args.tests):
        sys.exit("glTF corpus missing; run ./dev.sh fetch gltf_sample_assets")
    suite = corpus.Data("gltf")
    keys = cases(args.tests)
    chosen = [key for key in keys if suite.runs(key, args.match)]
    if not chosen:
        sys.exit("no glTF cases selected")
    if args.list:
        variants = {}
        for key in chosen:
            variants.setdefault(key.split("/")[1], []).append(key)
        return corpus.listing(sorted(variants.items()))
    corpus.require(args.binary, "gltf")

    paths = [os.path.join(args.tests, *key.split("/")) for key in chosen]
    required = [required_extensions(path) for path in paths]
    reads = supported(args.binary, {name for names in required for name in names})
    results = answers(args.binary, paths)

    parts = {}
    skipped = {}
    skips = 0
    outcomes = {}
    failures = 0
    for key, needs, result in zip(chosen, required, results):
        unread = [name for name in needs if name not in reads]
        if unread and result["status"] == "NotSupported":
            skips += 1
            for name in unread:
                skipped[name] = skipped.get(name, 0) + 1
            if args.verbose:
                print("%s\n    skipped: requires %s" % (key, ", ".join(unread)))
            continue
        passes = result["status"] == "" and result["absent"] == 0 and result["problem"] == ""
        reason = suite.recorded(key)
        outcomes[key] = 1 if passes else 0
        row = parts.setdefault(key.split("/")[1], [0, 0])
        row[1] += 1
        if passes:
            row[0] += 1
            if reason is not None:
                failures += 1
                print("%s passes now; drop its erratum" % key)
        elif reason is not None:
            row[0] += 1
            if args.verbose:
                print("%s\n    %s" % (key, reason))
        else:
            failures += 1
            print("%s  %s" % (key, json.dumps(result, ensure_ascii=False)))

    rows = [(variant, passed, count) for variant, (passed, count) in sorted(parts.items())]
    for variant, passed, count in rows:
        print("%s  %d/%d" % (variant.ljust(max(len(row[0]) for row in rows)), passed, count))
    notes = []
    if skipped:
        notes = ["Skipped, and in no part's count: the %d cases that require an extension that lib/gltf does not" % skips,
                 "read, which it refuses as NotSupported. A case that requires several of them counts under each.", ""]
        notes += corpus.table(("Extension", "Cases"), [(name, str(skipped[name])) for name in sorted(skipped)])
        print("\n%d skipped for a required extension that lib/gltf does not read:" % skips)
        for name in sorted(skipped):
            print("  %s  %d" % (name, skipped[name]))
    print("\n%d/%d pass or are recorded; %d fail" % (sum(row[1] for row in rows), sum(row[2] for row in rows),
                                                     failures))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args):
        suite.write_scoreboard(rows, notes)
    stale = [] if args.match else suite.stale(set(keys))
    for kind, key in stale:
        print("%s names no case (%s)" % (key, kind))
    return int(bool(failures or stale))


if __name__ == "__main__":
    sys.exit(main())
