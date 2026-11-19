// Studio Kit's navigation history: the places that a user went to, in order, which Back and Forward walk.
//
// A place is text that the application can go to again, such as an address. The application tells the history each
// place that the user arrives at, however the user arrived, and the history keeps the walk: a place that differs from
// the current one comes after it, and drops the places ahead of it, which a step back left. Stepping does not record,
// so the place that a step goes to is already the current one when the application arrives there.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The places that a user went to, in order, and the one that the user is at.
typedef struct PrKitHistory PrKitHistory;

// Creates an empty history that keeps at most `capacity` places, at least one, dropping the oldest past it, or frees
// one.
PrKitHistory* prKitHistoryCreate(uint32_t capacity);
void prKitHistoryDestroy(PrKitHistory* PR_NULLABLE history);

// Records that the user is at `place`. A place equal to the current one records nothing, and an empty place is none.
void prKitHistoryVisit(PrKitHistory* history, PrStringView place);

// Returns true if a step of `delta` places, back if negative and forward if positive, stays in the history.
bool prKitHistoryCanStep(const PrKitHistory* history, int32_t delta);

// Steps `delta` places, back if negative and forward if positive, and returns the place stepped to, which becomes the
// current one. Returns the null view, and steps nowhere, if the step leaves the history. The view is valid until the
// history changes.
PrStringView prKitHistoryStep(PrKitHistory* history, int32_t delta);

// The places, oldest first, and the index of the current one, which is the count while the history is empty. The views
// are valid until the history changes.
uint32_t prKitHistoryCount(const PrKitHistory* history);
PrStringView prKitHistoryPlace(const PrKitHistory* history, uint32_t index);
uint32_t prKitHistoryCurrent(const PrKitHistory* history);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
