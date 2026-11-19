// Studio Kit's navigation history: a ring of places, and the current one.

#include "pr/kit_history.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"

namespace pr_impl {

namespace {

// The places, oldest first, in a ring of `capacity` slots that begins at `start`, so that dropping the oldest place
// moves nothing.
struct KitHistory {
    RawArray<PrRawString> slots;
    uint32_t start;
    uint32_t count;
    uint32_t current;               // the count while the history is empty
};

// Returns the slot of the place at `index`, oldest first.
PrRawString& slotOf(const KitHistory* history, uint32_t index) {
    return history->slots.data[(history->start + index) % history->slots.count];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrKitHistory : KitHistory {};

PrKitHistory* prKitHistoryCreate(uint32_t capacity) {
    PrKitHistory* history = allocZeroed<PrKitHistory>();
    history->slots.resizeZeroed(capacity > 0 ? capacity : 1);
    return history;
}

void prKitHistoryDestroy(PrKitHistory* PR_NULLABLE history) {
    if (history == nullptr)
        return;
    for (uint32_t i = 0; i < history->slots.count; ++i)
        prRawStringReset(&history->slots.data[i]);
    history->slots.reset();
    prMemoryFree(history);
}

void prKitHistoryVisit(PrKitHistory* history, PrStringView place) {
    if (place.size == 0)
        return;
    if (history->current < history->count && prRawStringView(&slotOf(history, history->current)) == place)
        return;
    // The places ahead of the current one are those that a step back left, which a new place replaces.
    uint32_t kept = history->current < history->count ? history->current + 1 : 0;
    for (uint32_t i = kept; i < history->count; ++i)
        prRawStringReset(&slotOf(history, i));
    history->count = kept;
    if (history->count == history->slots.count) {
        prRawStringReset(&slotOf(history, 0));
        history->start = (history->start + 1) % history->slots.count;
        --history->count;
    }
    prRawStringReplace(&slotOf(history, history->count), place);
    history->current = history->count;
    ++history->count;
}

bool prKitHistoryCanStep(const PrKitHistory* history, int32_t delta) {
    if (history->count == 0)
        return false;
    int64_t to = int64_t(history->current) + delta;
    return to >= 0 && to < int64_t(history->count);
}

PrStringView prKitHistoryStep(PrKitHistory* history, int32_t delta) {
    if (!prKitHistoryCanStep(history, delta))
        return PrStringView{};
    history->current = uint32_t(int64_t(history->current) + delta);
    return prRawStringView(&slotOf(history, history->current));
}

uint32_t prKitHistoryCount(const PrKitHistory* history) {
    return history->count;
}

PrStringView prKitHistoryPlace(const PrKitHistory* history, uint32_t index) {
    PR_ASSERT(index < history->count);
    return prRawStringView(&slotOf(history, index));
}

uint32_t prKitHistoryCurrent(const PrKitHistory* history) {
    return history->current;
}
