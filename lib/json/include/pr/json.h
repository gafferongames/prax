// JSON: text read into a document of values, and text written from values.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The values read from one JSON text. A document holds its values in document order, so the root is value 0. An array
// or object is followed by its contents, and an object holds each member as a key string followed by the member's
// value. Strings are unescaped into the document, which does not refer to the text it read.
typedef struct PrJson PrJson;

// A value of a document, by its position in document order.
typedef uint32_t PrJsonValueIndex;

// The index that a lookup returns when there is no such value.
PR_CONSTANT PrJsonValueIndex kPrJsonMissing = 0xffffffffu;

// How deep arrays and objects may nest.
enum { kPrJsonMaxDepth = 1024 };

// The kinds of JSON value.
enum {
    PrJsonKind_Null,
    PrJsonKind_False,
    PrJsonKind_True,
    PrJsonKind_Number,
    PrJsonKind_String,
    PrJsonKind_Array,
    PrJsonKind_Object,
};
typedef uint8_t PrJsonKind;

// Creates an empty document, or frees one.
PrJson* prJsonCreate(void);
void prJsonDestroy(PrJson* PR_NULLABLE document);

// What a parse records beside the values.
enum {
    // The span of each value's text (prJsonSpan), for a tool that links values to their source, such as an editor. It
    // costs 16 bytes a value, and limits the text to 4 GiB.
    PrJsonParseFlag_Spans = 1u << 0,
};
typedef uint8_t PrJsonParseFlags;

// Reads the JSON in `text` into the document, replacing what it held and keeping its storage. `text` is UTF-8 with an
// optional byte order mark. An escaped lone surrogate reads as U+FFFD. Returns false if the text is not JSON or nests
// deeper than kPrJsonMaxDepth, or is longer than 4 GiB with PrJsonParseFlag_Spans. On failure, the document is empty
// and `*errorAt` is the byte offset where reading stopped.
bool prJsonParse(PrJson* document, PrStringView text, PrJsonParseFlags flags, size_t* PR_NULLABLE errorAt);

// Returns the number of values in the document, at any depth. An empty document has none.
uint32_t prJsonValueCount(const PrJson* document);

// Returns the kind of the value at `index`, or PrJsonKind_Null if `index` is past the end of the document.
PrJsonKind prJsonKind(const PrJson* document, PrJsonValueIndex index);

// Returns the index of the value after the one at `index`, skipping everything that value contains.
PrJsonValueIndex prJsonNext(const PrJson* document, PrJsonValueIndex index);

// Returns the bytes of the string at `index`, valid until the document changes, or the null view if that value is not a
// string. An empty string is the empty view.
PrStringView prJsonString(const PrJson* document, PrJsonValueIndex index);

// Returns the number at `index`, or `fallback` if that value is not a number. True and false read as 1 and 0.
double prJsonNumber(const PrJson* document, PrJsonValueIndex index, double fallback);

// Returns the number of elements of the array or members of the object at `index`, or zero for any other value.
uint32_t prJsonCount(const PrJson* document, PrJsonValueIndex index);

// Returns the index of the value of the member named `key` in the object at `object`, or of the last such member if
// there are several. Returns kPrJsonMissing if there is none, or if the value at `object` is not an object.
PrJsonValueIndex prJsonMember(const PrJson* document, PrJsonValueIndex object, PrStringView key);

// Returns the index of element `at` of the array at `array`. Returns kPrJsonMissing if `at` is past the end, or if the
// value at `array` is not an array.
PrJsonValueIndex prJsonElement(const PrJson* document, PrJsonValueIndex array, uint32_t at);

// Return the index of the first element or member key of the array or object at `container`, and of the one after
// `index`. They return kPrJsonMissing if there is none. The value of a member is at the index after its key.
//
//     for (PrJsonValueIndex i = prJsonFirst(d, a); i != kPrJsonMissing; i = prJsonAfter(d, a, i))
PrJsonValueIndex prJsonFirst(const PrJson* document, PrJsonValueIndex container);
PrJsonValueIndex prJsonAfter(const PrJson* document, PrJsonValueIndex container, PrJsonValueIndex index);

// Finds the value that the JSON Pointer `pointer` names (RFC 6901), from the root, and writes its index to `*out`: the
// empty pointer names the root, and each `/` and the reference token after it, with `~1` for `/` and `~0` for `~`,
// names a member of an object or an element of an array by its decimal index. Returns Syntax for a pointer that is not
// empty and does not begin with `/`, for a `~` that no `0` or `1` follows, and for an array index with a leading zero,
// and NotFound for a pointer that names no value, as `-`, the element past the end, does. On failure, `*out` is
// kPrJsonMissing.
PrStatus prJsonValueFromPointer(const PrJson* document, PrStringView pointer, PrJsonValueIndex* out);

// ---------------------------------------------------------------------------------------------------------------------
// Source

// The bytes of the text that a value was read from, as offsets into the parsed text, which counts a byte order mark. A
// string's span includes its quotes, and an array's or object's includes its brackets. A member's key is a value of its
// own, and the member's value has a span of its own after it.
typedef struct PrJsonSpan {
    uint32_t begin;
    uint32_t end;               // one past the last byte
} PrJsonSpan;

// Returns true if the last parse recorded spans (PrJsonParseFlag_Spans).
bool prJsonHasSpans(const PrJson* document);

// Returns the span of the value at `index`, which must be a value of a document that has spans.
PrJsonSpan prJsonSpan(const PrJson* document, PrJsonValueIndex index);

// Returns the innermost value whose span holds the byte at `offset`, or kPrJsonMissing if no value's span holds it, as
// for the space around the root. A key is a value, so an offset in a member's key returns the key. The document must
// have spans. The lookup costs a binary search over the values and a step up for each level of nesting.
PrJsonValueIndex prJsonValueFromOffset(const PrJson* document, uint32_t offset);

// Returns the JSON Pointer (RFC 6901) that names the value at `index`, which prJsonValueFromPointer reads back to it
// unless a later member of an object has the same name. The pointer of a member's key names the member's value. The
// root's pointer is empty. The document must have spans. The cost is one step for each level of nesting. The caller
// releases the pointer with prRawStringReset.
PrRawString prJsonPointer(const PrJson* document, PrJsonValueIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Writers

// JSON text written one value at a time, in document order. A writer places the separators itself: the caller appends
// each value, and each key before an object's member, and ends each array and object that it begins. The text is
// compact, with no space between tokens.
typedef struct PrJsonWriter PrJsonWriter;

// Creates an empty writer, or frees one.
PrJsonWriter* prJsonWriterCreate(void);
void prJsonWriterDestroy(PrJsonWriter* PR_NULLABLE writer);

// Empties the writer's text, keeping its storage, so that it writes a new document.
void prJsonWriterClear(PrJsonWriter* writer);

// Begin or end an array or an object. An end must match the innermost container that is open.
void prJsonWriterBeginArray(PrJsonWriter* writer);
void prJsonWriterEndArray(PrJsonWriter* writer);
void prJsonWriterBeginObject(PrJsonWriter* writer);
void prJsonWriterEndObject(PrJsonWriter* writer);

// Appends the key of the next member of the innermost open object, which must be an object that is not waiting for a
// member's value.
void prJsonWriterKey(PrJsonWriter* writer, PrStringView key);

// Append a value: an array's next element, the value of the member whose key was appended last, or the document's root.
// A string escapes the quote, the backslash and the control characters, and replaces ill-formed UTF-8 with U+FFFD. A
// number is written in its shortest form that reads back to the same double, and a NaN or an infinity, which JSON
// cannot represent, as null.
void prJsonWriterString(PrJsonWriter* writer, PrStringView value);
void prJsonWriterNumber(PrJsonWriter* writer, double value);
void prJsonWriterBool(PrJsonWriter* writer, bool value);
void prJsonWriterNull(PrJsonWriter* writer);

// Appends a copy of the value at `index` in `document`, with everything that it contains.
void prJsonWriterValue(PrJsonWriter* writer, const PrJson* document, PrJsonValueIndex index);

// Returns the text written so far, valid until the writer changes. The text is a complete document once the root is
// written and every container is ended.
PrStringView prJsonWriterText(const PrJsonWriter* writer);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
