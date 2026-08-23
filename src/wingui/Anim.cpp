/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Timer.h"
#include "base/Win.h"

#include "wingui/Anim.h"

// Honour "Play animations in Windows" (Settings > Accessibility > Visual
// effects). When it is off every transition snaps straight to its end state.
// Deliberately not cached: this is only asked on a state change (mouse enters a
// button, a press starts), never per frame, and a cache would have to be
// invalidated from a WM_SETTINGCHANGE handler that the animating windows -
// which are child windows - never see.
bool AnimEnabled() {
    BOOL enabled = TRUE;
    if (!SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        return true;
    }
    return enabled != FALSE;
}

// Milliseconds off the monotonic performance counter. GetTickCount64 would do
// for durations this short only if the system timer happened to be at 1 ms;
// at its usual ~15.6 ms a 120 ms animation would run in 8 visible jumps.
double AnimNowMs() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart == 0) {
            freq.QuadPart = 1;
        }
    }
    LARGE_INTEGER now = TimeGet();
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// Cubic ease-out: fast at the start, settling at the end. Nothing overshoots -
// a bouncy curve on chrome this small reads as a glitch, not as polish.
float AnimEaseOut(float t) {
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

COLORREF AnimLerpColor(COLORREF from, COLORREF to, float t) {
    if (t <= 0.0f) {
        return from;
    }
    if (t >= 1.0f) {
        return to;
    }
    u8 r1, g1, b1, r2, g2, b2;
    UnpackColor(from, r1, g1, b1);
    UnpackColor(to, r2, g2, b2);
    int r = (int)((float)r1 + ((float)r2 - (float)r1) * t + 0.5f);
    int g = (int)((float)g1 + ((float)g2 - (float)g1) * t + 0.5f);
    int b = (int)((float)b1 + ((float)b2 - (float)b1) * t + 0.5f);
    return MkColor((u8)limitValue(r, 0, 255), (u8)limitValue(g, 0, 255), (u8)limitValue(b, 0, 255));
}

int AnimLerpInt(int from, int to, float t) {
    if (t <= 0.0f) {
        return from;
    }
    if (t >= 1.0f) {
        return to;
    }
    return from + (int)(((float)(to - from)) * t + 0.5f);
}

float AnimVal::Value() const {
    if (durMs <= 0) {
        return to;
    }
    double elapsed = AnimNowMs() - startMs;
    if (elapsed <= 0.0) {
        return from;
    }
    if (elapsed >= (double)durMs) {
        return to;
    }
    float t = AnimEaseOut((float)(elapsed / (double)durMs));
    return from + (to - from) * t;
}

bool AnimVal::IsAnimating() const {
    if (durMs <= 0) {
        return false;
    }
    return (AnimNowMs() - startMs) < (double)durMs;
}

// Aims at a new target. Already heading there is a no-op, so repeated
// WM_MOUSEMOVE with an unchanged hover state doesn't restart the ease.
void AnimVal::SetTarget(float target, int dur) {
    if (target == to) {
        return;
    }
    if (dur <= 0 || !AnimEnabled()) {
        Set(target);
        return;
    }
    from = Value();
    to = target;
    startMs = AnimNowMs();
    durMs = dur;
}

void AnimVal::Set(float v) {
    from = v;
    to = v;
    startMs = AnimNowMs();
    durMs = 0;
}

void AnimTimer::Init(HWND h, UINT_PTR timerId) {
    hwnd = h;
    id = timerId;
}

void AnimTimer::Start() {
    if (running || !hwnd || !AnimEnabled()) {
        return;
    }
    if (SetTimer(hwnd, id, kAnimTickMs, nullptr)) {
        running = true;
    }
}

// Must leave no timer behind: the owning window may be destroyed at any point
// mid-animation. KillTimer on an already-destroyed HWND is a harmless no-op,
// and the timer dies with the window anyway.
void AnimTimer::Stop() {
    if (!running) {
        return;
    }
    running = false;
    if (hwnd) {
        KillTimer(hwnd, id);
    }
}
