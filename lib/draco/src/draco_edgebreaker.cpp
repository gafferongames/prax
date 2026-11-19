// Decoding the connectivity of a mesh that Edgebreaker codes: the faces as a corner table, the seams of the attributes,
// the points, and the order of each group of attributes' values.
//
// The bitstream specification describes the decoding in its sections EdgeBreaker Decoder, EdgeBreaker Traversal,
// Boundary Decoder and Attributes Decoder. This follows the reference decoder where the two differ, since the order of
// a mesh's points is the reference decoder's.

#include "draco/src/draco_edgebreaker.h"

namespace pr_impl {

namespace {

// The symbols of Edgebreaker, as the standard traversal's bits give them.
constexpr uint32_t kSymbolC = 0;
constexpr uint32_t kSymbolS = 1;
constexpr uint32_t kSymbolL = 3;
constexpr uint32_t kSymbolR = 5;
constexpr uint32_t kSymbolE = 7;
constexpr uint32_t kSymbolInvalid = 8;

// How the symbols are coded (the bitstream's edgebreaker_traversal_type).
enum {
    TraversalType_Standard,                 // as bits
    TraversalType_Predictive,               // as bits, with a bit that says whether a symbol is the one predicted
    TraversalType_Valence,                  // with rANS, in the context of the valence of the active vertex
    TraversalType_Count,
};
typedef uint8_t TraversalType;

// The valences that the valence traversal tells apart, and their number.
constexpr uint32_t kMinValence = 2;
constexpr uint32_t kMaxValence = 7;
constexpr uint32_t kValenceContexts = kMaxValence - kMinValence + 1;

// How an attribute's values are sequenced (the bitstream's att_dec_decoder_type and att_dec_traversal_method).
constexpr uint8_t kVertexAttribute = 0;
constexpr uint8_t kTraversalDepthFirst = 0;
constexpr uint8_t kTraversalPredictionDegree = 1;

// A topology split: the symbol at which the encoder met a face that it had left behind, and the symbol that left it.
// Both are counted in the encoder's order, which is the reverse of the decoder's.
struct SplitEvent {
    uint32_t source;
    uint32_t split;
    uint32_t rightEdge;
};

// The decoder of a mesh's symbols and of the bits that go with them.
struct Traversal {
    TraversalType type;
    DracoBits symbols;
    DracoAnsBits startFaces;
    RawArray<DracoAnsBits> seams;       // by seam table
    DracoAnsBits prediction;
    uint32_t predicted;                 // the symbol that the predictive traversal predicts, or kSymbolInvalid
    uint32_t last;
    bool hasContext;
    uint32_t context;
    RawArray<uint32_t> valences;        // by vertex: the faces at it so far
    RawArray<uint32_t> contextSymbols[kValenceContexts];
};

// The state of decoding the faces.
struct Faces {
    Draco* draco;
    DracoCornerTable* table;
    Traversal traversal;
    uint32_t maxVertices;
    RawArray<SplitEvent> splits;
    RawArray<uint32_t> splitCorner;     // by symbol: the active corner that a split left for it, or kDracoInvalid
    RawArray<uint32_t> activeCorners;
    RawArray<uint32_t> invalidVertices;
    RawArray<uint8_t> vertexIsHole;     // by vertex: nonzero until a face closes the fan around it
};

void resetFaces(Faces* faces) {
    faces->traversal.seams.reset();
    faces->traversal.valences.reset();
    for (RawArray<uint32_t>& coded : faces->traversal.contextSymbols)
        coded.reset();
    faces->splits.reset();
    faces->splitCorner.reset();
    faces->activeCorners.reset();
    faces->invalidVertices.reset();
    faces->vertexIsHole.reset();
}

inline uint32_t nextCorner(uint32_t corner) {
    return DracoConnectivity::next(corner);
}

inline uint32_t previousCorner(uint32_t corner) {
    return DracoConnectivity::previous(corner);
}

inline void setOpposite(DracoCornerTable* table, uint32_t a, uint32_t b) {
    table->opposite.data[a] = b;
    table->opposite.data[b] = a;
}

inline uint32_t addVertex(DracoCornerTable* table) {
    table->vertexCorner.push(kDracoInvalid);
    return table->vertexCount++;
}

// ---------------------------------------------------------------------------------------------------------------------
// The traversal's symbols

// Reads the topology splits: the symbols of each as differences, and then the edge of each as a bit (the bitstream's
// ParseTopologySplitEvents).
bool readSplits(Faces* faces, uint32_t faceCount) {
    DracoReader* reader = &faces->draco->reader;
    uint32_t count = 0;
    if (!dracoReadVarint32(reader, &count) || count > faceCount)
        return false;
    uint32_t last = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t delta = 0;
        SplitEvent event = {};
        if (!dracoReadVarint32(reader, &delta) || delta > 0x7fffffffu - last)
            return false;
        event.source = last + delta;
        if (!dracoReadVarint32(reader, &delta) || delta > event.source)
            return false;
        event.split = event.source - delta;
        last = event.source;
        faces->splits.push(event);
    }
    size_t bytes = (size_t(count) + 7) / 8;
    if (reader->left() < bytes)
        return false;
    DracoBits bits = dracoBitsBegin(reader, bytes);
    for (uint32_t i = 0; i < count; ++i) {
        if (!dracoBitsRead(&bits, 1, &faces->splits.data[i].rightEdge))
            return false;
    }
    reader->at += bytes;
    return true;
}

// Reads what the traversal decodes from: the symbols, the start faces and the seams, each a run of its own (the
// bitstream's EdgebreakerTraversalStart).
bool startTraversal(Faces* faces, uint32_t seamTableCount, uint32_t symbolCount) {
    DracoReader* reader = &faces->draco->reader;
    Traversal* traversal = &faces->traversal;
    traversal->predicted = kSymbolInvalid;
    traversal->last = kSymbolInvalid;
    if (traversal->type != TraversalType_Valence) {
        uint64_t size = 0;
        if (!dracoReadVarint64(reader, &size) || size > reader->left())
            return false;
        // A symbol takes one bit at least, unless it is predicted.
        if (traversal->type == TraversalType_Standard && symbolCount > size * 8)
            return false;
        traversal->symbols = dracoBitsBegin(reader, size_t(size));
        reader->at += size;
    }
    if (!dracoAnsBitsStart(reader, &traversal->startFaces))
        return false;
    for (uint32_t i = 0; i < seamTableCount; ++i) {
        DracoAnsBits bits = {};
        if (!dracoAnsBitsStart(reader, &bits))
            return false;
        traversal->seams.push(bits);
    }
    if (traversal->type == TraversalType_Standard)
        return true;

    traversal->valences.resizeZeroed(faces->maxVertices);
    if (traversal->type == TraversalType_Predictive) {
        uint32_t splitSymbols = 0;
        return dracoReadU32(reader, &splitSymbols) && splitSymbols < faces->maxVertices &&
            dracoAnsBitsStart(reader, &traversal->prediction);
    }
    // The symbols of each context, which are read from the last to the first.
    for (uint32_t i = 0; i < kValenceContexts; ++i) {
        uint32_t count = 0;
        if (!dracoReadVarint32(reader, &count) || count > faces->table->faceCount)
            return false;
        traversal->contextSymbols[i].resize(count);
        if (!dracoDecodeSymbols(reader, count, 1, traversal->contextSymbols[i].data))
            return false;
    }
    return true;
}

inline uint32_t readStandardSymbol(Traversal* traversal) {
    uint32_t symbol = 0;
    if (!dracoBitsRead(&traversal->symbols, 1, &symbol))
        return kSymbolInvalid;
    if (symbol == kSymbolC)
        return symbol;
    uint32_t suffix = 0;
    if (!dracoBitsRead(&traversal->symbols, 2, &suffix))
        return kSymbolInvalid;
    return symbol | suffix << 1;
}

inline uint32_t readSymbol(Traversal* traversal) {
    static const uint32_t kValenceSymbols[] = {kSymbolC, kSymbolS, kSymbolL, kSymbolR, kSymbolE};
    switch (traversal->type) {
        case TraversalType_Standard:
            return readStandardSymbol(traversal);
        case TraversalType_Predictive:
            if (traversal->predicted != kSymbolInvalid && dracoAnsBit(&traversal->prediction))
                traversal->last = traversal->predicted;
            else
                traversal->last = readStandardSymbol(traversal);
            return traversal->last;
        default: {
            // The first symbol has no context, and is the triangle that starts a component.
            if (!traversal->hasContext) {
                traversal->last = kSymbolE;
                return kSymbolE;
            }
            RawArray<uint32_t>& coded = traversal->contextSymbols[traversal->context];
            if (coded.count == 0 || coded.last() > 4)
                return kSymbolInvalid;
            traversal->last = kValenceSymbols[coded.last()];
            coded.pop();
            return traversal->last;
        }
    }
}

// Counts the new face at the vertices of `corner`, its first corner, and sets what the next symbol is decoded with: the
// prediction, or the context of the valence of the vertex that the traversal turns around next.
inline void reachCorner(Traversal* traversal, const DracoCornerTable* table, uint32_t corner) {
    if (traversal->type == TraversalType_Standard)
        return;
    uint32_t* valences = traversal->valences.data;
    uint32_t tip = table->cornerVertex.data[corner];
    uint32_t next = table->cornerVertex.data[nextCorner(corner)];
    uint32_t previous = table->cornerVertex.data[previousCorner(corner)];
    switch (traversal->last) {
        case kSymbolC:
        case kSymbolS:
            valences[next] += 1;
            valences[previous] += 1;
            break;
        case kSymbolR:
            valences[tip] += 1;
            valences[next] += 1;
            valences[previous] += 2;
            break;
        case kSymbolL:
            valences[tip] += 1;
            valences[next] += 2;
            valences[previous] += 1;
            break;
        case kSymbolE:
            valences[tip] += 2;
            valences[next] += 2;
            valences[previous] += 2;
            break;
        default:
            break;
    }
    if (traversal->type == TraversalType_Predictive) {
        traversal->predicted = kSymbolInvalid;
        if (traversal->last == kSymbolC || traversal->last == kSymbolR)
            traversal->predicted = valences[next] < 6 ? kSymbolR : kSymbolC;
        return;
    }
    uint32_t valence = valences[next];
    valence = valence < kMinValence ? kMinValence : valence > kMaxValence ? kMaxValence : valence;
    traversal->context = valence - kMinValence;
    traversal->hasContext = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Faces

// Builds the corner table from the symbols, a face for each, and then from the start faces, a face for each active
// corner that a bit says has one. Returns false for symbols that describe no mesh.
bool decodeFaces(Faces* faces, uint32_t symbolCount, bool removeInvalidVertices, uint32_t* vertexCount) {
    DracoCornerTable* table = faces->table;
    Traversal* traversal = &faces->traversal;
    RawArray<uint32_t>& active = faces->activeCorners;
    uint32_t* cornerVertex = table->cornerVertex.data;
    const uint32_t* opposite = table->opposite.data;
    // The corners that merging vertices may visit, in all: a mesh visits each a few times, and symbols that describe no
    // mesh are not let take more time than that.
    uint64_t mergeBudget = uint64_t(table->faceCount) * 3 * 64 + 4096;
    uint32_t faceCount = 0;

    for (uint32_t symbolId = 0; symbolId < symbolCount; ++symbolId) {
        uint32_t corner = 3 * faceCount++;
        bool checkSplit = false;
        uint32_t symbol = readSymbol(traversal);
        if (symbol == kSymbolC) {
            // The face closes the fan around the vertex after the active corner, between the active edge and the edge
            // that the fan began with.
            if (active.count == 0)
                return false;
            uint32_t a = active.last();
            uint32_t pivot = cornerVertex[nextCorner(a)];
            uint32_t leftMost = table->vertexCorner.data[pivot];
            if (leftMost == kDracoInvalid)
                return false;
            uint32_t b = nextCorner(leftMost);
            if (a == b || opposite[a] != kDracoInvalid || opposite[b] != kDracoInvalid)
                return false;
            setOpposite(table, a, corner + 1);
            setOpposite(table, b, corner + 2);
            uint32_t previousOfA = cornerVertex[previousCorner(a)];
            uint32_t nextOfB = cornerVertex[nextCorner(b)];
            if (pivot == previousOfA || pivot == nextOfB)
                return false;
            cornerVertex[corner] = pivot;
            cornerVertex[corner + 1] = nextOfB;
            cornerVertex[corner + 2] = previousOfA;
            table->vertexCorner.data[previousOfA] = corner + 2;
            faces->vertexIsHole.data[pivot] = 0;
            active.last() = corner;
        } else if (symbol == kSymbolR || symbol == kSymbolL) {
            // The face has a new vertex, opposite the active edge.
            if (active.count == 0)
                return false;
            uint32_t a = active.last();
            if (opposite[a] != kDracoInvalid)
                return false;
            uint32_t tip = symbol == kSymbolR ? corner + 2 : corner + 1;
            uint32_t left = symbol == kSymbolR ? corner + 1 : corner;
            uint32_t right = symbol == kSymbolR ? corner : corner + 2;
            setOpposite(table, tip, a);
            uint32_t vertex = addVertex(table);
            if (table->vertexCount > faces->maxVertices)
                return false;
            cornerVertex[tip] = vertex;
            table->vertexCorner.data[vertex] = tip;
            uint32_t rightVertex = cornerVertex[previousCorner(a)];
            cornerVertex[right] = rightVertex;
            table->vertexCorner.data[rightVertex] = right;
            cornerVertex[left] = cornerVertex[nextCorner(a)];
            active.last() = corner;
            checkSplit = true;
        } else if (symbol == kSymbolS) {
            // The face joins two active edges: the one on top, and the one under it or the one that a topology split
            // left for this symbol.
            if (active.count == 0)
                return false;
            uint32_t b = active.last();
            active.pop();
            if (faces->splitCorner.data[symbolId] != kDracoInvalid)
                active.push(faces->splitCorner.data[symbolId]);
            if (active.count == 0)
                return false;
            uint32_t a = active.last();
            if (a == b || opposite[a] != kDracoInvalid || opposite[b] != kDracoInvalid)
                return false;
            setOpposite(table, a, corner + 2);
            setOpposite(table, b, corner + 1);
            uint32_t kept = cornerVertex[previousCorner(a)];
            cornerVertex[corner] = kept;
            cornerVertex[corner + 1] = cornerVertex[nextCorner(a)];
            uint32_t previousOfB = cornerVertex[previousCorner(b)];
            cornerVertex[corner + 2] = previousOfB;
            table->vertexCorner.data[previousOfB] = corner + 2;
            // The two edges met at two vertices that are one: the fan of the second becomes the first's.
            uint32_t merged = nextCorner(b);
            uint32_t mergedVertex = cornerVertex[merged];
            if (traversal->type != TraversalType_Standard)
                traversal->valences.data[kept] += traversal->valences.data[mergedVertex];
            table->vertexCorner.data[kept] = table->vertexCorner.data[mergedVertex];
            uint32_t first = merged;
            while (merged != kDracoInvalid) {
                cornerVertex[merged] = kept;
                uint32_t across = opposite[nextCorner(merged)];
                merged = nextCorner(across);
                if (merged == first || mergeBudget-- == 0)
                    return false;
            }
            table->vertexCorner.data[mergedVertex] = kDracoInvalid;
            if (removeInvalidVertices)
                faces->invalidVertices.push(mergedVertex);
            active.last() = corner;
        } else if (symbol == kSymbolE) {
            // A triangle of three new vertices, which starts a component.
            uint32_t vertex = addVertex(table);
            addVertex(table);
            addVertex(table);
            if (table->vertexCount > faces->maxVertices)
                return false;
            for (uint32_t i = 0; i < 3; ++i) {
                cornerVertex[corner + i] = vertex + i;
                table->vertexCorner.data[vertex + i] = corner + i;
            }
            active.push(corner);
            checkSplit = true;
        } else {
            return false;
        }
        reachCorner(traversal, table, active.last());

        if (checkSplit) {
            // The splits are in the encoder's order, so the last is the first that the decoder meets.
            uint32_t encoderSymbolId = symbolCount - symbolId - 1;
            while (faces->splits.count != 0) {
                SplitEvent event = faces->splits.last();
                if (event.source > encoderSymbolId)
                    return false;
                if (event.source != encoderSymbolId)
                    break;
                faces->splits.pop();
                uint32_t top = active.last();
                uint32_t left = event.rightEdge != 0 ? nextCorner(top) : previousCorner(top);
                if (event.split < symbolCount)
                    faces->splitCorner.data[symbolCount - event.split - 1] = left;
            }
        }
    }
    if (table->vertexCount > faces->maxVertices)
        return false;

    // An active corner that is left is at a boundary, or at a face that the symbols do not code, between it and the two
    // boundary edges that follow its edge.
    while (active.count != 0) {
        uint32_t a = active.last();
        active.pop();
        if (!dracoAnsBit(&traversal->startFaces))
            continue;
        if (faceCount >= table->faceCount)
            return false;
        uint32_t nextVertex = cornerVertex[nextCorner(a)];
        uint32_t leftMost = table->vertexCorner.data[nextVertex];
        if (leftMost == kDracoInvalid)
            return false;
        uint32_t b = nextCorner(leftMost);
        uint32_t tipVertex = cornerVertex[nextCorner(b)];
        leftMost = table->vertexCorner.data[tipVertex];
        if (leftMost == kDracoInvalid)
            return false;
        uint32_t c = nextCorner(leftMost);
        if (a == b || a == c || b == c || opposite[a] != kDracoInvalid || opposite[b] != kDracoInvalid ||
                opposite[c] != kDracoInvalid) {
            return false;
        }
        uint32_t previousVertex = cornerVertex[nextCorner(c)];
        uint32_t corner = 3 * faceCount++;
        setOpposite(table, corner, a);
        setOpposite(table, corner + 1, b);
        setOpposite(table, corner + 2, c);
        cornerVertex[corner] = tipVertex;
        cornerVertex[corner + 1] = previousVertex;
        cornerVertex[corner + 2] = nextVertex;
        faces->vertexIsHole.data[tipVertex] = 0;
        faces->vertexIsHole.data[previousVertex] = 0;
        faces->vertexIsHole.data[nextVertex] = 0;
    }
    if (faceCount != table->faceCount)
        return false;

    // A mesh whose points are its vertices keeps no vertex that a merge emptied: the last vertex takes its place.
    uint32_t count = table->vertexCount;
    for (uint32_t i = 0; i < faces->invalidVertices.count; ++i) {
        uint32_t invalid = faces->invalidVertices.data[i];
        while (count != 0 && table->vertexCorner.data[count - 1] == kDracoInvalid)
            --count;
        if (count == 0)
            return false;
        uint32_t source = count - 1;
        if (source < invalid)
            continue;
        // Every corner around the source: to the left of its left-most corner, which ends at once at a boundary, and
        // then to the right.
        uint32_t start = table->vertexCorner.data[source];
        uint32_t at = start;
        bool leftward = true;
        while (at != kDracoInvalid) {
            if (cornerVertex[at] != source)
                return false;
            cornerVertex[at] = invalid;
            if (leftward) {
                at = nextCorner(opposite[nextCorner(at)]);
                if (at == kDracoInvalid) {
                    at = previousCorner(opposite[previousCorner(start)]);
                    leftward = false;
                } else if (at == start) {
                    break;
                }
            } else {
                at = previousCorner(opposite[previousCorner(at)]);
            }
        }
        table->vertexCorner.data[invalid] = start;
        table->vertexCorner.data[source] = kDracoInvalid;
        faces->vertexIsHole.data[invalid] = faces->vertexIsHole.data[source];
        faces->vertexIsHole.data[source] = 0;
        --count;
    }
    *vertexCount = count;
    return true;
}

// Checks that the corner table describes a surface: each corner is at a vertex that has corners, a vertex's left-most
// corner is at it, and the two faces across an edge agree on its vertices. The corners that are reached from a vertex's
// left-most corner around it are then the vertex's own, so everything that walks around vertices visits a corner once.
bool checkTable(const DracoCornerTable* table) {
    uint32_t cornerCount = table->faceCount * 3;
    const uint32_t* cornerVertex = table->cornerVertex.data;
    for (uint32_t c = 0; c < cornerCount; ++c) {
        uint32_t vertex = cornerVertex[c];
        if (vertex >= table->vertexCount || table->vertexCorner.data[vertex] == kDracoInvalid)
            return false;
        uint32_t across = table->opposite.data[c];
        if (across == kDracoInvalid)
            continue;
        if (cornerVertex[nextCorner(c)] != cornerVertex[previousCorner(across)] ||
                cornerVertex[previousCorner(c)] != cornerVertex[nextCorner(across)]) {
            return false;
        }
    }
    for (uint32_t v = 0; v < table->vertexCount; ++v) {
        uint32_t corner = table->vertexCorner.data[v];
        if (corner != kDracoInvalid && cornerVertex[corner] != v)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Seams and points

// Reads which edges are seams of each seam table, a bit for each edge that two faces share, and marks them. A boundary
// edge is a seam of every table (the bitstream's DecodeAttributeSeams).
void decodeSeams(Draco* draco, Traversal* traversal) {
    const DracoCornerTable* table = &draco->table;
    uint32_t cornerCount = table->faceCount * 3;
    const uint32_t* cornerVertex = table->cornerVertex.data;
    auto mark = [&](DracoSeamTable* seams, uint32_t corner) {
        seams->cornerOnSeam.data[corner] = 1;
        seams->vertexOnSeam.data[cornerVertex[nextCorner(corner)]] = 1;
        seams->vertexOnSeam.data[cornerVertex[previousCorner(corner)]] = 1;
    };
    for (uint32_t c = 0; c < cornerCount; ++c) {
        uint32_t across = table->opposite.data[c];
        if (across == kDracoInvalid) {
            for (uint32_t i = 0; i < draco->seamTables.count; ++i)
                mark(&draco->seamTables.data[i], c);
            continue;
        }
        // An edge is coded at the first of its two faces.
        if (across / 3 < c / 3)
            continue;
        for (uint32_t i = 0; i < draco->seamTables.count; ++i) {
            if (dracoAnsBit(&traversal->seams.data[i])) {
                mark(&draco->seamTables.data[i], c);
                mark(&draco->seamTables.data[i], across);
            }
        }
    }
}

// Numbers the vertices of a seam table: around each vertex of the corner table, a new vertex begins at each seam.
bool numberSeamVertices(const DracoCornerTable* table, DracoSeamTable* seams) {
    DracoConnectivity whole = dracoWhole(table);
    DracoConnectivity cut = dracoCut(seams);
    for (uint32_t v = 0; v < table->vertexCount; ++v) {
        uint32_t corner = table->vertexCorner.data[v];
        if (corner == kDracoInvalid)
            continue;
        // The first corner is the one after a seam, which is the left-most if no seam meets the vertex.
        uint32_t first = corner;
        if (seams->vertexOnSeam.data[v] != 0) {
            for (uint32_t at = cut.swingLeft(first); at != kDracoInvalid; at = cut.swingLeft(at)) {
                if (at == corner)
                    return false;
                first = at;
            }
        }
        uint32_t vertex = seams->vertexCorner.count;
        seams->vertexCorner.push(first);
        seams->cornerVertex.data[first] = vertex;
        for (uint32_t at = whole.swingRight(first); at != kDracoInvalid && at != first; at = whole.swingRight(at)) {
            if (seams->cornerOnSeam.data[nextCorner(at)] != 0) {
                vertex = seams->vertexCorner.count;
                seams->vertexCorner.push(at);
            }
            seams->cornerVertex.data[at] = vertex;
        }
    }
    // A corner that no walk reached has no vertex, and nothing can be decoded at it.
    for (uint32_t c = 0; c < seams->cornerVertex.count; ++c) {
        if (seams->cornerVertex.data[c] == kDracoInvalid)
            return false;
    }
    return true;
}

// Gives each corner its point. Around a vertex, a new point begins wherever the vertex of some seam table changes, so a
// point has one value of every attribute (the bitstream's Attribute_AssignPointsToCorners).
bool assignPoints(Draco* draco, const RawArray<uint8_t>& vertexIsHole, uint32_t vertexCount) {
    const DracoCornerTable* table = &draco->table;
    uint32_t cornerCount = table->faceCount * 3;
    draco->faceCount = table->faceCount;
    draco->faces.resize(cornerCount);
    uint32_t* cornerPoint = draco->faces.data;
    if (draco->seamTables.count == 0) {
        // The points are the vertices.
        for (uint32_t c = 0; c < cornerCount; ++c) {
            cornerPoint[c] = table->cornerVertex.data[c];
            if (cornerPoint[c] >= vertexCount)
                return false;
        }
        draco->pointCount = vertexCount;
        return true;
    }

    DracoConnectivity whole = dracoWhole(table);
    uint32_t pointCount = 0;
    for (uint32_t c = 0; c < cornerCount; ++c)
        cornerPoint[c] = 0;
    for (uint32_t v = 0; v < table->vertexCount; ++v) {
        uint32_t corner = table->vertexCorner.data[v];
        if (corner == kDracoInvalid)
            continue;
        // Around a vertex in the interior, the walk starts at a seam, so that no point is split by where it started.
        uint32_t first = corner;
        if (vertexIsHole.data[v] == 0) {
            for (uint32_t i = 0; i < draco->seamTables.count; ++i) {
                const DracoSeamTable& seams = draco->seamTables.data[i];
                if (seams.vertexOnSeam.data[v] == 0)
                    continue;
                uint32_t vertex = seams.cornerVertex.data[corner];
                bool found = false;
                for (uint32_t at = whole.swingRight(corner); at != corner; at = whole.swingRight(at)) {
                    if (at == kDracoInvalid)
                        return false;
                    if (seams.cornerVertex.data[at] != vertex) {
                        first = at;
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }
        cornerPoint[first] = pointCount++;
        uint32_t previous = first;
        for (uint32_t at = whole.swingRight(first); at != kDracoInvalid && at != first; at = whole.swingRight(at)) {
            bool seam = false;
            for (uint32_t i = 0; i < draco->seamTables.count && !seam; ++i) {
                const DracoSeamTable& seams = draco->seamTables.data[i];
                seam = seams.cornerVertex.data[at] != seams.cornerVertex.data[previous];
            }
            cornerPoint[at] = seam ? pointCount++ : cornerPoint[previous];
            previous = at;
        }
    }
    draco->pointCount = pointCount;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sequences

// A traversal of a mesh's faces that gives its vertices an order.
struct Sequencer {
    const Draco* draco;
    DracoSequence* sequence;
    RawArray<uint8_t> faceVisited;
    RawArray<uint8_t> vertexVisited;
    RawArray<uint32_t> stacks[3];       // the corners to continue from, by priority; depth first uses the first
    RawArray<uint32_t> degree;          // by vertex: the visited faces around it, for the prediction degree

    bool isFaceVisited(uint32_t corner) const { return corner == kDracoInvalid || faceVisited.data[corner / 3] != 0; }

    // Gives `vertex` the next value, which is coded at `corner`.
    void visit(uint32_t vertex, uint32_t corner) {
        vertexVisited.data[vertex] = 1;
        sequence->valueCorner.data[sequence->valueCount] = corner;
        sequence->valuePoint.data[sequence->valueCount] = draco->faces.data[corner];
        sequence->vertexValue.data[vertex] = sequence->valueCount++;
    }
};

// Visits the faces that are reached from `corner` depth first, turning right around a vertex until its fan closes (the
// bitstream's EdgeBreakerTraverser_ProcessCorner).
bool traverseDepthFirst(Sequencer* sequencer, uint32_t corner) {
    const DracoConnectivity& connectivity = sequencer->sequence->connectivity;
    if (sequencer->isFaceVisited(corner))
        return true;
    RawArray<uint32_t>& stack = sequencer->stacks[0];
    stack.clear();
    stack.push(corner);
    uint32_t next = connectivity.vertex(DracoConnectivity::next(corner));
    uint32_t previous = connectivity.vertex(DracoConnectivity::previous(corner));
    if (sequencer->vertexVisited.data[next] == 0)
        sequencer->visit(next, DracoConnectivity::next(corner));
    if (sequencer->vertexVisited.data[previous] == 0)
        sequencer->visit(previous, DracoConnectivity::previous(corner));

    while (stack.count != 0) {
        corner = stack.last();
        if (sequencer->isFaceVisited(corner)) {
            stack.pop();
            continue;
        }
        for (;;) {
            sequencer->faceVisited.data[corner / 3] = 1;
            uint32_t vertex = connectivity.vertex(corner);
            if (sequencer->vertexVisited.data[vertex] == 0) {
                bool onBoundary = connectivity.swingLeft(connectivity.leftMostCorner(vertex)) == kDracoInvalid;
                sequencer->visit(vertex, corner);
                if (!onBoundary) {
                    corner = connectivity.rightCorner(corner);
                    if (corner == kDracoInvalid)
                        return false;
                    continue;
                }
            }
            uint32_t right = connectivity.rightCorner(corner);
            uint32_t left = connectivity.leftCorner(corner);
            bool rightVisited = sequencer->isFaceVisited(right);
            bool leftVisited = sequencer->isFaceVisited(left);
            if (rightVisited && leftVisited) {
                stack.pop();
                break;
            }
            if (rightVisited) {
                corner = left;
            } else if (leftVisited) {
                corner = right;
            } else {
                // Both sides are open: the right is taken first, and the left is kept for later.
                stack.last() = left;
                stack.push(right);
                break;
            }
        }
    }
    return true;
}

// Visits the faces that are reached from `corner` in the order that leaves the most vertices with two visited faces to
// predict them from (the bitstream's PredictionDegree_TraverseFromCorner).
bool traversePredictionDegree(Sequencer* sequencer, uint32_t corner) {
    constexpr uint32_t kPriorities = 3;
    const DracoConnectivity& connectivity = sequencer->sequence->connectivity;
    if (sequencer->degree.count == 0)
        return true;
    uint32_t best = 0;
    auto priorityOf = [&](uint32_t at) {
        uint32_t tip = connectivity.vertex(at);
        if (sequencer->vertexVisited.data[tip] != 0)
            return 0u;
        return ++sequencer->degree.data[tip] > 1 ? 1u : 2u;
    };
    auto pop = [&]() {
        for (uint32_t i = best; i < kPriorities; ++i) {
            if (sequencer->stacks[i].count != 0) {
                uint32_t result = sequencer->stacks[i].last();
                sequencer->stacks[i].pop();
                best = i;
                return result;
            }
        }
        return kDracoInvalid;
    };

    sequencer->stacks[0].push(corner);
    uint32_t corners[3] = {DracoConnectivity::next(corner), DracoConnectivity::previous(corner), corner};
    for (uint32_t at : corners) {
        uint32_t vertex = connectivity.vertex(at);
        if (sequencer->vertexVisited.data[vertex] == 0)
            sequencer->visit(vertex, at);
    }
    while ((corner = pop()) != kDracoInvalid) {
        if (sequencer->isFaceVisited(corner))
            continue;
        for (;;) {
            sequencer->faceVisited.data[corner / 3] = 1;
            uint32_t vertex = connectivity.vertex(corner);
            if (sequencer->vertexVisited.data[vertex] == 0)
                sequencer->visit(vertex, corner);
            uint32_t right = connectivity.rightCorner(corner);
            uint32_t left = connectivity.leftCorner(corner);
            bool rightVisited = sequencer->isFaceVisited(right);
            bool leftVisited = sequencer->isFaceVisited(left);
            if (!leftVisited) {
                uint32_t priority = priorityOf(left);
                if (rightVisited && priority <= best) {
                    corner = left;
                    continue;
                }
                sequencer->stacks[priority].push(left);
                best = priority < best ? priority : best;
            }
            if (!rightVisited) {
                uint32_t priority = priorityOf(right);
                if (priority <= best) {
                    corner = right;
                    continue;
                }
                sequencer->stacks[priority].push(right);
                best = priority < best ? priority : best;
            }
            break;
        }
    }
    return true;
}

// Makes a group's sequence: the order of its values, which the traversal gives, and the value of each point.
bool makeSequence(Draco* draco, DracoSequence* sequence, uint32_t vertexValueCount, uint8_t method) {
    const DracoConnectivity& connectivity = sequence->connectivity;
    uint32_t faceCount = draco->table.faceCount;
    Sequencer sequencer = {};
    sequencer.draco = draco;
    sequencer.sequence = sequence;
    sequencer.faceVisited.resizeZeroed(faceCount);
    sequencer.vertexVisited.resizeZeroed(connectivity.vertexCount());
    sequence->vertexValue.resizeZeroed(vertexValueCount);
    // A vertex is visited once, so the values are as many as the vertices at most.
    sequence->valueCorner.resize(connectivity.vertexCount());
    sequence->valuePoint.resize(connectivity.vertexCount());
    if (method == kTraversalPredictionDegree)
        sequencer.degree.resizeZeroed(connectivity.vertexCount());
    bool ok = true;
    for (uint32_t f = 0; ok && f < faceCount; ++f) {
        if (method == kTraversalPredictionDegree)
            ok = traversePredictionDegree(&sequencer, 3 * f);
        else
            ok = traverseDepthFirst(&sequencer, 3 * f);
    }
    sequencer.faceVisited.reset();
    sequencer.vertexVisited.reset();
    for (RawArray<uint32_t>& stack : sequencer.stacks)
        stack.reset();
    sequencer.degree.reset();
    sequence->valueCorner.count = sequence->valueCount;
    sequence->valuePoint.count = sequence->valueCount;

    // A point with no corner keeps the first value, so a mesh with points has a value.
    if (!ok || (draco->pointCount != 0 && sequence->valueCount == 0))
        return false;
    sequence->pointValue.resizeZeroed(draco->pointCount);
    for (uint32_t c = 0; c < faceCount * 3; ++c) {
        uint32_t value = sequence->vertexValue.data[connectivity.vertex(c)];
        if (value >= draco->pointCount)
            return false;
        sequence->pointValue.data[draco->faces.data[c]] = value;
    }
    return true;
}

// Decodes the connectivity with the state `faces`, which the caller resets.
bool decodeEdgebreaker(Draco* draco, Faces* state) {
    Faces& faces = *state;
    DracoReader* reader = &draco->reader;
    DracoCornerTable* table = &draco->table;
    faces.draco = draco;
    faces.table = table;
    uint32_t encodedVertices = 0;
    uint32_t faceCount = 0;
    uint8_t seamTableCount = 0;
    uint32_t symbolCount = 0;
    uint32_t splitSymbolCount = 0;
    if (!dracoReadU8(reader, &faces.traversal.type) || faces.traversal.type >= TraversalType_Count ||
            !dracoReadVarint32(reader, &encodedVertices) || !dracoReadVarint32(reader, &faceCount)) {
        return false;
    }
    // The checks of the reference decoder: a face has three corners, each at a vertex, and the faces have no more edges
    // than the vertices have pairs.
    if (faceCount > 0x7fffffffu / 3 || encodedVertices > faceCount * 3)
        return false;
    if (uint64_t(encodedVertices) * (uint64_t(encodedVertices) - 1) / 2 < 3 * faceCount / 2)
        return false;
    if (!dracoReadU8(reader, &seamTableCount) || !dracoReadVarint32(reader, &symbolCount))
        return false;
    // Each symbol is a face, and each start face is one more for a component, which has three symbols at least.
    if (faceCount < symbolCount || faceCount > symbolCount + symbolCount / 3)
        return false;
    if (!dracoReadVarint32(reader, &splitSymbolCount) || splitSymbolCount > symbolCount)
        return false;
    // A point is a corner at most, so the limit on faces bounds everything that is allocated.
    if (faceCount > draco->limits.maxFaces)
        return dracoFail(draco, PrStatus_Limit);

    table->faceCount = faceCount;
    table->cornerVertex.resize(faceCount * 3);
    table->opposite.resize(faceCount * 3);
    for (uint32_t c = 0; c < faceCount * 3; ++c) {
        table->cornerVertex.data[c] = kDracoInvalid;
        table->opposite.data[c] = kDracoInvalid;
    }
    faces.maxVertices = encodedVertices + splitSymbolCount;
    faces.vertexIsHole.resize(faces.maxVertices + 3);
    for (uint32_t v = 0; v < faces.vertexIsHole.count; ++v)
        faces.vertexIsHole.data[v] = 1;
    faces.splitCorner.resize(symbolCount);
    for (uint32_t i = 0; i < symbolCount; ++i)
        faces.splitCorner.data[i] = kDracoInvalid;

    uint32_t vertexCount = 0;
    if (!readSplits(&faces, faceCount) || !startTraversal(&faces, seamTableCount, symbolCount) ||
            !decodeFaces(&faces, symbolCount, seamTableCount == 0, &vertexCount) || !checkTable(table)) {
        return false;
    }

    uint32_t cornerCount = faceCount * 3;
    for (uint32_t i = 0; i < seamTableCount; ++i) {
        DracoSeamTable seams = {};
        seams.cornerOnSeam.resizeZeroed(cornerCount);
        seams.vertexOnSeam.resizeZeroed(table->vertexCount);
        seams.cornerVertex.resize(cornerCount);
        for (uint32_t c = 0; c < cornerCount; ++c)
            seams.cornerVertex.data[c] = kDracoInvalid;
        draco->seamTables.push(seams);
    }
    decodeSeams(draco, &faces.traversal);
    for (uint32_t i = 0; i < seamTableCount; ++i) {
        DracoSeamTable& seams = draco->seamTables.data[i];
        seams.opposite.resize(cornerCount);
        for (uint32_t c = 0; c < cornerCount; ++c)
            seams.opposite.data[c] = seams.cornerOnSeam.data[c] != 0 ? kDracoInvalid : table->opposite.data[c];
    }
    for (uint32_t i = 0; i < seamTableCount; ++i) {
        if (!numberSeamVertices(table, &draco->seamTables.data[i]))
            return false;
    }
    if (!assignPoints(draco, faces.vertexIsHole, vertexCount))
        return false;
    if (draco->pointCount > draco->limits.maxPoints)
        return dracoFail(draco, PrStatus_Limit);
    return true;
}

} // namespace

bool dracoDecodeEdgebreaker(Draco* draco) {
    Faces faces = {};
    bool ok = decodeEdgebreaker(draco, &faces);
    resetFaces(&faces);
    return ok;
}

bool dracoReadEdgebreakerSequences(Draco* draco, uint32_t count) {
    DracoReader* reader = &draco->reader;
    bool vertexDataUsed = false;
    bool seamTableUsed[256] = {};
    struct Group {
        int8_t data;
        uint8_t type;
        uint8_t method;
    };
    Group groups[256];
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t data = 0;
        if (!dracoReadU8(reader, &data) || !dracoReadU8(reader, &groups[i].type) ||
                !dracoReadU8(reader, &groups[i].method)) {
            return false;
        }
        groups[i].data = int8_t(data);
        // A group is sequenced with a seam table of its own, or with the data of the vertices, which one group has.
        bool& used = groups[i].data >= 0 ? seamTableUsed[groups[i].data] : vertexDataUsed;
        if ((groups[i].data >= 0 && uint32_t(groups[i].data) >= draco->seamTables.count) || used)
            return false;
        used = true;
        if (groups[i].method > kTraversalPredictionDegree)
            return false;
        // Only the corner table whole is traversed by prediction degree, and only a group with a seam table is cut by
        // it.
        if (groups[i].type != kVertexAttribute && (groups[i].method != kTraversalDepthFirst || groups[i].data < 0))
            return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        DracoSequence initial = {};
        draco->sequences.push(initial);
        DracoSequence* sequence = &draco->sequences.data[i];
        sequence->connectivity = dracoWhole(&draco->table);
        uint32_t vertexValueCount = draco->table.vertexCount;
        if (groups[i].data >= 0) {
            const DracoSeamTable* seams = &draco->seamTables.data[groups[i].data];
            if (groups[i].type != kVertexAttribute)
                sequence->connectivity = dracoCut(seams);
            if (seams->vertexCorner.count > vertexValueCount)
                vertexValueCount = seams->vertexCorner.count;
        }
        if (!makeSequence(draco, sequence, vertexValueCount, groups[i].method))
            return false;
    }
    return true;
}

} // namespace pr_impl
