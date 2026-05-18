// Unit tests for BadWindowRegistry — pure-logic dispatch of BadWindow
// notifications keyed by Display*. No live X server required; we use
// synthetic Display* values as opaque keys.

#include <gtest/gtest.h>
#include "capture/BadWindowRegistry.h"
#include <X11/Xlib.h>

namespace {

Display* fakeDisplay(uintptr_t key) {
    return reinterpret_cast<Display*>(key);
}

} // namespace

TEST(BadWindowRegistry, ConsumeReturnsFalseForUnregistered) {
    auto* unknown = fakeDisplay(0xdead0001);
    EXPECT_FALSE(BadWindowRegistry::consume(unknown));
}

TEST(BadWindowRegistry, NoteOnUnregisteredDoesNotPanic) {
    auto* ghost = fakeDisplay(0xdead0002);
    BadWindowRegistry::note(ghost);   // silently dropped
    EXPECT_FALSE(BadWindowRegistry::consume(ghost));
}

TEST(BadWindowRegistry, NoteAndConsumeRoundTripsOnSameDisplay) {
    auto* d = fakeDisplay(0xa001);
    BadWindowRegistry::add(d);
    EXPECT_FALSE(BadWindowRegistry::consume(d));     // initially clean
    BadWindowRegistry::note(d);
    EXPECT_TRUE(BadWindowRegistry::consume(d));      // flag observed
    EXPECT_FALSE(BadWindowRegistry::consume(d));     // flag cleared on consume
    BadWindowRegistry::remove(d);
}

TEST(BadWindowRegistry, NoteOnOneDisplayDoesNotBleedIntoAnother) {
    auto* a = fakeDisplay(0xa002);
    auto* b = fakeDisplay(0xb002);
    BadWindowRegistry::add(a);
    BadWindowRegistry::add(b);

    BadWindowRegistry::note(a);
    EXPECT_FALSE(BadWindowRegistry::consume(b)) << "b must not see a's flag";
    EXPECT_TRUE(BadWindowRegistry::consume(a));

    BadWindowRegistry::note(b);
    EXPECT_FALSE(BadWindowRegistry::consume(a)) << "a must not see b's flag";
    EXPECT_TRUE(BadWindowRegistry::consume(b));

    BadWindowRegistry::remove(a);
    BadWindowRegistry::remove(b);
}

TEST(BadWindowRegistry, RemoveWipesFlagSoReusedDisplayStartsClean) {
    auto* d = fakeDisplay(0xc003);
    BadWindowRegistry::add(d);
    BadWindowRegistry::note(d);
    BadWindowRegistry::remove(d);

    // Same Display* pointer value re-used by a hypothetical new session.
    BadWindowRegistry::add(d);
    EXPECT_FALSE(BadWindowRegistry::consume(d))
        << "stale flag from prior registration must not leak through remove/add";
    BadWindowRegistry::remove(d);
}
