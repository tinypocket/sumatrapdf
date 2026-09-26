/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "wingui/UIModels.h"

#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Theme.h"

//- Splitter

static Kind kindSplitter = "splitter";

static const WCHAR* kResizeOverlayClass = L"SplitterResizeOverlayWnd";

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#endif

#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

static void OnSplitterPaint(Splitter* splitter) {
    HWND hwnd = splitter->hwnd;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    Rect rc = HwndClientRect(hwnd);
    if (!splitter->transparentBackground) {
        AutoDeleteBrush bg = CreateSolidBrush(splitter->bgColor);
        HdcFillRect(hdc, rc, bg);
    }
    Rect line = rc;
    int thickness = splitter->paintThickness;
    if (thickness > 0) {
        if (splitter->type == SplitterType::Vert) {
            line.x = (rc.dx - thickness) / 2;
            line.dx = thickness;
        } else {
            line.y = (rc.dy - thickness) / 2;
            line.dy = thickness;
        }
    }
    AutoDeleteBrush accent = CreateSolidBrush(AccentColor(splitter->bgColor, 30));
    HdcFillRect(hdc, line, accent);
    EndPaint(hwnd, &ps);
}

static WORD dotPatternBmp[8] = {0x00aa, 0x0055, 0x00aa, 0x0055, 0x00aa, 0x0055, 0x00aa, 0x0055};

static void RegisterResizeOverlayClass();

static void HideResizeOverlay(Splitter* splitter) {
    if (!splitter || !splitter->resizeOverlayHwnd) {
        return;
    }
    ShowWindow(splitter->resizeOverlayHwnd, SW_HIDE);
}

static void EnsureResizeOverlay(Splitter* splitter) {
    if (!splitter || splitter->resizeOverlayHwnd) {
        return;
    }
    RegisterResizeOverlayClass();
    HWND parent = GetParent(splitter->hwnd);
    HWND owner = GetAncestor(parent, GA_ROOT);
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    DWORD style = WS_POPUP;
    HWND hwnd = CreateWindowExW(exStyle, kResizeOverlayClass, nullptr, style, 0, 0, 0, 0, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    ReportIf(!hwnd);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)splitter);
    splitter->resizeOverlayHwnd = hwnd;
}

static void UpdateResizeOverlay(Splitter* splitter, Point pos) {
    if (!splitter) {
        return;
    }
    EnsureResizeOverlay(splitter);
    if (!splitter->resizeOverlayHwnd) {
        return;
    }

    HWND parent = GetParent(splitter->hwnd);
    Point origin = HwndClientToScreen(parent, Point());
    Rect splitterRc = HwndWindowRect(splitter->hwnd);

    int x = 0, y = 0, dx = 0, dy = 0;
    bool isVert = splitter->type != SplitterType::Horiz;
    if (isVert) {
        x = origin.x + pos.x - 2;
        y = splitterRc.y;
        dx = 4;
        dy = splitterRc.dy;
    } else {
        x = splitterRc.x;
        y = origin.y + pos.y - 2;
        dx = splitterRc.dx;
        dy = 4;
    }

    SetWindowPos(splitter->resizeOverlayHwnd, HWND_TOP, x, y, dx, dy, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    HwndInvalidate(splitter->resizeOverlayHwnd, true);
}

static LRESULT CALLBACK ResizeOverlayWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        auto* splitter = (Splitter*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (splitter && splitter->brush) {
            SetBrushOrgEx(hdc, 0, 0, nullptr);
            HdcFillRect(hdc, ToRect(ps.rcPaint), splitter->brush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void RegisterResizeOverlayClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEX wcex{};
    FillWndClassEx(wcex, kResizeOverlayClass, ResizeOverlayWndProc);
    RegisterClassExW(&wcex);
    registered = true;
}

Splitter::Splitter() {
    kind = kindSplitter;
}

Splitter::~Splitter() {
    if (resizeOverlayHwnd) {
        DestroyWindow(resizeOverlayHwnd);
        resizeOverlayHwnd = nullptr;
    }
    DeleteObject(brush);
    DeleteObject(bmp);
}

HWND Splitter::Create(const CreateArgs& args) {
    ReportIf(!args.parent);

    isLive = args.isLive;
    type = args.type;
    paintThickness = args.paintThickness;
    transparentBackground = args.transparentBackground;
    dragThreshold = args.dragThreshold;
    auto bgCol = args.backgroundColor;
    if (bgCol == kColorUnset) {
        bgCol = GetSysColor(COLOR_BTNFACE);
    }
    SetColors(kColorUnset, bgCol);

    bmp = CreateBitmap(8, 8, 1, 1, dotPatternBmp);
    ReportIf(!bmp);
    brush = CreatePatternBrush(bmp);
    ReportIf(!brush);

    if (!isLive) {
        RegisterResizeOverlayClass();
    }

    CreateCustomArgs cargs;
    // cargs.className = L"SplitterWndClass";
    cargs.parent = args.parent;
    cargs.style = WS_CHILDWINDOW;
    cargs.exStyle = transparentBackground ? WS_EX_TRANSPARENT : 0;
    CreateCustom(cargs);

    return hwnd;
}

static int SplitterAxisPos(Splitter* splitter, Point pos) {
    return splitter->type == SplitterType::Vert ? pos.x : pos.y;
}

static Splitter::MoveEvent SplitterMoveEvent(Splitter* splitter, bool finishedDragging, Point pos) {
    int delta = SplitterAxisPos(splitter, pos) - SplitterAxisPos(splitter, splitter->dragStartPos);
    Splitter::MoveEvent arg;
    arg.w = splitter;
    arg.finishedDragging = finishedDragging;
    arg.splitterPos = splitter->dragStartSplitterPos + delta;
    return arg;
}

static void BeginSplitterDrag(Splitter* splitter, Point parentPos) {
    HWND hwnd = splitter->hwnd;
    HWND parent = GetParent(hwnd);
    splitter->dragStartPos = parentPos;
    Point parentOrigin = HwndClientToScreen(parent, Point());
    Rect splitterRc = HwndWindowRect(hwnd);
    int size = splitter->type == SplitterType::Vert ? splitterRc.dx : splitterRc.dy;
    int thickness = splitter->paintThickness > 0 ? splitter->paintThickness : size;
    int windowPos =
        splitter->type == SplitterType::Vert ? splitterRc.x - parentOrigin.x : splitterRc.y - parentOrigin.y;
    splitter->dragStartSplitterPos = windowPos + (size - thickness) / 2;
    splitter->isDragging = splitter->dragThreshold <= 0;
    if (!splitter->isLive && splitter->isDragging) {
        UpdateResizeOverlay(splitter, parentPos);
    }
}

static bool UpdateSplitterDrag(Splitter* splitter, Point parentPos) {
    int delta = SplitterAxisPos(splitter, parentPos) - SplitterAxisPos(splitter, splitter->dragStartPos);
    if (!splitter->isDragging && abs(delta) >= splitter->dragThreshold) {
        splitter->isDragging = true;
    }
    if (!splitter->isDragging) {
        return true;
    }
    Splitter::MoveEvent arg = SplitterMoveEvent(splitter, false, parentPos);
    splitter->onMove.Call(&arg);
    if (arg.resizeAllowed && !splitter->isLive) {
        UpdateResizeOverlay(splitter, parentPos);
    }
    return arg.resizeAllowed;
}

static void EndSplitterDrag(Splitter* splitter, Point parentPos) {
    bool wasDragging = splitter->isDragging;
    if (!splitter->isLive) {
        HideResizeOverlay(splitter);
    }
    if (wasDragging) {
        Splitter::MoveEvent arg = SplitterMoveEvent(splitter, true, parentPos);
        splitter->onMove.Call(&arg);
    }
    splitter->isDragging = false;
    HwndScheduleRepaint(splitter->hwnd);
}

static void CancelSplitterDrag(Splitter* splitter) {
    if (!splitter->isLive) {
        HideResizeOverlay(splitter);
    }
    splitter->isDragging = false;
    splitter->activePointerId = 0;
    HwndScheduleRepaint(splitter->hwnd);
}

static Point PointerParentPos(Splitter* splitter, LPARAM lparam) {
    Point screenPos{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    return HwndScreenToClient(GetParent(splitter->hwnd), screenPos);
}

LRESULT Splitter::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (WM_ERASEBKGND == msg) {
        // TODO: should this be FALSE?
        return TRUE;
    }

    if (WM_LBUTTONDOWN == msg) {
        SetCapture(hwnd);
        Point mousePos{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        mousePos = HwndMapWindowPoint(hwnd, GetParent(hwnd), mousePos);
        BeginSplitterDrag(this, mousePos);
        return 1;
    }

    if (WM_LBUTTONUP == msg) {
        Point mousePos{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        mousePos = HwndMapWindowPoint(hwnd, GetParent(hwnd), mousePos);
        EndSplitterDrag(this, mousePos);
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        return 0;
    }

    if (WM_CAPTURECHANGED == msg) {
        if ((HWND)lparam != hwnd) {
            CancelSplitterDrag(this);
        }
        return 0;
    }

    // Touch contacts use WM_POINTER messages. Relying on their optional mouse
    // promotion is fragile: a gesture that begins just outside the splitter can
    // suppress the later promoted button-up and leave a sibling with capture.
    // Consume the complete pointer sequence and use its implicit pointer capture.
    UINT32 pointerId = LOWORD(wparam);
    if (WM_POINTERDOWN == msg) {
        if (activePointerId == 0) {
            // A touch promoted to a mouse-down in an adjacent document/tree can
            // retain mouse capture when gesture recognition suppresses its
            // matching mouse-up. A new direct pointer contact on the splitter
            // supersedes that stale interaction.
            if (GetCapture() && GetCapture() != hwnd) {
                ReleaseCapture();
            }
            activePointerId = pointerId;
            BeginSplitterDrag(this, PointerParentPos(this, lparam));
        }
        return 0;
    }
    if (WM_POINTERUPDATE == msg) {
        if (pointerId == activePointerId) {
            UpdateSplitterDrag(this, PointerParentPos(this, lparam));
        }
        return 0;
    }
    if (WM_POINTERUP == msg) {
        if (pointerId == activePointerId) {
            EndSplitterDrag(this, PointerParentPos(this, lparam));
            activePointerId = 0;
        }
        return 0;
    }
    if (WM_POINTERCAPTURECHANGED == msg) {
        if (pointerId == activePointerId) {
            CancelSplitterDrag(this);
        }
        return 0;
    }

    if (WM_CANCELMODE == msg) {
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        CancelSplitterDrag(this);
        return 0;
    }

    if (WM_MOUSEMOVE == msg) {
        LPWSTR curId = IDC_SIZENS;
        if (SplitterType::Vert == type) {
            curId = IDC_SIZEWE;
        }
        if (!mouseTracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            mouseTracking = true;
        }
        if (!isMouseOver) {
            isMouseOver = true;
            HwndScheduleRepaint(hwnd);
        }
        if (hwnd == GetCapture()) {
            Point mousePos{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            mousePos = HwndMapWindowPoint(hwnd, GetParent(hwnd), mousePos);
            if (!UpdateSplitterDrag(this, mousePos)) {
                curId = IDC_NO;
            }
        }
        SetCursorCached(curId);
        return 0;
    }

    if (WM_MOUSELEAVE == msg) {
        mouseTracking = false;
        if (isMouseOver) {
            isMouseOver = false;
            HwndScheduleRepaint(hwnd);
        }
        return 0;
    }

    if (WM_PAINT == msg) {
        OnSplitterPaint(this);
        return 0;
    }

    return WndProcDefault(hwnd, msg, wparam, lparam);
}
