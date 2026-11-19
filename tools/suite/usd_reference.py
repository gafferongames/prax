#!/usr/bin/env python3
"""Writes what Pixar's OpenUSD reads of a layer, for the usd suite to compare lib/usd's reading with.

This runs in the Python of the `usd_core` checkout, which has Pixar's `usd-core` wheel, and never in the Python that
runs the tools: tools/suite/usd.py starts it. Nothing of OpenUSD is built or linked.

    usd_reference.py <jobs.json>

The file holds a list of jobs. A job of a layer reads only the layer, through Sdf, and composes nothing: {"location":
the layer, as lib/usd's harness takes it, "dump": the file to write, "convert": a file to write the layer to in the
other format, or null}. A line of JSON answers it: {"status": "" or "Refused", "error": what OpenUSD said, "format":
the format's id, "converted": whether the layer was written again, "unread": what the layer holds that lib/usd does
not read, as "spline"}.

A job of a stage, {"location", "dump", "stage": true}, opens the stage whose root layer is at the location, with every
payload loaded, and writes the dump that `usd_corpus --stage` writes of lib/usd's stage (lib/usd/tests/usd_corpus.cpp
describes it). A line of JSON answers it: {"status", "error", "skipped": the attributes whose values value clips or
splines decide, which lib/usd does not read}. Two lines are OpenUSD's alone: an attribute that a schema defines is
marked, since OpenUSD takes its type and variability from the schema, and `K` says that an attribute's values are not
written.

The dump is the canonical text that lib/usd/tests/usd_corpus.cpp describes, in its loose form: Python has the scalars
of a dictionary without their types. Three things are written as the specification has them and not as OpenUSD stores
them, so that the two dumps agree where the two readers do:

- `subLayers` is an array of asset paths (usd-core/#sublayers-asset), which OpenUSD holds as strings.
- The fields that the specification requires of a spec are written with their fallbacks where OpenUSD has no value:
  `specifier`, `custom`, `typeName` and `variability` (usd-core/#required-fields).
- A prim's children are its lists of specs, which OpenUSD's fields of children's names stand for.
"""

import hashlib
import json
import os
import re
import struct
import sys

from pxr import Gf, Pcp, Sdf, Tf, Usd, UsdGeom, UsdShade, Vt

# The longest array that a dump writes element by element.
SHORT_ARRAY = 8

# The semantic aliases of the specification's types, by the type that underlies each.
ALIASES = {
    "color3d": "double3", "color3f": "float3", "color3h": "half3", "color4d": "double4", "color4f": "float4",
    "color4h": "half4", "normal3d": "double3", "normal3f": "float3", "normal3h": "half3", "point3d": "double3",
    "point3f": "float3", "point3h": "half3", "vector3d": "double3", "vector3f": "float3", "vector3h": "half3",
    "frame4d": "matrix4d", "texCoord2d": "double2", "texCoord2f": "float2", "texCoord2h": "half2",
    "texCoord3d": "double3", "texCoord3f": "float3", "texCoord3h": "half3",
}

# How the scalars of each type of numbers pack, and how many an element has.
PACK = {"half": "e", "float": "f", "double": "d", "timecode": "d"}
for scalar, code in (("half", "e"), ("float", "f"), ("double", "d")):
    for count in (2, 3, 4):
        PACK["%s%d" % (scalar, count)] = code * count
PACK.update({"quath": "eeee", "quatf": "ffff", "quatd": "dddd", "matrix2d": "d" * 4, "matrix3d": "d" * 9,
             "matrix4d": "d" * 16})
INTEGERS = ("uchar", "int", "uint", "int64", "uint64")
STRINGS = ("string", "token", "asset", "path", "pathExpression", "unregistered")

# The type that each class of OpenUSD's values holds.
CLASSES = {
    "Vec2i": "int2", "Vec3i": "int3", "Vec4i": "int4", "Vec2h": "half2", "Vec3h": "half3", "Vec4h": "half4",
    "Vec2f": "float2", "Vec3f": "float3", "Vec4f": "float4", "Vec2d": "double2", "Vec3d": "double3",
    "Vec4d": "double4", "Quath": "quath", "Quatf": "quatf", "Quatd": "quatd", "Matrix2d": "matrix2d",
    "Matrix3d": "matrix3d", "Matrix4d": "matrix4d", "AssetPath": "asset", "TimeCode": "timecode", "Path": "path",
    "PathExpression": "pathExpression", "Bool": "bool", "UChar": "uchar", "Int": "int", "UInt": "uint",
    "Int64": "int64", "UInt64": "uint64", "Half": "half", "Float": "float", "Double": "double", "String": "string",
    "Token": "token",
}
LIST_OPS = {"TokenListOp": "token", "StringListOp": "string", "PathListOp": "path", "ReferenceListOp": "reference",
            "PayloadListOp": "payload", "IntListOp": "int", "Int64ListOp": "int64", "UIntListOp": "uint",
            "UInt64ListOp": "uint64", "UnregisteredValueListOp": "unregistered"}


class Unread(Exception):
    """The layer holds something that lib/usd does not read, which the exception names."""


def quoted(text):
    out = ['"']
    for c in text.encode("utf-8", "surrogateescape"):
        out.append(chr(c) if 0x20 <= c < 0x7f and c not in (0x22, 0x5c) else "\\x%02x" % c)
    out.append('"')
    return "".join(out)


def hexed(code, value):
    return struct.pack("<" + code, value).hex()


def declared_type(tf_type):
    """Returns the type that a field of OpenUSD's type `tf_type` holds, and whether it is an array."""
    name = tf_type.typeName if tf_type else ""
    array = "vector<" in name or "VtArray<" in name
    for needle, kind in (("TfToken", "token"), ("string", "string"), ("SdfAssetPath", "asset"), ("double", "double"),
                         ("float", "float"), ("bool", "bool"), ("unsigned char", "uchar"), ("unsigned int", "uint"),
                         ("unsigned long", "uint64"), ("uint64", "uint64"), ("long", "int64"), ("int64", "int64"),
                         ("int", "int"), ("SdfTimeCode", "timecode")):
        if needle in name:
            return kind, array
    return None, array


def attribute_type(type_name):
    """Returns the type that an attribute's `typeName` names, and whether it is an array, or None for a name that is
    not the specification's."""
    array = type_name.endswith("[]")
    base = type_name[:-2] if array else type_name
    base = ALIASES.get(base, base)
    known = base in PACK or base in INTEGERS or base in ("bool", "string", "token", "asset", "pathExpression", "int2",
                                                         "int3", "int4")
    return (base, array) if known else (None, array)


def components(kind, value):
    """Returns the scalars of one element of a type of numbers, in the file format's order."""
    if kind.startswith("quat"):
        imaginary = value.GetImaginary()
        return [imaginary[0], imaginary[1], imaginary[2], value.GetReal()]
    if kind.startswith("matrix"):
        size = int(kind[6])
        return [value[r][c] for r in range(size) for c in range(size)]
    if kind == "timecode":
        return [value.GetValue() if hasattr(value, "GetValue") else float(value)]
    if len(PACK[kind]) == 1:
        return [value]
    return list(value)


def element(kind, value, loose):
    """Returns one element of `kind` as a dump writes it."""
    if kind == "bool":
        return "1" if value else "0"
    if kind in INTEGERS:
        return str(int(value))
    if kind in ("int2", "int3", "int4"):
        return ",".join(str(int(c)) for c in value)
    if kind in STRINGS:
        if kind == "asset":
            value = value.path if hasattr(value, "path") else value
        elif kind == "pathExpression":
            value = value.GetText() if hasattr(value, "GetText") else value
        elif kind == "unregistered":
            value = value.value if hasattr(value, "value") else value
        return quoted(str(value))
    if kind in PACK:
        parts = components(kind, value)
        if kind.startswith("quat"):
            parts = parts[3:] + parts[:3]
        return ",".join(hexed(PACK[kind][0], part) for part in parts)
    if kind == "layerOffset":
        return "%s,%s" % (hexed("d", value.offset), hexed("d", value.scale))
    if kind in ("reference", "payload"):
        text = "{%s %s %s" % (quoted(value.assetPath), quoted(str(value.primPath)),
                              element("layerOffset", value.layerOffset, loose))
        if kind == "reference":
            text += " " + dictionary(value.customData)
        return text + "}"
    raise Unread("a value of type %s" % kind)


def digest(kind, values):
    """Returns the SHA-1 of an array's elements, in the bytes that the file format gives them."""
    sha = hashlib.sha1()
    if kind in STRINGS:
        for value in values:
            if kind == "asset":
                value = value.path
            elif kind == "pathExpression":
                value = value.GetText()
            sha.update(str(value).encode("utf-8", "surrogateescape") + b"\0")
    else:
        try:
            sha.update(memoryview(values).tobytes())
        except TypeError:
            for value in values:
                if kind in PACK:
                    sha.update(struct.pack("<" + PACK[kind], *components(kind, value)))
                elif kind == "bool":
                    sha.update(b"\1" if value else b"\0")
                else:
                    raise Unread("an array of type %s" % kind)
    return "sha1 " + sha.hexdigest()


def array(kind, values, loose):
    count = len(values)
    if count > SHORT_ARRAY and kind not in ("layerOffset", "reference", "payload"):
        return "%s[] %d %s" % (kind, count, digest(kind, values))
    return " ".join(["%s[] %d" % (kind, count)] + [element(kind, value, loose) for value in values])


def dictionary(value):
    entries = sorted(value.items(), key=lambda item: item[0].encode("utf-8", "surrogateescape"))
    return "dict {%s}" % ",".join("%s:%s" % (quoted(key), encode(item, None, False, True)) for key, item in entries)


def list_op(value):
    kind = LIST_OPS[type(value).__name__]
    text = "listop %s explicit=%d" % (kind, 1 if value.isExplicit else 0)
    for name in ("explicit", "added", "prepended", "appended", "deleted", "ordered"):
        items = list(getattr(value, name + "Items"))
        text += " %s(%s)" % (name, " ".join([str(len(items))] + [element(kind, item, True) for item in items]))
    return text


def encode(value, kind, is_array, in_dictionary):
    """Returns `value` as a dump writes it. `kind` is the type that the field declares, for a Python scalar, which has
    no type of its own, and is None inside a dictionary, where the dump is loose."""
    name = type(value).__name__
    if name == "ValueBlock":
        return "none"
    if isinstance(value, dict):
        return dictionary(value)
    if name in LIST_OPS:
        return list_op(value)
    if name == "UnregisteredValue":
        inner = value.value
        if isinstance(inner, dict):
            return dictionary(inner)
        if type(inner).__name__ in LIST_OPS:
            return list_op(inner)
        return "unregistered " + quoted(str(inner))
    if name == "Specifier":
        return "specifier " + {Sdf.SpecifierDef: "def", Sdf.SpecifierOver: "over", Sdf.SpecifierClass: "class"}[value]
    if name == "Variability":
        return "variability " + ("varying" if value == Sdf.VariabilityVarying else "uniform")
    if name == "Permission":
        return "permission " + ("private" if value == Sdf.PermissionPrivate else "public")
    if name.endswith("Array") and name[:-5] in CLASSES:
        return array(CLASSES[name[:-5]], value, True)
    if name in CLASSES and name not in ("Bool", "Int", "Float", "String"):
        return "%s %s" % (CLASSES[name], element(CLASSES[name], value, True))
    if isinstance(value, (list, tuple)):
        if kind is None:
            raise Unread("a list in a dictionary")
        return array(kind, value, True)
    if isinstance(value, bool):
        return "bool " + ("1" if value else "0")
    if isinstance(value, int):
        return "%s %d" % ("int" if in_dictionary or kind is None else kind, value)
    if isinstance(value, float):
        if in_dictionary or kind is None:
            return "num " + hexed("d", value)
        return "%s %s" % (kind, hexed(PACK[kind], value))
    if isinstance(value, str):
        if in_dictionary or kind is None:
            return "str " + quoted(value)
        return "%s %s" % (kind, quoted(value))
    raise Unread("spline" if "Spline" in name else "a value of OpenUSD's type %s" % name)


def field_lines(layer, spec, form):
    """Returns the lines of a spec's fields and time samples."""
    lines = {}
    type_name = str(spec.typeName) if form == "attribute" else ""
    sample_kind, sample_array = attribute_type(type_name)
    for key in spec.ListInfoKeys():
        if key == "timeSamples":
            times = layer.ListTimeSamplesForPath(spec.path)
            lines["F timeSamples"] = "samples %d" % len(times)
            for time in times:
                sample = layer.QueryTimeSample(spec.path, time)
                lines["T " + hexed("d", time)] = encode(sample, sample_kind, sample_array, False)
            continue
        if key == "subLayerOffsets":
            lines["F " + key] = array("layerOffset", list(layer.subLayerOffsets), True)
            continue
        value = spec.GetInfo(key)
        if key == "subLayers":
            text = array("asset", list(value), True)
        elif key == "layerRelocates":
            text = " ".join(["relocate[] %d" % len(value)] +
                            ["%s>%s" % (quoted(str(source)), quoted(str(target))) for source, target in value])
        elif key == "variantSelection":
            pairs = sorted(value.items(), key=lambda item: item[0].encode("utf-8"))
            text = " ".join(["variantSelection[] %d" % len(pairs)] +
                            ["%s:%s" % (quoted(name), quoted(choice)) for name, choice in pairs])
        elif key == "default" and form == "attribute":
            text = encode(value, sample_kind, sample_array, False)
        elif key == "default":
            text = "path " + quoted(str(value))
        elif key == "typeName":
            text = "token " + quoted(str(value))
        else:
            kind, is_array = declared_type(spec.GetTypeForInfo(key))
            text = encode(value, kind, is_array, False)
        lines["F " + key] = text
    # The fields that the specification requires, which a format may leave to their fallbacks.
    if form in ("prim", "variant"):
        lines.setdefault("F specifier", "specifier over")
    if form in ("attribute", "relationship"):
        lines.setdefault("F custom", "bool 0")
    if form == "attribute":
        lines.setdefault("F typeName", 'token ""')
        lines.setdefault("F variability", "variability varying")
    return lines


def write_spec(out, layer, spec, form):
    out.append("S %s %s" % (form, spec.path))
    children = {}
    variant_sets = []
    if form in ("layer", "prim", "variant"):
        children["prims"] = [child.name for child in spec.nameChildren]
    if form in ("prim", "variant"):
        children["properties"] = [child.name for child in spec.properties]
        # OpenUSD's view of a prim's variant sets also lists the names that the prim only declares, which have no
        # spec.
        variant_sets = [name for name in spec.variantSets.keys()
                        if layer.GetObjectAtPath(spec.variantSets[name].path) is not None]
        children["variantSets"] = variant_sets
    for name in ("prims", "properties", "variantSets"):
        if children.get(name):
            out.append("C %s %s" % (name, " ".join(quoted(child) for child in children[name])))
    for key, text in sorted(field_lines(layer, spec, form).items()):
        out.append("%s %s" % (key, text))
    for child in getattr(spec, "properties", []) if form != "layer" else []:
        write_spec(out, layer, child, "attribute" if isinstance(child, Sdf.AttributeSpec) else "relationship")
    if form in ("prim", "variant"):
        for name in variant_sets:
            variant_set = spec.variantSets[name]
            out.append("S variantSet %s" % variant_set.path)
            variants = list(variant_set.variantList)
            if variants:
                out.append("C variants %s" % " ".join(quoted(variant.name) for variant in variants))
            for key, text in sorted(field_lines(layer, variant_set, "variantSet").items()):
                out.append("%s %s" % (key, text))
            for variant in variants:
                write_spec(out, layer, variant.primSpec, "variant")
    if form in ("layer", "prim", "variant"):
        for child in spec.nameChildren:
            write_spec(out, layer, child, "prim")


def run(job):
    answer = {"status": "", "error": "", "format": "", "converted": False, "unread": ""}
    try:
        layer = Sdf.Layer.FindOrOpen(job["location"])
    except Tf.ErrorException as error:
        layer = None
        answer["error"] = str(error)[-400:]
    if layer is None:
        answer["status"] = "Refused"
        return answer
    # A package's layer is the format of the file that the package holds.
    answer["format"] = layer.GetFileFormat().formatId
    out = []
    try:
        write_spec(out, layer, layer.pseudoRoot, "layer")
    except Unread as unread:
        answer["unread"] = str(unread)
        return answer
    with open(job["dump"], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    if job.get("convert"):
        try:
            answer["converted"] = bool(layer.Export(job["convert"]))
        except Tf.ErrorException as error:
            answer["error"] = str(error)[-400:]
    return answer


# The fields of a prim that a stage's dump leaves out: those that composition consumes, and those that it writes apart.
COMPOSED = ("specifier", "typeName", "references", "payload", "inheritPaths", "specializes", "variantSelection",
            "variantSetNames", "primOrder", "propertyOrder", "clips", "clipSets")

# The kind of composition error that the dump gives each of OpenUSD's.
ERRORS = {
    "ErrorArcCycle": "ArcCycle", "ErrorInvalidAssetPath": "ArcUnreadable", "ErrorMutedAssetPath": "ArcUnreadable",
    "ErrorUnresolvedPrimPath": "ArcUnresolved", "ErrorInvalidPrimPath": "ArcInvalidPath",
    "ErrorArcToProhibitedChild": "ArcInvalidPath",
    "ErrorInvalidSublayerPath": "SublayerUnreadable", "ErrorSublayerCycle": "SublayerCycle",
    "ErrorInvalidSublayerOffset": "InvalidOffset", "ErrorInvalidReferenceOffset": "InvalidOffset",
    "ErrorOpinionAtRelocationSource": "OpinionAtRelocateSource",
    "ErrorInvalidAuthoredRelocation": "InvalidRelocate", "ErrorInvalidConflictingRelocation": "InvalidRelocate",
    "ErrorInvalidSameTargetRelocations": "InvalidRelocate",
}

PROTOTYPE = re.compile(r"^/__Prototype_\d+")


class StageDump:
    """Writes a stage as `usd_corpus --stage` does."""

    def __init__(self, stage):
        self.stage = stage
        self.out = []
        self.names = {}
        self.queue = []
        self.skipped = 0

    def target(self, path):
        return PROTOTYPE.sub("/__P", str(path))

    def value(self, attribute, kind, is_array, time):
        """Returns what an attribute resolves to at a time, or None if lib/usd does not read what decides it."""
        info = attribute.GetResolveInfo(time) if time is not None else attribute.GetResolveInfo()
        source = info.GetSource()
        if source in (Usd.ResolveInfoSourceValueClips, getattr(Usd, "ResolveInfoSourceSpline", None)):
            return None
        if source in (Usd.ResolveInfoSourceNone, Usd.ResolveInfoSourceFallback) or info.ValueIsBlocked():
            return "none"
        value = attribute.Get(time) if time is not None else attribute.Get()
        if value is None:
            return "none"
        return encode(value, kind, is_array, False)

    def attribute(self, prim, attribute, name):
        out = self.out
        type_name = str(attribute.GetTypeName())
        defined = prim.GetPrimDefinition().GetPropertyDefinition(name)
        out.append("A %s %s %s %s%s" % (
            name, "custom" if attribute.IsCustom() else "-", type_name,
            "uniform" if attribute.GetVariability() == Sdf.VariabilityUniform else "varying",
            " schema" if defined else ""))
        # A value is written as the type that the strongest spec declares, which is the type that it was authored as.
        stack = attribute.GetPropertyStack()
        kind, is_array = attribute_type(str(stack[0].typeName) if stack else type_name)
        times = attribute.GetTimeSamples()
        wanted = [("D", None)]
        if times:
            wanted += [("X", times[0]), ("X", times[-1]), ("X", times[0] - 1.0), ("X", times[-1] + 1.0)]
        if len(times) > 1:
            wanted += [("I", (times[0] + times[1]) / 2), ("I", (times[-2] + times[-1]) / 2)]
        lines = []
        try:
            for letter, time in wanted:
                text = self.value(attribute, kind, is_array, time) if kind else None
                if text is None:
                    raise Unread("clips")
                lines.append("%s %s" % (letter, text) if time is None else "%s %s %s" % (letter, hexed("d", time), text))
                if letter == "D" and times:
                    packed = struct.pack("<%dd" % len(times), *times)
                    lines.append("T %d %s" % (len(times), hashlib.sha1(packed).hexdigest()))
        except Unread:
            self.skipped += 1
            lines = ["K"]
        out.extend(lines)
        if attribute.HasAuthoredConnections():
            out.append(" ".join(["G"] + [self.target(path) for path in attribute.GetConnections()]))

    def prim(self, prim, path):
        out = self.out
        specifiers = {Sdf.SpecifierDef: "def", Sdf.SpecifierOver: "over", Sdf.SpecifierClass: "class"}
        specifier = specifiers[prim.GetSpecifier()]
        flags = "".join(letter for letter, on in (
            ("a", prim.IsActive()), ("l", prim.IsLoaded()), ("d", prim.IsDefined()), ("b", prim.IsAbstract()),
            ("i", prim.IsInstance()), ("p", prim.HasPayload())) if on)
        line = "P %s %s %s %s" % (path, specifier, quoted(str(prim.GetTypeName())), flags)
        if prim.IsInstance():
            prototype = prim.GetPrototype().GetPath()
            if prototype not in self.names:
                self.queue.append(prototype)
                self.names[prototype] = len(self.queue)
            line += " /__P%d" % self.names[prototype]
        out.append(line)
        stack = prim.GetPrimStack()
        for spec in stack:
            out.append("S %s %s" % (os.path.basename(spec.layer.identifier), spec.path))
        metadata = prim.GetAllAuthoredMetadata()
        for key in sorted(metadata, key=lambda name: name.encode("utf-8")):
            if key in COMPOSED:
                continue
            value = metadata[key]
            name = type(value).__name__
            try:
                if name in LIST_OPS:
                    text = array(LIST_OPS[name], value.ApplyOperations([]), True)
                else:
                    declared = next((spec.GetTypeForInfo(key) for spec in stack if spec.HasInfo(key)), None)
                    kind, is_array = declared_type(declared)
                    text = encode(value, kind, is_array, False)
            except Unread:
                text = "?"
            out.append("M %s %s" % (key, text))
        sets = prim.GetVariantSets()
        for name in sets.GetNames():
            out.append("V %s %s" % (quoted(name), quoted(sets.GetVariantSelection(name))))
        for name in prim.GetAuthoredPropertyNames():
            attribute = prim.GetAttribute(name)
            if attribute:
                self.attribute(prim, attribute, name)
                continue
            relationship = prim.GetRelationship(name)
            out.append("R %s %s" % (name, "custom" if relationship.IsCustom() else "-"))
            if relationship.HasAuthoredTargets():
                out.append(" ".join(["G"] + [self.target(path) for path in relationship.GetTargets()]))
            out.append(" ".join(["F"] + [self.target(path) for path in relationship.GetForwardedTargets()]))
        if not prim.IsInstance():
            for child in prim.GetAllChildren():
                self.prim(child, "%s/%s" % (path, child.GetName()))

    def write(self):
        for child in self.stage.GetPseudoRoot().GetAllChildren():
            self.prim(child, "/" + child.GetName())
        done = 0
        while done < len(self.queue):
            prototype = self.stage.GetPrimAtPath(self.queue[done])
            done += 1
            for child in prototype.GetAllChildren():
                self.prim(child, "/__P%d/%s" % (done, child.GetName()))
        kinds = set()
        for error in self.stage.GetCompositionErrors():
            name = type(error).__name__
            kinds.add(ERRORS.get(name, name))
        self.out.extend("E " + kind for kind in sorted(kinds))


def run_stage(job):
    answer = {"status": "", "error": "", "skipped": 0}
    try:
        stage = Usd.Stage.Open(job["location"])
    except Tf.ErrorException as error:
        stage = None
        answer["error"] = str(error)[-400:]
    if stage is None:
        answer["status"] = "Refused"
        return answer
    dump = StageDump(stage)
    dump.write()
    answer["skipped"] = dump.skipped
    with open(job["dump"], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(dump.out) + "\n")
    return answer


def mesh_line(mesh, time):
    """Returns what OpenUSD reads of a mesh's topology, as `usd_corpus --geometry` writes it, with the checks that
    lib/usd's schema reading makes."""
    def array(attribute):
        """Returns an attribute's value as a list, or None without one. A value that is no array, of a type that the
        schema does not give the attribute, is a mesh that lib/usd refuses."""
        value = attribute.Get(time)
        if value is None:
            return None
        if not hasattr(value, "__len__"):
            raise Unread("not an array")
        return list(value)
    try:
        return checked_mesh_line(mesh, time, array)
    except Unread:
        return "invalid"


def checked_mesh_line(mesh, time, array):
    points = array(mesh.GetPointsAttr())
    if points is None:
        return None
    # Points that are not vectors of three numbers are of a type that the schema does not give them.
    if any(not hasattr(point, "__len__") or len(point) != 3 for point in points):
        return "invalid"
    counts = array(mesh.GetFaceVertexCountsAttr()) or []
    indices = array(mesh.GetFaceVertexIndicesAttr()) or []
    holes = array(mesh.GetHoleIndicesAttr()) or []
    if any(count < 3 for count in counts) or sum(counts) != len(indices):
        return "invalid"
    if any(i < 0 or i >= len(points) for i in indices) or any(h < 0 or h >= len(counts) for h in holes):
        return "invalid"
    primvar = UsdGeom.PrimvarsAPI(mesh.GetPrim()).GetPrimvar("normals")
    if primvar and primvar.HasAuthoredValue():
        interpolation = primvar.GetInterpolation()
        normals = primvar.GetIndices(time) if primvar.IsIndexed() else primvar.Get(time)
    else:
        interpolation = mesh.GetNormalsInterpolation()
        normals = mesh.GetNormalsAttr().Get(time)
    if normals is not None and not hasattr(normals, "__len__"):
        return "invalid"
    count = len(normals) if normals is not None else 0
    wanted = {"constant": 1, "uniform": len(counts), "faceVarying": len(indices)}.get(interpolation, len(points))
    if count != 0 and count != wanted:
        return "invalid"
    orientation = "leftHanded" if mesh.GetOrientationAttr().Get() == "leftHanded" else "rightHanded"
    return "%d %d %d %d %s %d %s" % (len(points), len(counts), len(indices), len(holes), orientation, count,
                                     interpolation)


def run_geometry(job):
    """Writes what OpenUSD's schemas of geometry read of each prim of a stage, as `usd_corpus --geometry` does."""
    answer = {"status": "", "error": ""}
    try:
        stage = Usd.Stage.Open(job["location"])
    except Tf.ErrorException as error:
        stage = None
        answer["error"] = str(error)[-400:]
    if stage is None:
        answer["status"] = "Refused"
        return answer
    time = Usd.TimeCode(stage.GetStartTimeCode())
    out = []
    for prim in Usd.PrimRange.AllPrims(stage.GetPseudoRoot()):
        if prim.IsPseudoRoot():
            continue
        path = str(prim.GetPath())
        if prim.IsA(UsdGeom.Xformable):
            xformable = UsdGeom.Xformable(prim)
            matrix = xformable.GetLocalTransformation(time)
            resets = xformable.GetResetXformStack()
            numbers = " ".join("%.9f" % matrix[row][column] for row in range(4) for column in range(4))
            out.append("X %s %d %s" % (path, 1 if resets else 0, numbers))
        if prim.IsA(UsdGeom.Mesh):
            line = mesh_line(UsdGeom.Mesh(prim), time)
            if line is not None:
                out.append("M %s %s" % (path, line))
            bound = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial(UsdShade.Tokens.preview)[0]
            out.append("B %s %s" % (path, bound.GetPath() if bound else "-"))
    with open(job["dump"], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + ("\n" if out else ""))
    return answer


def main():
    with open(sys.argv[1], encoding="utf-8") as f:
        jobs = json.load(f)
    for job in jobs:
        try:
            if job.get("geometry"):
                answer = run_geometry(job)
            else:
                answer = run_stage(job) if job.get("stage") else run(job)
        except Exception as error:  # A defect of this script is reported with its job, and the run goes on.
            answer = {"status": "Error", "error": "%s: %s" % (type(error).__name__, error), "format": "",
                      "converted": False, "unread": ""}
        sys.stdout.write(json.dumps(answer) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
