/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Dpi.h"
#include "base/DirScan.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/Win.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/VirtWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "FileHistory.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Accelerators.h"
#include "CommandPalette.h"
#include "FilterHighlightDraw.h"
#include "FileThumbnails.h"
#include "Toolbar.h"
#include "TopBar.h"
#include "TouchMetrics.h"
#include "Rail.h"
#include "Menu.h"
#include "TipText.h"
#include "HomePage.h"
#include "Translations.h"
#include "Version.h"
#include "Theme.h"
#include "AppSettings.h"
#include "DarkModeSubclass.h"
#include "SvgIcons.h"

// how the shared tip code (TipText.cpp) opens a url link
static void OpenTipUrl(Str url) {
    // documentation links open in the embedded manual browser
    if (!MaybeLaunchDocumentation(url)) {
        SumatraLaunchBrowser(url);
    }
}

struct TipUrlHookInstaller {
    TipUrlHookInstaller() { gTipOpenUrl = OpenTipUrl; }
};
static TipUrlHookInstaller gTipUrlHookInstaller;

#ifndef ABOUT_USE_LESS_COLORS
#define ABOUT_LINE_OUTER_SIZE 2
#else
#define ABOUT_LINE_OUTER_SIZE 1
#endif
#define ABOUT_LINE_SEP_SIZE 1

static Str sumatraTips = StrL(R"tips(You can [customize scrollbar](CmdChangeScrollbar).
You can [customize keyboard shortcuts](Help/Customize-keyboard-shortcuts).
You can [customize toolbar](Help/Customize-toolbar).
Press (Key/CmdCommandPalette) to open [command palette](CmdCommandPalette).
To open file from history open [command palette](CmdCommandPalette) with (Key/CmdCommandPalette) and type `#`.
You can [extract text from PDF file](Help/Tool-x-extract-text-from-pdf).
You can [toggle menu bar](CmdToggleMenuBar) with (Key/CmdToggleMenuBar).
You can [toggle toolbar](CmdToggleToolbar) with (Key/CmdToggleToolbar).
You can [edit PDF annotations](Help/Editing-annotations).
You can preview where a citation, figure or footnote link points by hovering it - enable in [advanced settings](CmdAdvancedSettings) via CitationHoverDelay.
)tips");

static Str sumatraPromos = StrL(R"promos(Try [Edna](https://edna.arslexis.io): a note taking web app for power users.
Try [MarkLexis](https://marklexis.arslexis.io): a bookmarking web application.
)promos");

static Str promoFromServer;

// must fit all non-empty lines in sumatraTips / sumatraPromos
constexpr int kMaxHomeTips = 16;
constexpr int kMaxHomePromos = 8;

static ParsedTip gParsedTipsStorage[kMaxHomeTips];
static int gParsedTipCount = 0;
static ParsedTip gParsedPromosStorage[kMaxHomePromos];
static int gParsedPromoCount = 0;
static bool gTipsParsed = false;
static bool gSelectedIsPromo = false;
static int gSelectedTipIdx = -1;

static void ResetHomeCloseBtn();

static int ParseTipsFromString(Str src, Str prefix, ParsedTip* buffer, int bufferCap) {
    StrVec lines;
    Split(&lines, src, "\n");
    int n = 0;
    for (int i = 0; i < len(lines); i++) {
        Str line = lines[i];
        if (!str::IsEmptyOrWhiteSpace(line)) {
            n++;
        }
    }
    if (n == 0) {
        return 0;
    }
    ReportIf(n > bufferCap);
    int count = 0;
    for (int i = 0; i < len(lines); i++) {
        Str line = lines[i];
        if (str::IsEmptyOrWhiteSpace(line)) {
            continue;
        }
        if (prefix) {
            TempStr prefixed = str::JoinTemp(prefix, line);
            ParseTip(buffer[count], prefixed);
        } else {
            ParseTip(buffer[count], line);
        }
        count++;
    }
    return count;
}

static void PickRandomTipOrPromo() {
    bool pickPromo = (gParsedPromoCount > 0) && (rand() % 100 < 30);
    if (pickPromo) {
        gSelectedIsPromo = true;
        gSelectedTipIdx = rand() % gParsedPromoCount;
    } else if (gParsedTipCount > 0) {
        gSelectedIsPromo = false;
        gSelectedTipIdx = rand() % gParsedTipCount;
    }
}

static void EnsureTipsParsed() {
    if (gTipsParsed) {
        return;
    }
    gParsedTipCount = ParseTipsFromString(sumatraTips, "Tip: ", gParsedTipsStorage, kMaxHomeTips);
    gParsedPromoCount = ParseTipsFromString(sumatraPromos, {}, gParsedPromosStorage, kMaxHomePromos);
    gTipsParsed = true;
    PickRandomTipOrPromo();
}

static void ClearHomeLayoutCache();

void FreeHomePageTips() {
    FreeTintedToolbarImageLists();
    HomePageInvalidateLibrary();
    if (gTipsParsed) {
        for (int i = 0; i < gParsedTipCount; i++) {
            gParsedTipsStorage[i].Reset();
        }
        for (int i = 0; i < gParsedPromoCount; i++) {
            gParsedPromosStorage[i].Reset();
        }
        gParsedTipCount = 0;
        gParsedPromoCount = 0;
        gTipsParsed = false;
    }
    str::Free(promoFromServer);
    ResetHomeCloseBtn();
    ClearHomeLayoutCache();
}

static void PickAnotherRandomTip() {
    bool prevIsPromo = gSelectedIsPromo;
    int prev = gSelectedTipIdx;
    // keep picking until we get a different one
    int maxIter = 100;
    while (maxIter-- > 0) {
        PickRandomTipOrPromo();
        if (gSelectedIsPromo != prevIsPromo || gSelectedTipIdx != prev) {
            return;
        }
    }
}

constexpr COLORREF kAboutBorderCol = RGB(0, 0, 0);

constexpr int kAboutLeftRightSpaceDx = 8;
constexpr int kAboutMarginDx = 10;
constexpr int kAboutBoxMarginDy = 6;
constexpr int kAboutTxtDy = 6;
constexpr int kAboutRectPadding = 8;

constexpr int kInnerPadding = 8;

static const Str kSumatraTxtFont = StrL("Arial Black");
constexpr int kSumatraTxtFontSize = 24;

static const Str kVersionTxtFont = StrL("Arial Black");
constexpr int kVersionTxtFontSize = 12;

#define LAYOUT_LTR 0

static ATOM gAtomAbout;
static HWND gHwndAbout;
static Tooltip* gAboutTooltip = nullptr;
static Str gClickedURL;

struct AboutLayoutInfoEl {
    /* static data, must be provided */
    Str leftTxt;
    Str rightTxt;
    Str url;

    /* data calculated by the layout */
    Rect leftPos;
    Rect rightPos;
};

static AboutLayoutInfoEl gAboutLayoutInfo[] = {
    {"build", "Built: " __DATE__ " " __TIME__, nullptr},
    {"website", "SumatraPDF website", kWebsiteURL},
    {"manual", "SumatraPDF manual", kManualURL},
    {"forums", "SumatraPDF forums", "https://github.com/sumatrapdfreader/sumatrapdf/discussions"},
    {"programming", "The Programmers", "https://github.com/sumatrapdfreader/sumatrapdf/blob/master/AUTHORS"},
    {"licenses", "Various Open Source", "https://github.com/sumatrapdfreader/sumatrapdf/blob/master/AUTHORS"},
#if defined(GIT_COMMIT_ID_STR)
    {"last change", "git commit " GIT_COMMIT_ID_STR,
     "https://github.com/sumatrapdfreader/sumatrapdf/commit/" GIT_COMMIT_ID_STR},
#endif
#if defined(PRE_RELEASE_VER)
    {"a note", "Pre-release version, for testing only!", nullptr},
#endif
#ifdef DEBUG
    {"a note", "Debug version, for testing only!", nullptr},
#endif
    {nullptr, nullptr, nullptr}};

static Vec<StaticLink*> gStaticLinks;

void SetPromoString(Str s) {
    if (!s) return;
    str::ReplaceWithCopy(&promoFromServer, s);
}

static TempStr GetAppVersionTemp() {
    TempStr s = str::DupTemp("v" CURR_VERSION_STRA);
    if (IsProcess64()) {
        s = str::JoinTemp(s, StrL(" 64-bit"));
    } else {
        s = str::JoinTemp(s, StrL(" 32-bit"));
    }
    if (gIsDebugBuild) {
        s = str::JoinTemp(s, StrL(" (dbg)"));
    }
    return s;
}

constexpr COLORREF kCol1 = RGB(196, 64, 50);
constexpr COLORREF kCol2 = RGB(227, 107, 35);
constexpr COLORREF kCol3 = RGB(93, 160, 40);
constexpr COLORREF kCol4 = RGB(69, 132, 190);
constexpr COLORREF kCol5 = RGB(112, 115, 207);

static void DrawSumatraVersion(HDC hdc, Rect rect) {
    uint fmt = DT_LEFT | DT_NOCLIP;
    HFONT fontSumatraTxt = HdcCreateSimpleFont(hdc, kSumatraTxtFont, kSumatraTxtFontSize);
    HFONT fontVersionTxt = HdcCreateSimpleFont(hdc, kVersionTxtFont, kVersionTxtFontSize);

    SetBkMode(hdc, TRANSPARENT);

    Str txt = kAppName;
    Size txtSize = HdcMeasureText(hdc, txt, fmt, fontSumatraTxt);
    Rect mainRect(rect.x + ((rect.dx - txtSize.dx) / 2), rect.y + ((rect.dy - txtSize.dy) / 2), txtSize.dx, txtSize.dy);

    // draw SumatraPDF in colorful way
    Point pt = mainRect.TL();
    // colorful version
    static COLORREF cols[] = {kCol1, kCol2, kCol3, kCol4, kCol5, kCol5, kCol4, kCol3, kCol2, kCol1};
    char buf[2] = {};
    for (int i = 0; i < len(kAppName); i++) {
        SetTextColor(hdc, cols[i % dimofi(cols)]);
        buf[0] = kAppName[i];
        HdcDrawText(hdc, buf, pt, fmt, fontSumatraTxt);
        txtSize = HdcMeasureText(hdc, buf, fmt, fontSumatraTxt);
        pt.x += txtSize.dx;
    }

    SetTextColor(hdc, ThemeWindowTextColor());
    int x = mainRect.x + mainRect.dx + DpiScale(hdc, kInnerPadding);
    int y = mainRect.y;

    TempStr ver = GetAppVersionTemp();
    Point p = {x, y};
    HdcDrawText(hdc, ver, p, fmt, fontVersionTxt);
    p.y += DpiScale(hdc, 13);
    if (gIsPreReleaseBuild) {
        HdcDrawText(hdc, "Pre-release", p, fmt);
    }
}

// draw on the bottom right
static Rect DrawHideFrequentlyReadLink(HWND hwnd, HDC hdc, Str txt) {
    HFONT fontLeftTxt = HdcGetUiFont(hdc, kFontSizeBody);

    VirtWndText w(hwnd, txt, fontLeftTxt);
    w.isRtl = IsUIRtl();
    w.withUnderline = true;
    Size txtSize = w.GetIdealSize(true);

    auto col = ThemeWindowLinkColor();
    ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, 1, col), true);

    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    Rect rc = HwndClientRect(hwnd);

    int innerPadding = DpiScale(hwnd, kInnerPadding);
    Rect r = {0, 0, txtSize.dx, txtSize.dy};
    PositionRB(rc, r);
    MoveXY(r, -innerPadding, -innerPadding);
    w.SetBounds(r);
    w.Paint(hdc);

    // make the click target larger
    r.Inflate(innerPadding, innerPadding);
    return r;
}

static Size CalcSumatraVersionSize(HDC hdc) {
    HFONT fontSumatraTxt = HdcCreateSimpleFont(hdc, kSumatraTxtFont, kSumatraTxtFontSize);
    HFONT fontVersionTxt = HdcCreateSimpleFont(hdc, kVersionTxtFont, kVersionTxtFontSize);

    /* calculate minimal top box size */
    Size sz = HdcMeasureText(hdc, kAppName, fontSumatraTxt);
    sz.dy = sz.dy + DpiScale(hdc, kAboutBoxMarginDy * 2);

    /* consider version and version-sub strings */
    TempStr ver = GetAppVersionTemp();
    Size txtSize = HdcMeasureText(hdc, ver, fontVersionTxt);
    int minWidth = txtSize.dx + DpiScale(hdc, 8);
    int dx = std::max(txtSize.dx, minWidth);
    sz.dx += 2 * (dx + DpiScale(hdc, kInnerPadding));
    return sz;
}

static TempStr TrimGitTemp(Str s) {
    if (gitCommidId && str::EndsWith(s, gitCommidId)) {
        int sLen = len(s);
        int gitLen = len(gitCommidId);
        return str::DupTemp(Str(s.s, sLen - gitLen - 7));
    }
    return s;
}

/* Draws the about screen and remembers some state for hyperlinking.
   It transcribes the design I did in graphics software - hopeless
   to understand without seeing the design. */
static void DrawAbout(HWND hwnd, HDC hdc, Rect rect, Vec<StaticLink*>& staticLinks) {
    auto col = ThemeWindowTextColor();
    AutoDeletePen penBorder(CreatePen(PS_SOLID, ABOUT_LINE_OUTER_SIZE, col));
    AutoDeletePen penDivideLine(CreatePen(PS_SOLID, ABOUT_LINE_SEP_SIZE, col));
    col = ThemeWindowLinkColor();
    AutoDeletePen penLinkLine(CreatePen(PS_SOLID, ABOUT_LINE_SEP_SIZE, col));

    HFONT fontLeftTxt = HdcCreateSimpleFont(hdc, kLeftTextFont, kLeftTextFontSize);
    HFONT fontRightTxt = HdcCreateSimpleFont(hdc, kRightTextFont, kRightTextFontSize);

    ScopedSelectObject font(hdc, fontLeftTxt); /* Just to remember the orig font */

    Rect rc = HwndClientRect(hwnd);
    col = ThemeMainWindowBackgroundColor();
    AutoDeleteBrush brushAboutBg = CreateSolidBrush(col);
    HdcFillRect(hdc, rc, brushAboutBg);

    /* render title */
    Rect titleRect(rect.TL(), CalcSumatraVersionSize(hdc));

    ScopedSelectObject brush(hdc, CreateSolidBrush(col), true);
    ScopedSelectObject pen(hdc, penBorder);
#ifndef ABOUT_USE_LESS_COLORS
    Rectangle(hdc, rect.x, rect.y + ABOUT_LINE_OUTER_SIZE, rect.x + rect.dx,
              rect.y + titleRect.dy + ABOUT_LINE_OUTER_SIZE);
#else
    Rect titleBgBand(0, rect.y, rc.dx, titleRect.dy);
    RECT rcLogoBg = titleBgBand.ToRECT();
    HdcFillRect(hdc, ToRect(rcLogoBg), bgBrush);
    HdcDrawLine(hdc, Rect(0, rect.y, rc.dx, 0));
    HdcDrawLine(hdc, Rect(0, rect.y + titleRect.dy, rc.dx, 0));
#endif

    titleRect.Offset((rect.dx - titleRect.dx) / 2, 0);
    DrawSumatraVersion(hdc, titleRect);

    /* render attribution box */
    col = ThemeWindowTextColor();
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);

#ifndef ABOUT_USE_LESS_COLORS
    Rectangle(hdc, rect.x, rect.y + titleRect.dy, rect.x + rect.dx, rect.y + rect.dy);
#endif

    /* render text on the left*/
    SelectObject(hdc, fontLeftTxt);
    uint fmt = DT_LEFT | DT_NOCLIP;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        auto& pos = el->leftPos;
        HdcDrawText(hdc, el->leftTxt, pos, fmt);
    }

    /* render text on the right */
    SelectObject(hdc, fontRightTxt);
    SelectObject(hdc, penLinkLine);
    DeleteVecMembers(staticLinks);
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        bool hasUrl = CanAccessDisk() && el->url;
        if (hasUrl) {
            col = ThemeWindowLinkColor();
        } else {
            col = ThemeWindowTextColor();
        }
        SetTextColor(hdc, col);
        TempStr s = TrimGitTemp(el->rightTxt);
        auto& pos = el->rightPos;
        HdcDrawText(hdc, s, pos, fmt);

        if (hasUrl) {
            int underlineY = pos.y + pos.dy - 3;
            HdcDrawLine(hdc, Rect(pos.x, underlineY, pos.dx, 0));
            auto* sl = new StaticLink(pos, el->url, el->url);
            staticLinks.Append(sl);
        }
    }

    SelectObject(hdc, penDivideLine);
    Rect divideLine(gAboutLayoutInfo[0].rightPos.x - DpiScale(hwnd, kAboutLeftRightSpaceDx), rect.y + titleRect.dy + 4,
                    0, rect.y + rect.dy - 4 - gAboutLayoutInfo[0].rightPos.y);
    HdcDrawLine(hdc, divideLine);
}

static void UpdateAboutLayoutInfo(HWND hwnd, HDC hdc, Rect* rect) {
    HFONT fontLeftTxt = HdcCreateSimpleFont(hdc, kLeftTextFont, kLeftTextFontSize);
    HFONT fontRightTxt = HdcCreateSimpleFont(hdc, kRightTextFont, kRightTextFontSize);

    /* calculate minimal top box size */
    Size headerSize = CalcSumatraVersionSize(hdc);

    /* calculate left text dimensions */
    int leftLargestDx = 0;
    int leftDy = 0;
    uint fmt = DT_LEFT;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        Size txtSize = HdcMeasureText(hdc, el->leftTxt, fmt, fontLeftTxt);
        el->leftPos.dx = txtSize.dx;
        el->leftPos.dy = txtSize.dy;

        if (el == &gAboutLayoutInfo[0]) {
            leftDy = el->leftPos.dy;
        } else {
            ReportIf(leftDy != el->leftPos.dy);
        }
        leftLargestDx = std::max(leftLargestDx, el->leftPos.dx);
    }

    /* calculate right text dimensions */
    int rightLargestDx = 0;
    int rightDy = 0;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        TempStr s = TrimGitTemp(el->rightTxt);
        Size txtSize = HdcMeasureText(hdc, s, fmt, fontRightTxt);
        el->rightPos.dx = txtSize.dx;
        el->rightPos.dy = txtSize.dy;

        if (el == &gAboutLayoutInfo[0]) {
            rightDy = el->rightPos.dy;
        } else {
            ReportIf(rightDy != el->rightPos.dy);
        }
        rightLargestDx = std::max(rightLargestDx, el->rightPos.dx);
    }

    int leftRightSpaceDx = DpiScale(hwnd, kAboutLeftRightSpaceDx);
    int marginDx = DpiScale(hwnd, kAboutMarginDx);
    int aboutTxtDy = DpiScale(hwnd, kAboutTxtDy);
    /* calculate total dimension and position */
    Rect minRect;
    minRect.dx = leftRightSpaceDx + leftLargestDx + ABOUT_LINE_SEP_SIZE + rightLargestDx + leftRightSpaceDx;
    minRect.dx = std::max(minRect.dx, headerSize.dx);
    minRect.dx += (2 * ABOUT_LINE_OUTER_SIZE) + (2 * marginDx);

    minRect.dy = headerSize.dy;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        minRect.dy += rightDy + aboutTxtDy;
    }
    minRect.dy += (2 * ABOUT_LINE_OUTER_SIZE) + 4;

    Rect rc = HwndClientRect(hwnd);
    minRect.x = (rc.dx - minRect.dx) / 2;
    minRect.y = (rc.dy - minRect.dy) / 2;

    if (rect) {
        *rect = minRect;
    }

    /* calculate text positions */
    int linePosX = ABOUT_LINE_OUTER_SIZE + marginDx + leftLargestDx + leftRightSpaceDx;
    int currY = minRect.y + headerSize.dy + 4;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        el->leftPos.x = minRect.x + linePosX - leftRightSpaceDx - el->leftPos.dx;
        el->leftPos.y = currY + ((rightDy - leftDy) / 2);
        el->rightPos.x = minRect.x + linePosX + leftRightSpaceDx;
        el->rightPos.y = currY;
        currY += rightDy + aboutTxtDy;
    }
}

static void OnPaintAbout(HWND hwnd) {
    PAINTSTRUCT ps;
    Rect rc;
    HDC hdc = BeginPaint(hwnd, &ps);
    SetLayout(hdc, LAYOUT_LTR);
    UpdateAboutLayoutInfo(hwnd, hdc, &rc);
    DrawAbout(hwnd, hdc, rc, gStaticLinks);
    EndPaint(hwnd, &ps);
}

static void CopyAboutInfoToClipboard() {
    str::Builder info(512);
    TempStr ver = GetAppVersionTemp();
    info.Append(fmt("%s %s\r\n", Str(kAppName), ver));
    for (int i = len(info) - 2; i > 0; i--) {
        info.AppendChar('-');
    }
    info.Append("\r\n");
    // concatenate all the information into a single string
    // (cf. CopyPropertiesToClipboard in SumatraProperties.cpp)
    int maxLen = 0;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        maxLen = std::max(maxLen, len(el->leftTxt));
    }
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        for (int i = maxLen - len(el->leftTxt); i > 0; i--) {
            info.AppendChar(' ');
        }
        info.Append(fmt("%s: %s\r\n", el->leftTxt, el->url ? el->url.s : el->rightTxt));
    }
    CopyTextToClipboard(ToStr(info));
}

TempStr GetStaticLinkAtTemp(Vec<StaticLink*>& linkInfo, int x, int y, StaticLink** info) {
    if (!CanAccessDisk()) {
        return {};
    }

    Point pt(x, y);
    for (int i = 0; i < len(linkInfo); i++) {
        if (linkInfo[i]->rect.Contains(pt)) {
            auto* link = linkInfo[i];
            if (info) {
                *info = link;
            }
            return str::DupTemp(link->target);
        }
    }

    return {};
}

static void CreateInfotipForLink(StaticLink* linkInfo) {
    if (gAboutTooltip != nullptr) {
        return;
    }

    Tooltip::CreateArgs args;
    args.parent = gHwndAbout;
    args.font = GetAppFont(gHwndAbout);
    args.isRtl = IsUIRtl();

    gAboutTooltip = new Tooltip();
    gAboutTooltip->Create(args);
    gAboutTooltip->SetSingle(linkInfo->tooltip, linkInfo->rect, false);
}

static void DeleteInfotip() {
    if (gAboutTooltip == nullptr) {
        return;
    }
    // gAboutTooltip->Hide();
    delete gAboutTooltip;
    gAboutTooltip = nullptr;
}

static LRESULT CALLBACK WndProcAbout(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TempStr url;
    Point pt;

    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    switch (msg) {
        case WM_CREATE:
            ReportIf(gHwndAbout);
            if (UseDarkModeLib()) {
                DarkMode::setDarkTitleBarEx(hwnd, true);
            }
            break;

        case WM_ERASEBKGND:
            // do nothing, helps to avoid flicker
            return TRUE;

        case WM_PAINT:
            OnPaintAbout(hwnd);
            break;

        case WM_SETCURSOR:
            pt = HwndGetCursorPos(hwnd);
            if (!pt.IsEmpty()) {
                StaticLink* linkInfo;
                if (GetStaticLinkAtTemp(gStaticLinks, pt.x, pt.y, &linkInfo)) {
                    CreateInfotipForLink(linkInfo);
                    SetCursorCached(IDC_HAND);
                    return TRUE;
                }
            }
            DeleteInfotip();
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_LBUTTONDOWN: {
            url = GetStaticLinkAtTemp(gStaticLinks, x, y, nullptr);
            str::ReplaceWithCopy(&gClickedURL, url);
        } break;

        case WM_LBUTTONUP:
            url = GetStaticLinkAtTemp(gStaticLinks, x, y, nullptr);
            if (url && str::Eq(url, gClickedURL)) {
                SumatraLaunchBrowser(url);
            }
            break;

        case WM_CHAR:
            if (VK_ESCAPE == wp) {
                DestroyWindow(hwnd);
            }
            break;

        case WM_COMMAND:
            if (CmdCopySelection == LOWORD(wp)) {
                CopyAboutInfoToClipboard();
            }
            break;

        case WM_DESTROY:
            DeleteInfotip();
            ReportIf(!gHwndAbout);
            gHwndAbout = nullptr;
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

constexpr const WCHAR* kAboutClassName = L"SUMATRA_PDF_ABOUT";

void ShowAboutWindow(MainWindow* win) {
    if (gHwndAbout) {
        SetActiveWindow(gHwndAbout);
        return;
    }

    if (!gAtomAbout) {
        WNDCLASSEX wcex;
        FillWndClassEx(wcex, kAboutClassName, WndProcAbout);
        HMODULE h = GetModuleHandleW(nullptr);
        wcex.hIcon = LoadIcon(h, MAKEINTRESOURCE(GetAppIconID()));
        gAtomAbout = RegisterClassEx(&wcex);
        ReportIf(!gAtomAbout);
    }

    TempStr brandedTitle = str::ReplaceTemp(_TRA("About SumatraPDF"), StrL("SumatraPDF"), StrL(kAppName));
    WCHAR* title = CWStrTemp(brandedTitle);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int dx = CW_USEDEFAULT;
    int dy = CW_USEDEFAULT;
    HINSTANCE h = GetModuleHandleW(nullptr);
    gHwndAbout = CreateWindowExW(0, kAboutClassName, title, style, x, y, dx, dy, nullptr, nullptr, h, nullptr);
    if (!gHwndAbout) {
        return;
    }

    HwndSetRtl(gHwndAbout, IsUIRtl());

    // get the dimensions required for the about box's content
    Rect rc;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(gHwndAbout, &ps);
    SetLayout(hdc, LAYOUT_LTR);
    UpdateAboutLayoutInfo(gHwndAbout, hdc, &rc);
    EndPaint(gHwndAbout, &ps);
    int rectPadding = DpiScale(gHwndAbout, kAboutRectPadding);
    rc.Inflate(rectPadding, rectPadding);

    // resize the new window to just match these dimensions
    Rect wRc = HwndWindowRect(gHwndAbout);
    Rect cRc = HwndClientRect(gHwndAbout);
    wRc.dx += rc.dx - cRc.dx;
    wRc.dy += rc.dy - cRc.dy;
    MoveWindow(gHwndAbout, wRc.x, wRc.y, wRc.dx, wRc.dy, FALSE);

    HwndPositionInCenterOf(gHwndAbout, win->hwndFrame);
    ShowWindow(gHwndAbout, SW_SHOW);
}

void DrawAboutPage(MainWindow* win, HDC hdc) {
    Rect rc = HwndClientRect(win->hwndCanvas);
    UpdateAboutLayoutInfo(win->hwndCanvas, hdc, &rc);
    DrawAbout(win->hwndCanvas, hdc, rc, win->staticLinks);
    if (HasPermission(Perm::SavePreferences | Perm::DiskAccess) && SettingsRememberOpenedFiles()) {
        Rect rect = DrawHideFrequentlyReadLink(win->hwndCanvas, hdc, _TRA("Show frequently read"));
        auto* sl = new StaticLink(rect, kLinkShowList);
        win->staticLinks.Append(sl);
    }
}

/* alternate static page to display when no document is loaded */

constexpr int kThumbsSeparatorDy = 2;
constexpr int kThumbsBorderDx = 1;
#define kThumbsMarginLeft DpiScale(hdc, 40)
#define kThumbsMarginRight DpiScale(hdc, 40)
#define kThumbsMarginTop DpiScale(hdc, 50)
#define kThumbsMarginBottom DpiScale(hdc, 40)
// the card size in device px. Everything else about the grid (margins, gaps,
// the list view's own thumbnails) is DpiScaled, so the card is too; the
// bitmap is rendered at the fixed kThumbnailRenderDx and scaled into this.
#define kThumbCardDx DpiScale(hdc, kThumbnailDx)
#define kThumbCardDy DpiScale(hdc, kThumbnailDy)
#define kThumbsSpaceBetweenX DpiScale(hdc, kHomeCardGap)
#define kThumbsSpaceBetweenY DpiScale(hdc, 58)
#define kThumbsBottomBoxDy DpiScale(hdc, 50)
#define kHomeListThumbDx DpiScale(hdc, 30)
#define kHomeListThumbDy DpiScale(hdc, 40)
#define kHomeListRowDy DpiScale(hdc, 46)
#define kHomeListRowGapDx DpiScale(hdc, 8)

// ThumbnailLayout::fileSize cache: AppendBlanks zero-fills, so set
// kSizeNotFetched after each AppendBlanks (default member init never runs).
constexpr i64 kSizeNotFetched = -2;
constexpr i64 kSizeFetchFail = -1;

struct ThumbnailLayout {
    Rect rcPage;
    Size szThumb;
    Rect rcText;
    Rect rcListRow;
    Rect rcListThumb;
    Rect rcListFileName;
    Rect rcListPath;
    Rect rcListSize;
    Rect rcListRemove;
    Rect rcListPin;
    FileState* fs = nullptr; // info needed to draw the thumbnail
    StaticLink* sl = nullptr;
    // Cached file::GetSize() so we don't hit the disk on every paint.
    // AppendBlanks zero-fills, so set to kSizeNotFetched after AppendBlanks.
    i64 fileSize = kSizeNotFetched;
    // false until MeasureHomeListRowText() has split rcListFileName into name +
    // directory. Also relies on AppendBlanks zero-fill, so false must mean "not
    // measured yet"
    bool listTextMeasured = false;
};

static TempStr FileSizeForHomeListTemp(i64 size);

// true if r overlaps the visible thumbs band (optionally with a small margin)
static bool IsHomeThumbOnScreen(const Rect& r, const Rect& thumbsArea, int marginY = 0) {
    if (r.IsEmpty() || thumbsArea.IsEmpty()) {
        return false;
    }
    Rect band = thumbsArea;
    if (marginY > 0) {
        band.y -= marginY;
        band.dy += 2 * marginY;
    }
    return !r.Intersect(band).IsEmpty();
}

bool HomePageIsListView() {
    return gGlobalPrefs && str::EqI(gGlobalPrefs->homePageViewMode, StrL("list"));
}

void SetHomePageListView(bool listView) {
    Str mode = listView ? StrL("list") : StrL("thumbnails");
    str::ReplaceWithCopy(&gGlobalPrefs->homePageViewMode, mode);
}

struct HomePageLayout {
    // args in
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    Rect rc;
    MainWindow* win = nullptr;

    Rect rcAppWithVer; // SumatraPDF colorful text + version
    Rect rcLine;       // line under bApp
    Rect rcIconOpen;
    Rect rcIconListView;
    Rect rcIconThumbnailView;

    HIMAGELIST himlOpen = nullptr;
    VirtWndText* freqRead = nullptr;
    VirtWndText* openDoc = nullptr;
    VirtWndText* browseFolder = nullptr; // null when it doesn't fit the header row
    VirtWndText* hideShowFreqRead = nullptr;
    Vec<ThumbnailLayout> thumbnails; // info for each thumbnail
    int totalContentDy = 0;          // total height of all thumbnail rows
    int thumbsVisibleDy = 0;         // visible height for thumbnails area
    Rect rcThumbsArea;               // clip rect for thumbnails

    // search filter
    StrVec filterWords;
    Vec<u8> highlighted;
    Rect rcSearchBorder; // border rect drawn around the edit control
    Rect rcOpenBtn;      // accent pill behind the Open icon + label

    // tip layout
    Rect rcTip;               // background rect for tip area
    ParsedTip* tip = nullptr; // points into gParsedTipsStorage or gParsedPromosStorage, not owned

    ~HomePageLayout();
};

HomePageLayout::~HomePageLayout() {
    delete freqRead;
    delete openDoc;
    delete browseFolder;
}

constexpr int kOpenDocumentYShift = 7;
constexpr int kThumbsMiddleMargin = 32;
constexpr int kSearchEditDy = kHomeSearchDy;
constexpr int kHeaderSearchGapY = 12;
constexpr int kSearchThumbnailsGapY = 12;

static WNDPROC DefWndProcHomeSearch = nullptr;

static void HomeSelectFromSearchReturnCol(MainWindow* win);
static void HomePageShowSelectionTooltip(MainWindow* win);

static LRESULT CALLBACK WndProcHomeSearch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_DOWN) {
        // down from the search box moves into the file list (issue #1136),
        // restoring the column we left from when going up
        MainWindow* win = FindMainWindowByHwnd(GetParent(hwnd));
        if (win) {
            HomeSelectFromSearchReturnCol(win);
            HwndSetFocus(win->hwndCanvas);
            HwndInvalidate(win->hwndCanvas);
            HomePageShowSelectionTooltip(win);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        HwndSetText(hwnd, "");
        MainWindow* win = FindMainWindowByHwnd(GetParent(hwnd));
        if (win) {
            HwndSetFocus(win->hwndCanvas);
            win->RedrawAll(true);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        HWND parent = GetParent(hwnd);
        return SendMessageW(parent, msg, wp, lp);
    }
    return CallWindowProcW(DefWndProcHomeSearch, hwnd, msg, wp, lp);
}

// Home-list entries with a path (same set as thumbnails when search is empty).
static int CountHomePageFiles() {
    Vec<FileState*> all;
    if (gGlobalPrefs && gGlobalPrefs->homePageSortByFrequentlyRead) {
        gFileHistory.GetFrequencyOrder(all);
    } else {
        gFileHistory.GetRecentlyOpenedOrder(all);
    }
    int n = 0;
    for (FileState* fs : all) {
        if (fs && len(fs->filePath) > 0) {
            n++;
        }
    }
    return n;
}

// Cue banner when the search field is empty: "Search N files (Ctrl + F)".
static void UpdateHomeSearchCueBanner(MainWindow* win) {
    if (!win || !win->hwndHomeSearch) {
        return;
    }
    // _TRA returns Str; pass .s into type-safe fmt for the format string.
    TempStr cue = IsTouchChrome(win)
                      ? str::DupTemp(win->touchView == TouchView::Library ? "Search library" : "Search files")
                      : fmt(_TRA("Search %d files (Ctrl + F)").s, CountHomePageFiles());
    Edit_SetCueBannerText(win->hwndHomeSearch, CWStrTemp(cue));
}

static void EnsureHomeSearchCreated(MainWindow* win) {
    if (win->hwndHomeSearch) {
        UpdateHomeSearchCueBanner(win);
        return;
    }
    HMODULE hmod = GetModuleHandleW(nullptr);
    DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
    DWORD exStyle = 0;
    win->hwndHomeSearch = CreateWindowExW(exStyle, WC_EDITW, L"", style, 0, 0, 100, kSearchEditDy, win->hwndCanvas,
                                          nullptr, hmod, nullptr);
    HDC hdc = GetDC(win->hwndCanvas);
    HFONT font = HdcGetUiFont(hdc, kFontSizeLabel);
    ReleaseDC(win->hwndCanvas, hdc);
    SetWindowFont(win->hwndHomeSearch, font, TRUE);
    if (!DefWndProcHomeSearch) {
        DefWndProcHomeSearch = (WNDPROC)GetWindowLongPtr(win->hwndHomeSearch, GWLP_WNDPROC);
    }
    SetWindowLongPtr(win->hwndHomeSearch, GWLP_WNDPROC, (LONG_PTR)WndProcHomeSearch);
    UpdateHomeSearchCueBanner(win);
    // add left/right padding so text doesn't overlap the border
    int margin = DpiScale(win->hwndCanvas, 6);
    SendMessage(win->hwndHomeSearch, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
    // restore the query from before the edit control was destroyed
    // (e.g. by switching to a document tab and back)
    Str query = win->touchView == TouchView::Library ? win->librarySearchQuery : win->homeSearchQuery;
    if (len(query) > 0) {
        HwndSetText(win->hwndHomeSearch, query);
    }
}

void HomePageDestroySearch(MainWindow* win) {
    if (win->hwndHomeSearch) {
        TempStr query = HwndGetTextTemp(win->hwndHomeSearch);
        Str* savedQuery = win->touchView == TouchView::Library ? &win->librarySearchQuery : &win->homeSearchQuery;
        str::ReplaceWithCopy(savedQuery, query);
        DestroyWindow(win->hwndHomeSearch);
        win->hwndHomeSearch = nullptr;
    }
}

void HomePageFocusSearch(MainWindow* win) {
    EnsureHomeSearchCreated(win);
    ShowWindow(win->hwndHomeSearch, SW_SHOW);
    HwndSetFocus(win->hwndHomeSearch);
}

void PickAnotherRandomPromotion() {
    PickAnotherRandomTip();
}

// Just the path here — no file::GetSize during layout/scroll, it's too slow on
// network drives (was ~11% of home-page scroll CPU). The size is appended when
// the tooltip is actually shown, see LinkTooltipTemp().
static TempStr HomeThumbTooltipTemp(Str path) {
    return str::DupTemp(path);
}

// A thumbnail / list-row tooltip is the file path, then two spaces and a
// human-readable size. Looking the size up on hover (rather than when the link
// is created) keeps it off the layout/scroll path, where file::GetSize() on a
// network drive is slow enough to be felt. Links whose target isn't a file
// (urls, commands) keep their plain tooltip.
TempStr LinkTooltipTemp(StaticLink* link) {
    Str tip = link->tooltip;
    if (!tip || !link->target) {
        return str::DupTemp(tip);
    }
    i64 size = file::GetSize(link->target);
    if (size < 0) {
        return str::DupTemp(tip);
    }
    return fmt("%s  %s", tip, str::FormatSizeShortTemp(size, nullptr));
}

// --- scroll-friendly layout cache: full LayoutHomePage only when content/size/
// filter changes; pure scrollY changes just offset stored thumb rects ---
struct HomePageLayoutCache {
    bool valid = false;
    Rect canvasRc{};
    int scrollY = 0;
    int nFiles = 0;
    bool listView = false;
    bool sortByFreq = false;
    bool showTips = false;
    int tipIdx = -1;
    bool tipIsPromo = false;
    Str filterText{}; // owned

    Rect rcThumbsArea{};
    Rect rcSearchBorder{};
    Rect rcOpenBtn{};
    Rect rcIconOpen{};
    Rect rcIconListView{};
    Rect rcIconThumbnailView{};
    Rect rcTip{};
    Rect rcAppWithVer{};
    Rect rcLine{};
    Rect rcFreqRead{};
    Rect rcOpenDoc{};
    Rect rcBrowseFolder{}; // empty when the link doesn't fit
    int totalContentDy = 0;
    int thumbsVisibleDy = 0;
    ParsedTip* tip = nullptr;
    Vec<ThumbnailLayout> thumbs;
    StrVec filterWords;
};

static HomePageLayoutCache gHomeLayoutCache;

static void ClearHomeLayoutCache() {
    gHomeLayoutCache.valid = false;
    str::Free(gHomeLayoutCache.filterText);
    gHomeLayoutCache.filterText = {};
    gHomeLayoutCache.thumbs.Reset();
    gHomeLayoutCache.filterWords.Reset();
    gHomeLayoutCache.tip = nullptr;
    gHomeLayoutCache.nFiles = 0;
    gHomeLayoutCache.scrollY = 0;
}

// The cache holds raw FileState* (ThumbnailLayout::fs) owned by gGlobalPrefs.
// Reloading settings frees and rebuilds those, so the cache has to be dropped
// first or hover / selection reads freed memory (crash 8c34d7eda). It is
// rebuilt on the next paint.
void HomePageInvalidateLayoutCache() {
    ClearHomeLayoutCache();
}

static void OffsetThumbnailLayouts(Vec<ThumbnailLayout>& thumbs, int dy) {
    if (dy == 0) {
        return;
    }
    for (ThumbnailLayout& t : thumbs) {
        t.rcPage.y += dy;
        t.rcText.y += dy;
        t.rcListRow.y += dy;
        t.rcListThumb.y += dy;
        t.rcListFileName.y += dy;
        t.rcListPath.y += dy;
        t.rcListSize.y += dy;
        t.rcListRemove.y += dy;
        t.rcListPin.y += dy;
    }
}

// rebuild hit-test links for currently visible file rows/thumbs (scroll-safe)
static void HomePageAppendFileStaticLinks(HomePageLayout& l) {
    MainWindow* win = l.win;
    bool list = HomePageIsListView();
    for (ThumbnailLayout& thumb : l.thumbnails) {
        FileState* fs = thumb.fs;
        if (!fs || !fs->filePath) {
            continue;
        }
        Str path = fs->filePath;
        if (list) {
            Rect slRect = thumb.rcListRow.Intersect(l.rcThumbsArea);
            if (slRect.IsEmpty()) {
                continue;
            }
            TempStr removeTarget = str::JoinTemp(kLinkHomeRemoveFilePrefix, path);
            TempStr pinTarget = str::JoinTemp(kLinkHomePinFilePrefix, path);
            Str pinTip = fs->isPinned ? _TRA("Unpin") : _TRA("Pin");
            win->staticLinks.Append(new StaticLink(thumb.rcListRemove.Intersect(l.rcThumbsArea), removeTarget,
                                                   _TRA("Remove from Frequently Read")));
            win->staticLinks.Append(new StaticLink(thumb.rcListPin.Intersect(l.rcThumbsArea), pinTarget, pinTip));
            thumb.sl = new StaticLink(slRect, path, HomeThumbTooltipTemp(path));
            win->staticLinks.Append(thumb.sl);
        } else {
            Rect slRect = thumb.rcText.Union(thumb.rcPage).Intersect(l.rcThumbsArea);
            if (slRect.IsEmpty()) {
                continue;
            }
            thumb.sl = new StaticLink(slRect, path, HomeThumbTooltipTemp(path));
            win->staticLinks.Append(thumb.sl);
        }
    }
}

static void HomePageAppendChromeStaticLinks(HomePageLayout& l) {
    MainWindow* win = l.win;
    win->staticLinks.Append(new StaticLink(l.rcIconListView, kLinkHomeListView, _TRA("Show as list")));
    win->staticLinks.Append(new StaticLink(l.rcIconThumbnailView, kLinkHomeThumbnailView, _TRA("Show as thumbnails")));

    Rect rcOpen = l.rcIconOpen;
    if (l.openDoc) {
        rcOpen = rcOpen.Union(l.openDoc->lastBounds);
    }
    rcOpen.Inflate(10, 10);
    win->staticLinks.Append(new StaticLink(rcOpen, kLinkOpenFile));

    if (l.browseFolder) {
        // urls starting with "Cmd" are dispatched as commands (CanvasAboutUI.cpp)
        Rect rcBrowse = l.browseFolder->lastBounds;
        rcBrowse.Inflate(6, 10);
        win->staticLinks.Append(new StaticLink(rcBrowse, StrL("CmdNavigateFilesInFolder")));
    }

    if (l.tip) {
        for (auto& link : l.tip->links) {
            Rect linkRect;
            for (int i = link.firstWord; i <= link.lastWord; i++) {
                auto& w = l.tip->words[i];
                Rect wr = {w.x, w.y, w.dx, w.dy};
                if (i == link.firstWord) {
                    linkRect = wr;
                } else {
                    linkRect = linkRect.Union(wr);
                }
            }
            win->staticLinks.Append(new StaticLink(linkRect, link.cmd, link.cmd));
        }
        win->staticLinks.Append(new StaticLink(l.rcTip, kLinkNextTip));
    }
}

static TempStr HomeSearchQueryTemp(MainWindow* win) {
    if (!win->hwndHomeSearch) {
        return {};
    }
    return HwndGetTextTemp(win->hwndHomeSearch);
}

static bool HomeLayoutCacheMatches(MainWindow* win, const Rect& rc, Str filterText) {
    auto& c = gHomeLayoutCache;
    if (!c.valid) {
        return false;
    }
    if (c.canvasRc != rc) {
        return false;
    }
    if (c.listView != HomePageIsListView()) {
        return false;
    }
    if (c.sortByFreq != (gGlobalPrefs && gGlobalPrefs->homePageSortByFrequentlyRead)) {
        return false;
    }
    if (c.showTips != (gGlobalPrefs && gGlobalPrefs->showTips)) {
        return false;
    }
    if (c.tipIdx != gSelectedTipIdx || c.tipIsPromo != gSelectedIsPromo) {
        return false;
    }
    if (!str::Eq(c.filterText, filterText)) {
        return false;
    }
    // pin/remove/reorder changes FileState pointers or order → invalidate
    // (nFiles alone is not enough: pin does not change count)
    return true;
}

// true if cached thumb FileState* sequence still matches the current file list
static bool HomeLayoutCacheFilesMatch(const Vec<FileState*>& files) {
    auto& c = gHomeLayoutCache;
    if (len(files) != c.nFiles || len(c.thumbs) != c.nFiles) {
        return false;
    }
    for (int i = 0; i < c.nFiles; i++) {
        if (c.thumbs[i].fs != files[i]) {
            return false;
        }
    }
    return true;
}

static void CollectHomePageFiles(MainWindow* win, Vec<FileState*>& fileStates, StrVec& filterWords) {
    Vec<FileState*> allFileStates;
    if (gGlobalPrefs->homePageSortByFrequentlyRead) {
        gFileHistory.GetFrequencyOrder(allFileStates);
    } else {
        gFileHistory.GetRecentlyOpenedOrder(allFileStates);
    }

    TempStr searchQuery = HomeSearchQueryTemp(win);
    bool hasFilter = searchQuery && searchQuery.s[0];
    if (hasFilter) {
        SplitFilterToWords(searchQuery, filterWords);
    }
    for (int i = 0; i < len(allFileStates); i++) {
        FileState* fs = allFileStates[i];
        if (len(fs->filePath) == 0) {
            continue;
        }
        if (hasFilter) {
            TempStr baseName = path::GetBaseNameTemp(fs->filePath);
            if (!FilterMatches(baseName, filterWords)) {
                continue;
            }
        }
        fileStates.Append(fs);
    }
}

static void SaveHomeLayoutCache(const HomePageLayout& l, Str filterText, int scrollY) {
    auto& c = gHomeLayoutCache;
    c.valid = true;
    c.canvasRc = l.rc;
    c.scrollY = scrollY;
    c.nFiles = len(l.thumbnails);
    c.listView = HomePageIsListView();
    c.sortByFreq = gGlobalPrefs && gGlobalPrefs->homePageSortByFrequentlyRead;
    c.showTips = gGlobalPrefs && gGlobalPrefs->showTips;
    c.tipIdx = gSelectedTipIdx;
    c.tipIsPromo = gSelectedIsPromo;
    str::ReplaceWithCopy(&c.filterText, filterText);
    c.rcThumbsArea = l.rcThumbsArea;
    c.rcSearchBorder = l.rcSearchBorder;
    c.rcOpenBtn = l.rcOpenBtn;
    c.rcIconOpen = l.rcIconOpen;
    c.rcIconListView = l.rcIconListView;
    c.rcIconThumbnailView = l.rcIconThumbnailView;
    c.rcTip = l.rcTip;
    c.rcAppWithVer = l.rcAppWithVer;
    c.rcLine = l.rcLine;
    c.rcFreqRead = l.freqRead ? l.freqRead->lastBounds : Rect{};
    c.rcOpenDoc = l.openDoc ? l.openDoc->lastBounds : Rect{};
    c.rcBrowseFolder = l.browseFolder ? l.browseFolder->lastBounds : Rect{};
    c.totalContentDy = l.totalContentDy;
    c.thumbsVisibleDy = l.thumbsVisibleDy;
    c.tip = l.tip;
    c.thumbs = l.thumbnails;
    for (ThumbnailLayout& t : c.thumbs) {
        t.sl = nullptr; // links are owned by win->staticLinks, recreated each paint
    }
    c.filterWords = l.filterWords;
}

// rebuild chrome VirtWndText + copy cached geometry into l (no full layout)
static void ApplyHomeLayoutCache(HomePageLayout& l, int scrollY) {
    auto& c = gHomeLayoutCache;
    auto* win = l.win;
    auto* hdc = l.hdc;
    auto* hwnd = l.hwnd;
    bool isRtl = IsUIRtl();

    // clamp scroll using cached content height
    int maxScrollY = std::max(0, c.totalContentDy - c.thumbsVisibleDy);
    if (scrollY > maxScrollY) {
        scrollY = maxScrollY;
        win->homePageScrollY = scrollY;
    }
    if (scrollY < 0) {
        scrollY = 0;
        win->homePageScrollY = 0;
    }

    int dy = c.scrollY - scrollY; // content moves opposite scroll direction
    OffsetThumbnailLayouts(c.thumbs, dy);
    c.scrollY = scrollY;

    l.rcThumbsArea = c.rcThumbsArea;
    l.rcSearchBorder = c.rcSearchBorder;
    l.rcOpenBtn = c.rcOpenBtn;
    l.rcIconOpen = c.rcIconOpen;
    l.rcIconListView = c.rcIconListView;
    l.rcIconThumbnailView = c.rcIconThumbnailView;
    l.rcTip = c.rcTip;
    l.rcAppWithVer = c.rcAppWithVer;
    l.rcLine = c.rcLine;
    l.totalContentDy = c.totalContentDy;
    l.thumbsVisibleDy = c.thumbsVisibleDy;
    l.tip = c.tip;
    l.thumbnails = c.thumbs;
    l.filterWords = c.filterWords;

    l.himlOpen = TbGetImageList(win->hwndToolbar);

    HFONT hdrFont = HdcGetUiFont(hdc, kFontSizeTitle, kFontWeightStrong);
    HFONT fontText = HdcGetUiFont(hdc, kFontSizeLabel, kFontWeightStrong);

    Str txt = _TRA("Recently Opened");
    if (gGlobalPrefs->homePageSortByFrequentlyRead) {
        txt = _TRA("Frequently Read");
    }
    VirtWndText* hdr = new VirtWndText(hwnd, txt, hdrFont);
    hdr->isRtl = isRtl;
    hdr->SetBounds(c.rcFreqRead);
    l.freqRead = hdr;

    VirtWndText* openDoc = new VirtWndText(hwnd, _TRA("Open a document..."), fontText);
    openDoc->isRtl = isRtl;
    openDoc->withUnderline = true;
    openDoc->SetBounds(c.rcOpenDoc);
    l.openDoc = openDoc;

    if (!c.rcBrowseFolder.IsEmpty()) {
        auto* browse = new VirtWndText(hwnd, _TRA("Navigate Files in Folder"), fontText);
        browse->isRtl = isRtl;
        browse->withUnderline = true;
        browse->SetBounds(c.rcBrowseFolder);
        l.browseFolder = browse;
    }

    HomePageAppendChromeStaticLinks(l);
    HomePageAppendFileStaticLinks(l);
}

// after paint, keep lazy fileSize values in the cache for the next frame
static void SyncHomeLayoutCacheFileSizes(const HomePageLayout& l) {
    auto& c = gHomeLayoutCache;
    if (!c.valid || len(c.thumbs) != len(l.thumbnails)) {
        return;
    }
    for (int i = 0; i < len(c.thumbs); i++) {
        c.thumbs[i].fileSize = l.thumbnails[i].fileSize;
        // geometry may have been filled for newly visible list rows
        c.thumbs[i].rcListFileName = l.thumbnails[i].rcListFileName;
        c.thumbs[i].rcListPath = l.thumbnails[i].rcListPath;
        c.thumbs[i].rcListSize = l.thumbnails[i].rcListSize;
        c.thumbs[i].listTextMeasured = l.thumbnails[i].listTextMeasured;
        c.thumbs[i].szThumb = l.thumbnails[i].szThumb;
    }
}

static void LayoutHomePage(HomePageLayout& l) {
    EnsureTipsParsed();

    Vec<FileState*> allFileStates;
    if (gGlobalPrefs->homePageSortByFrequentlyRead) {
        gFileHistory.GetFrequencyOrder(allFileStates);
    } else {
        gFileHistory.GetRecentlyOpenedOrder(allFileStates);
    }
    auto* hwnd = l.hwnd;
    auto* hdc = l.hdc;
    auto rc = l.rc;
    auto* win = l.win;

    // filter by search query if present
    TempStr searchQuery = nullptr;
    if (win->hwndHomeSearch) {
        searchQuery = HwndGetTextTemp(win->hwndHomeSearch);
    }
    bool hasFilter = searchQuery && searchQuery.s[0];
    if (hasFilter) {
        SplitFilterToWords(searchQuery, l.filterWords);
    }
    Vec<FileState*> fileStates;
    for (int i = 0; i < len(allFileStates); i++) {
        FileState* fs = allFileStates[i];
        // a state without a path can't be opened or thumbnailed - don't show it
        if (len(fs->filePath) == 0) {
            continue;
        }
        if (hasFilter) {
            TempStr baseName = path::GetBaseNameTemp(fs->filePath);
            if (!FilterMatches(baseName, l.filterWords)) {
                continue;
            }
        }
        fileStates.Append(fs);
    }

    bool isRtl = IsUIRtl();
    HFONT fontText = HdcGetUiFont(hdc, kFontSizeLabel, kFontWeightStrong);
    HFONT hdrFont = HdcGetUiFont(hdc, kFontSizeTitle, kFontWeightStrong);

    Size sz = CalcSumatraVersionSize(hdc);
    {
        Rect& r = l.rcAppWithVer;
        r.x = rc.dx - sz.dx - 3;
        r.y = 0;
        r.SetSize(sz);
    }

    l.rcLine = {0, sz.dy, rc.dx, 0};

    // --- Pre-compute thumbnail grid x offset so header can align with it ---
    // use unfiltered count so layout stays stable when search filters results
    int nFilesForLayout = len(allFileStates);
    int colsForLayout =
        (rc.dx - kThumbsMarginLeft - kThumbsMarginRight + kThumbsSpaceBetweenX) / (kThumbCardDx + kThumbsSpaceBetweenX);
    int thumbsColsForLayout = std::max(colsForLayout, 1);
    int thumbsStartX = rc.x + kThumbsMarginLeft +
                       ((rc.dx - (thumbsColsForLayout * kThumbCardDx) -
                         ((thumbsColsForLayout - 1) * kThumbsSpaceBetweenX) - kThumbsMarginLeft - kThumbsMarginRight) /
                        2);
    if (thumbsStartX < DpiScale(hdc, kInnerPadding)) {
        thumbsStartX = DpiScale(hdc, kInnerPadding);
    } else if (nFilesForLayout == 0) {
        thumbsStartX = kThumbsMarginLeft;
    }
    int thumbsContentWidth = (thumbsColsForLayout * kThumbCardDx) + ((thumbsColsForLayout - 1) * kThumbsSpaceBetweenX);

    // --- Step 1: layout header at the top ---
    l.himlOpen = TbGetImageList(win->hwndToolbar);
    Rect rcIconView(0, 0, 0, 0);
    ImageList_GetIconSize(l.himlOpen, &rcIconView.dx, &rcIconView.dy);

    Str txt = _TRA("Recently Opened");
    if (gGlobalPrefs->homePageSortByFrequentlyRead) {
        txt = _TRA("Frequently Read");
    }
    VirtWndText* hdr = new VirtWndText(hwnd, txt, hdrFont);
    l.freqRead = hdr;
    hdr->isRtl = isRtl;
    Size txtSize = hdr->GetIdealSize(true);

    int hdrY = DpiScale(hdc, 8);
    int iconGap = DpiScale(hdc, 4);
    int titleGap = DpiScale(hdc, 8);
    int viewIconsDx = (2 * rcIconView.dx) + iconGap;
    Rect rcHdr(thumbsStartX + viewIconsDx + titleGap, hdrY, txtSize.dx, txtSize.dy);
    l.rcIconThumbnailView = {thumbsStartX, rcHdr.y + ((rcHdr.dy - rcIconView.dy) / 2), rcIconView.dx, rcIconView.dy};
    l.rcIconListView = {l.rcIconThumbnailView.x + rcIconView.dx + iconGap, l.rcIconThumbnailView.y, rcIconView.dx,
                        rcIconView.dy};
    if (isRtl) {
        int groupDx = viewIconsDx + titleGap + rcHdr.dx;
        int groupX = rc.dx - thumbsStartX - groupDx;
        rcHdr.x = groupX;
        l.rcIconListView = {rcHdr.x + rcHdr.dx + titleGap, l.rcIconListView.y, rcIconView.dx, rcIconView.dy};
        l.rcIconThumbnailView = {l.rcIconListView.x + rcIconView.dx + iconGap, l.rcIconThumbnailView.y, rcIconView.dx,
                                 rcIconView.dy};
    }
    hdr->SetBounds(rcHdr);
    win->staticLinks.Append(new StaticLink(l.rcIconListView, kLinkHomeListView, _TRA("Show as list")));
    win->staticLinks.Append(new StaticLink(l.rcIconThumbnailView, kLinkHomeThumbnailView, _TRA("Show as thumbnails")));

    /* "Open a document" link next to header */
    Rect rcIconOpen(0, 0, 0, 0);
    ImageList_GetIconSize(l.himlOpen, &rcIconOpen.dx, &rcIconOpen.dy);

    txt = _TRA("Open a document...");
    auto* openDoc = new VirtWndText(hwnd, txt, fontText);
    openDoc->isRtl = isRtl;
    openDoc->withUnderline = true;
    txtSize = openDoc->GetIdealSize(true);

    // the Open button is a filled pill now, so it needs room for its padding
    int openDocSpacing = DpiScale(hdc, 32);
    rcIconOpen.x = rcHdr.x + rcHdr.dx + openDocSpacing;
    rcIconOpen.y = rcHdr.y + rcHdr.dy - rcIconOpen.dy - kOpenDocumentYShift + 3;
    if (isRtl) {
        rcIconOpen.x = rcHdr.x - openDocSpacing - rcIconOpen.dx;
    }
    l.rcIconOpen = rcIconOpen;

    Rect rcOpenDoc(rcIconOpen.x + rcIconOpen.dx + 3, rcHdr.y + rcHdr.dy - txtSize.dy - kOpenDocumentYShift, txtSize.dx,
                   txtSize.dy);
    if (isRtl) {
        rcOpenDoc.x = rcIconOpen.x - rcOpenDoc.dx - 3;
    }
    openDoc->SetBounds(rcOpenDoc);

    l.openDoc = openDoc;
    {
        // the accent pill covers the icon and the label, so it has to be built
        // here where both rects are known
        Rect btn = rcOpenDoc.Union(rcIconOpen);
        btn.Inflate(DpiScale(hdc, 14), DpiScale(hdc, 9));
        l.rcOpenBtn = btn;
    }

    /* "Navigate Files in Folder" link after it; dropped when the header row is
       too narrow to hold it (the row doesn't wrap) */
    // tracked here rather than read back from the VirtWndText: its `bounds` is
    // not the laid-out text rect, which is why the search field used to be
    // placed on top of this link
    Rect rcBrowseUsed{};
    {
        auto* browse = new VirtWndText(hwnd, _TRA("Navigate Files in Folder"), fontText);
        browse->isRtl = isRtl;
        browse->withUnderline = true;
        Size browseSize = browse->GetIdealSize(true);
        int x = isRtl ? rcOpenDoc.x - openDocSpacing - browseSize.dx : rcOpenDoc.x + rcOpenDoc.dx + openDocSpacing;
        Rect rcBrowse(x, rcOpenDoc.y, browseSize.dx, browseSize.dy);
        int margin = DpiScale(hdc, 8);
        bool fits = isRtl ? (rcBrowse.x >= margin) : (rcBrowse.x + rcBrowse.dx <= rc.dx - margin);
        if (fits) {
            browse->SetBounds(rcBrowse);
            l.browseFolder = browse;
            rcBrowseUsed = rcBrowse;
        } else {
            delete browse;
        }
    }

    rcOpenDoc = rcOpenDoc.Union(rcIconOpen);
    rcOpenDoc.Inflate(10, 10);
    auto* sl = new StaticLink(rcOpenDoc, kLinkOpenFile);
    win->staticLinks.Append(sl);

    int headerBottomY = rcHdr.y + rcHdr.dy;

    // --- Position search edit ---
    EnsureHomeSearchCreated(win);
    int searchEditDy = DpiScale(hdc, kSearchEditDy);
    int headerSearchGap = DpiScale(hdc, kHeaderSearchGapY);
    int searchThumbsGap = DpiScale(hdc, kSearchThumbnailsGapY);

    // The redesign puts search in the header row rather than on its own line.
    // It only fits once the title, the view toggle and the Open button have
    // taken their width, so fall back to the row below on a narrow window.
    int rowLeft = thumbsStartX;
    int rowRight = thumbsStartX + thumbsContentWidth;
    // the design's 320 assumes its header has nothing but title / search /
    // toggle / Open; we also carry the "Navigate Files in Folder" link, so
    // require a narrower but still usable field before claiming the row
    int searchMinDx = DpiScale(hdc, 240);
    int rowGap = DpiScale(hdc, 24);
    int usedRight = l.rcOpenBtn.x + l.rcOpenBtn.dx;
    if (!rcBrowseUsed.IsEmpty()) {
        usedRight = std::max(usedRight, rcBrowseUsed.x + rcBrowseUsed.dx);
    }
    int freeDx = rowRight - usedRight - rowGap;
    bool searchInHeader = !isRtl && freeDx >= searchMinDx;
    {
        int borderDy = searchEditDy + 2; // 1px border on each side
        int borderDx, borderX, borderY;
        if (searchInHeader) {
            borderDx = freeDx;
            borderX = rowRight - borderDx;
            // centered on the header row, which is taller than the field
            borderY = rcHdr.y + ((rcHdr.dy - borderDy) / 2);
            headerBottomY = std::max(headerBottomY, borderY + borderDy);
        } else {
            borderDx = thumbsContentWidth * 3 / 4;
            borderDx = std::max(borderDx, DpiScale(hdc, 200));
            borderX = thumbsStartX + ((thumbsContentWidth - borderDx) / 2);
            borderY = headerBottomY + headerSearchGap;
        }
        l.rcSearchBorder = {borderX, borderY, borderDx, borderDy};
        // measure font height so we can vertically center the edit
        HFONT editFont = (HFONT)SendMessage(win->hwndHomeSearch, WM_GETFONT, 0, 0);
        TEXTMETRIC tm;
        HFONT oldFont = (HFONT)SelectObject(hdc, editFont);
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, oldFont);
        int fontDy = tm.tmHeight + tm.tmExternalLeading + 2; // +2 for caret padding
        int editDy = std::min(fontDy, searchEditDy);
        int editY = borderY + 1 + ((searchEditDy - editDy) / 2);
        MoveWindow(win->hwndHomeSearch, borderX + 1, editY, borderDx - 2, editDy, TRUE);
    }
    // border is 1px top + 1px bottom = 2px. In the header row the field costs
    // no extra height, only the gap before the thumbnails.
    int searchAreaDy = searchInHeader ? searchThumbsGap : (headerSearchGap + searchEditDy + 2 + searchThumbsGap);
    headerBottomY += searchAreaDy;

    // --- Step 2: calculate tip area at the bottom (before thumbnails) ---
    int tipHeight = 0;
    HFONT fontTip = HdcGetUiFont(hdc, kFontSizeBody);
    ParsedTip* tip = nullptr;
    if (gGlobalPrefs->showTips && gSelectedTipIdx >= 0) {
        if (gSelectedIsPromo && gSelectedTipIdx < gParsedPromoCount) {
            tip = &gParsedPromosStorage[gSelectedTipIdx];
        } else if (!gSelectedIsPromo && gSelectedTipIdx < gParsedTipCount) {
            tip = &gParsedTipsStorage[gSelectedTipIdx];
        }
    }
    if (tip) {
        MeasureTipWords(*tip, hdc, fontTip);
        int tipPadding = DpiScale(hdc, 8);
        // do a preliminary layout to get the height (use thumbnails content width)
        LayoutTip(*tip, thumbsContentWidth, 0, 0);
        tipHeight = tip->totalDy + (2 * tipPadding);
    }

    // --- Step 3: middle area for thumbnails/list ---
    // content starts directly after headerBottomY (which includes kSearchThumbnailsGapY)
    int thumbsTopY = headerBottomY;
    int thumbsBottomY = rc.dy - tipHeight - kThumbsMiddleMargin;
    int thumbsVisibleDy = std::max(0, thumbsBottomY - thumbsTopY);

    l.rcThumbsArea = {0, thumbsTopY, rc.dx, thumbsVisibleDy};

    int nFiles = len(fileStates);
    bool showList = HomePageIsListView();
    // Leave room above the first row so RoundRect / selection outline top edges
    // aren't clipped by rcThumbsArea (they extend a few px upward).
    int thumbsContentPadTop = showList ? DpiScale(hdc, 2) : DpiScale(hdc, 5);
    int thumbsRows = 0;
    int thumbsContentDy = 0;
    if (showList) {
        thumbsRows = nFiles;
        thumbsContentDy = nFiles * kHomeListRowDy;
    } else {
        thumbsRows = (nFiles + thumbsColsForLayout - 1) / thumbsColsForLayout;
        if (thumbsRows > 0) {
            thumbsContentDy = (thumbsRows * (kThumbCardDy + kThumbsSpaceBetweenY)) - kThumbsSpaceBetweenY;
        }
    }
    if (thumbsContentDy > 0) {
        thumbsContentDy += thumbsContentPadTop;
    }

    int scrollY = win->homePageScrollY;
    int maxScrollY = std::max(0, thumbsContentDy - thumbsVisibleDy);
    if (scrollY > maxScrollY) {
        scrollY = maxScrollY;
        win->homePageScrollY = scrollY;
    }
    l.totalContentDy = thumbsContentDy;
    l.thumbsVisibleDy = thumbsVisibleDy;

    Point ptOff(thumbsStartX, thumbsTopY + thumbsContentPadTop - scrollY);

    if (showList) {
        int listX = thumbsStartX;
        if (isRtl) {
            listX = rc.dx - thumbsStartX - thumbsContentWidth;
        }
        int listIconDx = l.rcIconListView.dx;
        int listIconGap = DpiScale(hdc, 6);
        // fixed size column — never call file::GetSize during layout (disk/network I/O)
        int listSizeDx = DpiScale(hdc, 56);
        // one-row margin so a quick scroll still has measured name/path splits ready
        int listPrefetchY = kHomeListRowDy;
        for (int row = 0; row < nFiles; row++) {
            ThumbnailLayout& thumb = *l.thumbnails.AppendBlanks(1);
            thumb.fileSize = kSizeNotFetched;
            FileState* fs = fileStates[row];
            thumb.fs = fs;
            Rect rcRow(listX, ptOff.y + (row * kHomeListRowDy), thumbsContentWidth, kHomeListRowDy);
            thumb.rcListRow = rcRow;
            bool onScreen = IsHomeThumbOnScreen(rcRow, l.rcThumbsArea, listPrefetchY);

            Rect rcThumb(rcRow.x, rcRow.y + ((rcRow.dy - kHomeListThumbDy) / 2), kHomeListThumbDx, kHomeListThumbDy);
            Rect rcPin(rcRow.x + rcRow.dx - listIconDx, rcRow.y + ((rcRow.dy - listIconDx) / 2), listIconDx,
                       listIconDx);
            Rect rcRemove(rcPin.x - listIconGap - listIconDx, rcPin.y, listIconDx, listIconDx);
            Rect rcSize(rcRemove.x - listIconGap - listSizeDx, rcRow.y, listSizeDx, rcRow.dy);
            Rect rcFileName(rcThumb.x + rcThumb.dx + kHomeListRowGapDx, rcRow.y,
                            rcSize.x - (rcThumb.x + rcThumb.dx + kHomeListRowGapDx) - kHomeListRowGapDx, rcRow.dy);
            if (isRtl) {
                rcThumb.x = rcRow.x + rcRow.dx - rcThumb.dx;
                rcPin.x = rcRow.x;
                rcRemove.x = rcPin.x + listIconDx + listIconGap;
                rcSize.x = rcRemove.x + listIconDx + listIconGap;
                rcFileName.x = rcSize.x + rcSize.dx + kHomeListRowGapDx;
                rcFileName.dx = rcThumb.x - rcFileName.x - kHomeListRowGapDx;
            }
            rcFileName.dx = std::max(rcFileName.dx, 0);
            // rcFileName is the whole name+path span; MeasureHomeListRowText()
            // splits it when the row is first painted. Doing it here would
            // measure text for every history entry on every layout, and doing it
            // only for rows that happen to be on screen *now* left rows scrolled
            // in later without their directory (#5870 follow-up).
            thumb.rcListThumb = rcThumb;
            thumb.rcListPin = rcPin;
            thumb.rcListRemove = rcRemove;
            thumb.rcListSize = rcSize;
            thumb.rcListFileName = rcFileName;
            // already-cached in-memory thumb size only (no LoadThumbnail / disk)
            if (onScreen && fs->thumbnail) {
                thumb.szThumb = fs->thumbnail->GetSize();
            }
            if (!onScreen) {
                continue;
            }
            Str path = fs->filePath;
            Rect slRect = rcRow.Intersect(l.rcThumbsArea);
            if (!slRect.IsEmpty()) {
                TempStr removeTarget = str::JoinTemp(kLinkHomeRemoveFilePrefix, path);
                TempStr pinTarget = str::JoinTemp(kLinkHomePinFilePrefix, path);
                Str pinTip = fs->isPinned ? _TRA("Unpin") : _TRA("Pin");
                win->staticLinks.Append(new StaticLink(rcRemove.Intersect(l.rcThumbsArea), removeTarget,
                                                       _TRA("Remove from Frequently Read")));
                win->staticLinks.Append(new StaticLink(rcPin.Intersect(l.rcThumbsArea), pinTarget, pinTip));
                thumb.sl = new StaticLink(slRect, path, HomeThumbTooltipTemp(path));
                win->staticLinks.Append(thumb.sl);
            }
        }
    } else {
        int thumbPrefetchY = kThumbCardDy + kThumbsSpaceBetweenY;
        for (int row = 0; row < thumbsRows; row++) {
            for (int col = 0; col < thumbsColsForLayout; col++) {
                if ((row * thumbsColsForLayout) + col >= nFiles) {
                    // no more files to display
                    thumbsRows = col > 0 ? row + 1 : row;
                    break;
                }
                ThumbnailLayout& thumb = *l.thumbnails.AppendBlanks(1);
                thumb.fileSize = kSizeNotFetched;
                FileState* fs = fileStates[(row * thumbsColsForLayout) + col];
                thumb.fs = fs;

                Rect rcPage(ptOff.x + (col * (kThumbCardDx + kThumbsSpaceBetweenX)),
                            ptOff.y + (row * (kThumbCardDy + kThumbsSpaceBetweenY)), kThumbCardDx, kThumbCardDy);
                if (isRtl) {
                    rcPage.x = rc.dx - rcPage.x - rcPage.dx;
                }
                bool onScreen = IsHomeThumbOnScreen(rcPage, l.rcThumbsArea, thumbPrefetchY);
                // only use already-resident thumbnails for aspect adjust — never LoadThumbnail
                // during layout (disk I/O dominated scroll/paint CPU)
                if (onScreen && fs->thumbnail) {
                    // the card keeps its full box whatever the page aspect is;
                    // the image is fitted inside it when drawn, so the grid
                    // stays on a regular baseline like the redesign's
                    thumb.szThumb = fs->thumbnail->GetSize();
                }
                thumb.rcPage = rcPage;
                {
                    // pin badge in the card's top-right corner, a finger-sized
                    // target per the redesign
                    int badge = DpiScale(hdc, kHomeBadgeDy);
                    int inset = DpiScale(hdc, 8);
                    int bx = isRtl ? rcPage.x + inset : rcPage.x + rcPage.dx - badge - inset;
                    thumb.rcListPin = Rect{bx, rcPage.y + inset, badge, badge};
                }
                int iconSpace = DpiScale(hdc, 20);
                Rect rcText(rcPage.x + iconSpace, rcPage.y + rcPage.dy + 3, rcPage.dx - iconSpace, iconSpace);
                if (isRtl) {
                    rcText.x -= iconSpace;
                }
                thumb.rcText = rcText;
                if (!onScreen) {
                    continue;
                }
                Str path = fs->filePath;
                // the badge sits on top of the card, so register it first:
                // GetStaticLinkAtTemp returns the first rect that matches
                Rect pinRect = thumb.rcListPin.Intersect(l.rcThumbsArea);
                if (!pinRect.IsEmpty()) {
                    // as in the redesign: a pinned card offers unpin, a plain
                    // recent card offers removal from the list
                    TempStr badgeTarget;
                    Str badgeTip;
                    if (fs->isPinned) {
                        badgeTarget = str::JoinTemp(kLinkHomePinFilePrefix, path);
                        badgeTip = _TRA("Unpin");
                    } else {
                        badgeTarget = str::JoinTemp(kLinkHomeRemoveFilePrefix, path);
                        badgeTip = _TRA("Remove from the list");
                    }
                    win->staticLinks.Append(new StaticLink(pinRect, badgeTarget, badgeTip));
                }
                Rect slRect = rcText.Union(rcPage).Intersect(l.rcThumbsArea);
                if (!slRect.IsEmpty()) {
                    thumb.sl = new StaticLink(slRect, path, HomeThumbTooltipTemp(path));
                    win->staticLinks.Append(thumb.sl);
                }
            }
        }
    }

    // layout tip at the bottom
    if (tip) {
        Rect rcClient = HwndClientRect(win->hwndCanvas);
        int tipPadding = DpiScale(hdc, 8);

        int tipY = rcClient.dy - tipHeight;
        // background spans full window width
        l.rcTip = {0, tipY, rcClient.dx, tipHeight};
        l.tip = tip;

        // text area aligned with thumbnails
        int tipStartX = thumbsStartX;
        int tipStartY = tipY + tipPadding;
        LayoutTip(*tip, thumbsContentWidth, tipStartX, tipStartY);

        // register tip links; per-link rects first so they take priority in hit testing
        for (auto& link : tip->links) {
            // compute bounding rect of all words in this link
            Rect linkRect;
            for (int i = link.firstWord; i <= link.lastWord; i++) {
                auto& w = tip->words[i];
                Rect wr = {w.x, w.y, w.dx, w.dy};
                if (i == link.firstWord) {
                    linkRect = wr;
                } else {
                    linkRect = linkRect.Union(wr);
                }
            }
            auto* slTip = new StaticLink(linkRect, link.cmd, link.cmd);
            win->staticLinks.Append(slTip);
        }
        // tip background: clicking outside of links picks another tip
        auto* slBg = new StaticLink(l.rcTip, kLinkNextTip);
        win->staticLinks.Append(slBg);
    }
}

static void GetFileStateIcon(FileState* fs) {
    if (fs->himl) {
        return;
    }
    SHFILEINFO sfi{};
    sfi.iIcon = -1;
    uint flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
    WCHAR* filePathW = CWStrTemp(fs->filePath);
    fs->himl = (HIMAGELIST)SHGetFileInfoW(filePathW, 0, &sfi, sizeof(sfi), flags);
    fs->iconIdx = sfi.iIcon;
}

// --- Close (✕) button for Frequently Read thumbnails (issue #283, #5745) ---
//
// Drawn directly onto the home-page canvas (over the top-right corner of the
// thumbnail under the mouse) rather than as a separate top-level window. The
// separate window could be left behind, drawing stray crosses over a document
// (#5745). Styled like the tab close button (gray X on a white circle; red
// circle + white X on hover).
//
// To keep updates cheap, the glyph is painted/erased in just its own rect: the
// double-buffer (win->buffer) holds the page without the button, so erasing is
// a small BitBlt of that area back to the window. A full home-page repaint
// resets the button (it reappears on the next mouse move).

struct HomeCloseBtn {
    MainWindow* win = nullptr; // window the button currently belongs to
    Str filePath;              // file removed when the button is clicked
    Rect rc;                   // button rect (canvas client coords)
    Rect thumbRc;              // thumbnail rect (canvas client coords)
    bool isHover = false;
    bool visible = false;
};
static HomeCloseBtn gHomeCloseBtn;
// where the glyph is currently painted on the window, so we can erase exactly
// that area (empty when nothing is painted)
static Rect gHomeCloseBtnPaintedRc;

static void DrawHomeCloseGlyph(HDC hdc, const Rect& rc, bool isHover) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    u8 a = isHover ? 255 : 215;
    int w = rc.dx;
    int h = rc.dy;
    if (isHover) {
        COLORREF bg = kColCloseXHoverBg; // runtime var so GetXValue() isn't a constant cast
        Gdiplus::SolidBrush br(Gdiplus::Color(a, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
        g.FillEllipse(&br, rc.x, rc.y, w - 1, h - 1);
    } else {
        // white circle so the gray X stays visible on any thumbnail background
        Gdiplus::SolidBrush br(Gdiplus::Color(a, 255, 255, 255));
        g.FillEllipse(&br, rc.x, rc.y, w - 1, h - 1);
    }
    COLORREF xcol = isHover ? kColCloseXHover : kColCloseX;
    Gdiplus::Pen pen(Gdiplus::Color(a, GetRValue(xcol), GetGValue(xcol), GetBValue(xcol)), 2.0f);
    int pad = w / 3;
    g.DrawLine(&pen, rc.x + pad, rc.y + pad, rc.x + w - pad, rc.y + h - pad);
    g.DrawLine(&pen, rc.x + w - pad, rc.y + pad, rc.x + pad, rc.y + h - pad);
}

// erase whatever glyph is currently on the window by blitting the button-free
// page back from the double-buffer
static void EraseHomeCloseGlyph(MainWindow* win) {
    Rect& pr = gHomeCloseBtnPaintedRc;
    if (pr.IsEmpty()) {
        return;
    }
    if (win && win->buffer) {
        HDC hdc = GetDC(win->hwndCanvas);
        HDC bufDC = win->buffer->GetDC();
        if (hdc && bufDC) {
            BitBlt(hdc, pr.x, pr.y, pr.dx, pr.dy, bufDC, pr.x, pr.y, SRCCOPY);
        }
        if (hdc) {
            ReleaseDC(win->hwndCanvas, hdc);
        }
    } else {
        // no buffer to restore from: fall back to invalidating the area
        if (win) {
            RECT r = ToRECT(pr);
            HwndInvalidateRect(win->hwndCanvas, ToRect(r), false);
        }
    }
    pr = {};
}

// re-paint the button: erase the previous glyph, then draw the current one
static void RepaintHomeCloseBtn(MainWindow* win) {
    EraseHomeCloseGlyph(win);
    HomeCloseBtn& b = gHomeCloseBtn;
    if (!b.visible || !win) {
        return;
    }
    HDC hdc = GetDC(win->hwndCanvas);
    if (hdc) {
        DrawHomeCloseGlyph(hdc, b.rc, b.isHover);
        ReleaseDC(win->hwndCanvas, hdc);
        gHomeCloseBtnPaintedRc = b.rc;
    }
}

// clear state without touching the window (used by a full home-page repaint,
// which redraws everything anyway)
static void ResetHomeCloseBtn() {
    HomeCloseBtn& b = gHomeCloseBtn;
    b.visible = false;
    b.isHover = false;
    str::Free(b.filePath);
    b.filePath = {};
    gHomeCloseBtnPaintedRc = {};
}

void HomePageHideCloseButton() {
    HomeCloseBtn& b = gHomeCloseBtn;
    if (!b.visible && gHomeCloseBtnPaintedRc.IsEmpty()) {
        return;
    }
    MainWindow* win = b.win;
    EraseHomeCloseGlyph(win);
    b.visible = false;
    b.isHover = false;
    str::Free(b.filePath);
    b.filePath = {};
}

// compute the button rect (canvas client coords) for a thumbnail link
static Rect HomeCloseBtnRectForThumb(MainWindow* win, const Rect& thumb) {
    int sz = DpiScale(win->hwndCanvas, 18);
    int margin = DpiScale(win->hwndCanvas, 5);
    int bx = IsUIRtl() ? (thumb.x + margin) : (thumb.x + thumb.dx - sz - margin);
    int by = thumb.y + margin;
    return Rect(bx, by, sz, sz);
}

void HomePageUpdateCloseButton(MainWindow* win, int x, int y) {
    if (!win || !CanAccessDisk() || HomePageIsListView()) {
        HomePageHideCloseButton();
        return;
    }
    HomeCloseBtn& b = gHomeCloseBtn;
    Point pt(x, y);

    // already showing a button: update hover state as the mouse moves over /
    // off the glyph, but keep it while the mouse stays on the same thumbnail
    if (b.visible) {
        bool overBtn = b.rc.Contains(pt);
        if (overBtn != b.isHover) {
            b.isHover = overBtn;
            RepaintHomeCloseBtn(win);
        }
        if (b.thumbRc.Contains(pt)) {
            return;
        }
    }

    StaticLink* link = nullptr;
    TempStr target = GetStaticLinkAtTemp(win->staticLinks, x, y, &link);
    // a thumbnail link's target is an absolute file path; everything else (a
    // "<...>" command, a "Cmd..." tip link, a URL) is not, so it gets no button
    bool isThumb = len(target) > 0 && link && path::IsAbsolute(target);
    if (!isThumb) {
        HomePageHideCloseButton();
        return;
    }
    if (b.visible && b.filePath && str::Eq(b.filePath, target)) {
        return; // same thumbnail, nothing to do
    }
    b.win = win;
    b.thumbRc = link->rect;
    b.rc = HomeCloseBtnRectForThumb(win, link->rect);
    str::ReplaceWithCopy(&b.filePath, target);
    b.isHover = b.rc.Contains(pt);
    b.visible = true;
    RepaintHomeCloseBtn(win);
}

// called from a left-button click; if the click is on the close button, remove
// the file and return true so the caller doesn't also open the thumbnail
bool HomePageOnCloseButtonClick(MainWindow* win, int x, int y) {
    HomeCloseBtn& b = gHomeCloseBtn;
    if (!b.visible || !b.rc.Contains(Point(x, y))) {
        return false;
    }
    TempStr path = str::DupTemp(b.filePath);
    HomePageHideCloseButton();
    if (win && len(path) > 0) {
        ForgetFileFromFrequentlyRead(win, path);
    }
    return true;
}

void HomePageOnCanvasMouseLeave() {
    // the button is part of the canvas now, so leaving the canvas always hides it
    HomePageHideCloseButton();
}

// A soft drop shadow, approximated by stacking a few rounded rects that step
// from the page background toward a darker tone. GDI has no blur, and going
// through GDI+ just for this would mean another surface per card.
static void DrawHomeShadow(HDC hdc, const Rect& r, int radius, COLORREF pageBg) {
    constexpr int kLayers = 4;
    for (int i = kLayers; i >= 1; i--) {
        int spread = DpiScale(hdc, i);
        Rect sr = r;
        sr.Inflate(spread, spread);
        sr.y += DpiScale(hdc, 1); // cast downward, like the design's shadows
        // Each layer steps further from the background. AccentColor darkens a
        // light color and lightens a dark one, so the shadow reads on both.
        COLORREF col = AccentColor(pageBg, 5 * (kLayers - i + 1));
        AutoDeleteBrush br = CreateSolidBrush(col);
        AutoDeletePen pen = CreatePen(PS_SOLID, 1, col);
        ScopedSelectObject selBr(hdc, br);
        ScopedSelectObject selPen(hdc, pen);
        int d = std::min((radius + spread) * 2, std::min(sr.dx, sr.dy));
        RoundRect(hdc, sr.x, sr.y, sr.x + sr.dx, sr.y + sr.dy, d, d);
    }
}

// readable text color on top of a filled swatch (the accent Open button)
static COLORREF HomeTextColorOn(COLORREF bg) {
    return IsLightColor(bg) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// filled rounded rect, used for the home chrome (search field, view toggle,
// the Open button). radius is in unscaled px
static void FillHomeRoundRect(HDC hdc, const Rect& r, int radius, COLORREF col, COLORREF borderCol = kColorUnset) {
    int d = DpiScale(hdc, radius) * 2;
    d = std::min(d, std::min(r.dx, r.dy));
    AutoDeleteBrush br = CreateSolidBrush(col);
    // a themed fill can be the same color as what's behind it (the search field
    // on a dark theme); those need the border to be visible at all
    AutoDeletePen pen = CreatePen(PS_SOLID, 1, borderCol == kColorUnset ? col : borderCol);
    ScopedSelectObject selBr(hdc, br);
    ScopedSelectObject selPen(hdc, pen);
    RoundRect(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy, d, d);
}

static void DrawHomeViewButton(HDC hdc, HIMAGELIST himl, Rect r, TbIcon icon, bool selected) {
    if (selected) {
        Rect chip = r;
        chip.Inflate(DpiScale(hdc, 4), DpiScale(hdc, 4));
        FillHomeRoundRect(hdc, chip, 11, ThemeControlBackgroundColor());
    }
    ImageList_Draw(himl, (int)icon, hdc, r.x, r.y, ILD_NORMAL);
}

static Rect FitRectInRect(Size src, Rect dst) {
    if (src.dx <= 0 || src.dy <= 0 || dst.dx <= 0 || dst.dy <= 0) {
        return dst;
    }
    int dx = dst.dx;
    int dy = src.dy * dx / src.dx;
    if (dy > dst.dy) {
        dy = dst.dy;
        dx = src.dx * dy / src.dy;
    }
    Rect r(dst.x + ((dst.dx - dx) / 2), dst.y + ((dst.dy - dy) / 2), dx, dy);
    return r;
}

static TempStr FileSizeForHomeListTemp(i64 size) {
    if (size < 0) {
        return str::DupTemp("");
    }
    return str::FormatSizeShortTemp(size, nullptr);
}

// light blue outline marking the keyboard-selected entry (issue #1136).
// A fixed color: it has to read as "selected" against both the light and the
// dark page background
constexpr COLORREF kHomeSelectionColor = RGB(0x4c, 0xa6, 0xff);

static void DrawHomeSelectionOutline(HDC hdc, const Rect& r, int radius) {
    int penDx = DpiScale(hdc, 2);
    ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, penDx, kHomeSelectionColor), true);
    ScopedSelectObject brush(hdc, GetStockBrush(NULL_BRUSH));
    RoundRect(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy, radius, radius);
}

// Give the file name the width it needs and put the directory path in what's
// left, right-aligned (mirrored for RTL). Done on first paint of a row, not
// during layout: measuring every history entry made layout (and so scrolling)
// slow, and measuring only the rows visible at layout time meant rows scrolled
// into view later never got a directory.
static void MeasureHomeListRowText(HDC hdc, ThumbnailLayout& thumb, HFONT font, bool isRtl) {
    if (thumb.listTextMeasured) {
        return;
    }
    thumb.listTextMeasured = true;

    Rect rcFileName = thumb.rcListFileName;
    TempStr fileName = path::GetBaseNameTemp(thumb.fs->filePath);
    int nameDx = HdcMeasureText(hdc, Str(fileName), font).dx + DpiScale(hdc, 4);
    int minPathDx = DpiScale(hdc, 80);
    if (nameDx + kHomeListRowGapDx + minPathDx > rcFileName.dx) {
        // no room for a path, the name gets the whole span
        return;
    }
    int pathDx = rcFileName.dx - nameDx - kHomeListRowGapDx;
    if (isRtl) {
        thumb.rcListPath = Rect(rcFileName.x, rcFileName.y, pathDx, rcFileName.dy);
        rcFileName.x = rcFileName.x + rcFileName.dx - nameDx;
    } else {
        thumb.rcListPath = Rect(rcFileName.x + nameDx + kHomeListRowGapDx, rcFileName.y, pathDx, rcFileName.dy);
    }
    rcFileName.dx = nameDx;
    thumb.rcListFileName = rcFileName;
}

// True when keyboard focus is in the home search box (hide list selection then).
static bool HomeSearchHasFocus(MainWindow* win) {
    return win && win->hwndHomeSearch && GetFocus() == win->hwndHomeSearch;
}

static void DrawHomeListRow(HomePageLayout& l, ThumbnailLayout& thumb, HFONT fontText, COLORREF backgroundColor,
                            bool isRtl, bool isSelected) {
    HDC hdc = l.hdc;
    FileState* fs = thumb.fs;
    Rect row = thumb.rcListRow;
    if (!IsHomeThumbOnScreen(row, l.rcThumbsArea)) {
        return;
    }
    MeasureHomeListRowText(hdc, thumb, fontText, isRtl);
    // no selection chrome while typing in the search box
    if (isSelected && !HomeSearchHasFocus(l.win)) {
        DrawHomeSelectionOutline(hdc, Rect(row.x, row.y, row.dx, row.dy - 1), 4);
    }

    COLORREF lineCol = AccentColor(ThemeMainWindowBackgroundColor(), 30);
    ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, 1, lineCol), true);
    HdcDrawLine(hdc, Rect(row.x, row.y + row.dy - 1, row.dx, 0));

    // LoadThumbnail only hits disk the first time; result stays on fs->thumbnail
    RenderedBitmap* thumbImg = LoadThumbnail(fs);
    Rect thumbBox = thumb.rcListThumb;
    if (thumbImg) {
        Rect thumbDst = FitRectInRect(thumbImg->GetSize(), thumbBox);
        thumbImg->Blit(hdc, thumbDst);
        thumb.szThumb = thumbImg->GetSize();
    }
    Str path = fs->filePath;
    TempStr fileName = path::GetBaseNameTemp(path);
    UINT nameFmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_RIGHT : DT_LEFT);
    SelectObject(hdc, fontText);
    {
        DrawMaybeHighlightedText(hdc, thumb.rcListFileName, fileName, l.filterWords, l.highlighted, backgroundColor,
                                 isRtl, false, nameFmt);
    }

    // directory path, right-aligned and muted, in the space the file name doesn't need.
    // Must use DrawTextW: dirPath is UTF-8; DrawTextA treated it as the system ANSI
    // code page and mangled non-ASCII path characters (#5824).
    if (!thumb.rcListPath.IsEmpty()) {
        TempStr dirPath = path::GetDirTemp(path);
        SetTextColor(hdc, ThemeWindowTextDisabledColor());
        UINT pathFmt = DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_LEFT : DT_RIGHT);
        Rect pathRect = thumb.rcListPath;
        HdcDrawText(hdc, dirPath, pathRect, pathFmt);
    }

    // file::GetSize once per row, then cache on ThumbnailLayout (scroll reuses
    // it). kSizeNotFetched = not tried; kSizeFetchFail = GetSize failed; >= 0
    // is a real size (including empty files).
    if (thumb.fileSize == kSizeNotFetched) {
        i64 sz = file::GetSize(path);
        thumb.fileSize = (sz < 0) ? kSizeFetchFail : sz;
    }
    TempStr fileSize = FileSizeForHomeListTemp(thumb.fileSize);
    SetTextColor(hdc, ThemeWindowTextColor());
    UINT sizeFmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_LEFT : DT_RIGHT);
    Rect sizeRect = thumb.rcListSize;
    HdcDrawText(hdc, fileSize, sizeRect, sizeFmt);

    ImageList_Draw(l.himlOpen, (int)TbIcon::Close, hdc, thumb.rcListRemove.x, thumb.rcListRemove.y, ILD_NORMAL);
    if (fs->isPinned) {
        HdcFillRect(hdc, thumb.rcListPin, ThemeControlBackgroundColor());
    }
    ImageList_Draw(l.himlOpen, (int)TbIcon::Pin, hdc, thumb.rcListPin.x, thumb.rcListPin.y, ILD_NORMAL);
}

static void DrawHomePageLayout(HomePageLayout& l) {
    bool isRtl = IsUIRtl();
    auto* hdc = l.hdc;
    auto* win = l.win;
    auto backgroundColor = ThemeMainWindowBackgroundColor();

    {
        Rect rc = HwndClientRect(win->hwndCanvas);
        auto color = ThemeMainWindowBackgroundColor();
        HdcFillRect(hdc, rc, color);
    }

    // draw search edit border and background on the canvas
    {
        COLORREF bgCol = ThemeControlBackgroundColor();
        const Rect& sb = l.rcSearchBorder;
        // a pill, per the redesign. The edit control itself is inset far enough
        // that the rounded ends stay clear of the text
        FillHomeRoundRect(hdc, sb, sb.dy / 2, bgCol, AccentColor(bgCol, 40));
    }

    if (false) {
        const Rect& r = l.rcAppWithVer;
        DrawSumatraVersion(hdc, r);
    }

    auto color = ThemeWindowTextColor();
    if (false) {
        ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, 1, color), true);
        HdcDrawLine(hdc, l.rcLine);
    }
    HFONT fontText = HdcGetUiFont(hdc, kFontSizeLabel, kFontWeightStrong);

    AutoDeletePen penThumbBorder(CreatePen(PS_SOLID, kThumbsBorderDx, ThemeEdgeColor()));
    color = ThemeWindowLinkColor();
    AutoDeletePen penLinkLine(CreatePen(PS_SOLID, 1, color));

    SelectObject(hdc, penThumbBorder);
    SetBkMode(hdc, TRANSPARENT);
    color = ThemeWindowTextColor();
    SetTextColor(hdc, color);

    {
        // segmented control: one track, the active button raised on top of it
        Rect track = l.rcIconThumbnailView.Union(l.rcIconListView);
        track.Inflate(DpiScale(hdc, 7), DpiScale(hdc, 7));
        FillHomeRoundRect(hdc, track, 14, AccentColor(ThemeControlBackgroundColor(), 25));
    }
    DrawHomeViewButton(hdc, l.himlOpen, l.rcIconThumbnailView, TbIcon::HomeThumbnails, !HomePageIsListView());
    DrawHomeViewButton(hdc, l.himlOpen, l.rcIconListView, TbIcon::HomeList, HomePageIsListView());
    l.freqRead->Paint(hdc);
    SelectObject(hdc, GetStockBrush(NULL_BRUSH));

    // clip thumbnails to the middle area
    {
        const Rect& ta = l.rcThumbsArea;
        HRGN thumbsClip = CreateRectRgn(ta.x, ta.y, ta.x + ta.dx, ta.y + ta.dy);
        SelectClipRgn(hdc, thumbsClip);
        DeleteObject(thumbsClip);
    }

    int nThumbs = len(l.thumbnails);
    // keep the keyboard selection inside the (possibly filtered) list
    if (win->homePageSelIdx >= nThumbs) {
        win->homePageSelIdx = nThumbs - 1;
    }
    // no selection rectangle while the search box has focus
    const bool showKeyboardSel = !HomeSearchHasFocus(win);
    // draw selection after restoring the thumbs clip so the outline on the
    // first row isn't clipped at the top edge of rcThumbsArea
    Rect pendingThumbSel;
    bool hasPendingThumbSel = false;
    // budget for re-rendering thumbnails cached at an older card size
    constexpr int kMaxThumbRegenPerPaint = 2;
    int nRegenerated = 0;
    for (int thumbIdx = 0; thumbIdx < nThumbs; thumbIdx++) {
        ThumbnailLayout& thumb = l.thumbnails[thumbIdx];
        FileState* fs = thumb.fs;
        bool isSelected = showKeyboardSel && (thumbIdx == win->homePageSelIdx);
        if (HomePageIsListView()) {
            DrawHomeListRow(l, thumb, fontText, backgroundColor, isRtl, isSelected);
            continue;
        }
        const Rect& page = thumb.rcPage;
        // skip off-screen thumbs (scroll was redoing Blit+text for every history
        // entry and dominated CPU in DrawHomePageLayout)
        if (!IsHomeThumbOnScreen(page.Union(thumb.rcText), l.rcThumbsArea)) {
            continue;
        }

        DrawHomeShadow(hdc, page, 10, backgroundColor);

        // disk load only first time; stays on fs->thumbnail afterwards
        RenderedBitmap* thumbImg = LoadThumbnail(fs);
        if (thumbImg && thumbImg->GetSize().dx != kThumbnailRenderDx && nRegenerated < kMaxThumbRegenPerPaint) {
            // cached at a different card size (the card geometry changed):
            // drop it and re-render in the background. Rate-limited so opening
            // Home with a long history doesn't start a render storm. Dropping
            // it first means the stale branch can't run again for this file,
            // so a file that fails to render is simply left without one.
            nRegenerated++;
            RemoveThumbnail(fs);
            CreateThumbnailFromFileAsync(fs);
            thumbImg = nullptr;
        }
        if (thumbImg) {
            thumb.szThumb = thumbImg->GetSize();
            // the card is a surface and the page sits on it, inset, as in the
            // redesign - rather than the image filling the card edge to edge
            AutoDeleteBrush brCard = CreateSolidBrush(ThemeControlBackgroundColor());
            ScopedSelectObject selCard(hdc, brCard);
            RoundRect(hdc, page.x, page.y, page.x + page.dx, page.y + page.dy, 10, 10);
            SelectObject(hdc, GetStockBrush(NULL_BRUSH));

            int inset = DpiScale(hdc, 10);
            Rect innerBox = page;
            innerBox.Inflate(-inset, -inset);
            Rect inner = FitRectInRect(thumb.szThumb, innerBox);
            thumbImg->Blit(hdc, inner);
        }
        if (!thumbImg) {
            // no thumbnail yet: the card is still a surface, not a hole
            AutoDeleteBrush brCard = CreateSolidBrush(ThemeControlBackgroundColor());
            ScopedSelectObject selCard(hdc, brCard);
            RoundRect(hdc, page.x, page.y, page.x + page.dx, page.y + page.dy, 10, 10);
            SelectObject(hdc, GetStockBrush(NULL_BRUSH));
        } else {
            RoundRect(hdc, page.x, page.y, page.x + page.dx, page.y + page.dy, 10, 10);
        }

        const Rect& rect = thumb.rcText;
        Str path = fs->filePath;
        TempStr fileName = path::GetBaseNameTemp(path);
        UINT fmt = DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_RIGHT : DT_LEFT);

        SelectObject(hdc, fontText);
        {
            DrawMaybeHighlightedText(hdc, rect, fileName, l.filterWords, l.highlighted, backgroundColor, isRtl, false,
                                     fmt);
        }

        GetFileStateIcon(fs);
        int x = isRtl ? page.x + page.dx - DpiScale(hdc, 16) : page.x;
        ImageList_Draw(fs->himl, fs->iconIdx, hdc, x, rect.y, ILD_TRANSPARENT);

        // second line: file size, in the muted text color
        if (thumb.fileSize == kSizeNotFetched) {
            thumb.fileSize = file::GetSize(fs->filePath);
            if (thumb.fileSize < 0) {
                thumb.fileSize = kSizeFetchFail;
            }
        }
        TempStr meta = FileSizeForHomeListTemp(thumb.fileSize);
        if (!str::IsEmptyOrWhiteSpace(meta)) {
            Rect rcMeta = rect;
            rcMeta.y += rect.dy;
            COLORREF prevCol = SetTextColor(hdc, ThemeWindowDarkerTextColor());
            // meta is a size down from the name, per the type scale
            HFONT fontMeta = HdcGetUiFont(hdc, kFontSizeMeta);
            HdcDrawText(hdc, meta, rcMeta, fmt, fontMeta);
            SelectObject(hdc, fontText);
            SetTextColor(hdc, prevCol);
        }

        // pin badge: a filled circle so the icon reads over the page image
        if (!thumb.rcListPin.IsEmpty()) {
            const Rect& badge = thumb.rcListPin;
            COLORREF badgeBg = ThemeControlBackgroundColor();
            FillHomeRoundRect(hdc, badge, badge.dy / 2, badgeBg);
            COLORREF pinCol = fs->isPinned ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor();
            TbIcon badgeIcon = fs->isPinned ? TbIcon::Pin : TbIcon::Close;
            int pinDy = DpiScale(hdc, 16);
            HIMAGELIST pinIml = GetTintedToolbarImageList(pinDy, pinCol, badgeBg);
            if (pinIml) {
                int px = badge.x + ((badge.dx - pinDy) / 2);
                int py = badge.y + ((badge.dy - pinDy) / 2);
                ImageList_Draw(pinIml, (int)badgeIcon, hdc, px, py, ILD_NORMAL);
            }
        }

        if (isSelected) {
            Rect sel = page.Union(rect);
            sel.Inflate(DpiScale(hdc, 4), DpiScale(hdc, 3));
            pendingThumbSel = sel;
            hasPendingThumbSel = true;
        }
    }

    // restore full clip region
    SelectClipRgn(hdc, nullptr);

    if (hasPendingThumbSel) {
        DrawHomeSelectionOutline(hdc, pendingThumbSel, 10);
    }

    color = ThemeWindowLinkColor();
    SetTextColor(hdc, color);
    SelectObject(hdc, penLinkLine);

    // primary action: a filled accent pill around the icon and the label
    COLORREF accentCol = ThemeWindowLinkColor();
    COLORREF onAccent = HomeTextColorOn(accentCol);
    FillHomeRoundRect(hdc, l.rcOpenBtn, l.rcOpenBtn.dy / 2, accentCol);

    int x = l.rcIconOpen.x;
    int y = l.rcIconOpen.y;
    int openIconIdx = (int)TbIcon::Open;
    // the shared toolbar icons are baked against the toolbar background, so on
    // the accent fill they'd carry a light box around them
    HIMAGELIST himlOnAccent = GetTintedToolbarImageList(l.rcIconOpen.dy, onAccent, accentCol);
    ImageList_Draw(himlOnAccent ? himlOnAccent : l.himlOpen, openIconIdx, hdc, x, y, ILD_NORMAL);

    // the label sits on the accent fill, so it can't use the link color
    l.openDoc->textColor = onAccent;
    l.openDoc->withUnderline = false;
    l.openDoc->Paint(hdc);
    if (l.browseFolder) {
        l.browseFolder->Paint(hdc);
    }

    if (false) {
        Rect rcFreqRead = DrawHideFrequentlyReadLink(win->hwndCanvas, hdc, _TRA("Hide frequently read"));
        auto* sl = new StaticLink(rcFreqRead, kLinkHideList);
        win->staticLinks.Append(sl);
    }

    // draw tip at the bottom
    if (l.tip) {
        COLORREF tipBgCol = ThemeControlBackgroundColor();
        HdcFillRect(hdc, l.rcTip, tipBgCol);

        HFONT fontTip = HdcGetUiFont(hdc, kFontSizeBody);
        COLORREF textCol = ThemeWindowTextColor();
        COLORREF linkCol = ThemeWindowLinkColor();
        DrawTipWords(hdc, *l.tip, fontTip, textCol, linkCol);
    }
}

static WindowTab* FindTouchOpenTabByPath(Str filePath) {
    for (MainWindow* candidateWin : gWindows) {
        for (WindowTab* tab : candidateWin->Tabs()) {
            if (tab && !tab->IsNonDocumentTab() && path::IsSame(tab->filePath, filePath)) {
                return tab;
            }
        }
    }
    return nullptr;
}

static void DrawTouchFileCardPath(MainWindow* win, HDC hdc, Str filePath, FileState* fs,
                                  RenderedBitmap* explicitThumbnail, const Rect& card, bool showProgress,
                                  const Rect* linkClip = nullptr, bool twoLineName = false, bool pinnable = false) {
    COLORREF pageBg = ThemeWindowControlBackgroundColor();
    DrawHomeShadow(hdc, card, DpiScale(hdc, 10), pageBg);
    FillHomeRoundRect(hdc, card, DpiScale(hdc, 10), RGB(255, 255, 255), ThemeEdgeColor());

    Rect thumbRc = card;
    thumbRc.Inflate(-DpiScale(hdc, 14), -DpiScale(hdc, 14));
    RenderedBitmap* thumb = explicitThumbnail ? explicitThumbnail : (fs ? LoadThumbnail(fs) : nullptr);
    if (thumb) {
        Rect dst = FitRectInRect(thumb->GetSize(), thumbRc);
        thumb->Blit(hdc, dst);
    } else {
        FillHomeRoundRect(hdc, thumbRc, DpiScale(hdc, 6), RGB(234, 229, 222));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("page"), thumbRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 12));
        if (fs) {
            CreateThumbnailFromFileAsync(fs);
        }
    }

    if (showProgress && fs) {
        WindowTab* openTab = FindTouchOpenTabByPath(filePath);
        int pageNo = openTab && openTab->ctrl ? openTab->ctrl->CurrentPageNo() : std::max(1, fs->pageNo);
        int pageCount = openTab && openTab->ctrl ? openTab->ctrl->PageCount() : 0;
        TempStr progress =
            pageCount > 0 ? fmt("%d%%", std::clamp(pageNo * 100 / pageCount, 1, 100)) : fmt("p. %d", pageNo);
        Rect chip{card.x + DpiScale(hdc, 10), card.y + card.dy - DpiScale(hdc, 32), DpiScale(hdc, 44),
                  DpiScale(hdc, 22)};
        FillHomeRoundRect(hdc, chip, chip.dy / 2, RGB(43, 43, 43));
        SetTextColor(hdc, RGB(255, 255, 255));
        HdcDrawTextTabular(hdc, progress, chip, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                           HdcGetUiFont(hdc, 11, FW_SEMIBOLD));
    }

    TempStr name = path::GetBaseNameTemp(filePath);
    Rect nameRc{card.x, card.y + card.dy + DpiScale(hdc, 9), card.dx, DpiScale(hdc, twoLineName ? 38 : 19)};
    SetTextColor(hdc, ThemeWindowTextColor());
    UINT nameFlags = DT_END_ELLIPSIS | DT_NOPREFIX;
    nameFlags |= twoLineName ? DT_WORDBREAK : DT_SINGLELINE;
    HdcDrawText(hdc, name, nameRc, nameFlags, HdcGetUiFont(hdc, 13, FW_SEMIBOLD));

    i64 size = file::GetSize(filePath);
    TempStr meta = size >= 0 ? str::FormatSizeShortTemp(size, nullptr) : str::DupTemp("");
    if (showProgress && fs) {
        WindowTab* openTab = FindTouchOpenTabByPath(filePath);
        int pageNo = openTab && openTab->ctrl ? openTab->ctrl->CurrentPageNo() : std::max(1, fs->pageNo);
        int pageCount = openTab && openTab->ctrl ? openTab->ctrl->PageCount() : 0;
        meta = pageCount > 0 ? fmt("%d / %d · %s", pageNo, pageCount, meta) : fmt("Page %d · %s", pageNo, meta);
    }
    Rect metaRc{nameRc.x, nameRc.y + nameRc.dy + DpiScale(hdc, 1), nameRc.dx, DpiScale(hdc, 17)};
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, meta, metaRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 12));

    // Pin badge (Library grid): a small tap target in the card's top-right that
    // pins the file into the Recent view. Appended before the whole-card link so
    // the first-match hit test in GetStaticLinkAtTemp lets the badge win.
    if (pinnable) {
        bool isPinned = fs && fs->isPinned;
        int badgeDx = DpiScale(hdc, 28);
        Rect badge{card.x + card.dx - badgeDx - DpiScale(hdc, 6), card.y + DpiScale(hdc, 6), badgeDx, badgeDx};
        COLORREF badgeBg = ThemeControlBackgroundColor();
        FillHomeRoundRect(hdc, badge, badge.dy / 2, badgeBg);
        COLORREF pinCol = isPinned ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor();
        int pinDy = DpiScale(hdc, 16);
        HIMAGELIST pinIml = GetTintedToolbarImageList(pinDy, pinCol, badgeBg);
        if (pinIml) {
            int px = badge.x + ((badge.dx - pinDy) / 2);
            int py = badge.y + ((badge.dy - pinDy) / 2);
            ImageList_Draw(pinIml, (int)TbIcon::Pin, hdc, px, py, ILD_NORMAL);
        }
        Rect badgeLink = linkClip ? badge.Intersect(*linkClip) : badge;
        if (!badgeLink.IsEmpty()) {
            TempStr pinTarget = str::JoinTemp(kLinkHomePinFilePrefix, filePath);
            Str pinTip = isPinned ? _TRA("Unpin") : _TRA("Pin");
            win->staticLinks.Append(new StaticLink(badgeLink, pinTarget, pinTip));
        }
    }

    Rect linkRc = card.Union(metaRc);
    if (linkClip) {
        linkRc = linkRc.Intersect(*linkClip);
    }
    if (!linkRc.IsEmpty()) {
        win->staticLinks.Append(new StaticLink(linkRc, filePath, HomeThumbTooltipTemp(filePath)));
    }
}

static void DrawTouchFileCard(MainWindow* win, HDC hdc, FileState* fs, const Rect& card, bool showProgress) {
    DrawTouchFileCardPath(win, hdc, fs->filePath, fs, nullptr, card, showProgress);
}

static int TouchCardColumns(HWND hwnd, int width) {
    int cardDx = DpiScale(hwnd, 148);
    int gap = DpiScale(hwnd, 20);
    return std::max(1, (width + gap) / (cardDx + gap));
}

static int TouchCardRows(int count, int columns) {
    return count == 0 ? 0 : ((count + columns - 1) / columns);
}

bool HandleTouchHomeLink(MainWindow* win, Str url) {
    if (!win) {
        return false;
    }
    if (str::TrimPrefix(url, kLinkHomeOpenTabPrefix)) {
        SetTouchDocumentTab(win, ParseInt(url));
        return true;
    }
    if (!str::TrimPrefix(url, kLinkHomeCloseTabPrefix)) {
        return false;
    }
    int tabIndex = ParseInt(url);
    WindowTab* tab = win->GetTab(tabIndex);
    if (!tab || tab->IsNonDocumentTab()) {
        return true;
    }
    int documentCount = 0;
    for (WindowTab* candidate : win->Tabs()) {
        if (candidate && !candidate->IsNonDocumentTab()) {
            documentCount++;
        }
    }
    if (documentCount > 1) {
        CloseTab(tab, false);
        win->RedrawAll(true);
    }
    return true;
}

struct TouchHomeTabThumbnailRequest {
    WindowTab* tab = nullptr;
    int pageNo = 1;
};

static void TouchHomeTabThumbnailFinished(TouchHomeTabThumbnailRequest* request, RenderedBitmap* thumbnail) {
    WindowTab* tab = request->tab;
    if (IsWindowTabValid(tab)) {
        tab->touchHomeThumbnailRequested = false;
        if (tab->ctrl && tab->ctrl->CurrentPageNo() == request->pageNo) {
            delete tab->touchHomeThumbnail;
            tab->touchHomeThumbnail = thumbnail;
            tab->touchHomeThumbnailPage = request->pageNo;
            thumbnail = nullptr;
        }
        if (tab->win && tab->win->hwndCanvas) {
            HwndInvalidate(tab->win->hwndCanvas, false);
        }
    }
    delete thumbnail;
    delete request;
}

static void DrawTouchOpenTabCard(MainWindow* win, HDC hdc, WindowTab* tab, int tabIndex, const Rect& card) {
    COLORREF paper = ThemeWindowControlBackgroundColor();
    DrawHomeShadow(hdc, card, DpiScale(hdc, 12), paper);
    FillHomeRoundRect(hdc, card, DpiScale(hdc, 12), paper, ThemeEdgeColor());
    int titleDy = DpiScale(hdc, 30);
    Rect title{card.x, card.y, card.dx, titleDy};
    FillHomeRoundRect(hdc, title, DpiScale(hdc, 12), RGB(43, 43, 43));
    HdcFillRect(hdc, Rect{title.x, title.y + title.dy / 2, title.dx, title.dy / 2}, RGB(43, 43, 43));
    Rect close{title.x + title.dx - DpiScale(hdc, 26), title.y + DpiScale(hdc, 7), DpiScale(hdc, 16),
               DpiScale(hdc, 16)};
    int pdfDx = DpiScale(hdc, 13);
    Rect pdf{title.x + DpiScale(hdc, 10), title.y + (title.dy - pdfDx) / 2, pdfDx, pdfDx};
    Rect nameRect{pdf.x + pdf.dx + DpiScale(hdc, 8), title.y, close.x - pdf.x - pdf.dx - DpiScale(hdc, 10), title.dy};
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HdcDrawText(hdc, tab->GetTabTitle(), nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                HdcGetUiFont(hdc, 12, FW_MEDIUM));
    SetTextColor(hdc, RGB(210, 210, 210));
    HdcDrawText(hdc, StrL("×"), close, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 14, FW_MEDIUM));
    FillHomeRoundRect(hdc, pdf, DpiScale(hdc, 2), RgbToCOLORREF(0xe8927c));

    Rect thumb{card.x, card.y + titleDy, card.dx, card.dy - titleDy};
    FillHomeRoundRect(hdc, thumb, DpiScale(hdc, 6), RGB(234, 229, 222));
    int pageNo = tab->ctrl ? tab->ctrl->CurrentPageNo() : 1;
    if (tab->touchHomeThumbnailPage != pageNo) {
        delete tab->touchHomeThumbnail;
        tab->touchHomeThumbnail = nullptr;
        tab->touchHomeThumbnailPage = pageNo;
    }
    if (!tab->touchHomeThumbnail && !tab->touchHomeThumbnailRequested && tab->filePath) {
        tab->touchHomeThumbnailRequested = true;
        auto* request = new TouchHomeTabThumbnailRequest{tab, pageNo};
        auto* onRendered = NewFunc1(TouchHomeTabThumbnailFinished, request);
        CreateThumbnailFromFileAsync(tab->filePath, pageNo, onRendered);
    }
    RenderedBitmap* bitmap = tab->touchHomeThumbnail;
    if (bitmap) {
        Rect inset = thumb;
        inset.Inflate(-DpiScale(hdc, 10), -DpiScale(hdc, 10));
        bitmap->Blit(hdc, FitRectInRect(bitmap->GetSize(), inset));
    }
    win->staticLinks.Append(
        new StaticLink(close, fmt("%s%d", Str(kLinkHomeCloseTabPrefix), tabIndex), StrL("Close tab")));
    win->staticLinks.Append(new StaticLink(card, fmt("%s%d", Str(kLinkHomeOpenTabPrefix), tabIndex)));
}

static void DrawTouchHomePage(MainWindow* win, HDC hdc) {
    Rect rc = HwndClientRect(win->hwndCanvas);
    HdcFillRect(hdc, rc, ThemeWindowControlBackgroundColor());
    int headerDy = DpiScale(hdc, 64);
    HdcFillRect(hdc, Rect{0, 0, rc.dx, headerDy}, ThemeWindowControlBackgroundColor());
    HdcFillRect(hdc, Rect{0, headerDy - 1, rc.dx, 1}, ThemeEdgeColor());

    Rect title{DpiScale(hdc, 24), 0, DpiScale(hdc, 112), headerDy};
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, StrL("Recent files"), title, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 18, FW_SEMIBOLD));

    Rect open{title.x + title.dx + DpiScale(hdc, 12), DpiScale(hdc, 12), DpiScale(hdc, 112), DpiScale(hdc, 40)};
    FillHomeRoundRect(hdc, open, open.dy / 2, RgbToCOLORREF(0xb4530a));
    SetTextColor(hdc, RGB(255, 255, 255));
    int openIconDy = DpiScale(hdc, 16);
    HIMAGELIST openIcons = GetTintedToolbarImageList(openIconDy, RGB(255, 255, 255), RgbToCOLORREF(0xb4530a));
    if (openIcons) {
        ImageList_Draw(openIcons, (int)TbIcon::Open, hdc, open.x + DpiScale(hdc, 15),
                       open.y + (open.dy - openIconDy) / 2, ILD_NORMAL);
    }
    Rect openText{open.x + DpiScale(hdc, 38), open.y, open.dx - DpiScale(hdc, 48), open.dy};
    HdcDrawText(hdc, StrL("Open file"), openText, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    win->staticLinks.Append(new StaticLink(open, Str(kLinkOpenFile)));

    EnsureHomeSearchCreated(win);
    int searchDx = DpiScale(hdc, kHomeSearchMinDx);
    Rect search{(rc.dx - searchDx) / 2, DpiScale(hdc, 12), searchDx, DpiScale(hdc, 40)};
    FillHomeRoundRect(hdc, search, search.dy / 2, ThemeHotBackgroundColor());
    int iconDy = DpiScale(hdc, 16);
    HIMAGELIST iml = GetTintedToolbarImageList(iconDy, ThemeWindowDarkerTextColor(), ThemeHotBackgroundColor());
    if (iml) {
        ImageList_Draw(iml, (int)TbIcon::Search, hdc, search.x + DpiScale(hdc, 14), search.y + (search.dy - iconDy) / 2,
                       ILD_NORMAL);
    }
    MoveWindow(win->hwndHomeSearch, search.x + DpiScale(hdc, 38), search.y + DpiScale(hdc, 4),
               search.dx - DpiScale(hdc, 50), search.dy - DpiScale(hdc, 8), TRUE);
    HwndShow(win->hwndHomeSearch);

    Vec<FileState*> files;
    StrVec filterWords;
    CollectHomePageFiles(win, files, filterWords);
    Vec<FileState*> pinned;
    Vec<FileState*> recent;
    for (FileState* fs : files) {
        (fs->isPinned ? pinned : recent).Append(fs);
    }

    int pad = DpiScale(hdc, 24);
    int gap = DpiScale(hdc, 20);
    int cardDx = DpiScale(hdc, 148);
    int cardDy = DpiScale(hdc, 196);
    int cardBlockDy = cardDy + DpiScale(hdc, 50);
    Vec<int> openTabIndexes;
    for (int i = 0; i < win->TabCount(); i++) {
        WindowTab* tab = win->GetTab(i);
        if (tab && !tab->IsNonDocumentTab()) {
            openTabIndexes.Append(i);
        }
    }
    int openSectionDy = len(openTabIndexes) > 0 ? DpiScale(hdc, 18 + 14 + 170 + 28) : 0;
    int columns = TouchCardColumns(win->hwndCanvas, rc.dx - 2 * pad);
    int pinnedRows = TouchCardRows(len(pinned), columns);
    int recentRows = TouchCardRows(len(recent), columns);
    int contentDy = pad + openSectionDy +
                    (pinnedRows ? DpiScale(hdc, 31) + pinnedRows * cardBlockDy + (pinnedRows - 1) * gap + gap : 0) +
                    DpiScale(hdc, 31) + recentRows * cardBlockDy + std::max(0, recentRows - 1) * gap + pad;
    int viewportDy = std::max(0, rc.dy - headerDy);
    win->homePageScrollY = std::clamp(win->homePageScrollY, 0, std::max(0, contentDy - viewportDy));

    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, 0, headerDy, rc.dx, rc.dy);
    int y = headerDy + pad - win->homePageScrollY;
    if (len(openTabIndexes) > 0) {
        Rect labelRc{pad, y, rc.dx - 2 * pad, DpiScale(hdc, 18)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("CURRENTLY OPEN"), labelRc, DT_SINGLELINE | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
        y += DpiScale(hdc, 32);
        int openCardDx = DpiScale(hdc, 190);
        int openCardDy = DpiScale(hdc, 170);
        int openGap = DpiScale(hdc, 16);
        int openContentDx = len(openTabIndexes) * openCardDx + std::max(0, len(openTabIndexes) - 1) * openGap;
        int openViewportDx = std::max(0, rc.dx - 2 * pad);
        win->homeOpenScrollMaxX = std::max(0, openContentDx - openViewportDx);
        win->homeOpenScrollX = std::clamp(win->homeOpenScrollX, 0, win->homeOpenScrollMaxX);
        win->homeOpenCarouselRect = {pad, y, openViewportDx, openCardDy + DpiScale(hdc, 4)};
        int carouselDc = SaveDC(hdc);
        IntersectClipRect(hdc, win->homeOpenCarouselRect.x, win->homeOpenCarouselRect.y,
                          win->homeOpenCarouselRect.x + win->homeOpenCarouselRect.dx,
                          win->homeOpenCarouselRect.y + win->homeOpenCarouselRect.dy);
        for (int i = 0; i < len(openTabIndexes); i++) {
            int tabIndex = openTabIndexes[i];
            Rect card{pad - win->homeOpenScrollX + i * (openCardDx + openGap), y, openCardDx, openCardDy};
            if (card.x < pad + openViewportDx && card.x + card.dx > pad) {
                DrawTouchOpenTabCard(win, hdc, win->GetTab(tabIndex), tabIndex, card);
            }
        }
        RestoreDC(hdc, carouselDc);
        y += openCardDy + DpiScale(hdc, 28);
    } else {
        win->homeOpenScrollX = 0;
        win->homeOpenScrollMaxX = 0;
        win->homeOpenCarouselRect = {};
    }
    auto drawGroup = [&](Str label, const Vec<FileState*>& group, bool showProgress) {
        if (len(group) == 0 && str::Eq(label, StrL("PINNED"))) {
            return;
        }
        Rect labelRc{pad, y, rc.dx - 2 * pad, DpiScale(hdc, 18)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, label, labelRc, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
        y += DpiScale(hdc, 31);
        for (int i = 0; i < len(group); i++) {
            int col = i % columns;
            int row = i / columns;
            Rect card{pad + col * (cardDx + gap), y + row * (cardBlockDy + gap), cardDx, cardDy};
            if (card.y + cardBlockDy >= headerDy && card.y < rc.dy) {
                DrawTouchFileCard(win, hdc, group[i], card, showProgress);
            }
        }
        int rows = TouchCardRows(len(group), columns);
        y += rows * cardBlockDy + std::max(0, rows - 1) * gap + gap;
    };
    drawGroup(StrL("PINNED"), pinned, false);
    drawGroup(StrL("RECENT"), recent, true);
    RestoreDC(hdc, saved);
}

struct TouchLibraryFolderData {
    int parent = -1;
    int depth = 0;
    int directCount = 0;
    int directFolderCount = 0;
    bool hasChildren = false;
};

struct TouchLibraryFileData {
    RenderedBitmap* thumbnail = nullptr;
    bool thumbnailRequested = false;
};

static StrVecWithData<TouchLibraryFolderData> gTouchLibraryFolders;
static StrVecWithData<TouchLibraryFileData> gTouchLibraryFiles;
static bool gTouchLibraryValid = false;

struct TouchLibraryThumbnailRequest {
    Str filePath;
    ~TouchLibraryThumbnailRequest() { str::Free(filePath); }
};

static void TouchLibraryThumbnailFinished(TouchLibraryThumbnailRequest* request, RenderedBitmap* thumbnail) {
    int fileIdx = gTouchLibraryFiles.FindI(request->filePath);
    if (fileIdx >= 0 && thumbnail) {
        TouchLibraryFileData* fileData = gTouchLibraryFiles.AtData(fileIdx);
        delete fileData->thumbnail;
        fileData->thumbnail = thumbnail;
        thumbnail = nullptr;
        for (MainWindow* win : gWindows) {
            if (win->hwndCanvas && win->touchView == TouchView::Library) {
                HwndInvalidate(win->hwndCanvas, false);
            }
        }
    }
    delete thumbnail;
    delete request;
}

void HomePageInvalidateLibrary() {
    gTouchLibraryFolders.Reset();
    for (int i = 0; i < len(gTouchLibraryFiles); i++) {
        delete gTouchLibraryFiles.AtData(i)->thumbnail;
    }
    gTouchLibraryFiles.Reset();
    gTouchLibraryValid = false;
}

static bool TouchLibraryCanOpenFile(Str filePath) {
    FileType kind = GuessFileTypeFromName(filePath, true);
    return IsSupportedFileType(kind, true) || DocIsSupportedFileType(kind);
}

static void AppendTouchLibraryFolder(StrVecWithData<TouchLibraryFolderData>& folders, Str path) {
    if (len(path) > 0 && folders.FindI(path) < 0) {
        folders.Append(path, TouchLibraryFolderData{});
    }
}

static void AppendTouchLibraryFile(StrVecWithData<TouchLibraryFileData>& files, Str path) {
    if (len(path) > 0 && files.FindI(path) < 0) {
        files.Append(path, TouchLibraryFileData{});
    }
}

static void ScanTouchLibraryRoot(Str root, StrVecWithData<TouchLibraryFolderData>& folders,
                                 StrVecWithData<TouchLibraryFileData>& files) {
    TempStr normalized = path::NormalizeTemp(root);
    if (!normalized || !dir::Exists(normalized)) {
        return;
    }
    AppendTouchLibraryFolder(folders, normalized);
    DirIter di{normalized};
    di.includeFiles = true;
    di.includeDirs = true;
    di.recurse = true;
    for (DirIterEntry* de : di) {
        if (IsDirectory(de)) {
            AppendTouchLibraryFolder(folders, de->filePath);
        } else if (IsRegularFile(de) && TouchLibraryCanOpenFile(de->filePath)) {
            AppendTouchLibraryFile(files, de->filePath);
        }
    }
}

static void FinishTouchLibraryModel(StrVecWithData<TouchLibraryFolderData>& folders,
                                    StrVecWithData<TouchLibraryFileData>& files) {
    SortNatural(&folders);
    SortNatural(&files);
    for (int i = 0; i < len(folders); i++) {
        *folders.AtData(i) = TouchLibraryFolderData{};
    }
    for (Str filePath : files) {
        int folder = folders.FindI(path::GetDirTemp(filePath));
        if (folder >= 0) {
            folders.AtData(folder)->directCount++;
        }
    }
    for (int i = 0; i < len(folders); i++) {
        TempStr parentPath = path::GetDirTemp(folders[i]);
        int parent = folders.FindI(parentPath);
        if (parent >= 0 && parent != i) {
            folders.AtData(i)->parent = parent;
            folders.AtData(parent)->hasChildren = true;
            folders.AtData(parent)->directFolderCount++;
        }
    }
    for (int i = 0; i < len(folders); i++) {
        int depth = 0;
        int parent = folders.AtData(i)->parent;
        while (parent >= 0 && depth < len(folders)) {
            depth++;
            parent = folders.AtData(parent)->parent;
        }
        folders.AtData(i)->depth = depth;
    }
}

static void BuildTouchLibraryModel() {
    HomePageInvalidateLibrary();
    Vec<Str>* roots = gGlobalPrefs->libraryFolders;
    if (roots && len(*roots) > 0) {
        for (Str root : *roots) {
            ScanTouchLibraryRoot(root, gTouchLibraryFolders, gTouchLibraryFiles);
        }
    }
    FinishTouchLibraryModel(gTouchLibraryFolders, gTouchLibraryFiles);
    gTouchLibraryValid = true;
}

static TempStr PickTouchLibraryFolderTemp(MainWindow* win) {
    if (!CanAccessDisk()) {
        return {};
    }
    ScopedComPtr<IFileOpenDialog> dlg;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        return {};
    }
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(CWStrTemp(_TRA("Add folder to Library")));
    if (win->librarySelectedFolderPath) {
        ScopedComPtr<IShellItem> initial;
        hr = SHCreateItemFromParsingName(CWStrTemp(win->librarySelectedFolderPath), nullptr, IID_PPV_ARGS(&initial));
        if (SUCCEEDED(hr) && initial) {
            dlg->SetFolder(initial);
        }
    }
    hr = dlg->Show(win->hwndFrame);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    if (FAILED(hr)) {
        return {};
    }
    ScopedComPtr<IShellItem> result;
    hr = dlg->GetResult(&result);
    if (FAILED(hr) || !result) {
        return {};
    }
    PWSTR folderW = nullptr;
    hr = result->GetDisplayName(SIGDN_FILESYSPATH, &folderW);
    if (FAILED(hr) || !folderW) {
        return {};
    }
    TempStr folder = str::DupTemp(ToUtf8Temp(WStr(folderW)));
    CoTaskMemFree(folderW);
    return folder;
}

void AddTouchLibraryFolder(MainWindow* win) {
    if (!win || !gGlobalPrefs) {
        return;
    }
    TempStr picked = PickTouchLibraryFolderTemp(win);
    if (!picked) {
        return;
    }
    TempStr normalized = path::NormalizeTemp(picked);
    Vec<Str>* roots = gGlobalPrefs->libraryFolders;
    if (!roots) {
        roots = new Vec<Str>();
        gGlobalPrefs->libraryFolders = roots;
    }
    bool alreadyAdded = false;
    for (Str root : *roots) {
        if (path::IsSame(root, normalized)) {
            alreadyAdded = true;
            break;
        }
    }
    if (!alreadyAdded) {
        roots->Append(str::Dup(normalized));
        SaveSettings();
    }
    str::ReplaceWithCopy(&win->librarySelectedFolderPath, normalized);
    win->libraryExpandedFolderPaths.Reset();
    win->libraryTreeScrollY = 0;
    win->libraryFilesScrollY = 0;
    HomePageInvalidateLibrary();
    win->RedrawAll(true);
}

static bool TouchLibraryPathIn(Vec<Str>* paths, Str folder) {
    if (!paths) {
        return false;
    }
    for (Str path : *paths) {
        if (path::IsSame(path, folder)) {
            return true;
        }
    }
    return false;
}

static bool TouchLibraryPathWithin(Str path, Str root) {
    if (path::IsSame(path, root)) {
        return true;
    }
    if (len(path) <= len(root) || !str::StartsWithI(path, root)) {
        return false;
    }
    return path::IsSep(path.s[len(root)]);
}

static void TouchLibraryAddPath(Vec<Str>** pathsPtr, Str folder) {
    Vec<Str>* paths = *pathsPtr;
    if (!paths) {
        paths = new Vec<Str>();
        *pathsPtr = paths;
    }
    if (!TouchLibraryPathIn(paths, folder)) {
        paths->Append(str::Dup(folder));
    }
}

static bool TouchLibraryRemovePath(Vec<Str>* paths, Str folder, bool descendants = false) {
    if (!paths) {
        return false;
    }
    bool removed = false;
    for (int i = len(*paths) - 1; i >= 0; i--) {
        Str path = (*paths)[i];
        bool matches = descendants ? TouchLibraryPathWithin(path, folder) : path::IsSame(path, folder);
        if (matches) {
            paths->RemoveAt(i);
            str::Free(path);
            removed = true;
        }
    }
    return removed;
}

static void TouchLibrarySelectFallback(MainWindow* win) {
    str::FreePtr(&win->librarySelectedFolderPath);
    for (int i = 0; i < len(gTouchLibraryFolders); i++) {
        Str folder = gTouchLibraryFolders[i];
        if (!TouchLibraryPathIn(gGlobalPrefs->libraryHiddenFolders, folder)) {
            str::ReplaceWithCopy(&win->librarySelectedFolderPath, folder);
            win->librarySelectedFolder = i;
            return;
        }
    }
    win->librarySelectedFolder = 0;
}

bool HandleTouchLibraryLink(MainWindow* win, Str url) {
    if (!win || !gGlobalPrefs) {
        return false;
    }
    if (str::TrimPrefix(url, kLinkLibraryMenuPrefix)) {
        str::ReplaceWithCopy(&win->libraryRowMenuPath, url);
    } else if (str::TrimPrefix(url, kLinkLibraryPinPrefix)) {
        if (!TouchLibraryRemovePath(gGlobalPrefs->libraryPinnedFolders, url)) {
            TouchLibraryAddPath(&gGlobalPrefs->libraryPinnedFolders, url);
        }
        str::FreePtr(&win->libraryRowMenuPath);
        SaveSettings();
    } else if (str::TrimPrefix(url, kLinkLibraryHidePrefix)) {
        TouchLibraryAddPath(&gGlobalPrefs->libraryHiddenFolders, url);
        str::FreePtr(&win->libraryRowMenuPath);
        if (win->librarySelectedFolderPath && TouchLibraryPathWithin(win->librarySelectedFolderPath, url)) {
            TouchLibrarySelectFallback(win);
        }
        SaveSettings();
    } else if (str::Eq(url, kLinkLibraryManage)) {
        win->libraryManageFoldersOpen = true;
        str::FreePtr(&win->libraryRowMenuPath);
    } else if (str::Eq(url, kLinkLibraryManageDone)) {
        win->libraryManageFoldersOpen = false;
    } else if (str::TrimPrefix(url, kLinkLibraryRemoveRootPrefix)) {
        Vec<Str>* roots = gGlobalPrefs->libraryFolders;
        if (roots) {
            for (int i = len(*roots) - 1; i >= 0; i--) {
                Str root = (*roots)[i];
                if (path::IsSame(root, url)) {
                    roots->RemoveAt(i);
                    str::Free(root);
                }
            }
        }
        TouchLibraryRemovePath(gGlobalPrefs->libraryPinnedFolders, url, true);
        TouchLibraryRemovePath(gGlobalPrefs->libraryHiddenFolders, url, true);
        if (win->librarySelectedFolderPath && TouchLibraryPathWithin(win->librarySelectedFolderPath, url)) {
            str::FreePtr(&win->librarySelectedFolderPath);
        }
        HomePageInvalidateLibrary();
        SaveSettings();
    } else if (str::TrimPrefix(url, kLinkLibraryUnhidePrefix)) {
        TouchLibraryRemovePath(gGlobalPrefs->libraryHiddenFolders, url);
        SaveSettings();
    } else if (str::Eq(url, kLinkLibraryClearSearch)) {
        if (win->hwndHomeSearch) {
            HwndSetText(win->hwndHomeSearch, "");
            HwndSetFocus(win->hwndHomeSearch);
        }
    } else if (str::Eq(url, kLinkLibraryContentView)) {
        win->libraryListView = false;
        win->libraryFilesScrollY = 0;
    } else if (str::Eq(url, kLinkLibraryListView)) {
        win->libraryListView = true;
        win->libraryFilesScrollY = 0;
    } else {
        return false;
    }
    win->RedrawAll(true);
    return true;
}

void SelectTouchLibraryFolder(MainWindow* win, Str folderPath) {
    if (!win || !folderPath) {
        return;
    }
    str::ReplaceWithCopy(&win->librarySelectedFolderPath, folderPath);
    win->libraryFilesScrollY = 0;

    Str current = folderPath;
    for (;;) {
        TempStr parent = path::GetDirTemp(current);
        if (!parent || path::IsSame(parent, current)) {
            break;
        }
        if (gTouchLibraryFolders.FindI(parent) >= 0 && win->libraryExpandedFolderPaths.FindI(parent) < 0) {
            win->libraryExpandedFolderPaths.Append(parent);
        }
        current = parent;
    }
    win->RedrawAll(true);
}

bool CloseTouchLibraryTransientUi(MainWindow* win) {
    if (!win || win->touchView != TouchView::Library) {
        return false;
    }
    if (win->libraryManageFoldersOpen) {
        win->libraryManageFoldersOpen = false;
        win->RedrawAll(true);
        return true;
    }
    if (win->libraryRowMenuPath) {
        str::FreePtr(&win->libraryRowMenuPath);
        win->RedrawAll(true);
        return true;
    }
    return false;
}

static bool TouchLibraryFolderHidden(const StrVecWithData<TouchLibraryFolderData>& folders, int idx) {
    int hiddenAt = idx;
    while (hiddenAt >= 0) {
        if (TouchLibraryPathIn(gGlobalPrefs->libraryHiddenFolders, folders[hiddenAt])) {
            return true;
        }
        hiddenAt = folders.AtData(hiddenAt)->parent;
    }
    return false;
}

static bool TouchLibraryFolderVisible(MainWindow* win, const StrVecWithData<TouchLibraryFolderData>& folders, int idx) {
    if (TouchLibraryFolderHidden(folders, idx)) {
        return false;
    }
    int parent = folders.AtData(idx)->parent;
    while (parent >= 0) {
        if (win->libraryExpandedFolderPaths.FindI(folders[parent]) < 0) {
            return false;
        }
        parent = folders.AtData(parent)->parent;
    }
    return true;
}

[[maybe_unused]] static void DrawTouchLibraryPage(MainWindow* win, HDC hdc) {
    HomePageDestroySearch(win);
    Rect rc = HwndClientRect(win->hwndCanvas);
    HdcFillRect(hdc, rc, ThemeWindowControlBackgroundColor());
    int leftDx = DpiScale(hdc, 260);
    int headerDy = DpiScale(hdc, 64);
    HdcFillRect(hdc, Rect{0, 0, leftDx, rc.dy}, ThemeHotBackgroundColor());
    HdcFillRect(hdc, Rect{leftDx - 1, 0, 1, rc.dy}, ThemeEdgeColor());
    HdcFillRect(hdc, Rect{leftDx, headerDy - 1, rc.dx - leftDx, 1}, ThemeEdgeColor());
    SetBkMode(hdc, TRANSPARENT);

    if (!gTouchLibraryValid) {
        BuildTouchLibraryModel();
    }
    auto& folders = gTouchLibraryFolders;
    auto& files = gTouchLibraryFiles;
    if (len(folders) > 0) {
        int selected = win->librarySelectedFolderPath ? folders.FindI(win->librarySelectedFolderPath) : -1;
        if (selected < 0) {
            selected = std::clamp(win->librarySelectedFolder, 0, len(folders) - 1);
            str::ReplaceWithCopy(&win->librarySelectedFolderPath, folders[selected]);
        }
        win->librarySelectedFolder = selected;
    } else {
        win->librarySelectedFolder = 0;
        str::FreePtr(&win->librarySelectedFolderPath);
    }

    Rect libraryLabel{DpiScale(hdc, 16), DpiScale(hdc, 17), leftDx - DpiScale(hdc, 32), DpiScale(hdc, 18)};
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, StrL("LIBRARY"), libraryLabel, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    int y = DpiScale(hdc, 48);
    for (int i = 0; i < len(folders); i++) {
        if (!TouchLibraryFolderVisible(win, folders, i)) {
            continue;
        }
        auto* data = folders.AtData(i);
        Rect row{DpiScale(hdc, 8), y, leftDx - DpiScale(hdc, 16), DpiScale(hdc, 40)};
        bool selected = i == win->librarySelectedFolder;
        if (selected) {
            FillHomeRoundRect(hdc, row, DpiScale(hdc, 8), RgbToCOLORREF(0xfbeee2));
        }
        int contentX = row.x + DpiScale(hdc, 10 + data->depth * 20);
        Rect chevron{contentX, row.y, DpiScale(hdc, 18), row.dy};
        if (data->hasChildren) {
            bool collapsed = win->libraryExpandedFolderPaths.FindI(folders[i]) < 0;
            SetTextColor(hdc, selected ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, collapsed ? StrL("›") : StrL("⌄"), chevron,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 15, FW_MEDIUM));
            win->staticLinks.Append(new StaticLink(chevron, fmt("%s%s", Str(kLinkLibraryTogglePrefix), folders[i])));
        }
        int folderDy = DpiScale(hdc, 18);
        HIMAGELIST iml = GetTintedToolbarImageList(folderDy, selected ? ThemeWindowLinkColor() : ThemeWindowTextColor(),
                                                   selected ? RgbToCOLORREF(0xfbeee2) : ThemeHotBackgroundColor());
        if (iml) {
            ImageList_Draw(iml, (int)TbIcon::Folder, hdc, chevron.x + chevron.dx + DpiScale(hdc, 4),
                           row.y + (row.dy - folderDy) / 2, ILD_NORMAL);
        }
        TempStr name = path::GetBaseNameTemp(folders[i]);
        if (!name) {
            name = str::DupTemp(folders[i]);
        }
        int nameX = chevron.x + chevron.dx + DpiScale(hdc, 28);
        Rect countRc{row.x + row.dx - DpiScale(hdc, 34), row.y, DpiScale(hdc, 28), row.dy};
        Rect nameRc{nameX, row.y, std::max(0, countRc.x - nameX - DpiScale(hdc, 4)), row.dy};
        SetTextColor(hdc, selected ? ThemeWindowLinkColor() : ThemeWindowTextColor());
        HdcDrawText(hdc, name, nameRc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_MEDIUM));
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawTextTabular(hdc, fmt("%d", data->directCount), countRc,
                           DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 12));
        win->staticLinks.Append(
            new StaticLink(row, fmt("%s%s", Str(kLinkLibraryFolderPrefix), folders[i]), folders[i]));
        y += row.dy;
    }

    Rect add{DpiScale(hdc, 12), rc.dy - DpiScale(hdc, 56), leftDx - DpiScale(hdc, 24), DpiScale(hdc, 44)};
    SetTextColor(hdc, ThemeWindowLinkColor());
    HdcDrawText(hdc, StrL("+  Add folder"), add, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    win->staticLinks.Append(new StaticLink(add, Str(kLinkLibraryAddFolder)));

    Str selectedPath = len(folders) > 0 ? folders[win->librarySelectedFolder] : Str{};
    TempStr selectedName = selectedPath ? path::GetBaseNameTemp(selectedPath) : str::DupTemp("Library");
    Rect header{leftDx + DpiScale(hdc, 24), 0, rc.dx - leftDx - DpiScale(hdc, 48), headerDy};
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, selectedName, header, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                HdcGetUiFont(hdc, 18, FW_SEMIBOLD));

    Vec<int> selectedFiles;
    for (int i = 0; i < len(files); i++) {
        Str filePath = files[i];
        if (selectedPath && str::EqI(path::GetDirTemp(filePath), selectedPath)) {
            selectedFiles.Append(i);
        }
    }
    int pad = DpiScale(hdc, 24);
    int gap = DpiScale(hdc, 20);
    int cardDx = DpiScale(hdc, 148);
    int cardDy = DpiScale(hdc, 196);
    int columns = TouchCardColumns(win->hwndCanvas, rc.dx - leftDx - 2 * pad);
    for (int i = 0; i < len(selectedFiles); i++) {
        int col = i % columns;
        int row = i / columns;
        Rect card{leftDx + pad + col * (cardDx + gap), headerDy + pad + row * (cardDy + DpiScale(hdc, 70)), cardDx,
                  cardDy};
        int fileIdx = selectedFiles[i];
        Str filePath = files[fileIdx];
        TouchLibraryFileData* fileData = files.AtData(fileIdx);
        if (!fileData->thumbnail && !fileData->thumbnailRequested) {
            fileData->thumbnailRequested = true;
            auto* request = new TouchLibraryThumbnailRequest();
            request->filePath = str::Dup(filePath);
            auto* onRendered = NewFunc1(TouchLibraryThumbnailFinished, request);
            CreateThumbnailFromFileAsync(filePath, 1, onRendered);
        }
        DrawTouchFileCardPath(win, hdc, filePath, gFileHistory.FindByPath(filePath), fileData->thumbnail, card, false,
                              nullptr, true, true);
    }
    if (len(selectedFiles) == 0) {
        Rect empty{leftDx + DpiScale(hdc, 40), headerDy + DpiScale(hdc, 40), rc.dx - leftDx - DpiScale(hdc, 80),
                   DpiScale(hdc, 40)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        Str message = len(folders) == 0 ? StrL("Add a folder to build your Library.") : StrL("This folder is empty.");
        HdcDrawText(hdc, message, empty, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 14));
    }
}

static void DrawTouchLibraryFolderIcon(HDC hdc, const Rect& rect, COLORREF fg, COLORREF bg) {
    int iconDy = DpiScale(hdc, 18);
    HIMAGELIST iml = GetTintedToolbarImageList(iconDy, fg, bg);
    if (iml) {
        ImageList_Draw(iml, (int)TbIcon::Folder, hdc, rect.x, rect.y + (rect.dy - iconDy) / 2, ILD_NORMAL);
    }
}

static void DrawTouchLibraryPin(HDC hdc, const Rect& rect, COLORREF color) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(GdiRgbFromCOLORREF(color));
    Gdiplus::PointF points[] = {
        {(float)rect.x + rect.dx * 0.27f, (float)rect.y + rect.dy * 0.16f},
        {(float)rect.x + rect.dx * 0.73f, (float)rect.y + rect.dy * 0.16f},
        {(float)rect.x + rect.dx * 0.73f, (float)rect.y + rect.dy * 0.84f},
        {(float)rect.x + rect.dx * 0.50f, (float)rect.y + rect.dy * 0.65f},
        {(float)rect.x + rect.dx * 0.27f, (float)rect.y + rect.dy * 0.84f},
    };
    graphics.FillPolygon(&brush, points, dimofi(points));
}

static void DrawTouchLibraryManageModal(MainWindow* win, HDC hdc, const Rect& rc) {
    DeleteVecMembers(win->staticLinks);
    Gdiplus::Graphics graphics(hdc);
    Gdiplus::SolidBrush scrim(Gdiplus::Color(90, 28, 26, 23));
    graphics.FillRectangle(&scrim, rc.x, rc.y, rc.dx, rc.dy);

    Vec<Str>* roots = gGlobalPrefs->libraryFolders;
    Vec<Str>* hidden = gGlobalPrefs->libraryHiddenFolders;
    int rootCount = roots ? len(*roots) : 0;
    int hiddenCount = hidden ? len(*hidden) : 0;
    int contentDy = 168 + rootCount * 36;
    if (hiddenCount > 0) {
        contentDy += 26 + hiddenCount * 34;
    }
    int desiredCardDy = std::max(284, contentDy + 80);
    int cardDx = std::min(DpiScale(hdc, 460), rc.dx - DpiScale(hdc, 32));
    int cardDy = std::min(DpiScale(hdc, std::min(desiredCardDy, 560)), rc.dy - DpiScale(hdc, 32));
    Rect card{(rc.dx - cardDx) / 2, (rc.dy - cardDy) / 2, cardDx, cardDy};
    FillHomeRoundRect(hdc, card, DpiScale(hdc, 14), ThemeWindowControlBackgroundColor());
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    Rect title{card.x + DpiScale(hdc, 24), card.y + DpiScale(hdc, 18), card.dx - DpiScale(hdc, 48), DpiScale(hdc, 28)};
    HdcDrawText(hdc, StrL("Manage library folders"), title, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 17, FW_SEMIBOLD));
    HdcFillRect(hdc, Rect{card.x, card.y + DpiScale(hdc, 60), card.dx, 1}, ThemeEdgeColor());

    int y = card.y + DpiScale(hdc, 76);
    auto drawSection = [&](Str label) {
        Rect row{card.x + DpiScale(hdc, 24), y, card.dx - DpiScale(hdc, 48), DpiScale(hdc, 22)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, label, row, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 12, FW_SEMIBOLD));
        y += row.dy + DpiScale(hdc, 4);
    };
    drawSection(StrL("ROOT FOLDERS"));
    if (roots) {
        for (Str root : *roots) {
            if (y + DpiScale(hdc, 36) > card.y + card.dy - DpiScale(hdc, 130)) {
                break;
            }
            Rect row{card.x + DpiScale(hdc, 24), y, card.dx - DpiScale(hdc, 48), DpiScale(hdc, 36)};
            DrawTouchLibraryFolderIcon(hdc, Rect{row.x, row.y, DpiScale(hdc, 20), row.dy}, ThemeWindowTextColor(),
                                       ThemeWindowControlBackgroundColor());
            TempStr name = path::GetBaseNameTemp(root);
            Rect nameRect{row.x + DpiScale(hdc, 28), row.y, row.dx - DpiScale(hdc, 68), row.dy};
            SetTextColor(hdc, ThemeWindowTextColor());
            HdcDrawText(hdc, name, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 13, FW_MEDIUM));
            Rect remove{row.x + row.dx - DpiScale(hdc, 34), row.y + DpiScale(hdc, 3), DpiScale(hdc, 30),
                        DpiScale(hdc, 30)};
            Gdiplus::Graphics removeGraphics(hdc);
            removeGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen removePen(GdiRgbFromCOLORREF(ThemeWindowLinkColor()), 1.6f);
            float rx = (float)remove.x + remove.dx * 0.31f;
            float ry = (float)remove.y + remove.dy * 0.34f;
            float rdx = remove.dx * 0.38f;
            float rdy = remove.dy * 0.42f;
            removeGraphics.DrawRectangle(&removePen, rx, ry, rdx, rdy);
            removeGraphics.DrawLine(&removePen, remove.x + remove.dx * 0.26f, remove.y + remove.dy * 0.28f,
                                    remove.x + remove.dx * 0.74f, remove.y + remove.dy * 0.28f);
            removeGraphics.DrawLine(&removePen, remove.x + remove.dx * 0.42f, remove.y + remove.dy * 0.21f,
                                    remove.x + remove.dx * 0.58f, remove.y + remove.dy * 0.21f);
            win->staticLinks.Append(
                new StaticLink(remove, fmt("%s%s", Str(kLinkLibraryRemoveRootPrefix), root), StrL("Remove folder")));
            y += row.dy;
        }
    }

    Rect addRow{card.x + DpiScale(hdc, 24), y + DpiScale(hdc, 8), card.dx - DpiScale(hdc, 48), DpiScale(hdc, 40)};
    Rect addButton{addRow.x + addRow.dx - DpiScale(hdc, 108), addRow.y, DpiScale(hdc, 108), addRow.dy};
    Rect placeholder{addRow.x, addRow.y, addRow.dx - addButton.dx - DpiScale(hdc, 8), addRow.dy};
    FillHomeRoundRect(hdc, placeholder, DpiScale(hdc, 8), ThemeHotBackgroundColor());
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(
        hdc, StrL("Choose a folder from your computer…"),
        Rect{placeholder.x + DpiScale(hdc, 12), placeholder.y, placeholder.dx - DpiScale(hdc, 24), placeholder.dy},
        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 12));
    FillHomeRoundRect(hdc, addButton, DpiScale(hdc, 8), RgbToCOLORREF(0xb4530a));
    SetTextColor(hdc, RGB(255, 255, 255));
    HdcDrawText(hdc, StrL("Add folder"), addButton, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    win->staticLinks.Append(new StaticLink(addButton, Str(kLinkLibraryAddFolder)));
    y = addRow.y + addRow.dy + DpiScale(hdc, 18);

    if (hidden && len(*hidden) > 0 && y < card.y + card.dy - DpiScale(hdc, 80)) {
        drawSection(StrL("HIDDEN FOLDERS"));
        for (Str folder : *hidden) {
            if (y + DpiScale(hdc, 34) > card.y + card.dy - DpiScale(hdc, 68)) {
                break;
            }
            Rect row{card.x + DpiScale(hdc, 24), y, card.dx - DpiScale(hdc, 48), DpiScale(hdc, 34)};
            TempStr name = path::GetBaseNameTemp(folder);
            Rect unhide{row.x + row.dx - DpiScale(hdc, 74), row.y + DpiScale(hdc, 3), DpiScale(hdc, 74),
                        DpiScale(hdc, 28)};
            Rect nameRect{row.x, row.y, row.dx - unhide.dx - DpiScale(hdc, 8), row.dy};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, name, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 13));
            FillHomeRoundRect(hdc, unhide, unhide.dy / 2, ThemeHotBackgroundColor());
            SetTextColor(hdc, ThemeWindowLinkColor());
            HdcDrawText(hdc, StrL("Unhide"), unhide, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 12, FW_SEMIBOLD));
            win->staticLinks.Append(new StaticLink(unhide, fmt("%s%s", Str(kLinkLibraryUnhidePrefix), folder)));
            y += row.dy;
        }
    }

    int footerY = card.y + card.dy - DpiScale(hdc, 64);
    HdcFillRect(hdc, Rect{card.x, footerY, card.dx, 1}, ThemeEdgeColor());
    Rect done{card.x + card.dx - DpiScale(hdc, 112), footerY + DpiScale(hdc, 14), DpiScale(hdc, 88), DpiScale(hdc, 36)};
    FillHomeRoundRect(hdc, done, DpiScale(hdc, 8), ThemeDisabledEdgeColor());
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, StrL("Done"), done, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    win->staticLinks.Append(new StaticLink(done, Str(kLinkLibraryManageDone)));
}

static int ClampTouchLibrarySidebarDx(MainWindow* win, int dx) {
    int minDx = DpiScale(win->hwndCanvas, 220);
    int maxDx =
        std::min(DpiScale(win->hwndCanvas, 520), HwndClientRect(win->hwndCanvas).dx - DpiScale(win->hwndCanvas, 280));
    maxDx = std::max(minDx, maxDx);
    return std::clamp(dx, minDx, maxDx);
}

static int TouchLibrarySidebarDx(MainWindow* win) {
    int dx = win->librarySidebarDx;
    if (dx <= 0) {
        dx = DpiScale(win->hwndCanvas, 260);
    }
    dx = ClampTouchLibrarySidebarDx(win, dx);
    win->librarySidebarDx = dx;
    return dx;
}

static Rect TouchLibrarySplitterHitRect(MainWindow* win) {
    int dx = TouchLibrarySidebarDx(win);
    int hitDx = DpiScale(win->hwndCanvas, kSidebarSplitterHitDx);
    return Rect{dx - hitDx / 2, 0, hitDx, HwndClientRect(win->hwndCanvas).dy};
}

static TempStr TouchLibraryFolderSummaryTemp(const TouchLibraryFolderData* data) {
    if (data->directFolderCount == 0) {
        return data->directCount == 1 ? str::DupTemp("1 file") : fmt("%d files", data->directCount);
    }
    if (data->directCount == 1 && data->directFolderCount == 1) {
        return str::DupTemp("1 file, 1 folder");
    }
    if (data->directCount == 1) {
        return fmt("1 file, %d folders", data->directFolderCount);
    }
    if (data->directFolderCount == 1) {
        return fmt("%d files, 1 folder", data->directCount);
    }
    return fmt("%d files, %d folders", data->directCount, data->directFolderCount);
}

static void DrawTouchLibraryFolderCard(MainWindow* win, HDC hdc, Str folderPath, const TouchLibraryFolderData* data,
                                       const Rect& card, const Rect& clip) {
    COLORREF pageBg = ThemeWindowControlBackgroundColor();
    DrawHomeShadow(hdc, card, DpiScale(hdc, 10), pageBg);
    FillHomeRoundRect(hdc, card, DpiScale(hdc, 10), RGB(255, 255, 255), ThemeEdgeColor());
    int iconDy = DpiScale(hdc, 48);
    HIMAGELIST iml = GetTintedToolbarImageList(iconDy, ThemeWindowDarkerTextColor(), RGB(255, 255, 255));
    if (iml) {
        ImageList_Draw(iml, (int)TbIcon::Folder, hdc, card.x + (card.dx - iconDy) / 2, card.y + (card.dy - iconDy) / 2,
                       ILD_NORMAL);
    }

    TempStr name = path::GetBaseNameTemp(folderPath);
    Rect nameRect{card.x, card.y + card.dy + DpiScale(hdc, 9), card.dx, DpiScale(hdc, 38)};
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, name, nameRect, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    Rect metaRect{nameRect.x, nameRect.y + nameRect.dy + DpiScale(hdc, 1), nameRect.dx, DpiScale(hdc, 17)};
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, TouchLibraryFolderSummaryTemp(data), metaRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX,
                HdcGetUiFont(hdc, 12));

    Rect linkRect = card.Union(metaRect).Intersect(clip);
    if (!linkRect.IsEmpty()) {
        win->staticLinks.Append(
            new StaticLink(linkRect, fmt("%s%s", Str(kLinkLibraryFolderPrefix), folderPath), folderPath));
    }
}

static void DrawTouchLibrarySolidFolderIcon(HDC hdc, const Rect& rect, COLORREF color) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(GdiRgbFromCOLORREF(color));
    float x = (float)rect.x;
    float y = (float)rect.y + rect.dy * 0.18f;
    float dx = (float)rect.dx;
    float dy = rect.dy * 0.68f;
    Gdiplus::PointF points[] = {{x, y + dy * 0.18f},
                                {x + dx * 0.34f, y + dy * 0.18f},
                                {x + dx * 0.45f, y},
                                {x + dx * 0.72f, y},
                                {x + dx * 0.82f, y + dy * 0.18f},
                                {x + dx, y + dy * 0.18f},
                                {x + dx, y + dy},
                                {x, y + dy}};
    graphics.FillPolygon(&brush, points, dimofi(points));
}

static void DrawTouchLibraryPdfIcon(HDC hdc, const Rect& rect) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush accent(GdiRgbFromCOLORREF(ThemeWindowLinkColor()));
    Gdiplus::PointF page[] = {{(float)rect.x, (float)rect.y},
                              {(float)(rect.x + rect.dx * 2 / 3), (float)rect.y},
                              {(float)(rect.x + rect.dx), (float)(rect.y + rect.dy / 3)},
                              {(float)(rect.x + rect.dx), (float)(rect.y + rect.dy)},
                              {(float)rect.x, (float)(rect.y + rect.dy)}};
    graphics.FillPolygon(&accent, page, dimofi(page));
    Gdiplus::SolidBrush fold(GdiRgbFromCOLORREF(ThemeWindowControlBackgroundColor()));
    Gdiplus::PointF corner[] = {{(float)(rect.x + rect.dx * 2 / 3), (float)rect.y},
                                {(float)(rect.x + rect.dx * 2 / 3), (float)(rect.y + rect.dy / 3)},
                                {(float)(rect.x + rect.dx), (float)(rect.y + rect.dy / 3)}};
    graphics.FillPolygon(&fold, corner, dimofi(corner));
}

static void DrawTouchLibraryPageV2(MainWindow* win, HDC hdc) {
    Rect rc = HwndClientRect(win->hwndCanvas);
    int leftDx = TouchLibrarySidebarDx(win);
    int headerDy = DpiScale(hdc, 64);
    HdcFillRect(hdc, rc, ThemeWindowControlBackgroundColor());
    HdcFillRect(hdc, Rect{0, 0, leftDx, rc.dy}, ThemeHotBackgroundColor());
    int splitterDx = DpiScale(hdc, kSidebarSplitterVisualDx);
    HdcFillRect(hdc, Rect{leftDx - splitterDx / 2, 0, splitterDx, rc.dy}, ThemeEdgeColor());
    HdcFillRect(hdc, Rect{leftDx, headerDy - 1, rc.dx - leftDx, 1}, ThemeEdgeColor());
    SetBkMode(hdc, TRANSPARENT);

    if (!gTouchLibraryValid) {
        BuildTouchLibraryModel();
    }
    auto& folders = gTouchLibraryFolders;
    auto& files = gTouchLibraryFiles;

    EnsureHomeSearchCreated(win);
    Rect search{DpiScale(hdc, 12), DpiScale(hdc, 12), leftDx - DpiScale(hdc, 24), DpiScale(hdc, 40)};
    FillHomeRoundRect(hdc, search, search.dy / 2, ThemeWindowControlBackgroundColor(), ThemeEdgeColor());
    int searchIconDy = DpiScale(hdc, 16);
    HIMAGELIST searchIcons =
        GetTintedToolbarImageList(searchIconDy, ThemeWindowDarkerTextColor(), ThemeWindowControlBackgroundColor());
    if (searchIcons) {
        ImageList_Draw(searchIcons, (int)TbIcon::Search, hdc, search.x + DpiScale(hdc, 14),
                       search.y + (search.dy - searchIconDy) / 2, ILD_NORMAL);
    }
    MoveWindow(win->hwndHomeSearch, search.x + DpiScale(hdc, 38), search.y + DpiScale(hdc, 4),
               search.dx - DpiScale(hdc, 72), search.dy - DpiScale(hdc, 8), TRUE);
    HwndShow(win->hwndHomeSearch);
    TempStr query = HwndGetTextTemp(win->hwndHomeSearch);
    if (len(query) > 0) {
        Rect clear{search.x + search.dx - DpiScale(hdc, 38), search.y, DpiScale(hdc, 38), search.dy};
        Rect clearCircle{clear.x + (clear.dx - DpiScale(hdc, 18)) / 2, clear.y + (clear.dy - DpiScale(hdc, 18)) / 2,
                         DpiScale(hdc, 18), DpiScale(hdc, 18)};
        FillHomeRoundRect(hdc, clearCircle, clearCircle.dy / 2, RgbToCOLORREF(0xeae5de));
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("×"), clear, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 15));
        win->staticLinks.Append(new StaticLink(clear, Str(kLinkLibraryClearSearch)));
    }

    int selected = win->librarySelectedFolderPath ? folders.FindI(win->librarySelectedFolderPath) : -1;
    if (selected < 0 || (selected < len(folders) && TouchLibraryFolderHidden(folders, selected))) {
        TouchLibrarySelectFallback(win);
        selected = win->librarySelectedFolderPath ? folders.FindI(win->librarySelectedFolderPath) : -1;
    }
    win->librarySelectedFolder = std::max(0, selected);

    int treeTop = DpiScale(hdc, 64);
    int treeBottom = rc.dy - DpiScale(hdc, 64);
    win->libraryTreeScrollY = std::clamp(win->libraryTreeScrollY, 0, win->libraryTreeScrollMaxY);
    int y = DpiScale(hdc, 68) - win->libraryTreeScrollY;
    Rect treeClip{0, treeTop, leftDx, std::max(0, treeBottom - treeTop)};
    int treeDc = SaveDC(hdc);
    IntersectClipRect(hdc, treeClip.x, treeClip.y, treeClip.x + treeClip.dx, treeClip.y + treeClip.dy);
    auto sectionLabel = [&](Str label) {
        Rect labelRect{DpiScale(hdc, 16), y, leftDx - DpiScale(hdc, 32), DpiScale(hdc, 24)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, label, labelRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 12, FW_SEMIBOLD));
        y += labelRect.dy;
    };
    Rect openMenuAnchor{};
    auto drawFolderRow = [&](int idx, bool pinnedRow) {
        if (idx < 0 || idx >= len(folders)) {
            return;
        }
        Str folder = folders[idx];
        TouchLibraryFolderData* data = folders.AtData(idx);
        bool isSelected = idx == selected;
        bool isPinned = TouchLibraryPathIn(gGlobalPrefs->libraryPinnedFolders, folder);
        Rect row{DpiScale(hdc, 8), y, leftDx - DpiScale(hdc, 16), DpiScale(hdc, 38)};
        y += row.dy;
        Rect visibleRow = row.Intersect(treeClip);
        if (visibleRow.IsEmpty()) {
            return;
        }
        COLORREF rowBg = ThemeHotBackgroundColor();
        if (isSelected) {
            rowBg = RgbToCOLORREF(0xfbeee2);
            FillHomeRoundRect(hdc, row, DpiScale(hdc, 8), rowBg);
        }
        int indent = pinnedRow ? 10 : 10 + data->depth * 20;
        int x = row.x + DpiScale(hdc, indent);
        Rect chevron{x, row.y, DpiScale(hdc, 18), row.dy};
        if (!pinnedRow && data->hasChildren) {
            bool collapsed = win->libraryExpandedFolderPaths.FindI(folder) < 0;
            SetTextColor(hdc, isSelected ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, collapsed ? StrL("›") : StrL("⌄"), chevron,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 15, FW_MEDIUM));
            Rect chevronLink = chevron.Intersect(treeClip);
            if (!chevronLink.IsEmpty()) {
                win->staticLinks.Append(
                    new StaticLink(chevronLink, fmt("%s%s", Str(kLinkLibraryTogglePrefix), folder)));
            }
        }
        Rect folderIcon{chevron.x + chevron.dx + DpiScale(hdc, 4), row.y, DpiScale(hdc, 18), row.dy};
        COLORREF fg = isSelected || pinnedRow ? ThemeWindowLinkColor() : ThemeWindowTextColor();
        DrawTouchLibraryFolderIcon(hdc, folderIcon, fg, isSelected ? rowBg : ThemeHotBackgroundColor());
        Rect countRect{row.x + row.dx - DpiScale(hdc, 28), row.y, DpiScale(hdc, 24), row.dy};
        Rect menuRect{countRect.x - DpiScale(hdc, 26), row.y + DpiScale(hdc, 8), DpiScale(hdc, 22), DpiScale(hdc, 22)};
        int nameX = folderIcon.x + folderIcon.dx + DpiScale(hdc, 7);
        int pinDx = isPinned && !pinnedRow ? DpiScale(hdc, 14) : 0;
        Rect nameRect{nameX, row.y, std::max(0, menuRect.x - nameX - DpiScale(hdc, 6) - pinDx), row.dy};
        TempStr name = path::GetBaseNameTemp(folder);
        SetTextColor(hdc, isSelected ? ThemeWindowLinkColor() : ThemeWindowTextColor());
        HdcDrawText(hdc, name, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_MEDIUM));
        if (pinDx > 0) {
            DrawTouchLibraryPin(
                hdc,
                Rect{nameRect.x + nameRect.dx + DpiScale(hdc, 2), row.y + DpiScale(hdc, 11), pinDx, DpiScale(hdc, 16)},
                RgbToCOLORREF(0xb4530a));
        }
        if (!pinnedRow) {
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, StrL("⋯"), menuRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 15, FW_SEMIBOLD));
            Rect menuLink = menuRect.Intersect(treeClip);
            if (!menuLink.IsEmpty()) {
                win->staticLinks.Append(new StaticLink(menuLink, fmt("%s%s", Str(kLinkLibraryMenuPrefix), folder)));
            }
        }
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawTextTabular(hdc, fmt("%d", data->directCount), countRect,
                           DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 12));
        win->staticLinks.Append(new StaticLink(visibleRow, fmt("%s%s", Str(kLinkLibraryFolderPrefix), folder), folder));
        if (win->libraryRowMenuPath && path::IsSame(win->libraryRowMenuPath, folder)) {
            openMenuAnchor = visibleRow;
        }
    };

    if (len(query) > 0) {
        bool anyFolder = false;
        sectionLabel(StrL("FOLDERS"));
        for (int i = 0; i < len(folders); i++) {
            if (!TouchLibraryFolderHidden(folders, i) && str::ContainsI(path::GetBaseNameTemp(folders[i]), query)) {
                drawFolderRow(i, true);
                anyFolder = true;
            }
        }
        bool anyFile = false;
        y += DpiScale(hdc, 8);
        sectionLabel(StrL("FILES"));
        for (int i = 0; i < len(files); i++) {
            Str filePath = files[i];
            if (!str::ContainsI(path::GetBaseNameTemp(filePath), query)) {
                continue;
            }
            TempStr parentPath = path::GetDirTemp(filePath);
            int folderIdx = folders.FindI(parentPath);
            if (folderIdx < 0 || TouchLibraryFolderHidden(folders, folderIdx)) {
                continue;
            }
            Rect row{DpiScale(hdc, 14), y, leftDx - DpiScale(hdc, 28), DpiScale(hdc, 48)};
            y += row.dy;
            Rect visibleRow = row.Intersect(treeClip);
            if (visibleRow.IsEmpty()) {
                anyFile = true;
                continue;
            }
            Rect nameRect{row.x + DpiScale(hdc, 26), row.y + DpiScale(hdc, 3), row.dx - DpiScale(hdc, 26),
                          DpiScale(hdc, 21)};
            SetTextColor(hdc, ThemeWindowTextColor());
            HdcDrawText(hdc, path::GetBaseNameTemp(filePath), nameRect,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_MEDIUM));
            Rect parentRect{nameRect.x, nameRect.y + nameRect.dy, nameRect.dx, DpiScale(hdc, 18)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, fmt("in %s", path::GetBaseNameTemp(parentPath)), parentRect,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 11));
            DrawTouchLibraryFolderIcon(hdc, Rect{row.x, row.y, DpiScale(hdc, 18), row.dy}, ThemeWindowDarkerTextColor(),
                                       ThemeHotBackgroundColor());
            win->staticLinks.Append(new StaticLink(visibleRow, fmt("%s%s", Str(kLinkLibraryFolderPrefix), parentPath)));
            anyFile = true;
        }
        if (!anyFolder && !anyFile) {
            Rect empty{DpiScale(hdc, 20), y + DpiScale(hdc, 16), leftDx - DpiScale(hdc, 40), DpiScale(hdc, 48)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, fmt("No matches for “%s”", query), empty, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 13));
        }
    } else {
        bool hasPinned = false;
        Vec<Str>* pinned = gGlobalPrefs->libraryPinnedFolders;
        if (pinned) {
            for (Str folder : *pinned) {
                int idx = folders.FindI(folder);
                if (idx >= 0 && !TouchLibraryFolderHidden(folders, idx)) {
                    if (!hasPinned) {
                        sectionLabel(StrL("PINNED"));
                        hasPinned = true;
                    }
                    drawFolderRow(idx, true);
                }
            }
        }
        if (hasPinned) {
            y += DpiScale(hdc, 8);
            HdcFillRect(hdc, Rect{DpiScale(hdc, 16), y, leftDx - DpiScale(hdc, 32), 1}, ThemeEdgeColor());
            y += DpiScale(hdc, 10);
        }
        sectionLabel(StrL("LIBRARY"));
        for (int i = 0; i < len(folders); i++) {
            if (TouchLibraryFolderVisible(win, folders, i)) {
                drawFolderRow(i, false);
            }
        }
    }

    int treeContentBottom = y + win->libraryTreeScrollY + DpiScale(hdc, 4);
    int oldTreeMax = win->libraryTreeScrollMaxY;
    win->libraryTreeScrollMaxY = std::max(0, treeContentBottom - treeBottom);
    win->libraryTreeScrollY = std::clamp(win->libraryTreeScrollY, 0, win->libraryTreeScrollMaxY);
    RestoreDC(hdc, treeDc);
    if (oldTreeMax != win->libraryTreeScrollMaxY) {
        HwndInvalidate(win->hwndCanvas, false);
    }

    Rect manage{DpiScale(hdc, 12), rc.dy - DpiScale(hdc, 56), leftDx - DpiScale(hdc, 24), DpiScale(hdc, 44)};
    SetTextColor(hdc, ThemeWindowLinkColor());
    Rect manageIcon{manage.x + DpiScale(hdc, 4), manage.y, DpiScale(hdc, 20), manage.dy};
    DrawTouchLibraryFolderIcon(hdc, manageIcon, ThemeWindowLinkColor(), ThemeHotBackgroundColor());
    Gdiplus::Graphics manageGraphics(hdc);
    manageGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen managePen(GdiRgbFromCOLORREF(ThemeWindowLinkColor()), 1.6f);
    float plusX = (float)manageIcon.x + manageIcon.dx * 0.77f;
    float plusY = (float)manageIcon.y + manageIcon.dy * 0.38f;
    float plusArm = (float)DpiScale(hdc, 3);
    manageGraphics.DrawLine(&managePen, plusX - plusArm, plusY, plusX + plusArm, plusY);
    manageGraphics.DrawLine(&managePen, plusX, plusY - plusArm, plusX, plusY + plusArm);
    Rect manageText{manage.x + DpiScale(hdc, 34), manage.y, manage.dx - DpiScale(hdc, 34), manage.dy};
    HdcDrawText(hdc, StrL("Manage folders"), manageText, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    win->staticLinks.Append(new StaticLink(manage, Str(kLinkLibraryManage)));

    Str selectedPath = selected >= 0 && selected < len(folders) ? folders[selected] : Str{};
    TempStr selectedName = selectedPath ? path::GetBaseNameTemp(selectedPath) : str::DupTemp("Library");
    int viewButtonDy = DpiScale(hdc, 36);
    Rect viewToggle{rc.dx - DpiScale(hdc, 24) - 2 * viewButtonDy, (headerDy - viewButtonDy) / 2, 2 * viewButtonDy,
                    viewButtonDy};
    FillHomeRoundRect(hdc, viewToggle, DpiScale(hdc, 11), ThemeHotBackgroundColor());
    Rect contentView{viewToggle.x, viewToggle.y, viewButtonDy, viewButtonDy};
    Rect listView{contentView.x + contentView.dx, viewToggle.y, viewButtonDy, viewButtonDy};
    Rect activeView = win->libraryListView ? listView : contentView;
    FillHomeRoundRect(hdc, activeView, DpiScale(hdc, 9), ThemeWindowControlBackgroundColor());
    int viewIconDy = DpiScale(hdc, 18);
    HIMAGELIST contentIcons = GetTintedToolbarImageList(
        viewIconDy, win->libraryListView ? ThemeWindowDarkerTextColor() : ThemeWindowLinkColor(),
        win->libraryListView ? ThemeHotBackgroundColor() : ThemeWindowControlBackgroundColor());
    if (contentIcons) {
        ImageList_Draw(contentIcons, (int)TbIcon::HomeThumbnails, hdc,
                       contentView.x + (contentView.dx - viewIconDy) / 2,
                       contentView.y + (contentView.dy - viewIconDy) / 2, ILD_NORMAL);
    }
    HIMAGELIST listIcons = GetTintedToolbarImageList(
        viewIconDy, win->libraryListView ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor(),
        win->libraryListView ? ThemeWindowControlBackgroundColor() : ThemeHotBackgroundColor());
    if (listIcons) {
        ImageList_Draw(listIcons, (int)TbIcon::HomeList, hdc, listView.x + (listView.dx - viewIconDy) / 2,
                       listView.y + (listView.dy - viewIconDy) / 2, ILD_NORMAL);
    }
    win->staticLinks.Append(new StaticLink(contentView, Str(kLinkLibraryContentView), StrL("Content view")));
    win->staticLinks.Append(new StaticLink(listView, Str(kLinkLibraryListView), StrL("List view")));

    Rect header{leftDx + DpiScale(hdc, 24), 0, std::max(0, viewToggle.x - leftDx - DpiScale(hdc, 40)), headerDy};
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, selectedName, header, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                HdcGetUiFont(hdc, 18, FW_SEMIBOLD));

    Vec<int> selectedFolders;
    for (int i = 0; i < len(folders); i++) {
        if (folders.AtData(i)->parent == selected && !TouchLibraryFolderHidden(folders, i)) {
            selectedFolders.Append(i);
        }
    }
    Vec<int> selectedFiles;
    for (int i = 0; i < len(files); i++) {
        if (selectedPath && str::EqI(path::GetDirTemp(files[i]), selectedPath)) {
            selectedFiles.Append(i);
        }
    }
    int pad = DpiScale(hdc, 24);
    int filesViewportDy = std::max(0, rc.dy - headerDy);
    int itemCount = len(selectedFolders) + len(selectedFiles);
    int filesContentDy = 0;
    int gap = DpiScale(hdc, 20);
    int cardDx = DpiScale(hdc, 148);
    int cardDy = DpiScale(hdc, 196);
    int columns = TouchCardColumns(win->hwndCanvas, rc.dx - leftDx - 2 * pad);
    int cardStepY = cardDy + DpiScale(hdc, 70);
    int listPadY = DpiScale(hdc, 8);
    int listRowDy = DpiScale(hdc, 44);
    if (win->libraryListView) {
        filesContentDy = 2 * listPadY + itemCount * listRowDy;
    } else {
        filesContentDy = pad + TouchCardRows(itemCount, columns) * cardStepY + pad;
    }
    win->libraryFilesScrollMaxY = std::max(0, filesContentDy - filesViewportDy);
    win->libraryFilesScrollY = std::clamp(win->libraryFilesScrollY, 0, win->libraryFilesScrollMaxY);
    Rect filesClip{leftDx, headerDy, std::max(0, rc.dx - leftDx), filesViewportDy};
    int filesDc = SaveDC(hdc);
    IntersectClipRect(hdc, filesClip.x, filesClip.y, filesClip.x + filesClip.dx, filesClip.y + filesClip.dy);

    if (win->libraryListView) {
        int rowY = headerDy + listPadY - win->libraryFilesScrollY;
        auto drawListRow = [&](bool isFolder, int idx) {
            Rect row{leftDx + DpiScale(hdc, 16), rowY, rc.dx - leftDx - DpiScale(hdc, 32), listRowDy};
            rowY += listRowDy;
            Rect visibleRow = row.Intersect(filesClip);
            if (visibleRow.IsEmpty()) {
                return;
            }
            Rect icon{row.x, row.y + (row.dy - DpiScale(hdc, 18)) / 2, DpiScale(hdc, 18), DpiScale(hdc, 18)};
            Str itemPath;
            TempStr name;
            TempStr meta;
            TempStr target;
            if (isFolder) {
                itemPath = folders[idx];
                name = path::GetBaseNameTemp(itemPath);
                meta = TouchLibraryFolderSummaryTemp(folders.AtData(idx));
                target = fmt("%s%s", Str(kLinkLibraryFolderPrefix), itemPath);
                DrawTouchLibrarySolidFolderIcon(hdc, icon, RgbToCOLORREF(0xc56a1a));
            } else {
                itemPath = files[idx];
                name = path::GetBaseNameTemp(itemPath);
                i64 size = file::GetSize(itemPath);
                meta = size >= 0 ? str::FormatSizeShortTemp(size, nullptr) : str::DupTemp("");
                target = str::DupTemp(itemPath);
                Rect pdfIcon{icon.x + DpiScale(hdc, 1), icon.y + DpiScale(hdc, 1), DpiScale(hdc, 16),
                             DpiScale(hdc, 16)};
                DrawTouchLibraryPdfIcon(hdc, pdfIcon);
            }
            Rect metaRect{row.x + row.dx - DpiScale(hdc, 170), row.y, DpiScale(hdc, 170), row.dy};
            Rect nameRect{icon.x + icon.dx + DpiScale(hdc, 10), row.y,
                          std::max(0, metaRect.x - icon.x - icon.dx - DpiScale(hdc, 22)), row.dy};
            SetTextColor(hdc, ThemeWindowTextColor());
            HdcDrawText(hdc, name, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 14));
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, meta, metaRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 12));
            HdcFillRect(hdc, Rect{row.x, row.y + row.dy - 1, row.dx, 1}, RgbToCOLORREF(0xeee9e1));
            win->staticLinks.Append(new StaticLink(visibleRow, target, itemPath));
        };
        for (int folderIdx : selectedFolders) {
            drawListRow(true, folderIdx);
        }
        for (int fileIdx : selectedFiles) {
            drawListRow(false, fileIdx);
        }
    } else {
        for (int i = 0; i < itemCount; i++) {
            int col = i % columns;
            int row = i / columns;
            Rect card{leftDx + pad + col * (cardDx + gap), headerDy + pad - win->libraryFilesScrollY + row * cardStepY,
                      cardDx, cardDy};
            Rect cardBlock = card;
            cardBlock.dy = cardStepY;
            if (cardBlock.Intersect(filesClip).IsEmpty()) {
                continue;
            }
            if (i < len(selectedFolders)) {
                int folderIdx = selectedFolders[i];
                DrawTouchLibraryFolderCard(win, hdc, folders[folderIdx], folders.AtData(folderIdx), card, filesClip);
                continue;
            }
            int fileIdx = selectedFiles[i - len(selectedFolders)];
            Str filePath = files[fileIdx];
            TouchLibraryFileData* fileData = files.AtData(fileIdx);
            if (!fileData->thumbnail && !fileData->thumbnailRequested) {
                fileData->thumbnailRequested = true;
                auto* request = new TouchLibraryThumbnailRequest();
                request->filePath = str::Dup(filePath);
                auto* onRendered = NewFunc1(TouchLibraryThumbnailFinished, request);
                CreateThumbnailFromFileAsync(filePath, 1, onRendered);
            }
            DrawTouchFileCardPath(win, hdc, filePath, gFileHistory.FindByPath(filePath), fileData->thumbnail, card,
                                  false, &filesClip, true, true);
        }
    }
    if (itemCount == 0) {
        Rect empty{leftDx + DpiScale(hdc, 40), headerDy + DpiScale(hdc, 40), rc.dx - leftDx - DpiScale(hdc, 80),
                   DpiScale(hdc, 40)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        Str message = len(folders) == 0 ? StrL("Add a folder to build your Library.") : StrL("This folder is empty.");
        HdcDrawText(hdc, message, empty, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 14));
    }
    RestoreDC(hdc, filesDc);

    if (!openMenuAnchor.IsEmpty() && win->libraryRowMenuPath) {
        DeleteVecMembers(win->staticLinks);
        int menuDx = DpiScale(hdc, 168);
        int menuDy = DpiScale(hdc, 76);
        int menuY = openMenuAnchor.y + openMenuAnchor.dy + DpiScale(hdc, 2);
        if (menuY + menuDy > treeBottom) {
            menuY = openMenuAnchor.y - menuDy - DpiScale(hdc, 2);
        }
        Rect menu{leftDx - menuDx - DpiScale(hdc, 10), menuY, menuDx, menuDy};
        FillHomeRoundRect(hdc, menu, DpiScale(hdc, 9), ThemeWindowControlBackgroundColor());
        bool pinned = TouchLibraryPathIn(gGlobalPrefs->libraryPinnedFolders, win->libraryRowMenuPath);
        Rect pinAction{menu.x + DpiScale(hdc, 6), menu.y + DpiScale(hdc, 4), menu.dx - DpiScale(hdc, 12),
                       DpiScale(hdc, 32)};
        Rect hideAction{pinAction.x, pinAction.y + pinAction.dy, pinAction.dx, pinAction.dy};
        SetTextColor(hdc, ThemeWindowTextColor());
        HdcDrawText(hdc, pinned ? StrL("Unpin folder") : StrL("Pin folder"),
                    Rect{pinAction.x + DpiScale(hdc, 10), pinAction.y, pinAction.dx - DpiScale(hdc, 20), pinAction.dy},
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_MEDIUM));
        HdcDrawText(
            hdc, StrL("Hide folder"),
            Rect{hideAction.x + DpiScale(hdc, 10), hideAction.y, hideAction.dx - DpiScale(hdc, 20), hideAction.dy},
            DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_MEDIUM));
        win->staticLinks.Append(
            new StaticLink(pinAction, fmt("%s%s", Str(kLinkLibraryPinPrefix), Str(win->libraryRowMenuPath))));
        win->staticLinks.Append(
            new StaticLink(hideAction, fmt("%s%s", Str(kLinkLibraryHidePrefix), Str(win->libraryRowMenuPath))));
    }

    if (win->libraryManageFoldersOpen) {
        DrawTouchLibraryManageModal(win, hdc, rc);
    }
}

void DrawHomePage(MainWindow* win, HDC hdc) {
    HWND hwnd = win->hwndFrame;
    // any home-page repaint (scroll, resize, filter) invalidates thumbnail
    // positions and rewrites the buffer, so drop the close button without
    // touching the window; it reappears on the next hover
    ResetHomeCloseBtn();
    DeleteVecMembers(win->staticLinks);

    if (IsTouchChrome(win)) {
        ClearHomeLayoutCache();
        if (win->touchView == TouchView::Library) {
            DrawTouchLibraryPageV2(win, hdc);
        } else {
            DrawTouchHomePage(win, hdc);
        }
        return;
    }

    HomePageLayout l;
    l.rc = HwndClientRect(win->hwndCanvas);
    l.hdc = hdc;
    l.hwnd = hwnd;
    l.win = win;

    TempStr filterText = HomeSearchQueryTemp(win);
    int scrollY = win->homePageScrollY;

    // Prefer the scroll-friendly path: when only scrollY changed, offset cached
    // thumb rects instead of re-running full LayoutHomePage (was ~30% of scroll CPU).
    bool usedCache = false;
    if (HomeLayoutCacheMatches(win, l.rc, filterText)) {
        Vec<FileState*> files;
        StrVec filterWords;
        CollectHomePageFiles(win, files, filterWords);
        if (HomeLayoutCacheFilesMatch(files)) {
            ApplyHomeLayoutCache(l, scrollY);
            usedCache = true;
        }
    }
    if (!usedCache) {
        LayoutHomePage(l);
        SaveHomeLayoutCache(l, filterText, win->homePageScrollY);
    }
    // Keep cue "Search N files …" in sync when history changes without recreating the edit.
    UpdateHomeSearchCueBanner(win);

    DrawHomePageLayout(l);
    SyncHomeLayoutCacheFileSizes(l);

    // update overlay scrollbar for home page if thumbnails overflow visible area
    bool showScrollbarV = ScrollbarsUseOverlay() && l.totalContentDy > l.thumbsVisibleDy;
    if (showScrollbarV) {
        if (!win->overlayScrollV) {
            win->overlayScrollV =
                OverlayScrollbarCreate(win->hwndCanvas, OverlayScrollbar::Type::Vert, ScrollbarsOverlayMode());
        }
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        si.nMin = 0;
        si.nMax = l.totalContentDy - 1;
        si.nPage = l.thumbsVisibleDy;
        si.nPos = win->homePageScrollY;
        OverlayScrollbarShow(win->overlayScrollV, true);
        OverlayScrollbarSetInfo(win->overlayScrollV, &si, TRUE);
    }
    // show thin scrollbar briefly to indicate content is scrollable
    OverlayScrollbarShow(win->overlayScrollV, showScrollbarV);
}

// --- keyboard navigation of the file list (issue #1136) ---

// Selection works off the layout cache, which is filled by the last paint, so
// the entries are exactly what's on screen (same order, same filtering).
static int HomeSelectableCount() {
    return gHomeLayoutCache.valid ? len(gHomeLayoutCache.thumbs) : 0;
}

// bounding box of an entry, in window coordinates for the current scroll
static Rect HomeEntryRect(const ThumbnailLayout& t) {
    if (HomePageIsListView()) {
        return t.rcListRow;
    }
    return t.rcPage.Union(t.rcText);
}

// how many thumbnails fit in a grid row: the run of entries sharing the y of
// the first one
static int HomeGridColumnCount() {
    auto& c = gHomeLayoutCache;
    int n = len(c.thumbs);
    if (n == 0) {
        return 1;
    }
    int y0 = c.thumbs[0].rcPage.y;
    int nCols = 0;
    while (nCols < n && c.thumbs[nCols].rcPage.y == y0) {
        nCols++;
    }
    return nCols > 0 ? nCols : 1;
}

// Select the first-row entry at the column remembered when leaving for search.
static void HomeSelectFromSearchReturnCol(MainWindow* win) {
    int n = HomeSelectableCount();
    if (n <= 0) {
        win->homePageSelIdx = 0;
        return;
    }
    if (HomePageIsListView()) {
        win->homePageSelIdx = 0;
        return;
    }
    int nCols = HomeGridColumnCount();
    nCols = std::max(nCols, 1);
    int col = win->homePageSearchReturnCol;
    col = std::max(col, 0);
    if (col >= nCols) {
        col = nCols - 1;
    }
    // first row only has min(nCols, n) entries
    int firstRowN = n < nCols ? n : nCols;
    if (col >= firstRowN) {
        col = firstRowN - 1;
    }
    win->homePageSelIdx = col;
}

// Keep layout-cache thumb rects in sync with homePageScrollY (without a full
// paint) so keyboard tooltips can use up-to-date geometry after scroll.
static void HomeSyncLayoutCacheScroll(MainWindow* win) {
    auto& c = gHomeLayoutCache;
    if (!c.valid || !win) {
        return;
    }
    int scrollY = win->homePageScrollY;
    int maxScrollY = std::max(0, c.totalContentDy - c.thumbsVisibleDy);
    if (scrollY > maxScrollY) {
        scrollY = maxScrollY;
        win->homePageScrollY = scrollY;
    }
    if (scrollY < 0) {
        scrollY = 0;
        win->homePageScrollY = 0;
    }
    int dy = c.scrollY - scrollY;
    OffsetThumbnailLayouts(c.thumbs, dy);
    c.scrollY = scrollY;
}

// scroll so the selected entry is fully visible
static void HomeScrollSelectionIntoView(MainWindow* win) {
    auto& c = gHomeLayoutCache;
    int idx = win->homePageSelIdx;
    if (!c.valid || idx < 0 || idx >= len(c.thumbs)) {
        return;
    }
    // rects must match current scroll before we measure visibility
    HomeSyncLayoutCacheScroll(win);
    Rect r = HomeEntryRect(c.thumbs[idx]);
    const Rect& area = c.rcThumbsArea;
    if (r.IsEmpty() || area.IsEmpty()) {
        return;
    }
    // scrollY grows as the content moves up
    int dy = 0;
    if (r.y < area.y) {
        dy = r.y - area.y;
    } else if (r.y + r.dy > area.y + area.dy) {
        dy = (r.y + r.dy) - (area.y + area.dy);
    }
    if (dy == 0) {
        return;
    }
    int newScrollY = win->homePageScrollY + dy;
    newScrollY = std::max(newScrollY, 0);
    win->homePageScrollY = newScrollY; // layout clamps against content height
    HomeSyncLayoutCacheScroll(win);
}

// Selection outline rect — must match DrawHomePageLayout / DrawHomeListRow.
static Rect HomeSelectionOutlineRect(const ThumbnailLayout& t, HWND hwnd) {
    if (HomePageIsListView()) {
        // list: outline is the row, 1px shorter (separator line)
        return Rect(t.rcListRow.x, t.rcListRow.y, t.rcListRow.dx, t.rcListRow.dy - 1);
    }
    // thumbnails: page ∪ name, inflated by the same amounts as paint
    Rect sel = t.rcPage.Union(t.rcText);
    sel.Inflate(DpiScale(hwnd, 4), DpiScale(hwnd, 3));
    return sel;
}

// Show/update the infotip for the keyboard-selected home entry. Placed just
// below the blue selection outline, left-aligned with the outline's left edge;
// shifted left if it would extend past the right edge of the last outline in
// that row.
static void HomePageShowSelectionTooltip(MainWindow* win) {
    if (!win || HomeSearchHasFocus(win)) {
        if (win) {
            win->DeleteToolTip();
        }
        return;
    }
    auto& c = gHomeLayoutCache;
    int idx = win->homePageSelIdx;
    if (!c.valid || idx < 0 || idx >= len(c.thumbs)) {
        win->DeleteToolTip();
        return;
    }
    HomeSyncLayoutCacheScroll(win);
    ThumbnailLayout& t = c.thumbs[idx];
    FileState* fs = t.fs;
    if (!fs || !fs->filePath) {
        win->DeleteToolTip();
        return;
    }

    // Same text as hover: path + size (size looked up only when shown)
    TempStr tip = HomeThumbTooltipTemp(fs->filePath);
    i64 size = file::GetSize(fs->filePath);
    if (size >= 0) {
        tip = fmt("%s  %s", tip, str::FormatSizeShortTemp(size, nullptr));
    }

    HWND hwnd = win->hwndCanvas;
    Rect outline = HomeSelectionOutlineRect(t, hwnd);
    // a little below the outline so the tip clears the blue border
    int tipClientX = outline.x;
    int tipClientY = outline.y + outline.dy + DpiScale(hwnd, 4);

    int rightEdgeClient = outline.x + outline.dx;
    if (!HomePageIsListView()) {
        int n = len(c.thumbs);
        int nCols = HomeGridColumnCount();
        nCols = std::max(nCols, 1);
        int col = idx % nCols;
        int rowStart = idx - col;
        int lastInRow = rowStart + nCols - 1;
        if (lastInRow >= n) {
            lastInRow = n - 1;
        }
        Rect lastOutline = HomeSelectionOutlineRect(c.thumbs[lastInRow], hwnd);
        rightEdgeClient = lastOutline.x + lastOutline.dx;
    }

    POINT tl{tipClientX, tipClientY};
    POINT tr{rightEdgeClient, tipClientY};
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &tr);
    win->ShowToolTipAt(tip, outline, Point(tl.x, tl.y), false, tr.x);
}

void HomePageSelectFirst(MainWindow* win) {
    win->homePageSelIdx = 0;
    win->homePageSearchReturnCol = 0;
}

void HomePageOnWindowActivate(MainWindow* win, bool active) {
    if (!win) {
        return;
    }
    if (!active) {
        win->DeleteToolTip();
        return;
    }
    // only restore the selection tip (positioned at the active entry, not cursor)
    if (win->IsCurrentTabAbout()) {
        HomePageShowSelectionTooltip(win);
    }
}

// Index of the file entry under (x,y), or -1. Uses layout-cache geometry so
// it matches the keyboard selection outline.
static int HomeEntryIndexAt(int x, int y) {
    auto& c = gHomeLayoutCache;
    if (!c.valid) {
        return -1;
    }
    Point pt(x, y);
    int n = len(c.thumbs);
    for (int i = 0; i < n; i++) {
        if (HomeEntryRect(c.thumbs[i]).Contains(pt)) {
            return i;
        }
    }
    return -1;
}

// Last mouse position that drove a selection change. Keyboard nav invalidates
// the canvas and Windows may re-send WM_MOUSEMOVE / WM_SETCURSOR with the same
// coordinates — ignore those so selection does not snap back under the cursor.
static Point gHomeHoverLastPt{-1, -1};

bool HomePageOnHover(MainWindow* win, int x, int y) {
    if (!win) {
        return false;
    }
    Point pt(x, y);
    if (pt.x == gHomeHoverLastPt.x && pt.y == gHomeHoverLastPt.y) {
        // no real mouse movement — leave selection alone
        return HomeEntryIndexAt(x, y) >= 0;
    }
    gHomeHoverLastPt = pt;

    HomeSyncLayoutCacheScroll(win);
    int idx = HomeEntryIndexAt(x, y);
    if (idx < 0) {
        return false;
    }
    if (idx != win->homePageSelIdx) {
        win->homePageSelIdx = idx;
        HwndInvalidate(win->hwndCanvas);
    }
    // tip always anchored to the active entry, never the cursor
    HomePageShowSelectionTooltip(win);
    return true;
}

Str HomePageSelectedFilePathTemp(MainWindow* win) {
    auto& c = gHomeLayoutCache;
    int idx = win->homePageSelIdx;
    if (!c.valid || idx < 0 || idx >= len(c.thumbs)) {
        return {};
    }
    FileState* fs = c.thumbs[idx].fs;
    if (!fs) {
        return {};
    }
    return str::DupTemp(fs->filePath);
}

void HomePageMoveSelection(MainWindow* win, int dCol, int dRow) {
    int n = HomeSelectableCount();
    if (n == 0) {
        win->DeleteToolTip();
        return;
    }
    int idx = win->homePageSelIdx;
    if (idx < 0 || idx >= n) {
        // nothing selected yet: any arrow key selects the first entry
        win->homePageSelIdx = 0;
        HomeScrollSelectionIntoView(win);
        HwndInvalidate(win->hwndCanvas);
        HomePageShowSelectionTooltip(win);
        return;
    }

    int nCols = HomePageIsListView() ? 1 : HomeGridColumnCount();
    int delta;
    if (HomePageIsListView()) {
        // one entry per row; left/right have nothing to move along
        delta = dRow;
    } else {
        delta = dCol + (dRow * nCols);
    }
    if (delta == 0) {
        return;
    }
    int newIdx = idx + delta;
    if (newIdx < 0) {
        // above the first row: hand focus to the search box, remember column
        if (dRow < 0 && win->hwndHomeSearch) {
            win->homePageSearchReturnCol = HomePageIsListView() ? 0 : (idx % nCols);
            win->DeleteToolTip();
            HwndSetFocus(win->hwndHomeSearch);
            HwndInvalidate(win->hwndCanvas); // drop selection outline while typing
            return;
        }
        newIdx = 0;
    }
    if (newIdx >= n) {
        newIdx = n - 1;
    }
    if (newIdx == idx) {
        return;
    }
    win->homePageSelIdx = newIdx;
    HomeScrollSelectionIntoView(win);
    HwndInvalidate(win->hwndCanvas);
    HomePageShowSelectionTooltip(win);
}

void HomePageOnVScroll(MainWindow* win, WPARAM wp) {
    USHORT msg = LOWORD(wp);
    HDC hdc = GetDC(win->hwndCanvas);
    int lineDy = HomePageIsListView() ? kHomeListRowDy : kThumbCardDy + kThumbsSpaceBetweenY;
    int pageDy = lineDy * 3;
    ReleaseDC(win->hwndCanvas, hdc);

    int newScrollY = win->homePageScrollY;
    switch (msg) {
        case SB_LINEUP:
            newScrollY -= lineDy;
            break;
        case SB_LINEDOWN:
            newScrollY += lineDy;
            break;
        case SB_PAGEUP:
            newScrollY -= pageDy;
            break;
        case SB_PAGEDOWN:
            newScrollY += pageDy;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            int pos = (int)(short)HIWORD(wp);
            // overlay scrollbar sends full position in HIWORD for THUMBTRACK
            if (win->overlayScrollV) {
                pos = win->overlayScrollV->nTrackPos;
            }
            newScrollY = pos;
            break;
        }
        case SB_TOP:
            newScrollY = 0;
            break;
        case SB_BOTTOM:
            newScrollY = INT_MAX; // will be clamped by layout
            break;
    }
    newScrollY = std::max(newScrollY, 0);
    if (newScrollY != win->homePageScrollY) {
        win->homePageScrollY = newScrollY;
        HwndInvalidate(win->hwndCanvas);
    }
}

static void ResizeTouchLibrarySidebar(MainWindow* win, int x) {
    int dx = ClampTouchLibrarySidebarDx(win, win->librarySidebarResizeStartDx + x - win->librarySidebarResizeStartX);
    if (dx != win->librarySidebarDx) {
        win->librarySidebarDx = dx;
        HwndInvalidate(win->hwndCanvas, false);
    }
}

bool HomePageOnLibraryResizeMouse(MainWindow* win, UINT msg, int x, int y) {
    if (!win || !IsTouchChrome(win) || win->touchView != TouchView::Library) {
        return false;
    }
    if (msg == WM_LBUTTONDOWN && !win->libraryManageFoldersOpen &&
        TouchLibrarySplitterHitRect(win).Contains(Point{x, y})) {
        win->librarySidebarResizing = true;
        win->librarySidebarResizeStartX = x;
        win->librarySidebarResizeStartDx = TouchLibrarySidebarDx(win);
        str::FreePtr(&win->urlOnLastButtonDown);
        CloseTouchLibraryTransientUi(win);
        SetCapture(win->hwndCanvas);
        SetCursorCached(IDC_SIZEWE);
        return true;
    }
    if (msg == WM_MOUSEMOVE && win->librarySidebarResizing) {
        ResizeTouchLibrarySidebar(win, x);
        SetCursorCached(IDC_SIZEWE);
        return true;
    }
    if (msg == WM_LBUTTONUP && win->librarySidebarResizing) {
        ResizeTouchLibrarySidebar(win, x);
        win->librarySidebarResizing = false;
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        return true;
    }
    if (msg == WM_CAPTURECHANGED && win->librarySidebarResizing) {
        win->librarySidebarResizing = false;
        return true;
    }
    return false;
}

bool HomePageSetLibraryResizeCursor(MainWindow* win) {
    if (!win || !IsTouchChrome(win) || win->touchView != TouchView::Library || win->libraryManageFoldersOpen) {
        return false;
    }
    Point pt = HwndGetCursorPos(win->hwndCanvas);
    if (!win->librarySidebarResizing && !TouchLibrarySplitterHitRect(win).Contains(pt)) {
        return false;
    }
    SetCursorCached(IDC_SIZEWE);
    return true;
}

void HomePageOnMouseWheel(MainWindow* win, int delta, Point canvasPt) {
    if (IsTouchChrome(win) && win->touchView == TouchView::Library) {
        if (win->libraryManageFoldersOpen) {
            return;
        }
        int step = DpiScale(win->hwndCanvas, 72);
        int dy = delta > 0 ? -step : step;
        if (canvasPt.x < TouchLibrarySidebarDx(win)) {
            win->libraryTreeScrollY = std::clamp(win->libraryTreeScrollY + dy, 0, win->libraryTreeScrollMaxY);
        } else {
            win->libraryFilesScrollY = std::clamp(win->libraryFilesScrollY + dy, 0, win->libraryFilesScrollMaxY);
        }
        HwndInvalidate(win->hwndCanvas);
        return;
    }

    HDC hdc = GetDC(win->hwndCanvas);
    int thumbsRowDy = HomePageIsListView() ? kHomeListRowDy : kThumbCardDy + kThumbsSpaceBetweenY;
    ReleaseDC(win->hwndCanvas, hdc);

    int scrollBy = thumbsRowDy / 3;
    if (delta > 0) {
        scrollBy = -scrollBy;
    }
    int newScrollY = win->homePageScrollY + scrollBy;
    newScrollY = std::max(newScrollY, 0);
    if (newScrollY != win->homePageScrollY) {
        win->homePageScrollY = newScrollY;
        HwndInvalidate(win->hwndCanvas);
    }
}

void HomePageOnMouseHWheel(MainWindow* win, int delta) {
    if (!IsTouchChrome(win) || win->touchView != TouchView::Home || win->homeOpenScrollMaxX <= 0) {
        return;
    }
    int step = DpiScale(win->hwndCanvas, 120);
    int dx = delta > 0 ? -step : step;
    int scrollX = std::clamp(win->homeOpenScrollX + dx, 0, win->homeOpenScrollMaxX);
    if (scrollX != win->homeOpenScrollX) {
        win->homeOpenScrollX = scrollX;
        HwndInvalidate(win->hwndCanvas);
    }
}

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#endif

#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

static Point TouchAboutPointerPos(MainWindow* win, LPARAM lp) {
    Point screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    return HwndScreenToClient(win->hwndCanvas, screen);
}

bool HomePageOnPointerEvent(MainWindow* win, UINT msg, WPARAM wp, LPARAM lp, Point* tapPt) {
    if (tapPt) {
        *tapPt = Point{-1, -1};
    }
    if (!win || !IsTouchChrome(win)) {
        return false;
    }
    UINT32 pointerId = LOWORD(wp);
    if (msg == WM_POINTERDOWN) {
        if (win->touchAboutPointerId != 0 || win->libraryManageFoldersOpen) {
            return false;
        }
        Point pt = TouchAboutPointerPos(win, lp);
        Rect canvas = HwndClientRect(win->hwndCanvas);
        int headerDy = DpiScale(win->hwndCanvas, 64);
        int area = 0;
        if (win->touchView == TouchView::Home && pt.y >= headerDy) {
            area = win->homeOpenCarouselRect.Contains(pt) ? 2 : 1;
        } else if (win->touchView == TouchView::Library && TouchLibrarySplitterHitRect(win).Contains(pt)) {
            area = 5;
        } else if (win->touchView == TouchView::Library && pt.y >= headerDy) {
            int leftDx = TouchLibrarySidebarDx(win);
            int manageTop = canvas.dy - DpiScale(win->hwndCanvas, 64);
            area = pt.x < leftDx && pt.y < manageTop ? 3 : (pt.x >= leftDx ? 4 : 0);
        }
        if (area == 0) {
            return false;
        }
        win->touchAboutPointerId = pointerId;
        win->touchAboutPanArea = area;
        win->touchAboutPanAxis = area == 2 ? 0 : 2;
        win->touchAboutPanStart = pt;
        win->touchAboutPanStartX = win->homeOpenScrollX;
        if (area == 5) {
            win->librarySidebarResizeStartX = pt.x;
            win->librarySidebarResizeStartDx = TouchLibrarySidebarDx(win);
        }
        if (area == 1 || area == 2) {
            win->touchAboutPanStartY = win->homePageScrollY;
        } else if (area == 3) {
            win->touchAboutPanStartY = win->libraryTreeScrollY;
        } else {
            win->touchAboutPanStartY = win->libraryFilesScrollY;
        }
        win->touchAboutPanMoved = false;
        win->touchAboutSuppressMouseUp = false;
        return true;
    }
    if (pointerId != win->touchAboutPointerId) {
        return false;
    }
    if (msg == WM_POINTERUPDATE) {
        Point pt = TouchAboutPointerPos(win, lp);
        int dx = pt.x - win->touchAboutPanStart.x;
        int dy = pt.y - win->touchAboutPanStart.y;
        int threshold = DpiScale(win->hwndCanvas, 6);
        if (!win->touchAboutPanMoved && abs(dx) < threshold && abs(dy) < threshold) {
            return true;
        }
        if (!win->touchAboutPanMoved) {
            win->touchAboutPanMoved = true;
            str::FreePtr(&win->urlOnLastButtonDown);
            CloseTouchLibraryTransientUi(win);
        }
        if (win->touchAboutPanArea == 2 && win->touchAboutPanAxis == 0) {
            win->touchAboutPanAxis = abs(dx) > abs(dy) ? 1 : 2;
        }
        if (win->touchAboutPanArea == 5) {
            ResizeTouchLibrarySidebar(win, pt.x);
        } else if (win->touchAboutPanArea == 2 && win->touchAboutPanAxis == 1) {
            win->homeOpenScrollX = std::clamp(win->touchAboutPanStartX - dx, 0, win->homeOpenScrollMaxX);
        } else if (win->touchAboutPanArea == 1 || win->touchAboutPanArea == 2) {
            win->homePageScrollY = std::max(0, win->touchAboutPanStartY - dy);
        } else if (win->touchAboutPanArea == 3) {
            win->libraryTreeScrollY = std::clamp(win->touchAboutPanStartY - dy, 0, win->libraryTreeScrollMaxY);
        } else if (win->touchAboutPanArea == 4) {
            win->libraryFilesScrollY = std::clamp(win->touchAboutPanStartY - dy, 0, win->libraryFilesScrollMaxY);
        }
        HwndInvalidate(win->hwndCanvas, false);
        return true;
    }
    if (msg == WM_POINTERUP) {
        Point pt = TouchAboutPointerPos(win, lp);
        int area = win->touchAboutPanArea;
        bool moved = win->touchAboutPanMoved;
        win->touchAboutSuppressMouseUp = moved;
        win->touchAboutPointerId = 0;
        win->touchAboutPanArea = 0;
        win->touchAboutPanAxis = 0;
        win->touchAboutPanMoved = false;
        if (!moved && area != 5 && tapPt) {
            *tapPt = pt;
        }
        return true;
    }
    if (msg == WM_POINTERCAPTURECHANGED) {
        win->touchAboutSuppressMouseUp = win->touchAboutPanMoved;
        win->touchAboutPointerId = 0;
        win->touchAboutPanArea = 0;
        win->touchAboutPanAxis = 0;
        win->touchAboutPanMoved = false;
        return true;
    }
    return false;
}
