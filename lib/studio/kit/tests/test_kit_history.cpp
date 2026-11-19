// Studio Kit's navigation history: visits, steps, and the places that a new visit drops.

#include "pr/kit_history.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the places of the history, with the current one in brackets.
String placesOf(const PrKitHistory* history) {
    String text;
    for (uint32_t i = 0; i < prKitHistoryCount(history); ++i) {
        if (i != 0)
            text.append(' ');
        bool current = i == prKitHistoryCurrent(history);
        text.append(current ? "[" : "");
        text.append(prKitHistoryPlace(history, i));
        text.append(current ? "]" : "");
    }
    return text;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("kit history: a visit to another place comes after the current one, and a visit to it records nothing") {
    PrKitHistory* history = prKitHistoryCreate(8);
    CHECK(prKitHistoryCount(history) == 0);
    CHECK_FALSE(prKitHistoryCanStep(history, -1));
    CHECK(prKitHistoryStep(history, -1).data == nullptr);
    prKitHistoryVisit(history, "");
    prKitHistoryVisit(history, "a");
    prKitHistoryVisit(history, "a");
    prKitHistoryVisit(history, "b");
    prKitHistoryVisit(history, "a");
    CHECK(placesOf(history) == "a b [a]");
    prKitHistoryDestroy(history);
}

TEST_CASE("kit history: steps walk the places without recording, and a visit after a step back drops the places ahead") {
    PrKitHistory* history = prKitHistoryCreate(8);
    prKitHistoryVisit(history, "a");
    prKitHistoryVisit(history, "b");
    prKitHistoryVisit(history, "c");
    CHECK_FALSE(prKitHistoryCanStep(history, 1));
    CHECK(prKitHistoryCanStep(history, -2));
    CHECK_FALSE(prKitHistoryCanStep(history, -3));
    CHECK(prKitHistoryStep(history, -1) == "b");
    // Arriving at the place that the step went to records nothing.
    prKitHistoryVisit(history, "b");
    CHECK(prKitHistoryStep(history, -1) == "a");
    CHECK(prKitHistoryStep(history, 2) == "c");
    CHECK(prKitHistoryStep(history, -2) == "a");
    CHECK(placesOf(history) == "[a] b c");
    prKitHistoryVisit(history, "d");
    CHECK(placesOf(history) == "a [d]");
    CHECK_FALSE(prKitHistoryCanStep(history, 1));
    prKitHistoryDestroy(history);
}

TEST_CASE("kit history: past its capacity, the oldest place leaves") {
    PrKitHistory* history = prKitHistoryCreate(3);
    const char* places[] = {"a", "b", "c", "d", "e"};
    for (const char* place : places)
        prKitHistoryVisit(history, place);
    CHECK(placesOf(history) == "c d [e]");
    CHECK(prKitHistoryStep(history, -2) == "c");
    prKitHistoryVisit(history, "f");
    prKitHistoryVisit(history, "g");
    prKitHistoryVisit(history, "h");
    CHECK(placesOf(history) == "f g [h]");
    prKitHistoryDestroy(history);
    prKitHistoryDestroy(nullptr);
}
