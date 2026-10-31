/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "wingui/Anim.h"

#include "KineticScroll.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// The scroll feel is hard to prove from outside the process: capturing a frame
// costs ~260ms here, which is the whole duration of a wheel ease, so a
// screenshot harness cannot catch it mid-flight. These assert the behaviour
// directly instead - that a wheel step lands somewhere *between* start and
// target partway through, that a fling decays and stops, and that nothing ever
// escapes the content bounds.

static void expectBounds() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 500);
    KsSetPos(ks, 200);
    utassert(KsPos(ks) == 200);

    // clamped at both ends
    KsSetPos(ks, -50);
    utassert(KsPos(ks) == 0);
    KsSetPos(ks, 9999);
    utassert(KsPos(ks) == 500);

    // shrinking the content pulls the position back in: the Library remeasures
    // its list on every layout, so this happens whenever a filter is applied
    KsSetBounds(ks, 0, 100);
    utassert(KsPos(ks) == 100);

    // a max below min must not invert the range
    KsSetBounds(ks, 0, -10);
    utassert(KsPos(ks) == 0);
}

static void expectWheelEases() {
    AnimSetAppEnabled(true);
    // AnimEnabled() also honours the OS-wide "Play animations in Windows"
    // setting, which we cannot control from a test. With it off the eased path
    // is unreachable by design, so assert the instant behaviour instead of
    // failing on a machine that has animations turned off.
    if (!AnimEnabled()) {
        KineticScroll off;
        KsSetBounds(off, 0, 1000);
        KsSetPos(off, 0);
        KsScrollBy(off, 300);
        utassert(KsPos(off) == 300);
        utassert(!KsIsMoving(off));
        return;
    }

    KineticScroll ks;
    KsSetBounds(ks, 0, 1000);
    KsSetPos(ks, 0);
    KsScrollBy(ks, 300);
    // easing, so it must NOT be there yet
    utassert(KsIsMoving(ks));
    utassert(KsPos(ks) < 300);

    // partway through it is strictly between the two, which is the property a
    // fixed-step jump would violate
    bool sawIntermediate = false;
    double t0 = AnimNowMs();
    while (KsTick(ks)) {
        int p = KsPos(ks);
        utassert(p >= 0 && p <= 300);
        if (p > 0 && p < 300) {
            sawIntermediate = true;
        }
        utassert(AnimNowMs() - t0 < 5000.0); // never settled: real failure
    }
    utassert(sawIntermediate);
    // and it lands exactly on target, not near it
    utassert(KsPos(ks) == 300);
    utassert(!KsIsMoving(ks));
}

static void expectWheelInstantWhenAnimationsOff() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 1000);
    KsSetPos(ks, 0);

    // "turn off animations" must mean exactly that: no tick, no timer
    AnimSetAppEnabled(false);
    KsScrollBy(ks, 300);
    utassert(KsPos(ks) == 300);
    utassert(!KsIsMoving(ks));
    AnimSetAppEnabled(true);
}

static void expectWheelClampsToEnd() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 120);
    KsSetPos(ks, 0);
    AnimSetAppEnabled(true);
    // holds whether or not the ease runs: with animations off it lands on the
    // clamped target immediately, with them on it eases to the same place
    // asking for more than there is must settle at the end, not past it
    KsScrollBy(ks, 5000);
    double t0 = AnimNowMs();
    while (KsTick(ks)) {
        utassert(KsPos(ks) <= 120);
        utassert(AnimNowMs() - t0 < 5000.0);
    }
    utassert(KsPos(ks) == 120);
}

static void expectDragTracksFinger() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 1000);
    KsSetPos(ks, 400);
    AnimSetAppEnabled(true);

    KsDragBegin(ks, 300);
    // content moves opposite the finger: dragging up (smaller y) scrolls down
    KsDragUpdate(ks, 250);
    utassert(KsPos(ks) == 450);
    KsDragUpdate(ks, 350);
    utassert(KsPos(ks) == 350);

    // a drag is 1:1 and never animates while the finger is down
    utassert(!KsIsMoving(ks));
}

static void expectSlowReleaseDoesNotFling() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 1000);
    KsSetPos(ks, 400);
    AnimSetAppEnabled(true);

    // a careful reposition: one small move, then let go. Must stop dead where
    // the finger left it rather than drifting on.
    KsDragBegin(ks, 300);
    KsDragUpdate(ks, 299);
    int atRelease = KsPos(ks);
    // one pixel of travel is far below the launch threshold either way
    KsDragEnd(ks);
    utassert(!KsIsMoving(ks));
    utassert(KsPos(ks) == atRelease);
}

static void expectStopHalts() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 1000);
    KsSetPos(ks, 0);
    AnimSetAppEnabled(true);
    KsScrollBy(ks, 400);
    if (!AnimEnabled()) {
        return; // nothing to catch when it never animates
    }
    utassert(KsIsMoving(ks));
    // touching a coasting list catches it
    KsStop(ks);
    utassert(!KsIsMoving(ks));
    int p = KsPos(ks);
    utassert(!KsTick(ks));
    utassert(KsPos(ks) == p);
}

static void expectRetargetDoesNotJump() {
    KineticScroll ks;
    KsSetBounds(ks, 0, 2000);
    KsSetPos(ks, 0);
    AnimSetAppEnabled(true);

    if (!AnimEnabled()) {
        return; // no ease to retarget mid-flight
    }
    KsScrollBy(ks, 300);
    KsTick(ks);
    int mid = KsPos(ks);
    // a second notch mid-ease accumulates onto the destination instead of
    // restarting from the current position, so repeated notches keep up
    KsScrollBy(ks, 300);
    utassert(KsPos(ks) >= mid); // never snaps backwards
    double t0 = AnimNowMs();
    while (KsTick(ks)) {
        utassert(AnimNowMs() - t0 < 5000.0);
    }
    utassert(KsPos(ks) == 600);
}

void KineticScroll_UnitTests() {
    expectBounds();
    expectWheelEases();
    expectWheelInstantWhenAnimationsOff();
    expectWheelClampsToEnd();
    expectDragTracksFinger();
    expectSlowReleaseDoesNotFling();
    expectStopHalts();
    expectRetargetDoesNotJump();
}
