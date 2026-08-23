/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Micro-animations for owner-drawn chrome: short ease-out transitions of
// colors and small offsets. A window owns one AnimTimer plus one AnimVal per
// animating quantity; the timer only ticks while something is actually moving.

// press feedback has to feel immediate; hover and the rail's active marker can
// take a touch longer
constexpr int kAnimPressMs = 120;
constexpr int kAnimPressReleaseMs = 160;
constexpr int kAnimHoverMs = 140;
constexpr int kAnimMarkerMs = 160;
// ~60 fps
constexpr uint kAnimTickMs = 16;

bool AnimEnabled();
double AnimNowMs();
float AnimEaseOut(float t);
COLORREF AnimLerpColor(COLORREF from, COLORREF to, float t);
int AnimLerpInt(int from, int to, float t);

// One scalar easing from `from` to `to`. Retargeting mid-flight eases from
// wherever it currently is, so it never jumps. A duration of 0 means "settled".
struct AnimVal {
    float from = 0.0f;
    float to = 0.0f;
    double startMs = 0.0;
    int durMs = 0;

    float Value() const;
    bool IsAnimating() const;
    void SetTarget(float target, int durMs);
    void Set(float v);
};

// Owns the WM_TIMER that drives one window's animations. Start() is idempotent;
// the owner stops it as soon as no AnimVal is animating.
struct AnimTimer {
    HWND hwnd = nullptr;
    UINT_PTR id = 0;
    bool running = false;

    void Init(HWND, UINT_PTR);
    void Start();
    void Stop();
};
