#!/usr/bin/env python3
"""Reads USD layers and composes USD stages with lib/usd, and compares both with Pixar's OpenUSD.

The corpus is two checkouts: `usd_assets`, the USD working group's sample and test scenes, and
`usd_spec_supplemental`, the layers that the Alliance for OpenUSD publishes beside its core specification. A case is
one layer: a .usd, .usda or .usdc file, or one of them in a .usdz package. Its key is its path under third_party, and
a layer in a package is `path/to/a.usdz[layer.usdc]`. A package is a case too, whose key is its path.

The reference is the `usd_core` checkout: Pixar's wheel in a Python environment of its own, which this runner starts
(tools/suite/usd_reference.py). It is never built or linked. The comparison is of layers, as Sdf reads them: specs,
their order under their parents, fields, list ops, defaults and time samples. Nothing is composed.

A layer passes if all of these hold:

- lib/usd and the reference both read it, or both refuse it.
- The two readings are equal: the dump that the usd_corpus harness writes of lib/usd's layer is, line for line, the
  dump that the reference script writes (lib/usd/tests/usd_corpus.cpp describes the dump). Integers, strings, tokens
  and paths are compared exactly. A float is compared by its bits. In a crate layer those are the file's bits. In a
  text layer they are the bits that each reader makes of the decimal text: lib/usd reads a number as a double,
  correctly rounded (prF64ParsePrefix), and narrows it to the attribute's float or half, and so does the reference, so
  the two agree to the bit or the case fails. The types of the scalars in a dictionary are the one thing that this
  comparison cannot see, because Python has them without their types.
- lib/usd reads the layer the same in the other format. The reference writes each layer again, a text layer as crate
  and a crate layer as text, lib/usd reads that file, and its two readings are compared with their types, which
  checks the dictionaries' types too.

Three differences are not failures, and the run counts them in its notes:

- A field that the reference knows no type for. The reference drops it as it reads a text layer, and lib/usd keeps
  its text (PrUsdType_Unregistered), so such a field is in lib/usd's reading alone.
- A layer that holds what lib/usd does not read yet, a spline, which lib/usd refuses as NotSupported. The case is
  skipped and is in no part's count.
- A layer whose second writing the reference cannot make. The layer is compared with the reference alone.

A package passes if lib/usd reads it and lists the files that Python's zipfile lists, in the same order. The notes
count the packages that keep the specification's rules of layout.

Composition has two more kinds of case.

A composition test is a folder of the supplement's composition tests, whose key is its `pcp.txt`: the results that
OpenUSD's own composition tests expect of each prim of the folder's entry layer. lib/usd composes the entry layer as
those results were made, with the `standin` variant set falling back to `render`, and the case passes if every prim
of the expected results has, in the dump that `usd_corpus --prims` writes, the same opinions in the same order, the
same variant selections, the same children in the same order, the same names that its children may not have, the same
properties and the same opinions of each property, the same targets and connections of each property, and, where the
results state them, the same retiming of each node of its graph and of each layer of the node's stack, to the two
decimals that the results have. An instance's children are compared at their paths under the instance, though lib/usd
composes them once in a prototype.

The errors that the results expect are compared by kind and by prim: each of their sentences is one of lib/usd's kinds
of error (EXPECTED_ERRORS), and a prim passes if lib/usd reports the same kinds for it. The errors of a layer stack,
as a relocate that breaks a rule, are compared as kinds alone. A target or a connection that maps to nothing is no
error of a stage, which resolves targets when it is asked for them, so the prim has that kind if `prUsdStageTargets`
counts one.

A stage is a layer file or a package of the corpus as a root layer, whose key is the layer's key and `#stage`. lib/usd
composes it, with every payload loaded, and so does the reference, and the case passes if the two dumps agree
(lib/usd/tests/usd_corpus.cpp describes the dump): the prims in order with their specifiers, types and flags, each
prim's opinions in strength order, its resolved metadata, its variant selections, its properties in order, and each
attribute's default, its sample times, its values at and outside its first and last samples, bit for bit, and its
values between samples, to a relative 1e-5, since the two interpolate in different arithmetic. Relationship targets,
forwarded targets and connections are compared as paths, and the kinds of composition error as a set. These are not
failures, and the notes count them:

- An attribute that a schema of OpenUSD's defines: the reference takes its type and variability from the schema, and
  lib/usd knows no schema, so only its name and its values are compared.
- An attribute whose values value clips or a spline decide, which lib/usd does not read: its values are not compared.
- A metadata field that the reference knows no type for, as for layers.
- A stage whose root layer lib/usd refuses as NotSupported is skipped, as its layer is.
- A stage that reaches a MaterialX document is skipped and is in no part's count. The reference is built without
  OpenUSD's usdMtlx, so it reads no such layer, and the two could never agree. lib/usd's answer names the formats of
  the layers that composing the stage read, and a layer in MaterialX's format (PrUsdFormat_MaterialX) marks the stage.

A failing composition test or stage is counted under its first cause, in the order that the notes list them.

The schemas of geometry have a case for each stage too, whose key is the layer's key and `#geometry`. lib/usd's schema
reading (`usd_corpus --geometry`, pr/usd_geom.h) and OpenUSD's UsdGeom and UsdShade read every prim of the stage that
is not in a prototype, and the case passes if the two agree: the local transform of each transformable prim at the
stage's start time code, to a relative 1e-5, whether it resets the transform stack, each mesh's counts of points,
faces, corners and holes, its orientation, its normals' number and interpolation, or that its topology does not hold
together, and the material that binds each mesh for the preview purpose. A stage that the stages' case skips is
skipped here too.

Each stage has a case of its reification too, whose key is the layer's key and `#reified`, which needs no reference, so
a stage that reaches a MaterialX document has one.
lib/usd's model source (pr/usd_model.h) reifies the stage in its own context three times: twice from one stage, and
once from the same layers composed again. The case passes if the three are equal (`usd_corpus --reified`): the canonical
text of each description (prModelSourceInfoWrite), its nodes, meshes, materials, tracks, lights and cameras, and the
data that each mesh reads, bit for bit. A stage that does not compose is the stages' case, and has no reification. The
notes count the stages whose reification names a value that varies over time and that no track holds.

A case that ends the reference's process is answered as a crash, and the cases after it are asked of a reference
started again, so no comparison is lost. Such a case is recorded in the errata with a reason that begins "The
reference crashes on this file.", and from then on the runner does not give it to the reference at all.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that
names no case.
"""

import ast
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

from tools.fetch import checkouts
from tools.suite import corpus

# The number of layers that one run of the harness reads, and the seconds that it has for each.
BATCH = 32
TIMEOUT = 120

# How the reason of an erratum begins if the reference ends with a signal on the case. The runner never gives such a
# case to the reference.
CRASHES = "The reference crashes on this file."

# The folder of the supplement's composition tests, under third_party.
COMPOSITION = "usd-spec-supplemental/releases/1.0.1/composition/tests/assets"

# The causes that a failing composition test or stage is counted under, in order: the first that applies.
CAUSES = ("refusal", "hierarchy", "prim", "opinions", "variants", "children", "prohibited", "properties", "attributes",
          "metadata", "values", "times", "interpolation", "offsets", "targets", "errors")

# The kind of composition error that lib/usd reports for each sentence of the expected results' errors, by words that
# only that sentence has. A cycle is several lines, of which one is counted.
EXPECTED_ERRORS = (
    ("invalid opinion at the relocation source", "OpinionAtRelocateSource"),
    ("Cycle detected:", "ArcCycle"),
    ("which is a prohibited child of its parent", "ArcInvalidPath"),
    ("Relocation from ", "InvalidRelocate"),
    ("is the target of multiple relocations", "InvalidRelocate"),
    ("refers to a path outside the scope of", "TargetInvalid"),
    ("is authored in a class but refers to an instance", "TargetInvalid"),
    ("Unresolved payload prim path", "ArcUnresolved"),
    ("Unresolved reference prim path", "ArcUnresolved"),
    ("has inconsistent spec types", "InconsistentProperty"),
    ("Sublayer hierarchy with root layer", "SublayerCycle"),
    ("Could not open asset", "ArcUnreadable"),
)

# The kinds of error that lib/usd reports of a layer stack and not of a prim, which are compared without their prims.
STACK_ERRORS = ("InvalidRelocate", "SublayerCycle", "SublayerUnreadable", "InvalidOffset")

# The checkouts that hold the corpus, and what the scoreboard calls each.
ROOTS = (("usd-assets", "assets"), ("usd-spec-supplemental", "specification"))

LAYERS = (".usd", ".usda", ".usdc")

# The diagnostics of a reified stage that name a value that varies over time and that no track holds (pr/usd_model.h).
UNREIFIED = ("mesh-animation", "instancer-animation", "environment-animation", "material-animation", "light-animation",
             "camera-animation", "time-range")


def cases(third_party):
    """Returns the key of each case, sorted: every layer file and every package under the corpus's checkouts, and
    every layer in each package."""
    keys = []
    for root, _ in ROOTS:
        for directory, _, names in os.walk(os.path.join(third_party, root)):
            for name in names:
                path = os.path.join(directory, name)
                key = os.path.relpath(path, third_party).replace(os.sep, "/")
                if name.lower().endswith(LAYERS):
                    keys.append(key)
                elif name.lower().endswith(".usdz"):
                    keys.append(key)
                    try:
                        with zipfile.ZipFile(path) as archive:
                            inner = [entry for entry in archive.namelist() if entry.lower().endswith(LAYERS)]
                    except (OSError, zipfile.BadZipFile):
                        inner = []
                    keys += ["%s[%s]" % (key, entry) for entry in inner]
    stages = [key + "#stage" for key in keys if "[" not in key]
    geometries = [key + "#geometry" for key in keys if "[" not in key]
    reifications = [key + "#reified" for key in keys if "[" not in key]
    tests = []
    folder = os.path.join(third_party, *COMPOSITION.split("/"))
    for name in sorted(os.listdir(folder) if os.path.isdir(folder) else []):
        if os.path.exists(os.path.join(folder, name, "pcp.txt")):
            tests.append("%s/%s/pcp.txt" % (COMPOSITION, name))
    return sorted(keys) + tests + sorted(stages) + sorted(geometries) + sorted(reifications)


def is_package(key):
    return key.lower().endswith(".usdz")


def is_layer(key):
    return not key.lower().endswith((".usdz", "/pcp.txt", "#stage", "#geometry", "#reified"))


def harness(binary, locations, dumps, mode=()):
    """Returns the harness's answer for the layer at each of `locations`, whose dumps it writes at `dumps`. `mode` is
    the option that says what the harness writes, and none is a layer's dump. A layer that ends the harness, or that
    it does not finish, is answered with the status Crash."""
    def run(batch, seconds):
        try:
            done = subprocess.run([binary, *mode, *[part for pair in batch for part in pair]], stdout=subprocess.PIPE,
                                  timeout=seconds)
        except subprocess.TimeoutExpired:
            return None, "the harness did not finish in %d seconds" % seconds
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        if done.returncode == 0 and len(lines) == len(batch):
            return [json.loads(line) for line in lines], ""
        return None, "the harness exited %d" % done.returncode

    results = []
    for start in range(0, len(locations), BATCH):
        batch = list(zip(locations[start:start + BATCH], dumps[start:start + BATCH]))
        answers, _ = run(batch, TIMEOUT * len(batch))
        if answers is not None:
            results += answers
            continue
        # One layer of the batch ended the harness, so each is read alone to find it.
        for pair in batch:
            answers, why = run([pair], TIMEOUT)
            results += answers or [{"status": "Crash", "diagnostics": why, "format": "", "version": "", "specs": 0}]
    return results


def crashes_reference(suite, key):
    """Returns true if the errata record that the reference crashes on the case, which is then never given to it."""
    return (suite.recorded(key) or "").startswith(CRASHES)


def reference(python, work, jobs):
    """Returns the reference's answer to each of `jobs`. A job that ends the reference is answered with the status
    Crash, and the jobs after it run in a reference started again, so that a crash loses no other job's answer. A job
    marked `crashes`, for a case that the errata record as crashing the reference, is answered Crash without being
    run: a process that dies leaves a crash report on the machine every time. A job marked `skipped`, for a case that
    the runner skips, is answered Skipped without being run."""
    crash = {"status": "Crash", "error": "the errata record that the reference crashes on this file, so it was not asked",
             "format": "", "converted": False, "unread": ""}
    skip = {"status": "Skipped", "error": "the runner skips the case, so it was not asked", "format": "",
            "converted": False, "unread": ""}
    asked = [job for job in jobs if not job.get("crashes") and not job.get("skipped")]
    answers = iter(ask_reference(python, work, asked))
    return [dict(crash) if job.get("crashes") else dict(skip) if job.get("skipped") else next(answers) for job in jobs]


# Why the runner skips a stage that reaches a MaterialX document: the reference is built without OpenUSD's usdMtlx,
# and reads no such layer.
MATERIALX = "a layer in MaterialX's format, which the reference does not read"


def unread_by_reference(answer):
    """Returns why the reference cannot be compared with lib/usd's answer for a stage, or None if it can: a layer that
    composing the stage read is in a format that the reference does not read."""
    return MATERIALX if "materialx" in answer.get("formats", []) else None


def ask_reference(python, work, jobs):
    """Returns the reference's answer to each of `jobs`, all of which it is started for."""
    script = os.path.join(corpus.ROOT, "tools", "suite", "usd_reference.py")
    results = []
    while len(results) < len(jobs):
        file = os.path.join(work, "jobs.json")
        with open(file, "w", encoding="utf-8") as f:
            json.dump(jobs[len(results):], f)
        # The script runs isolated from the environment, so that it imports the wheel and nothing else.
        done = subprocess.run([python, "-I", script, file], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        results += [json.loads(line) for line in lines if line.startswith("{")]
        if len(results) < len(jobs) and done.returncode != 0:
            # The job that it died on is the first without an answer. Record it in the errata, with the sentence of
            # CRASHES first, and it is not asked again.
            print("the reference exited %d on %s" % (done.returncode, jobs[len(results)]["location"]))
            results.append({"status": "Crash", "error": "the reference exited %d" % done.returncode, "format": "",
                            "converted": False, "unread": ""})
        elif len(results) < len(jobs):
            sys.exit("the reference answered %d of %d jobs" % (len(results), len(jobs)))
    return results


def read_dump(path):
    """Returns a dump as the lines of each spec by the spec's path: its form, its lists of children, its fields and
    its time samples, each by the words that name it."""
    specs = {}
    lines = None
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    for line in text.split("\n"):
        kind, _, rest = line.partition(" ")
        if kind == "S":
            form, _, spec = rest.partition(" ")
            lines = specs.setdefault(spec, {})
            lines["form"] = form
        elif kind in ("C", "F"):
            name, _, value = rest.partition(" ")
            lines["%s %s" % (kind, name)] = value
        elif kind == "T":
            time, _, value = rest.partition(" ")
            lines["T %s" % time] = value
    return specs


def unregistered(value):
    """Returns true if a field's value is one that the reference drops as it reads a text layer: a value, or a list
    op, of a field that it knows no type for."""
    return value.startswith(("unregistered ", "listop unregistered "))


def differences(mine, theirs, counts):
    """Returns what differs between two dumps, as sentences. With `counts`, the dumps are lib/usd's and the
    reference's of one file, and the fields that only `mine` has because the reference lists none of them are counted
    there. Without, they are lib/usd's of a layer and of the reference's second writing of it, which differ where the
    reference's writers do not keep what they read (second_writing)."""
    found = []
    for spec in sorted(set(mine) | set(theirs)):
        if spec not in mine or spec not in theirs:
            found.append("%s: only %s has the spec" % (spec, "lib/usd" if spec in mine else "the other"))
            continue
        ours, others = mine[spec], theirs[spec]
        for key in sorted(set(ours) | set(others)):
            if ours.get(key) == others.get(key):
                continue
            if counts is None and second_writing(key, ours.get(key), others.get(key)):
                continue
            if counts is not None and key not in others and unregistered(ours[key]):
                counts["unregistered"] = counts.get("unregistered", 0) + 1
                continue
            found.append("%s: %s: %s | %s" % (spec, key, ours.get(key, "absent"), others.get(key, "absent")))
    return found


# The bits of a negative zero in a double, a float and a half, as a dump writes them, and of the zero of each.
NEGATIVE_ZEROS = (("0000000000000080", "0000000000000000"), ("00000080", "00000000"), ("0080", "0000"))


def unsigned_zeros(value):
    """Returns a value of a dump with each negative zero written as zero."""
    words = []
    for word in value.replace(",", " , ").split(" "):
        for negative, zero in NEGATIVE_ZEROS:
            if word == negative:
                word = zero
        words.append(word)
    return " ".join(words)


def second_writing(key, ours, others):
    """Returns true if a difference between lib/usd's reading of a layer and of the reference's second writing of it
    is one that the reference's writers make, which no reader of the second file can undo:

    - The text writer writes a prim's properties, and a variant set's variants, in the order of their names.
    - The text writer does not write the prim type `__AnyType__`, with which OpenUSD's own tools mark a prim of no
      type in a crate file.
    - The crate writer stores a vector or a matrix whose numbers are small integers as those integers, which have no
      negative zero."""
    if key in ("C properties", "C variants"):
        return sorted((ours or "").split(" ")) == sorted((others or "").split(" "))
    if key == "F typeName":
        return {ours, others} == {'token "__AnyType__"', None}
    if ours is not None and others is not None:
        return unsigned_zeros(ours) == unsigned_zeros(others)
    return False


def new_prim():
    """Returns the facts of a prim without any: S its opinions, V its variant selections, C its children, X the names
    that its children may not have, N its properties, Q their opinions, G its targets and connections by property, O
    the retimings of its nodes, or None where the results state none, E its kinds of error, and R whether the results
    compose it, which they do not if they only state errors of it."""
    return {"S": [], "V": [], "C": [], "X": [], "N": [], "Q": [], "G": {}, "O": None, "E": set(), "R": False}


def parse_expected(path):
    """Returns the entry layer's name, the prims of a composition test's expected results, and the kinds of the errors
    that they expect of layer stacks: by path, each kind of fact under the letter that `usd_corpus --prims` writes it
    with. The errors are the kinds that lib/usd gives their sentences (EXPECTED_ERRORS)."""
    prims = {}
    current = None
    section = None
    entry = None
    name = None
    errors = None
    stack_errors = set()
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for line in text.split("\n"):
        match = re.match(r"Loading @(.*)@", line)
        if match:
            entry = entry or os.path.basename(match.group(1))
            continue
        match = re.match(r"Results for composing <(.*)>", line)
        if match:
            current = prims.setdefault(match.group(1), new_prim())
            current["R"] = True
            section = None
            errors = None
            continue
        match = re.match(r"Errors while composing <(.*)>", line)
        if match:
            current = None
            errors = prims.setdefault(match.group(1), new_prim())["E"]
            continue
        if line.startswith("Errors while"):
            current = None
            errors = stack_errors
            continue
        if errors is not None:
            for words, kind in EXPECTED_ERRORS:
                if words in line:
                    (stack_errors if kind in STACK_ERRORS else errors).add(kind)
        if current is None or not line.strip():
            continue
        if line.startswith("---"):
            section = None
        elif line.endswith(":") and not line.startswith((" ", "/")):
            section = line[:-1]
            if section == "Time Offsets":
                current["O"] = []
        elif section == "Prim Stack":
            current["S"].append(tuple(line.split()))
        elif section == "Variant Selections":
            match = re.match(r"\s*\{(.*) = (.*)\}", line)
            current["V"].append((match.group(1), match.group(2)))
        elif section == "Child names":
            current["C"] = ast.literal_eval(line.strip())
        elif section == "Prohibited child names":
            current["X"] = sorted(ast.literal_eval(line.strip()))
        elif section == "Property names":
            current["N"] = sorted(ast.literal_eval(line.strip()), key=lambda each: each.encode("utf-8"))
        elif section in ("Property stacks", "Relationship targets", "Attribute connections") and line.startswith("/"):
            name = line[:-1].split(".", 1)[1]
        elif section == "Property stacks":
            current["Q"].append((name,) + tuple(line.split()))
        elif section in ("Relationship targets", "Attribute connections"):
            current["G"].setdefault(name, []).append(line.strip())
        elif section == "Time Offsets":
            # A node is its layer, its path and its arc, and a layer of its stack is the layer and `sublayer`.
            match = re.match(r"\s*(\S+)\s+(?:(\S+)\s+)?(\S+)\s+\(offset=(.*), scale=(.*)\)", line)
            current["O"].append((match.group(1), match.group(2) or "", match.group(3), float(match.group(4)),
                                 float(match.group(5))))
    return entry, prims, stack_errors


def parse_prims(path):
    """Returns the prims of a dump that `usd_corpus --prims` wrote, and its kinds of error of layer stacks, as
    parse_expected returns them. A prim whose targets do not all map has the error that the expected results give
    such a target."""
    prims = {}
    current = None
    stack_errors = set()
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for line in text.split("\n"):
        kind, _, rest = line.partition(" ")
        if kind == "P":
            current = prims.setdefault(rest, new_prim())
        elif kind in ("C", "N"):
            current[kind] = rest.split(" ")
        elif kind in ("S", "V", "Q"):
            current[kind].append(tuple(rest.split(" ")))
        elif kind == "G":
            name, *paths = rest.split(" ")
            current["G"][name] = paths
        elif kind == "U":
            current["E"].add("TargetInvalid")
        elif kind == "E":
            error, prim, _ = (rest + "  ").split(" ", 2)
            if error in STACK_ERRORS or prim == "-":
                stack_errors.add(error)
            elif prim in prims:
                prims[prim]["E"].add(error)
        elif kind in ("O", "o", "X"):
            prim, *facts = rest.split(" ")
            if prim not in prims:
                continue
            if kind == "X":
                prims[prim]["X"] = sorted(facts)
                continue
            if prims[prim]["O"] is None:
                prims[prim]["O"] = []
            if kind == "O":
                prims[prim]["O"].append((facts[0], facts[1], facts[2], float(facts[3]), float(facts[4])))
            else:
                prims[prim]["O"].append((facts[0], "", "sublayer", float(facts[1]), float(facts[2])))
    return prims, stack_errors


def first_difference(ours, theirs):
    """Returns the first position at which two lists differ, which is the length of the shorter if it begins the
    longer."""
    return next((i for i, pair in enumerate(zip(ours, theirs)) if pair[0] != pair[1]), min(len(ours), len(theirs)))


def offsets_agree(ours, theirs):
    """Returns true if two lists of retimings are the same nodes and layers, with offsets and scales that agree to the
    two decimals that the expected results are written to."""
    return len(ours) == len(theirs) and all(
        a[:3] == b[:3] and abs(a[3] - b[3]) < 0.0051 and abs(a[4] - b[4]) < 0.0051 for a, b in zip(ours, theirs))


def composition_differences(mine, expected):
    """Returns what differs between lib/usd's prims and errors and a composition test's expected ones, each a pair of
    prims and kinds of error of layer stacks, as pairs of a cause and a sentence. A prim that the expected results
    have only errors for is not one that they compose, so only its errors are compared."""
    causes = {"S": "opinions", "V": "variants", "C": "children", "X": "prohibited", "N": "properties", "Q": "properties"}
    found = []
    mine, stack_errors = mine
    expected, expected_stack_errors = expected
    composed = {path for path, want in expected.items() if want["R"]}
    for path, want in expected.items():
        if path not in mine:
            if path in composed:
                found.append(("hierarchy", "%s: lib/usd has no such prim" % path))
            elif want["E"]:
                found.append(("errors", "%s: errors: none | %s" % (path, " ".join(sorted(want["E"])))))
            continue
        have = mine[path]
        if have["E"] != want["E"]:
            found.append(("errors", "%s: errors: %s | %s" % (path, " ".join(sorted(have["E"])) or "none",
                                                             " ".join(sorted(want["E"])) or "none")))
        if path not in composed:
            continue
        for key in "SVCXNQ":
            ours, theirs = have[key], want[key]
            if key in "VQ":
                ours = sorted(ours, key=lambda each: each[0].encode("utf-8"))
                theirs = sorted(theirs, key=lambda each: each[0].encode("utf-8"))
            if ours != theirs:
                at = first_difference(ours, theirs)
                found.append((causes[key], "%s: %s differ at %d: %s | %s" % (
                    path, key, at, " ".join(map(str, ours[at:at + 2])), " ".join(map(str, theirs[at:at + 2])))))
        for name in sorted(set(have["G"]) | set(want["G"])):
            ours, theirs = have["G"].get(name, []), want["G"].get(name, [])
            if ours != theirs:
                found.append(("targets", "%s.%s: targets: %s | %s" % (path, name, " ".join(ours) or "none",
                                                                      " ".join(theirs) or "none")))
        # The expected results state the retimings of a prim only if one of them is not the identity.
        if want["O"] is not None and not offsets_agree(have["O"] or [], want["O"]):
            at = first_difference(have["O"] or [], want["O"])
            found.append(("offsets", "%s: retimings differ at %d: %s | %s" % (
                path, at, (have["O"] or [])[at:at + 1], want["O"][at:at + 1])))
    found += [("hierarchy", "%s: only lib/usd has the prim" % path) for path in mine if path not in composed]
    if stack_errors != expected_stack_errors:
        found.append(("errors", "errors of layer stacks: %s | %s" % (" ".join(sorted(stack_errors)) or "none",
                                                                     " ".join(sorted(expected_stack_errors)) or "none")))
    return found


def parse_stage(path):
    """Returns a stage's dump as the paths of its prims in order, each prim's facts, and its kinds of error. A prim's
    facts are its line, its opinions, its metadata by field, its variants, and its properties in order, each a name,
    its line and its lines of values and targets by their letters."""
    order = []
    prims = {}
    errors = []
    current = None
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    for line in text.split("\n"):
        kind, _, rest = line.partition(" ")
        if kind == "P":
            name, _, facts = rest.partition(" ")
            current = {"P": facts, "S": [], "M": {}, "V": [], "properties": []}
            prims[name] = current
            order.append(name)
        elif kind == "E":
            errors.append(rest)
        elif current is None:
            continue
        elif kind in ("S", "V"):
            current[kind].append(rest)
        elif kind == "M":
            name, _, value = rest.partition(" ")
            current["M"][name] = value
        elif kind in ("A", "R"):
            name, _, facts = rest.partition(" ")
            current["properties"].append({"name": name, "kind": kind, "line": facts, "lines": []})
        elif kind in ("D", "T", "X", "I", "G", "F", "K") and current["properties"]:
            current["properties"][-1]["lines"].append((kind, rest))
    return order, prims, errors


def numbers_agree(mine, theirs):
    """Returns true if two values of a dump agree to a relative 1e-5 in each of their numbers, and exactly in
    everything else. Returns None if the values are digests, which say nothing of how far apart they are."""
    ours = mine.replace(",", " , ").split(" ")
    others = theirs.replace(",", " , ").split(" ")
    if len(ours) != len(others):
        return False
    for a, b in zip(ours, others):
        if a == b:
            continue
        if "sha1" in ours:
            return None
        packing = {4: "<e", 8: "<f", 16: "<d"}.get(len(a))
        if packing is None or len(a) != len(b) or not re.fullmatch("[0-9a-f]+", a + b):
            return False
        x = struct.unpack(packing, bytes.fromhex(a))[0]
        y = struct.unpack(packing, bytes.fromhex(b))[0]
        if not abs(x - y) <= 1e-5 * max(abs(x), abs(y), 1e-3):
            return False
    return True


def stage_differences(mine, theirs, counts):
    """Returns what differs between lib/usd's dump of a stage and the reference's, as pairs of a cause and a sentence.
    What the two differ in without failing is counted in `counts`."""
    found = []
    order, prims, errors = mine
    their_order, their_prims, their_errors = theirs
    if order != their_order:
        at = first_difference(order, their_order)
        found.append(("hierarchy", "the prims differ at %d: %s | %s" % (
            at, " ".join(order[at:at + 2]) or "none", " ".join(their_order[at:at + 2]) or "none")))
    for path in order:
        if path not in their_prims:
            continue
        ours, others = prims[path], their_prims[path]
        if ours["P"] != others["P"]:
            found.append(("prim", "%s: %s | %s" % (path, ours["P"], others["P"])))
        if ours["S"] != others["S"]:
            at = first_difference(ours["S"], others["S"])
            found.append(("opinions", "%s: opinions differ at %d: %s | %s" % (
                path, at, ", ".join(ours["S"][at:at + 2]) or "none", ", ".join(others["S"][at:at + 2]) or "none")))
        if ours["V"] != others["V"]:
            found.append(("variants", "%s: %s | %s" % (path, ours["V"], others["V"])))
        for field in sorted(set(ours["M"]) | set(others["M"])):
            have, want = ours["M"].get(field), others["M"].get(field)
            if have == want:
                continue
            if want is None and have.startswith(("unregistered ", "dict ", "unregistered[] ")):
                counts["unregistered"] = counts.get("unregistered", 0) + 1
                continue
            found.append(("metadata", "%s: %s: %s | %s" % (path, field, have or "absent", want or "absent")))
        names = [each["name"] for each in ours["properties"]]
        their_names = [each["name"] for each in others["properties"]]
        if names != their_names:
            found.append(("properties", "%s: properties: %s | %s" % (path, " ".join(names), " ".join(their_names))))
            continue
        for have, want in zip(ours["properties"], others["properties"]):
            where = "%s.%s" % (path, have["name"])
            line = want["line"]
            if line.endswith(" schema"):
                counts["schema"] = counts.get("schema", 0) + 1
            elif have["kind"] != want["kind"] or have["line"] != line:
                found.append(("attributes", "%s: %s %s | %s %s" % (where, have["kind"], have["line"], want["kind"], line)))
            skip = any(kind == "K" for kind, _ in want["lines"])
            if skip:
                counts["undecided"] = counts.get("undecided", 0) + 1
            theirs_by = {}
            for kind, rest in want["lines"]:
                theirs_by.setdefault(kind, []).append(rest)
            ours_by = {}
            for kind, rest in have["lines"]:
                ours_by.setdefault(kind, []).append(rest)
            for kind, cause in (("D", "values"), ("X", "values"), ("T", "times"), ("G", "targets"), ("F", "targets")):
                if skip and kind in "DXT":
                    continue
                if ours_by.get(kind, []) != theirs_by.get(kind, []):
                    pair = next(((a, b) for a, b in zip(ours_by.get(kind, []), theirs_by.get(kind, [])) if a != b),
                                (" ".join(ours_by.get(kind, [])) or "absent", " ".join(theirs_by.get(kind, [])) or "absent"))
                    found.append((cause, "%s: %s: %s | %s" % (where, kind, pair[0][:200], pair[1][:200])))
            for a, b in zip(ours_by.get("I", []), [] if skip else theirs_by.get("I", [])):
                agree = numbers_agree(a, b)
                if agree is None:
                    counts["digests"] = counts.get("digests", 0) + 1
                elif not agree:
                    found.append(("interpolation", "%s: I: %s | %s" % (where, a[:200], b[:200])))
    # The reference's stage composes no property across its specs, so it never finds this error.
    errors = [each for each in errors if each != "InconsistentProperty"]
    if errors != their_errors:
        found.append(("errors", "errors: %s | %s" % (" ".join(errors) or "none", " ".join(their_errors) or "none")))
    return found


def geometry_differences(mine, theirs):
    """Returns what differs between lib/usd's reading of a stage's schemas of geometry and OpenUSD's: each line of
    either dump that the other lacks, where a transform's numbers agree to a relative 1e-5, since the two compose
    operations in different arithmetic."""
    def parse(path):
        with open(path, encoding="utf-8") as f:
            return [line.rstrip("\n") for line in f if line.strip()]

    def same(a, b):
        if a == b:
            return True
        x, y = a.split(" "), b.split(" ")
        if x[0] != "X" or y[0] != "X" or x[:3] != y[:3] or len(x) != len(y):
            return False
        return all(abs(float(p) - float(q)) <= 1e-5 * max(1.0, abs(float(p)), abs(float(q))) for p, q in zip(x[3:], y[3:]))

    ours, other = parse(mine), parse(theirs)
    found = []
    for i in range(max(len(ours), len(other))):
        a = ours[i] if i < len(ours) else "(nothing)"
        b = other[i] if i < len(other) else "(nothing)"
        if not same(a, b):
            found.append("lib/usd: %s; the reference: %s" % (a, b))
    return found


def package_answer(binary, path):
    done = subprocess.run([binary, "--package", path], stdout=subprocess.PIPE)
    lines = done.stdout.decode("utf-8", "replace").splitlines()
    if done.returncode != 0 or len(lines) != 1:
        return {"status": "Crash", "diagnostics": "the harness exited %d" % done.returncode, "conforming": False,
                "files": []}
    return json.loads(lines[0])


def main():
    third_party = os.path.join(corpus.ROOT, "third_party")
    args = corpus.arguments(corpus.binary("usd_corpus"), third_party).parse_args()
    for root, _ in ROOTS:
        if not os.path.isdir(os.path.join(args.tests, root)):
            sys.exit("USD corpus missing; run ./dev.sh fetch usd_assets usd_spec_supplemental")
    python = checkouts.USD_CORE.interpreter()
    if not os.path.exists(python):
        sys.exit("the reference is missing; run ./dev.sh fetch usd_core")
    suite = corpus.Data("usd")
    keys = cases(args.tests)
    chosen = [key for key in keys if suite.runs(key, args.match)]
    if not chosen:
        sys.exit("no USD cases selected")
    if args.list:
        parts = {}
        for key in chosen:
            parts.setdefault(key.split("/")[0], []).append(key)
        return corpus.listing(sorted(parts.items()))
    corpus.require(args.binary, "usd")

    work = os.path.join(corpus.BUILD, "suite-usd")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    layers = [key for key in chosen if is_layer(key)]
    locations = [os.path.join(args.tests, *key.split("/")) for key in layers]
    dumps = [os.path.join(work, "%d" % i) for i in range(len(layers))]
    mine = harness(args.binary, locations, [dump + ".mine" for dump in dumps])
    # The reference writes each layer again in its other format, which lib/usd then reads.
    jobs = [{"location": location, "dump": dump + ".reference", "crashes": crashes_reference(suite, key),
             "convert": dump + (".again.usda" if answer["format"] == "crate" else ".again.usdc")}
            for key, location, dump, answer in zip(layers, locations, dumps, mine)]
    theirs = reference(python, work, jobs)
    converted = [i for i in range(len(layers)) if mine[i]["status"] == "" and theirs[i]["converted"]]
    second = dict(zip(converted, harness(args.binary, [jobs[i]["convert"] for i in converted],
                                         [dumps[i] + ".again" for i in converted])))

    names = dict(ROOTS)
    parts = {}
    outcomes = {}
    skipped = {}
    counts = {}
    failures = 0

    def record(key, part, passes, detail):
        nonlocal failures
        reason = suite.recorded(key)
        outcomes[key] = 1 if passes else 0
        row = parts.setdefault(part, [0, 0])
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
            print("%s" % key)
            for line in detail[:None if args.verbose else 6]:
                print("    %s" % (line if args.verbose else line[:300]))

    def skip_stage(key, ours):
        """Returns true, and counts the case as skipped, if a stage's case is not compared: lib/usd does not read its
        root layer, or composing it read a layer that the reference does not read."""
        why = unread_by_reference(ours)
        if ours["status"] == "NotSupported":
            why = "a root layer that lib/usd does not read"
        if why is None:
            return False
        skipped[why] = skipped.get(why, 0) + 1
        if args.verbose or args.match:
            print("%s\n    skipped: %s" % (key, why))
        return True

    for i, key in enumerate(layers):
        ours, other = mine[i], theirs[i]
        root = names[key.split("/")[0]]
        if other["status"] == "Refused":
            # A layer that the reference refuses is one that lib/usd refuses too.
            if args.verbose and ours["status"] != "":
                print("%s\n    both refuse; lib/usd: %s: %s\n    the reference: %s" % (
                    key, ours["status"], ours["diagnostics"], " ".join(other["error"][-200:].split())))
            record(key, root + ", refused", ours["status"] != "",
                   ["the reference refuses the layer, and lib/usd reads it: %s" % other["error"][-200:]])
            continue
        if other["status"] != "":
            record(key, root + ", unread", False, ["the reference: %s %s" % (other["status"], other["error"])])
            continue
        if other["unread"]:
            skipped[other["unread"]] = skipped.get(other["unread"], 0) + 1
            if args.verbose or args.match:
                print("%s\n    skipped: holds %s; lib/usd: %s %s" % (key, other["unread"], ours["status"],
                                                                     ours["diagnostics"]))
            continue
        part = "%s, %s" % (root, ours["format"] or ("crate" if other["format"] == "usdc" else "text"))
        if "[" in key:
            part = "%s, in a package" % root
        if ours["status"] != "":
            record(key, part, False, ["lib/usd: %s: %s" % (ours["status"], ours["diagnostics"])])
            continue
        detail = differences(read_dump(dumps[i] + ".mine.loose"), read_dump(dumps[i] + ".reference"), counts)
        if i in second:
            if second[i]["status"] != "":
                detail.append("lib/usd refuses the layer in its other format: %s: %s" %
                              (second[i]["status"], second[i]["diagnostics"]))
            else:
                between = differences(read_dump(dumps[i] + ".mine.strict"), read_dump(dumps[i] + ".again.strict"),
                                      None)
                detail += ["between the formats: " + line for line in between]
        else:
            counts["unconverted"] = counts.get("unconverted", 0) + 1
        record(key, part, not detail, detail)

    conforming = 0
    packages = [key for key in chosen if is_package(key)]
    for key in packages:
        path = os.path.join(args.tests, *key.split("/"))
        answer = package_answer(args.binary, path)
        try:
            with zipfile.ZipFile(path) as archive:
                listed = archive.namelist()
        except (OSError, zipfile.BadZipFile):
            listed = None
        detail = []
        if answer["status"] != "":
            detail.append("lib/usd: %s: %s" % (answer["status"], answer["diagnostics"]))
        elif answer["files"] != listed:
            detail.append("lib/usd lists %s, and zipfile lists %s" % (answer["files"], listed))
        conforming += 1 if answer["conforming"] else 0
        record(key, "%s, packages" % names[key.split("/")[0]], not detail, detail)

    causes = {}

    def record_cause(key, part, found):
        """Records a composition test or a stage, with the first of the causes that it fails for."""
        if found:
            cause = min((each[0] for each in found), key=CAUSES.index)
            if suite.recorded(key) is None:
                causes[(part, cause)] = causes.get((part, cause), 0) + 1
        record(key, part, not found, ["%s: %s" % each for each in found])

    # The composition tests of the supplement, against the results that they expect.
    tests = [key for key in chosen if key.endswith("/pcp.txt")]
    expected = [parse_expected(os.path.join(args.tests, *key.split("/"))) for key in tests]
    locations = [os.path.join(args.tests, *key.split("/")[:-1], "usda", each[0]) for key, each in zip(tests, expected)]
    dumps = [os.path.join(work, "composition-%d" % i) for i in range(len(tests))]
    for key, (_, want, stack_errors), dump, answer in zip(tests, expected, dumps,
                                                          harness(args.binary, locations, dumps, ("--prims",))):
        if answer["status"] != "":
            # A test whose entry layer no reader reads expects no prims.
            found = [("refusal", "lib/usd: %s %s" % (answer["status"], answer.get("diagnostics", "")))] if want else []
        else:
            found = composition_differences(parse_prims(dump), (want, stack_errors))
        record_cause(key, "specification, composition", found)

    # The stages, against the reference's.
    stages = [key for key in chosen if key.endswith("#stage")]
    locations = [os.path.join(args.tests, *key[:-len("#stage")].split("/")) for key in stages]
    dumps = [os.path.join(work, "stage-%d" % i) for i in range(len(stages))]
    mine = harness(args.binary, locations, [dump + ".mine" for dump in dumps], ("--stage",))
    theirs = reference(python, work, [{"location": location, "dump": dump + ".reference", "stage": True,
                                       "crashes": crashes_reference(suite, key),
                                       "skipped": unread_by_reference(ours) is not None}
                                      for key, location, dump, ours in zip(stages, locations, dumps, mine)])
    for key, dump, ours, other in zip(stages, dumps, mine, theirs):
        part = "%s, stages" % names[key.split("/")[0]]
        if other["status"] == "Refused":
            record(key, part + " refused", ours["status"] != "", ["the reference refuses the stage, and lib/usd composes it"])
            continue
        if skip_stage(key, ours):
            continue
        if other["status"] != "" or ours["status"] != "":
            record_cause(key, part, [("refusal", "lib/usd: %s; the reference: %s %s" % (
                ours["status"] or "composes it", other["status"] or "composes it", other.get("error", "")))])
            continue
        counts["undecided"] = counts.get("undecided", 0)
        record_cause(key, part, stage_differences(parse_stage(dump + ".mine"), parse_stage(dump + ".reference"), counts))

    # What the schemas of geometry read of each stage, against what OpenUSD's read (usd_corpus --geometry).
    geometries = [key for key in chosen if key.endswith("#geometry")]
    locations = [os.path.join(args.tests, *key[:-len("#geometry")].split("/")) for key in geometries]
    dumps = [os.path.join(work, "geometry-%d" % i) for i in range(len(geometries))]
    mine = harness(args.binary, locations, [dump + ".mine" for dump in dumps], ("--geometry",))
    theirs = reference(python, work, [{"location": location, "dump": dump + ".reference", "geometry": True,
                                       "crashes": crashes_reference(suite, key),
                                       "skipped": unread_by_reference(ours) is not None}
                                      for key, location, dump, ours in zip(geometries, locations, dumps, mine)])
    for key, dump, ours, other in zip(geometries, dumps, mine, theirs):
        part = "%s, geometry" % names[key.split("/")[0]]
        if skip_stage(key, ours):
            continue
        if other["status"] != "" or ours["status"] != "":
            record(key, part, other["status"] == "Refused" and ours["status"] != "",
                   ["lib/usd: %s; the reference: %s %s" % (ours["status"] or "composes it",
                                                           other["status"] or "composes it", other.get("error", ""))])
            continue
        found = geometry_differences(dump + ".mine", dump + ".reference")
        record(key, part, not found, found)

    # Each stage reified for a model three times in its own context, which converge (usd_corpus --reified).
    reifications = [key for key in chosen if key.endswith("#reified")]
    locations = [os.path.join(args.tests, *key[:-len("#reified")].split("/")) for key in reifications]
    dumps = [os.path.join(work, "reified-%d" % i) for i in range(len(reifications))]
    unreified = {}
    for key, ours in zip(reifications, harness(args.binary, locations, dumps, ("--reified",))):
        part = "%s, reified" % names[key.split("/")[0]]
        if ours["status"] == "Crash":
            record(key, part, False, ["the harness: %s" % ours["diagnostics"]])
            continue
        if ours["status"] != "":
            # A stage that does not compose is the stages' case, and has nothing to reify.
            continue
        for feature in ours["diagnostics"]:
            if feature in UNREIFIED:
                unreified[feature] = unreified.get(feature, 0) + 1
        record(key, part, ours["converges"],
               ["reifying the stage again differs at: %s" % ours["difference"]])

    rows = [(part, passed, count) for part, (passed, count) in sorted(parts.items())]
    for part, passed, count in rows:
        print("%s  %d/%d" % (part.ljust(max(len(row[0]) for row in rows)), passed, count))
    # What the run counts apart from the cases, which it prints and the scoreboard leaves out.
    print("Of the %d packages, %d keep the specification's rules of layout." % (len(packages), conforming))
    print("In lib/usd's reading alone, and not failures: %d fields that the reference knows no type for and drops." %
          counts.get("unregistered", 0))
    print("Compared with the reference alone: %d layers that the reference did not write again in their other format." %
          counts.get("unconverted", 0))
    print("Compared in part, and not failures of a stage: %d attributes that a schema of OpenUSD's defines, %d "
          "attributes whose values value clips or a spline decide, and %d values between samples that are too long for "
          "the dumps to hold." % (counts.get("schema", 0), counts.get("undecided", 0), counts.get("digests", 0)))
    notes = []
    if causes:
        notes += ["The failing composition tests and stages, by the first cause that each fails for.", ""]
        notes += corpus.table(("Part", "Cause", "Cases"), [(part, cause, str(causes[(part, cause)]))
                                                           for part, cause in sorted(causes, key=lambda each: (
                                                               each[0], CAUSES.index(each[1])))])
    if unreified:
        notes += [""] if notes else []
        notes += ["Reified at the context's start, as each diagnostic names what varies over time and no track holds: "
                  "the stages that have it.", ""]
        notes += corpus.table(("Diagnostic", "Stages"), [(name, str(unreified[name])) for name in sorted(unreified)])
    if skipped:
        notes += [""] if notes else []
        notes += ["Skipped, and in no part's count: the layers that hold what lib/usd does not read, and the stages "
                  "that reach a layer that lib/usd or the reference does not read.", ""]
        notes += corpus.table(("Holds", "Cases"), [(name, str(skipped[name])) for name in sorted(skipped)])
    print("\n".join(notes))
    print("\n%d/%d pass or are recorded; %d fail; %d skipped" % (
        sum(row[1] for row in rows), sum(row[2] for row in rows), failures, sum(skipped.values())))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args):
        suite.write_scoreboard(rows, notes)
    stale = [] if args.match else suite.stale(set(keys))
    for kind, key in stale:
        print("%s names no case (%s)" % (key, kind))
    return int(bool(failures or stale))


if __name__ == "__main__":
    sys.exit(main())
