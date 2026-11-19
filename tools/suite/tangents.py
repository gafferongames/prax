#!/usr/bin/env python3
"""Compares lib/mesh's tangents with the reference implementation of MikkTSpace over Khronos's glTF sample assets.

The corpus is the `gltf_sample_assets` checkout, and the assets are those that the model suite renders: the glTF and
glTF-Binary variants of each model (tools/suite/model.py). The tangents_compare harness gives prMeshTangents,
MikkTSpace and meshoptimizer the unindexed corners of each primitive that has texture coordinates, as lib/model gives
them to prMeshTangents, and compares each corner's tangent with MikkTSpace's
(tools/suite/tangents/tangents_compare.cpp).

A case is one asset, and its key is its path under Models, as Duck/glTF/Duck.gltf. It passes if every corner's
tangent and handedness from lib/mesh are MikkTSpace's, float for float, apart from the corners at which MikkTSpace has
no tangent of its own and lib/mesh writes its replacement (pr/mesh_tangents.h). An asset with no primitive to compare
is in no count. An asset that requires an extension that lib/gltf does not read is skipped, once lib/gltf has refused
it as NotSupported, and is in no count.

The scoreboard's notes give the corpus's corners as a whole: for lib/mesh and for meshoptimizer's generator in each of
its modes, how many corners equal MikkTSpace's, how many differ in handedness, and how far the tangents are from
MikkTSpace's. The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an
entry that names no case.
"""

import json
import os
import subprocess
import sys

from tools.suite import corpus
from tools.suite import gltf
from tools.suite import model

# The number of assets that one run of the harness compares.
BATCH = 16

# The generators that the harness compares with MikkTSpace, as it names them, and as the notes name them.
ENGINES = (("prax", "lib/mesh"), ("meshopt", "meshoptimizer"),
           ("meshoptCompatible", "meshoptimizer, meshopt_TangentCompatible"))

# The upper bounds of the harness's bins of angles, in radians, after the bin of corners that are equal.
BOUNDS = (1e-7, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1)


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
        # One asset of the batch ended the harness, so each is compared alone to find it.
        for path in batch:
            done = subprocess.run([binary, path], stdout=subprocess.PIPE)
            lines = done.stdout.decode("utf-8", "replace").splitlines()
            if done.returncode == 0 and len(lines) == 1:
                results.append(json.loads(lines[0]))
            else:
                results.append({"status": "Crash", "primitives": 0, "corners": 0, "engines": {}})
    return results


def passes(result):
    """Returns true if lib/mesh's tangents of an asset are MikkTSpace's."""
    prax = result["engines"]["prax"]
    return result["status"] == "" and prax["equal"] + prax["fallback"] == result["corners"]


def add(total, comparison):
    """Adds one asset's comparison of an engine to the run's total of it."""
    for name in ("equal", "signs", "fallback"):
        total[name] = total.get(name, 0) + comparison[name]
    total["max"] = max(total.get("max", 0), comparison["max"])
    total["bins"] = [a + b for a, b in zip(total.get("bins", [0] * len(comparison["bins"])), comparison["bins"])]


def within(total, bound):
    """Returns the corners of a total whose tangent is at most `bound` radians from MikkTSpace's."""
    return sum(total["bins"][:1 + BOUNDS.index(bound) + 1])


def notes_of(totals, corners):
    """Returns the scoreboard's notes on the corners of the cases counted."""
    rows = []
    for name, label in ENGINES:
        total = totals.get(name)
        if not total:
            continue
        rows.append((label, str(total["equal"]), str(within(total, 1e-6)), str(within(total, 1e-3)),
                     str(total["signs"]), "%.3g" % total["max"]))
    notes = ["The corners of the cases counted: %d, of which lib/mesh writes its replacement tangent at %d where" %
             (corners, totals["prax"]["fallback"] if "prax" in totals else 0),
             "MikkTSpace has none of its own. Of the others, how many each generator gives MikkTSpace's tangent, how many",
             "it gives a tangent within 1e-6 and 1e-3 radians of it, how many differ in handedness, and the largest",
             "angle from MikkTSpace's tangent, in radians:", ""]
    notes += corpus.table(("Generator", "Equal", "Within 1e-6", "Within 1e-3", "Handedness", "Largest"), rows)
    return notes


def main():
    ap = corpus.arguments(corpus.binary("tangents_compare"),
                          os.path.join(corpus.ROOT, "third_party", "glTF-Sample-Assets", "Models"))
    args = ap.parse_args()
    if not os.path.isdir(args.tests):
        sys.exit("glTF corpus missing; run ./dev.sh fetch gltf_sample_assets")
    suite = corpus.Data("tangents")
    keys = [key for key in gltf.cases(args.tests) if key.split("/")[1] in model.VARIANTS]
    chosen = [key for key in keys if suite.runs(key, args.match)]
    if not chosen:
        sys.exit("no tangents cases selected")
    if args.list:
        return corpus.listing([("assets", chosen)])
    corpus.require(args.binary, "tangents")

    paths = [os.path.join(args.tests, *key.split("/")) for key in chosen]
    required = [gltf.required_extensions(path) for path in paths]
    reads = gltf.supported(corpus.binary("gltf_corpus"), {name for names in required for name in names})
    results = answers(args.binary, paths)

    passed = 0
    counted = 0
    empty = 0
    skips = 0
    corners = 0
    totals = {}
    outcomes = {}
    failures = 0
    for key, needs, result in zip(chosen, required, results):
        unread = [name for name in needs if name not in reads]
        if unread and result["status"] == "NotSupported":
            skips += 1
            continue
        if result["status"] == "" and result["primitives"] == 0:
            empty += 1
            continue
        counted += 1
        corners += result["corners"]
        for name, comparison in result["engines"].items():
            add(totals.setdefault(name, {}), comparison)
        ok = passes(result)
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
            print("%s  %s" % (key, json.dumps(result, ensure_ascii=False)))
        if args.match:
            print("%s  %s" % (key, json.dumps(result, ensure_ascii=False)))

    rows = [("MikkTSpace", passed, counted)]
    print("MikkTSpace  %d/%d" % (passed, counted))
    notes = notes_of(totals, corners) if totals else []
    notes += ["", "In no count: the %d assets without a primitive that has texture coordinates, and the %d assets that" %
              (empty, skips), "require an extension that lib/gltf does not read."]
    print("\n".join(notes))
    print("\n%d/%d pass or are recorded; %d fail; %d without texture coordinates; %d skipped for a required extension" %
          (passed, counted, failures, empty, skips))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args):
        suite.write_scoreboard(rows, notes)
    stale = [] if args.match else suite.stale(set(keys))
    for kind, key in stale:
        print("%s names no case (%s)" % (key, kind))
    return int(bool(failures or stale))


if __name__ == "__main__":
    sys.exit(main())
