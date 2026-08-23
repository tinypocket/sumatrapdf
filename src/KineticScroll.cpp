/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "wingui/Anim.h"

#include "KineticScroll.h"

// Tuning. These are the numbers that decide whether a flick feels like paper or
// like a spreadsheet; they were picked to match the touch feel of the platform
// lists this chrome sits next to.

// per-millisecond velocity decay during a fling (~0.94 per 16ms frame)
constexpr float kFlingFriction = 0.996f;
// below this the fling is over: half a pixel per frame is not motion
constexpr float kFlingMinVelocity = 0.03f;
// a drag slower than this on release doesn't fling at all, so a careful
// reposition ends exactly where the finger left it
constexpr float kFlingLaunchVelocity = 0.12f;
// cap so a fast swipe across a short list can't rocket past everything
constexpr float kFlingMaxVelocity = 4.0f;
// weight of the newest sample in the smoothed drag velocity
constexpr float kVelocitySmoothing = 0.35f;
// a pause this long mid-drag means the finger stopped: drop the stale velocity
// so holding still before release doesn't fling
constexpr double kDragIdleMs = 90.0;
// Samples closer together than this are ignored for velocity. Dividing a small
// pixel delta by a near-zero dt yields an absurd speed - one pixel in 0.001ms
// reads as 1000 px/ms - which turned a careful one-pixel reposition into a
// fling. The position still tracks the finger exactly; only the velocity
// estimate waits for a usable time base.
constexpr double kMinSampleMs = 2.0;

static float ClampF(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void ClampPos(KineticScroll& ks) {
    ks.pos = ClampF(ks.pos, (float)ks.minPos, (float)ks.maxPos);
}

void KsSetBounds(KineticScroll& ks, int minPos, int maxPos) {
    if (maxPos < minPos) {
        maxPos = minPos;
    }
    ks.minPos = minPos;
    ks.maxPos = maxPos;
    ClampPos(ks);
    // an eased move whose destination is now out of range would ease to a
    // position the content no longer has
    ks.easeTo = ClampF(ks.easeTo, (float)minPos, (float)maxPos);
}

void KsSetPos(KineticScroll& ks, int pos) {
    ks.pos = (float)pos;
    ClampPos(ks);
    ks.easeDurMs = 0;
    ks.flinging = false;
    ks.velocity = 0.0f;
}

int KsPos(const KineticScroll& ks) {
    // round rather than truncate: truncating biases every partial frame toward
    // zero, which reads as a slight drift while easing
    return (int)(ks.pos + 0.5f);
}

bool KsIsMoving(const KineticScroll& ks) {
    return ks.flinging || ks.easeDurMs > 0;
}

void KsStop(KineticScroll& ks) {
    ks.easeDurMs = 0;
    ks.flinging = false;
    ks.velocity = 0.0f;
    ks.dragging = false;
    ks.dragVelocity = 0.0f;
}

void KsScrollBy(KineticScroll& ks, int delta, int durMs) {
    // a wheel notch during a fling should add to where it's heading, not fight
    // it from the current position
    float base = (ks.easeDurMs > 0) ? ks.easeTo : ks.pos;
    float target = ClampF(base + (float)delta, (float)ks.minPos, (float)ks.maxPos);

    ks.flinging = false;
    ks.velocity = 0.0f;

    if (!AnimEnabled() || durMs <= 0) {
        ks.pos = target;
        ks.easeDurMs = 0;
        return;
    }
    ks.easeFrom = ks.pos;
    ks.easeTo = target;
    ks.easeStartMs = AnimNowMs();
    ks.easeDurMs = durMs;
}

void KsDragBegin(KineticScroll& ks, int pointerPos) {
    ks.dragging = true;
    ks.dragStartPointer = pointerPos;
    ks.dragStartPos = ks.pos;
    ks.lastPointer = pointerPos;
    ks.lastSampleMs = AnimNowMs();
    ks.dragVelocity = 0.0f;
    // the finger owns the position now
    ks.easeDurMs = 0;
    ks.flinging = false;
    ks.velocity = 0.0f;
}

void KsDragUpdate(KineticScroll& ks, int pointerPos) {
    if (!ks.dragging) {
        return;
    }
    // content moves opposite the finger
    ks.pos = ks.dragStartPos - (float)(pointerPos - ks.dragStartPointer);
    ClampPos(ks);

    double now = AnimNowMs();
    double dt = now - ks.lastSampleMs;
    if (dt >= kDragIdleMs) {
        // finger paused: whatever it was doing before is no longer its intent
        ks.dragVelocity = 0.0f;
        ks.lastPointer = pointerPos;
        ks.lastSampleMs = now;
    } else if (dt >= kMinSampleMs) {
        float sample = -(float)(pointerPos - ks.lastPointer) / (float)dt;
        ks.dragVelocity = (sample * kVelocitySmoothing) + (ks.dragVelocity * (1.0f - kVelocitySmoothing));
        ks.lastPointer = pointerPos;
        ks.lastSampleMs = now;
    }
    // below kMinSampleMs: keep the previous anchor so the next usable sample
    // measures across the whole interval instead of losing that movement
}

void KsDragEnd(KineticScroll& ks) {
    if (!ks.dragging) {
        return;
    }
    ks.dragging = false;

    double now = AnimNowMs();
    // released long after the last movement: a hold, not a flick
    if (now - ks.lastSampleMs >= kDragIdleMs) {
        ks.dragVelocity = 0.0f;
    }
    float v = ks.dragVelocity;
    ks.dragVelocity = 0.0f;

    float mag = v < 0 ? -v : v;
    if (!AnimEnabled() || mag < kFlingLaunchVelocity) {
        ks.flinging = false;
        ks.velocity = 0.0f;
        return;
    }
    // already pinned against an edge in the direction of travel: nothing to
    // coast into, so don't start a fling that can't move
    if ((v < 0 && ks.pos <= (float)ks.minPos) || (v > 0 && ks.pos >= (float)ks.maxPos)) {
        ks.flinging = false;
        ks.velocity = 0.0f;
        return;
    }
    ks.velocity = ClampF(v, -kFlingMaxVelocity, kFlingMaxVelocity);
    ks.flinging = true;
    ks.lastTickMs = now;
}

bool KsTick(KineticScroll& ks) {
    double now = AnimNowMs();

    if (ks.easeDurMs > 0) {
        double elapsed = now - ks.easeStartMs;
        float t = (float)(elapsed / (double)ks.easeDurMs);
        if (t >= 1.0f) {
            ks.pos = ks.easeTo;
            ks.easeDurMs = 0;
            ClampPos(ks);
            return false;
        }
        if (t < 0.0f) {
            t = 0.0f;
        }
        float e = AnimEaseOut(t);
        ks.pos = ks.easeFrom + ((ks.easeTo - ks.easeFrom) * e);
        ClampPos(ks);
        return true;
    }

    if (!ks.flinging) {
        return false;
    }

    double dt = now - ks.lastTickMs;
    ks.lastTickMs = now;
    if (dt <= 0.0) {
        return true;
    }
    // a stalled timer (dragged window, breakpoint) must not teleport the list
    if (dt > 100.0) {
        dt = 100.0;
    }

    ks.pos += ks.velocity * (float)dt;

    // powf per frame is fine here and keeps the decay frame-rate independent,
    // so the same flick travels the same distance on a slow frame
    ks.velocity *= powf(kFlingFriction, (float)dt);

    if (ks.pos <= (float)ks.minPos || ks.pos >= (float)ks.maxPos) {
        ClampPos(ks);
        ks.flinging = false;
        ks.velocity = 0.0f;
        return false;
    }
    float mag = ks.velocity < 0 ? -ks.velocity : ks.velocity;
    if (mag < kFlingMinVelocity) {
        ks.flinging = false;
        ks.velocity = 0.0f;
        return false;
    }
    return true;
}
