/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// A scroll offset that moves the way a touch surface should: a wheel notch
// eases to its new position instead of jumping a fixed number of pixels, and
// letting go of a drag keeps the content moving and coasts to a stop.
//
// Shared by every touch-chrome list that scrolls (the Library folder view, the
// thumbnails panel) so they all decelerate identically - a list that scrolls
// differently from the one next to it is what makes a UI feel unfinished.
//
// The owner drives it: hold a KineticScroll per scrollable area, feed it wheel
// deltas and pointer positions, and call KsTick() from a ~16ms timer (AnimTimer
// in wingui/Anim.h) for as long as KsTick keeps returning true. Position stays
// clamped to [minPos, maxPos], so the caller can hand over raw content bounds.
//
// Honours the "animate UI" preference: with animations off, wheel steps and
// drag releases land immediately and nothing ever ticks.

// how long a wheel notch takes to ease into place
constexpr int kKineticWheelMs = 260;

struct KineticScroll {
    // current offset; fractional between frames, rounded by KsPos()
    float pos = 0.0f;

    // --- eased wheel / programmatic movement ---
    float easeFrom = 0.0f;
    float easeTo = 0.0f;
    double easeStartMs = 0.0;
    int easeDurMs = 0;

    // --- fling ---
    // px per millisecond; sign follows increasing pos
    float velocity = 0.0f;
    bool flinging = false;
    double lastTickMs = 0.0;

    // --- drag tracking ---
    bool dragging = false;
    int dragStartPointer = 0;
    float dragStartPos = 0.0f;
    int lastPointer = 0;
    double lastSampleMs = 0.0;
    // smoothed pointer speed, so one erratic sample before release can't throw
    // the fling off
    float dragVelocity = 0.0f;

    int minPos = 0;
    int maxPos = 0;
};

void KsSetBounds(KineticScroll&, int minPos, int maxPos);
void KsSetPos(KineticScroll&, int pos);
int KsPos(const KineticScroll&);
bool KsIsMoving(const KineticScroll&);
// stop dead wherever it is (view change, list rebuilt under it)
void KsStop(KineticScroll&);

// a wheel notch or any programmatic scroll; eases unless animations are off
void KsScrollBy(KineticScroll&, int delta, int durMs = kKineticWheelMs);

// pointerPos is along the scrolled axis, in any consistent coordinate space
void KsDragBegin(KineticScroll&, int pointerPos);
void KsDragUpdate(KineticScroll&, int pointerPos);
// releases into a fling carrying the drag's final speed
void KsDragEnd(KineticScroll&);

// advance to now; true while still moving (keep the timer running)
bool KsTick(KineticScroll&);
