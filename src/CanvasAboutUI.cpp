/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Timer.h"
#include "base/Win.h"

#include "wingui/FrameRateWnd.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "Commands.h"
#include "Canvas.h"
#include "Menu.h"
#include "HomePage.h"
#include "Theme.h"
#include "FileHistory.h"
#include "AppSettings.h"
#include "Rail.h"
#include "TopBar.h"

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#endif

#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

static void OnPaintAbout(MainWindow* win) {
    auto t = TimeGet();
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(win->hwndCanvas, &ps);
    if (!win->buffer) {
        EndPaint(win->hwndCanvas, &ps);
        return;
    }
    HDC bufDC = win->buffer->GetDC();
    GlobalPrefs* prefs = gGlobalPrefs;
    bool hasPerms = HasPermission(Perm::SavePreferences | Perm::DiskAccess);
    bool drawHome = hasPerms && prefs->rememberOpenedFiles && prefs->showStartPage;
    if (drawHome) {
        DrawHomePage(win, bufDC);
    } else {
        HomePageDestroySearch(win);
        DrawAboutPage(win, bufDC);
    }
    win->buffer->Flush(hdc);
    DrawCanvasKeyboardFocusIfNeeded(win, hdc);

    EndPaint(win->hwndCanvas, &ps);
    if (gShowFrameRate) {
        win->frameRateWnd->ShowFrameRateDur(TimeSinceInMs(t));
    }
}

static void OnMouseLeftButtonDownAbout(MainWindow* win, int x, int y, WPARAM /*key*/) {
    // lf("Left button clicked on %d %d", x, y);

    // remember a link under so that on mouse up we only activate
    // link if mouse up is on the same link as mouse down
    str::ReplaceWithCopy(&win->urlOnLastButtonDown, GetStaticLinkAtTemp(win->staticLinks, x, y, nullptr));
}

static bool IsLink(Str url) {
    if (str::StartsWithI(url, StrL("http:"))) {
        return true;
    }
    if (str::StartsWithI(url, StrL("https:"))) {
        return true;
    }
    if (str::StartsWithI(url, StrL("mailto:"))) {
        return true;
    }
    return false;
}

static void OnMouseMoveAbout(MainWindow* win, HWND hwnd, int x, int y) {
    HomePageUpdateCloseButton(win, x, y);
    // file entries: update active thumbnail and show tip at that entry
    HomePageOnHover(win, x, y);
    TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT)};
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
}

static void OnMouseLeftButtonUpAbout(MainWindow* win, int x, int y, WPARAM /*key*/) {
    // a click on the thumbnail's ✕ close button removes the file instead of
    // opening it
    if (HomePageOnCloseButtonClick(win, x, y)) {
        str::FreePtr(&win->urlOnLastButtonDown);
        return;
    }
    TempStr url = GetStaticLinkAtTemp(win->staticLinks, x, y, nullptr);
    bool clickedURL = url && str::Eq(url, win->urlOnLastButtonDown);
    str::FreePtr(&win->urlOnLastButtonDown);
    if (!clickedURL) {
        CloseTouchLibraryTransientUi(win);
        return;
    }
    if (str::Eq(url, kLinkOpenFile)) {
        HwndSendCommand(win->hwndFrame, CmdOpenFile);
    } else if (str::Eq(url, kLinkHideList)) {
        gGlobalPrefs->showStartPage = false;
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkShowList)) {
        gGlobalPrefs->showStartPage = true;
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkNextTip)) {
        PickAnotherRandomPromotion();
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkHomeListView)) {
        SetHomePageListView(true);
        win->homePageScrollY = 0;
        SaveSettings();
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkHomeThumbnailView)) {
        SetHomePageListView(false);
        win->homePageScrollY = 0;
        SaveSettings();
        win->RedrawAll(true);
    } else if (str::TrimPrefix(url, kLinkHomeRemoveFilePrefix)) {
        ForgetFileFromFrequentlyRead(win, url);
    } else if (str::TrimPrefix(url, kLinkHomePinFilePrefix)) {
        // Pinning works from both the Recent view and the Library grid. A file
        // browsed in the Library may have no history entry yet, so create one so
        // it can be pinned straight into the Recent view's Pinned section.
        FileState* fs = gFileHistory.FindByPath(url);
        if (!fs) {
            fs = NewFileState(url);
            gFileHistory.Append(fs);
        }
        if (fs) {
            fs->isPinned = !fs->isPinned;
            SaveSettings();
            win->DeleteToolTip();
            win->RedrawAll(true);
        }
    } else if (HandleTouchHomeLink(win, url)) {
        // handled against the real native tab collection
    } else if (str::TrimPrefix(url, kLinkLibraryFolderPrefix)) {
        SelectTouchLibraryFolder(win, url);
    } else if (str::TrimPrefix(url, kLinkLibraryTogglePrefix)) {
        int expanded = win->libraryExpandedFolderPaths.FindI(url);
        if (expanded >= 0) {
            win->libraryExpandedFolderPaths.RemoveAt(expanded);
        } else {
            win->libraryExpandedFolderPaths.Append(url);
        }
        win->RedrawAll(true);
    } else if (str::Eq(url, kLinkLibraryAddFolder)) {
        AddTouchLibraryFolder(win);
    } else if (HandleTouchLibraryLink(win, url)) {
        // handled by the Library's pinned/hidden/manage-folder state machine
    } else if (str::StartsWith(url, StrL("Cmd"))) {
        int cmdId = GetCommandIdByName(url);
        if (cmdId > 0) {
            HwndSendCommand(win->hwndFrame, cmdId);
        }
    } else if (IsLink(url)) {
        // documentation links open in the embedded manual browser
        if (!MaybeLaunchDocumentation(url)) {
            SumatraLaunchBrowser(url);
        }
    } else {
        // assume it's a thumbnail of a document
        auto path = url;
        ReportIf(!path);
        SetTouchView(win, TouchView::Doc);
        LoadArgs args(path, win);
        // ctrl forces always opening
        args.activateExisting = !IsCtrlPressed();
        args.activateExistingInWindow = true;
        StartLoadDocument(&args);
    }
    // HwndSetFocus(win->hwndFrame);
}

static void OnMouseRightButtonDownAbout(MainWindow* win, int x, int y, WPARAM /*key*/) {
    // lf("Right button clicked on %d %d", x, y);
    HwndSetFocus(win->hwndFrame);
    win->dragStart = Point(x, y);
}

static void OnMouseRightButtonUpAbout(MainWindow* win, int x, int y, WPARAM /*key*/) {
    int isDrag = IsDragDistance(x, win->dragStart.x, y, win->dragStart.y);
    if (isDrag) {
        return;
    }
    OnAboutContextMenu(win, x, y);
}

static LRESULT OnSetCursorAbout(MainWindow* win, HWND hwnd) {
    if (HomePageSetLibraryResizeCursor(win)) {
        return TRUE;
    }
    Point pt = HwndGetCursorPos(hwnd);
    if (!pt.IsEmpty()) {
        StaticLink* link = nullptr;
        if (GetStaticLinkAtTemp(win->staticLinks, pt.x, pt.y, &link)) {
            SetCursorCached(IDC_HAND);
            HomePageSetHotLink(win, link ? link->target : Str{});
            // File entries: selection/tip are driven only by real WM_MOUSEMOVE
            // (and keyboard). Do not call HomePageOnHover here — after arrow-key
            // selection the canvas invalidates and WM_SETCURSOR would snap the
            // active entry back under the stationary cursor.
            if (link && !path::IsAbsolute(link->target)) {
                // chrome links (open, tips, …) keep a simple hover tip
                win->ShowToolTip(LinkTooltipTemp(link), link->rect);
            }
        } else {
            // not on a link — hide tip; keyboard selection outline stays
            win->DeleteToolTip();
            SetCursorCached(IDC_ARROW);
            HomePageSetHotLink(win, Str{});
        }
        return TRUE;
    }

    win->DeleteToolTip();
    return FALSE;
}

LRESULT WndProcCanvasAbout(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    switch (msg) {
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == win->hwndHomeSearch) {
                HDC hdcEdit = (HDC)wp;
                SetTextColor(hdcEdit, ThemeWindowTextColor());
                COLORREF bg = ThemeControlBackgroundColor();
                if (IsTouchChrome(win)) {
                    bg = win->touchView == TouchView::Library ? ThemeWindowControlBackgroundColor()
                                                              : ThemeHotBackgroundColor();
                }
                SetBkColor(hdcEdit, bg);
                HBRUSH* brush = IsTouchChrome(win) ? &win->brHomeSearchBg : &win->brControlBgColor;
                if (IsTouchChrome(win) && win->homeSearchBgColor != bg) {
                    DeleteObject(*brush);
                    *brush = nullptr;
                    win->homeSearchBgColor = bg;
                }
                if (!*brush) {
                    *brush = CreateSolidBrush(bg);
                }
                return (LRESULT)*brush;
            }
            break;

        case WM_COMMAND:
            if ((HWND)lp == win->hwndHomeSearch) {
                UINT notify = HIWORD(wp);
                if (notify == EN_CHANGE) {
                    win->homePageScrollY = 0;
                    if (win->touchView == TouchView::Library) {
                        win->libraryTreeScrollY = 0;
                        // the same field filters the Recent cards
                        win->libraryFilesScrollY = 0;
                    }
                    // the filter changed the list, so select its first entry (#1136)
                    HomePageSelectFirst(win);
                    HwndInvalidate(win->hwndCanvas);
                    return 0;
                }
                // hide/show keyboard selection outline when focus enters/leaves search
                if (notify == EN_SETFOCUS || notify == EN_KILLFOCUS) {
                    HwndInvalidate(win->hwndCanvas);
                    return 0;
                }
            }
            break;

        case WM_KEYDOWN:
            // keyboard navigation of the file list (issue #1136). These keys are
            // routed here by MaybeTranslateAccelerator instead of scrolling
            switch (wp) {
                case VK_LEFT:
                    HomePageMoveSelection(win, -1, 0);
                    return 0;
                case VK_RIGHT:
                    HomePageMoveSelection(win, 1, 0);
                    return 0;
                case VK_UP:
                    HomePageMoveSelection(win, 0, -1);
                    return 0;
                case VK_DOWN:
                    HomePageMoveSelection(win, 0, 1);
                    return 0;
                case VK_RETURN: {
                    Str path = HomePageSelectedFilePathTemp(win);
                    if (!path) {
                        return 0;
                    }
                    SetTouchView(win, TouchView::Doc);
                    LoadArgs args(path, win);
                    // ctrl forces always opening, as for a click
                    args.activateExisting = !IsCtrlPressed();
                    args.activateExistingInWindow = true;
                    StartLoadDocument(&args);
                    return 0;
                }
            }
            break;

        case WM_MOUSEMOVE:
            {
                // Hover feedback from the move's own coordinates. The
                // WM_SETCURSOR path hit-tests the real cursor position, which
                // is right for the cursor but leaves the highlight stale when
                // the surface scrolls or repaints under a still pointer.
                StaticLink* hotLink = nullptr;
                GetStaticLinkAtTemp(win->staticLinks, x, y, &hotLink);
                HomePageSetHotLink(win, hotLink ? hotLink->target : Str{});
            }
            if (HomePageOnLibraryResizeMouse(win, msg, x, y)) {
                return 0;
            }
            OnMouseMoveAbout(win, hwnd, x, y);
            return 0;

        case WM_MOUSELEAVE:
            HomePageOnCanvasMouseLeave();
            return 0;

        case WM_LBUTTONDOWN:
            if (HomePageOnLibraryResizeMouse(win, msg, x, y)) {
                return 0;
            }
            CloseTouchDocumentOverlays(win);
            if (win->touchAboutPointerId == 0) {
                win->touchAboutSuppressMouseUp = false;
            }
            {
                // press feedback: the link under the finger sinks and darkens
                StaticLink* downLink = nullptr;
                GetStaticLinkAtTemp(win->staticLinks, x, y, &downLink);
                HomePageSetPressedLink(win, downLink ? downLink->target : Str{});
            }
            OnMouseLeftButtonDownAbout(win, x, y, wp);
            return 0;

        case WM_LBUTTONUP:
            if (HomePageOnLibraryResizeMouse(win, msg, x, y)) {
                return 0;
            }
            HomePageSetPressedLink(win, Str{});
            if (win->touchAboutSuppressMouseUp) {
                win->touchAboutSuppressMouseUp = false;
                str::FreePtr(&win->urlOnLastButtonDown);
                return 0;
            }
            OnMouseLeftButtonUpAbout(win, x, y, wp);
            return 0;

        case WM_LBUTTONDBLCLK:
            OnMouseLeftButtonDownAbout(win, x, y, wp);
            return 0;

        case WM_RBUTTONDOWN:
            OnMouseRightButtonDownAbout(win, x, y, wp);
            return 0;

        case WM_RBUTTONUP:
            OnMouseRightButtonUpAbout(win, x, y, wp);
            return 0;

        case WM_SETCURSOR:
            if (OnSetCursorAbout(win, hwnd)) {
                return TRUE;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_CONTEXTMENU:
            OnAboutContextMenu(win, 0, 0);
            return 0;

        case WM_PAINT:
            if (gRedrawLog) {
                logf("redraw: WM_PAINT hwnd=0x%p (canvas-about)\n", hwnd);
            }
            OnPaintAbout(win);
            return 0;

        case WM_VSCROLL:
            HomePageHideCloseButton();
            HomePageOnVScroll(win, wp);
            return 0;

        case WM_MOUSEWHEEL: {
            HomePageHideCloseButton();
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (IsShiftPressed()) {
                HomePageOnMouseHWheel(win, delta);
            } else {
                Point screenPt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                HomePageOnMouseWheel(win, delta, HwndScreenToClient(win->hwndCanvas, screenPt));
            }
            return 0;
        }

        case WM_MOUSEHWHEEL:
            HomePageHideCloseButton();
            HomePageOnMouseHWheel(win, -GET_WHEEL_DELTA_WPARAM(wp));
            return 0;

        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
        case WM_POINTERCAPTURECHANGED: {
            Point tapPt;
            if (HomePageOnPointerEvent(win, msg, wp, lp, &tapPt)) {
                if (tapPt.x >= 0 && tapPt.y >= 0) {
                    OnMouseLeftButtonDownAbout(win, tapPt.x, tapPt.y, 0);
                    OnMouseLeftButtonUpAbout(win, tapPt.x, tapPt.y, 0);
                }
                return 0;
            }
            break;
        }

        case WM_CAPTURECHANGED:
            if (HomePageOnLibraryResizeMouse(win, msg, x, y)) {
                return 0;
            }
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
