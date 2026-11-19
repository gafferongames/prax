// MaterialX documents as layers. The layout is OpenUSD's UsdMtlx's, which layers that reference a document name paths
// in, as `@look.mtlx@</MaterialX/Materials>`:
//
//     /MaterialX                       the document
//         /Materials/<material>        a Material for each surfacematerial, whose outputs:mtlx:surface connects to
//             /<shader>                its shader, which references /MaterialX/Shaders/<shader>
//             /NodeGraphs              a reference to /MaterialX/NodeGraphs
//         /Shaders/<shader>            a Shader for each node of a shader's type, with its values
//         /NodeGraphs                  a NodeGraph that holds
//             /<nodegraph>             a NodeGraph for each nodegraph, with a Shader for each of its nodes
//             /<node>                  a Shader for each node outside a node graph and of no shader's type
//
// A material holds its own reference to the node graphs, so that the connections of its shader name paths under the
// material, which any arc that brings the material maps with it. A node's `info:id` is its `nodedef`, or else
// `ND_<category>_<type>`. An input becomes `inputs:<name>` of its type, with its value or its connection, and a node's
// output `outputs:out`, or the output that a connection names. An input's color space, its own or its scope's, is the
// attribute's `colorSpace`. The document's file prefixes are not applied to its file names, as UsdMtlx does not apply
// them, and a file name resolves against the document, as any asset path of a layer does.

#include "usd/src/usd_mtlx.h"

#include "pr/materialx.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// A document's translation into a layer.
struct Mtlx {
    UsdLayer* layer;
    const PrMaterialxDocument* document;
    const PrMaterialxElement* elements;
    RawArray<char> path;
};

// Returns the text of the parts, joined, as a string of the layer.
PrStringView joined(Mtlx& x, PrStringView a, PrStringView b, PrStringView c = {}, PrStringView d = {}, PrStringView e = {}) {
    x.path.clear();
    const PrStringView parts[] = {a, b, c, d, e};
    for (PrStringView part : parts)
        x.path.append(part.data, uint32_t(part.size));
    return usdLayerIntern(x.layer, PrStringView{x.path.data, x.path.count});
}

void setToken(Mtlx& x, PrUsdSpecIndex spec, PrStringView field, PrStringView token) {
    PrStringView text = usdLayerIntern(x.layer, token);
    usdLayerSetField(x.layer, spec, field, usdLayerValue(x.layer, PrUsdType_Token, false, 1, &text));
}

// Returns a prim of `typeName` named `name` under `parent`, defined.
PrUsdSpecIndex definePrim(Mtlx& x, PrUsdSpecIndex parent, PrStringView name, PrStringView typeName) {
    bool existed = false;
    PrUsdSpecIndex spec = usdLayerAddSpec(x.layer, PrUsdSpecForm_Prim, parent, name, &existed);
    const PrUsdSpecifier def = PrUsdSpecifier_Def;
    usdLayerSetField(x.layer, spec, "specifier", usdLayerValue(x.layer, PrUsdType_Specifier, false, 1, &def));
    if (typeName.size != 0)
        setToken(x, spec, "typeName", typeName);
    return spec;
}

// Gives a prim a reference to the prim at `path` of the same layer.
void addInternalReference(Mtlx& x, PrUsdSpecIndex prim, PrStringView path) {
    PrUsdReference reference{.assetPath = usdLayerIntern(x.layer, ""), .primPath = path, .layerOffset = {0, 1}};
    PrUsdListOp* listOp = x.layer->arena.allocZeroed<PrUsdListOp>();
    listOp->itemType = PrUsdType_Reference;
    for (PrUsdValue& each : listOp->items)
        each = PrUsdValue{PrUsdType_Reference, true, 0, nullptr};
    listOp->items[PrUsdListOpList_Prepended] = usdLayerValue(x.layer, PrUsdType_Reference, true, 1, &reference);
    usdLayerSetField(x.layer, prim, "references", PrUsdValue{PrUsdType_ListOp, false, 1, listOp});
}

// The type of an attribute that holds a value of a MaterialX type.
struct AttributeType {
    PrStringView mtlx;
    PrStringView usd;
    PrUsdType type;
    uint32_t floats;            // the number of floats that a value parses into, for a type of floats
};

constexpr AttributeType kTypes[] = {
    {"boolean", "bool", PrUsdType_Bool, 0},
    {"color3", "color3f", PrUsdType_Float3, 3},
    {"color4", "color4f", PrUsdType_Float4, 4},
    {"filename", "asset", PrUsdType_Asset, 0},
    {"float", "float", PrUsdType_Float, 1},
    {"integer", "int", PrUsdType_Int, 0},
    {"string", "string", PrUsdType_String, 0},
    {"vector2", "float2", PrUsdType_Float2, 2},
    {"vector3", "float3", PrUsdType_Float3, 3},
    {"vector4", "float4", PrUsdType_Float4, 4},
};

// Returns the attribute type of a MaterialX type. A type of no value that a layer holds, as a shader's or a closure's,
// is a token without a value.
AttributeType attributeType(PrStringView type) {
    for (const AttributeType& known : kTypes) {
        if (known.mtlx == type)
            return known;
    }
    return AttributeType{type, "token", PrUsdType_Token, 0};
}

// Returns the attribute `name` of a prim, declared with a MaterialX type.
PrUsdSpecIndex declareAttribute(Mtlx& x, PrUsdSpecIndex prim, PrStringView name, PrStringView type) {
    bool existed = false;
    PrUsdSpecIndex spec = usdLayerAddSpec(x.layer, PrUsdSpecForm_Attribute, prim, name, &existed);
    if (!existed)
        setToken(x, spec, "typeName", attributeType(type).usd);
    return spec;
}

// Sets an attribute's value from a MaterialX value of `type`. A value that does not parse as its type is left unset.
void setValue(Mtlx& x, PrUsdSpecIndex attribute, PrStringView type, PrStringView text) {
    AttributeType known = attributeType(type);
    PrUsdValue value{};
    if (known.floats != 0) {
        float numbers[4] = {};
        if (!prMaterialxParseFloats(text, numbers, known.floats))
            return;
        value = usdLayerValue(x.layer, known.type, false, 1, numbers);
    } else if (known.type == PrUsdType_Bool) {
        bool flag = text == "true";
        if (!flag && text != "false")
            return;
        value = usdLayerValue(x.layer, PrUsdType_Bool, false, 1, &flag);
    } else if (known.type == PrUsdType_Int) {
        bool negative = text.size != 0 && text.data[0] == '-';
        uint64_t magnitude = 0;
        size_t skip = negative ? 1 : 0;
        if (!prU64Parse(text.data + skip, text.size - skip, &magnitude, 10) || magnitude > 0x7fffffffu)
            return;
        int32_t number = negative ? -int32_t(magnitude) : int32_t(magnitude);
        value = usdLayerValue(x.layer, PrUsdType_Int, false, 1, &number);
    } else if (known.type == PrUsdType_String || known.type == PrUsdType_Asset) {
        PrStringView stored = usdLayerIntern(x.layer, text);
        value = usdLayerValue(x.layer, known.type, false, 1, &stored);
    } else {
        return;
    }
    usdLayerSetField(x.layer, attribute, "default", value);
}

void setConnection(Mtlx& x, PrUsdSpecIndex attribute, PrStringView path) {
    PrUsdListOp* listOp = x.layer->arena.allocZeroed<PrUsdListOp>();
    listOp->itemType = PrUsdType_Path;
    listOp->isExplicit = true;
    for (PrUsdValue& each : listOp->items)
        each = PrUsdValue{PrUsdType_Path, true, 0, nullptr};
    listOp->items[PrUsdListOpList_Explicit] = usdLayerValue(x.layer, PrUsdType_Path, true, 1, &path);
    usdLayerSetField(x.layer, attribute, "connectionPaths", PrUsdValue{PrUsdType_ListOp, false, 1, listOp});
}

// Returns true if an element of the root is a node of a shader's type, which the layer keeps under Shaders.
bool isShader(const PrMaterialxElement& element) {
    return element.type == "surfaceshader" || element.type == "displacementshader" ||
        element.type == "volumeshader" || element.type == "lightshader";
}

// Returns true if an element is a node: an element of a node graph or of the root that is no input, output or
// definition.
bool isNode(const PrMaterialxElement& element) {
    PrStringView c = element.category;
    return element.name.size != 0 && usdNameIsIdentifier(element.name) && c != "input" && c != "output" &&
        c != "nodegraph" && c != "nodedef" && c != "typedef" && c != "implementation" && c != "look" &&
        c != "lookgroup" && c != "collection" && c != "geominfo" && c != "backdrop" && c != "variantset" &&
        c != "propertyset" && c != "materialassign" && c != "unit" && c != "unitdef" && c != "token" &&
        c != "parameter" && element.type != "material";
}

// The paths that the connections of a node's inputs name: the prim that holds the node's siblings, the prim that holds
// the document's node graphs, and the node graph whose interface the node's inputs name, which is empty outside one.
struct Scope {
    PrStringView nodes;
    PrStringView graphs;
    PrStringView graph;
};

// Adds the inputs of a node, or of a node graph, to its prim: those with values if `values`, and those with connections
// if `connections`.
void addInputs(Mtlx& x, PrMaterialxElementIndex element, PrUsdSpecIndex prim, const Scope& scope, bool values,
        bool connections) {
    const PrMaterialxDocument* d = x.document;
    for (PrMaterialxElementIndex at = x.elements[element].firstChild; at != kPrMaterialxNone; at =
            x.elements[at].nextSibling) {
        const PrMaterialxElement& input = x.elements[at];
        if (input.category != "input" || !usdNameIsIdentifier(input.name))
            continue;
        PrStringView name = joined(x, "inputs:", input.name);
        PrStringView nodeName = prMaterialxAttribute(d, at, "nodename");
        PrStringView nodeGraph = prMaterialxAttribute(d, at, "nodegraph");
        PrStringView interfaceName = prMaterialxAttribute(d, at, "interfacename");
        PrStringView output = prMaterialxAttribute(d, at, "output");
        PrStringView out = output.size != 0 ? output : PrStringView("out");
        PrStringView path{};
        if (nodeName.size != 0)
            path = joined(x, scope.nodes, "/", nodeName, ".outputs:", out);
        else if (nodeGraph.size != 0)
            path = joined(x, scope.graphs, "/", nodeGraph, ".outputs:", out);
        else if (interfaceName.size != 0 && scope.graph.size != 0)
            path = joined(x, scope.graph, ".inputs:", interfaceName);
        bool connected = path.size != 0;
        if (connected ? !connections : !values)
            continue;
        PrUsdSpecIndex attribute = declareAttribute(x, prim, name, input.type);
        if (connected) {
            setConnection(x, attribute, path);
            continue;
        }
        PrStringView value = prMaterialxAttribute(d, at, "value");
        if (value.data != nullptr)
            setValue(x, attribute, input.type, value);
        PrStringView colorSpace = prMaterialxScopedAttribute(d, at, "colorspace");
        if (colorSpace.size != 0 && (input.type == "filename" || prStringStartsWith(input.type, "color")))
            setToken(x, attribute, "colorSpace", colorSpace);
    }
}

// Adds a node as a Shader under `parent`, with its id, its inputs and its output.
void addNode(Mtlx& x, PrMaterialxElementIndex element, PrUsdSpecIndex parent, const Scope& scope, bool connections) {
    const PrMaterialxElement& node = x.elements[element];
    PrUsdSpecIndex prim = definePrim(x, parent, node.name, "Shader");
    PrStringView nodeDef = prMaterialxAttribute(x.document, element, "nodedef");
    PrStringView id = nodeDef.size != 0 ? nodeDef : joined(x, "ND_", node.category, "_", node.type);
    PrUsdSpecIndex info = declareAttribute(x, prim, "info:id", "token");
    const PrUsdVariability uniform = PrUsdVariability_Uniform;
    usdLayerSetField(x.layer, info, "variability", usdLayerValue(x.layer, PrUsdType_Variability, false, 1, &uniform));
    PrStringView stored = usdLayerIntern(x.layer, id);
    usdLayerSetField(x.layer, info, "default", usdLayerValue(x.layer, PrUsdType_Token, false, 1, &stored));
    if (node.type != "multioutput")
        declareAttribute(x, prim, "outputs:out", node.type);
    addInputs(x, element, prim, scope, true, connections);
}

// Declares the outputs that connections name on the nodes that they reach, which a node of several outputs does not
// declare for itself.
void declareOutputs(Mtlx& x, PrMaterialxElementIndex scope, PrUsdSpecIndex scopePrim) {
    const PrMaterialxDocument* d = x.document;
    for (PrMaterialxElementIndex node = x.elements[scope].firstChild; node != kPrMaterialxNone; node =
            x.elements[node].nextSibling) {
        for (PrMaterialxElementIndex at = x.elements[node].firstChild; at != kPrMaterialxNone; at =
                x.elements[at].nextSibling) {
            PrStringView output = prMaterialxAttribute(d, at, "output");
            PrStringView nodeName = prMaterialxAttribute(d, at, "nodename");
            if (output.size == 0 || nodeName.size == 0 || !usdNameIsIdentifier(nodeName))
                continue;
            bool existed = false;
            PrUsdSpecIndex target = usdLayerAddSpec(x.layer, PrUsdSpecForm_Prim, scopePrim, nodeName, &existed);
            if (existed)
                declareAttribute(x, target, joined(x, "outputs:", output), x.elements[at].type);
        }
    }
}

} // namespace

bool usdMtlxMatches(PrByteView bytes) {
    size_t at = 0;
    if (bytes.size >= 3 && bytes.data[0] == 0xef && bytes.data[1] == 0xbb && bytes.data[2] == 0xbf)
        at = 3;
    while (at < bytes.size && (bytes.data[at] == ' ' || bytes.data[at] == '\t' || bytes.data[at] == '\n' ||
            bytes.data[at] == '\r'))
        ++at;
    return at < bytes.size && bytes.data[at] == '<';
}

PrStatus usdMtlxRead(UsdLayer* layer, PrStringView text, PrTextBuffer* PR_NULLABLE diagnostics) {
    PrMaterialxDocument* document = nullptr;
    PrStatus status = prMaterialxDocumentCreate(text, diagnostics, &document);
    if (status != PrStatus_Ok)
        return status;
    uint32_t count = 0;
    Mtlx x{.layer = layer, .document = document, .elements = prMaterialxDocumentElements(document, &count)};
    layer->version = usdLayerIntern(layer, prMaterialxAttribute(document, 0, "version"));
    setToken(x, 0, "defaultPrim", "MaterialX");

    PrUsdSpecIndex root = definePrim(x, 0, "MaterialX", "");
    PrUsdSpecIndex materials = definePrim(x, root, "Materials", "");
    PrUsdSpecIndex shaders = definePrim(x, root, "Shaders", "");
    PrUsdSpecIndex graphs = definePrim(x, root, "NodeGraphs", "NodeGraph");
    const PrStringView graphsPath = "/MaterialX/NodeGraphs";

    // The node graphs, with their nodes, inputs and outputs, and the nodes outside them.
    for (PrMaterialxElementIndex at = x.elements[0].firstChild; at != kPrMaterialxNone; at = x.elements[at].nextSibling) {
        const PrMaterialxElement& element = x.elements[at];
        if (element.category == "nodegraph" && usdNameIsIdentifier(element.name)) {
            PrUsdSpecIndex graph = definePrim(x, graphs, element.name, "NodeGraph");
            PrStringView graphPath = joined(x, graphsPath, "/", element.name);
            addInputs(x, at, graph, Scope{graphsPath, graphsPath, {}}, true, false);
            Scope inner{graphPath, graphsPath, graphPath};
            for (PrMaterialxElementIndex node = element.firstChild; node != kPrMaterialxNone; node =
                    x.elements[node].nextSibling) {
                if (isNode(x.elements[node]))
                    addNode(x, node, graph, inner, true);
            }
            for (PrMaterialxElementIndex output = element.firstChild; output != kPrMaterialxNone;
                    output = x.elements[output].nextSibling) {
                const PrMaterialxElement& o = x.elements[output];
                PrStringView nodeName = prMaterialxAttribute(document, output, "nodename");
                if (o.category != "output" || !usdNameIsIdentifier(o.name))
                    continue;
                PrUsdSpecIndex attribute = declareAttribute(x, graph, joined(x, "outputs:", o.name), o.type);
                if (nodeName.size == 0)
                    continue;
                PrStringView nodeOutput = prMaterialxAttribute(document, output, "output");
                PrStringView out = nodeOutput.size != 0 ? nodeOutput : PrStringView("out");
                setConnection(x, attribute, joined(x, graphPath, "/", nodeName, ".outputs:", out));
            }
            declareOutputs(x, at, graph);
        } else if (isNode(element) && !isShader(element)) {
            addNode(x, at, graphs, Scope{graphsPath, graphsPath, {}}, true);
        }
    }
    declareOutputs(x, 0, graphs);

    // The shaders, with their values, and the materials, each with a shader of its own that connects under it.
    for (PrMaterialxElementIndex at = x.elements[0].firstChild; at != kPrMaterialxNone; at = x.elements[at].nextSibling) {
        if (isNode(x.elements[at]) && isShader(x.elements[at]))
            addNode(x, at, shaders, Scope{graphsPath, graphsPath, {}}, false);
    }
    for (PrMaterialxElementIndex at = x.elements[0].firstChild; at != kPrMaterialxNone; at = x.elements[at].nextSibling) {
        const PrMaterialxElement& element = x.elements[at];
        if (element.type != "material" || !usdNameIsIdentifier(element.name))
            continue;
        PrUsdSpecIndex material = definePrim(x, materials, element.name, "Material");
        PrStringView materialPath = joined(x, "/MaterialX/Materials/", element.name);
        PrUsdSpecIndex local = definePrim(x, material, "NodeGraphs", "");
        addInternalReference(x, local, graphsPath);
        PrStringView localGraphs = joined(x, materialPath, "/NodeGraphs");
        for (PrMaterialxElementIndex input = element.firstChild; input != kPrMaterialxNone; input =
                x.elements[input].nextSibling) {
            const PrMaterialxElement& i = x.elements[input];
            PrStringView shaderName = prMaterialxAttribute(document, input, "nodename");
            PrMaterialxElementIndex shader =
                shaderName.size != 0 ? prMaterialxChild(document, 0, shaderName) : kPrMaterialxNone;
            if (i.category != "input" || shader == kPrMaterialxNone || !isShader(x.elements[shader]))
                continue;
            PrStringView terminal{};
            if (i.type == "surfaceshader")
                terminal = "outputs:mtlx:surface";
            else if (i.type == "displacementshader")
                terminal = "outputs:mtlx:displacement";
            else if (i.type == "volumeshader")
                terminal = "outputs:mtlx:volume";
            else
                continue;
            PrUsdSpecIndex copy = definePrim(x, material, shaderName, "Shader");
            addInternalReference(x, copy, joined(x, "/MaterialX/Shaders/", shaderName));
            addInputs(x, shader, copy, Scope{localGraphs, localGraphs, {}}, false, true);
            PrUsdSpecIndex output = declareAttribute(x, material, terminal, "token");
            setConnection(x, output, joined(x, materialPath, "/", shaderName, ".outputs:out"));
        }
    }
    prMaterialxDocumentDestroy(document);
    return PrStatus_Ok;
}

} // namespace pr_impl
