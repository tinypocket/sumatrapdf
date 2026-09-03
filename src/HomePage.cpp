/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Dict.h"
#include "base/Dpi.h"
#include "base/DirScan.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/UITask.h"
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
#include "Canvas.h"
#include "wingui/Anim.h"
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
    FreeTouchLibraryModel();
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
    // name the architecture: an ARM64 build otherwise also reported "64-bit",
    // so there was no way to tell which one was installed on an ARM machine
    if (IsArmBuild()) {
        s = str::JoinTemp(s, StrL(" ARM64"));
    } else if (IsProcess64()) {
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

// a query always searches the whole Library, whichever sidebar row was
// selected before typing
#define kTouchHomeSearchCue "Search library"

static LRESULT CALLBACK WndProcHomeSearch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        LRESULT res = CallWindowProcW(DefWndProcHomeSearch, hwnd, msg, wp, lp);
        MainWindow* win = FindMainWindowByHwnd(GetParent(hwnd));
        if (win && IsTouchChrome(win)) {
            EditPaintThemedCue(hwnd, StrL(kTouchHomeSearchCue), ThemeWindowDarkerTextColor());
        }
        return res;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        // the cue shows only while unfocused
        HwndInvalidate(hwnd, false);
    }
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
// same entries GetFrequencyOrder / GetRecentlyOpenedOrder return, minus the sort
static int CountHomePageFiles() {
    int n = 0;
    if (!gFileHistory.states) {
        return 0;
    }
    for (FileState* fs : *gFileHistory.states) {
        if (fs && len(fs->filePath) > 0 && (!fs->isMissing || fs->isPinned)) {
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
    // the touch chrome paints its own cue (kTouchHomeSearchCue, see
    // WndProcHomeSearch) in the theme's muted color; the native banner is a
    // fixed system gray that all but vanishes on a dark theme
    TempStr cue =
        IsTouchChrome(win) ? str::DupTemp("") : fmt(_TRA("Search %d files (Ctrl + F)").s, CountHomePageFiles());
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

    // The touch chrome's cards carry their own pin badge and get their hover
    // and press feedback from the Library overlay. This legacy close button
    // drew a second, unexplained X in the corner on top of that, so it stays
    // on the classic home page only.
    if (IsTouchChrome(win)) {
        HomePageHideCloseButton();
        return;
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

// Translucent rounded fill. GDI's RoundRect has no alpha, and the Library
// feedback overlay has to sit on top of cards, rows and pills without knowing
// what is underneath, so this goes through GDI+ where the brush carries alpha.
// defined with the rest of the Library feedback code, below
static void DrawLibraryFeedback(MainWindow* win, HDC hdc);
static void DrawPinFlight(MainWindow* win, HDC hdc);
static void NoteLibraryContentSwap(MainWindow* win, HDC hdc, Rect content, Str key);
static void DrawLibraryContentSwap(MainWindow* win, HDC hdc);

static void FillHomeRoundRectAlpha(HDC hdc, const Rect& r, int radius, COLORREF col, u8 alpha) {
    if (alpha == 0 || r.dx <= 0 || r.dy <= 0) {
        return;
    }
    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int d = std::min(radius * 2, std::min(r.dx, r.dy));
    Gdiplus::GraphicsPath path;
    if (d > 1) {
        path.AddArc(r.x, r.y, d, d, 180.0f, 90.0f);
        path.AddArc(r.x + r.dx - d, r.y, d, d, 270.0f, 90.0f);
        path.AddArc(r.x + r.dx - d, r.y + r.dy - d, d, d, 0.0f, 90.0f);
        path.AddArc(r.x, r.y + r.dy - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    } else {
        path.AddRectangle(Gdiplus::Rect(r.x, r.y, r.dx, r.dy));
    }
    Gdiplus::Color c(alpha, GetRValue(col), GetGValue(col), GetBValue(col));
    Gdiplus::SolidBrush br(c);
    gfx.FillPath(&br, &path);
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

// file::GetSize for the Recent cards, which get redrawn on every hover tick.
// Small and path-keyed; the Library keeps sizes on its own per-file data.
struct HomeFileSizeEntry {
    i64 size = kSizeNotFetched;
    u32 fetchedAt = 0;
};
static StrVecWithData<HomeFileSizeEntry> gHomeFileSizes;
constexpr int kHomeFileSizeCacheMs = 30 * 1000;
constexpr int kHomeFileSizeCacheMax = 256;

static i64 HomeFileSizeCached(Str filePath) {
    u32 now = GetTickCount();
    int idx = gHomeFileSizes.FindI(filePath);
    if (idx < 0) {
        if (len(gHomeFileSizes) >= kHomeFileSizeCacheMax) {
            gHomeFileSizes.Reset();
        }
        idx = gHomeFileSizes.Append(filePath, HomeFileSizeEntry{});
    }
    HomeFileSizeEntry* e = gHomeFileSizes.AtData(idx);
    if (e->size == kSizeNotFetched || now - e->fetchedAt > (u32)kHomeFileSizeCacheMs) {
        e->size = file::GetSize(filePath);
        e->fetchedAt = now;
    }
    return e->size;
}

// Fits a folder line ("Books › Μελοδός › Kontakia") on one line by dropping
// components from the front, "… › Μελοδός › Kontakia": the end of the path is
// what tells one folder from another, so that is the part worth keeping.
static TempStr FitPathTailTemp(HDC hdc, Str line, int maxDx, HFONT font) {
    Str sep = StrL(" › ");
    Str ellipsis = StrL("… › ");
    if (HdcMeasureText(hdc, line, font).dx <= maxDx) {
        return str::DupTemp(line);
    }
    Str rest = line;
    for (;;) {
        int idx = str::IndexOf(rest, sep);
        if (idx < 0) {
            // a single component: DT_END_ELLIPSIS on the caller's side
            return str::DupTemp(rest);
        }
        rest = Str(rest.s + idx + len(sep), len(rest) - idx - len(sep));
        TempStr candidate = str::JoinTemp(ellipsis, rest);
        if (HdcMeasureText(hdc, candidate, font).dx <= maxDx) {
            return candidate;
        }
    }
}

static void DrawTouchFileCardPath(MainWindow* win, HDC hdc, Str filePath, FileState* fs,
                                  RenderedBitmap* explicitThumbnail, const Rect& card, bool showProgress,
                                  const Rect* linkClip = nullptr, bool twoLineName = false, bool pinnable = false,
                                  i64 knownSize = kSizeNotFetched, Str folderLine = {}, Str folderTarget = {},
                                  bool highlighted = false) {
    COLORREF pageBg = ThemeWindowControlBackgroundColor();
    if (highlighted) {
        // "Show in Library folder" landed here: an accent halo behind the
        // card, so the eye finds it among its neighbours
        COLORREF haloBg = 0;
        COLORREF haloFg = 0;
        ThemeAccentSurfaceColors(&haloBg, &haloFg);
        Rect halo = card;
        halo.Inflate(DpiScale(hdc, 6), DpiScale(hdc, 6));
        FillHomeRoundRect(hdc, halo, DpiScale(hdc, 14), haloBg, ThemeWindowLinkColor());
    }
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

    WindowTab* openTab = showProgress && fs ? FindTouchOpenTabByPath(filePath) : nullptr;
    int pageNo = openTab && openTab->ctrl ? openTab->ctrl->CurrentPageNo() : (fs ? std::max(1, fs->pageNo) : 1);
    int pageCount = openTab && openTab->ctrl ? openTab->ctrl->PageCount() : 0;
    if (showProgress && fs) {
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

    i64 size = knownSize == kSizeNotFetched ? HomeFileSizeCached(filePath) : knownSize;
    TempStr meta = size >= 0 ? str::FormatSizeShortTemp(size, nullptr) : str::DupTemp("");
    if (showProgress && fs) {
        meta = pageCount > 0 ? fmt("%d / %d · %s", pageNo, pageCount, meta) : fmt("Page %d · %s", pageNo, meta);
    }
    Rect metaRc{nameRc.x, nameRc.y + nameRc.dy + DpiScale(hdc, 1), nameRc.dx, DpiScale(hdc, 17)};
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, meta, metaRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 12));

    // Search results: where the file lives, since matches come from all over
    // the library. Tapping it opens that folder. Registered before the card's
    // own link so the first-match hit test resolves to it.
    if (len(folderLine) > 0) {
        HFONT folderFont = HdcGetUiFont(hdc, 11);
        Rect folderRc{nameRc.x, metaRc.y + metaRc.dy + DpiScale(hdc, 1), nameRc.dx, DpiScale(hdc, 16)};
        TempStr shown = FitPathTailTemp(hdc, folderLine, folderRc.dx, folderFont);
        SetTextColor(hdc, ThemeWindowLinkColor());
        HdcDrawText(hdc, shown, folderRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, folderFont);
        if (len(folderTarget) > 0) {
            Rect folderLink = linkClip ? folderRc.Intersect(*linkClip) : folderRc;
            if (!folderLink.IsEmpty()) {
                win->staticLinks.Append(new StaticLink(folderLink, folderTarget, folderLine));
            }
        }
    }

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

static void DrawTouchFileCard(MainWindow* win, HDC hdc, FileState* fs, const Rect& card, bool showProgress,
                              const Rect* linkClip = nullptr) {
    // pinnable: the Recent surface gets the same pin badge the Library grid
    // has, so a file can be pinned (or unpinned) without going to the Library
    DrawTouchFileCardPath(win, hdc, fs->filePath, fs, nullptr, card, showProgress, linkClip, false, true);
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

// Recent folders: the distinct parent directories of the recent files, in the
// same most-recent-first order. Drawn as small pills rather than cards - a
// folder is a destination, not a document, and it has no thumbnail to show.
static void CollectRecentFolders(const Vec<FileState*>& files, StrVec& out, int maxCount) {
    for (FileState* fs : files) {
        if (!fs || len(fs->filePath) == 0) {
            continue;
        }
        TempStr dir = path::GetDirTemp(fs->filePath);
        if (!dir || len(dir) == 0) {
            continue;
        }
        if (out.FindI(dir) >= 0) {
            continue; // already have this folder from a more recent file
        }
        out.Append(dir);
        if (len(out) >= maxCount) {
            return;
        }
    }
}

// The "Recent" surface: the PINNED and RECENT file-card sections. It used to be a view of its own (TouchView::Home);
// now it is drawn into the Library's content pane when its "Recent" sidebar row is selected. contentRc is that pane
// (right of the sidebar, below the header) and the pane's scroll position is win->libraryFilesScrollY.
static void DrawTouchRecentCards(MainWindow* win, HDC hdc, const Rect& contentRc) {
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
    // No "Currently open" carousel: an open document is still a recent file and
    // appears in the RECENT section like any other, so the carousel only
    // duplicated it and pushed everything else down.
    win->homeOpenScrollX = 0;
    win->homeOpenScrollMaxX = 0;
    win->homeOpenCarouselRect = {};
    // Lay the folder pills out first: their wrapped height feeds the scroll
    // extent below, so it has to be known before anything is drawn.
    StrVec recentFolders;
    CollectRecentFolders(files, recentFolders, 10);
    int pillDy = DpiScale(hdc, 34);
    int pillGap = DpiScale(hdc, 10);
    int pillPadX = DpiScale(hdc, 14);
    Vec<Rect> pillRects; // offsets relative to the block's top-left
    int pillBlockDy = 0;
    {
        int availDx = std::max(0, contentRc.dx - 2 * pad);
        HFONT pillFont = HdcGetUiFont(hdc, 13);
        ScopedSelectObject selPill(hdc, pillFont);
        int px = 0;
        int py = 0;
        for (int i = 0; i < len(recentFolders); i++) {
            TempStr name = path::GetBaseNameTemp(recentFolders[i]);
            Size sz = HdcGetTextExtentPoint32(hdc, name);
            int pillDx = sz.dx + 2 * pillPadX;
            if (px > 0 && px + pillDx > availDx) {
                px = 0;
                py += pillDy + pillGap;
            }
            pillRects.Append(Rect{px, py, pillDx, pillDy});
            px += pillDx + pillGap;
        }
        if (len(pillRects) > 0) {
            pillBlockDy = py + pillDy;
        }
    }
    int folderBlockDy = len(pillRects) > 0 ? DpiScale(hdc, 31) + pillBlockDy + gap : 0;

    int columns = TouchCardColumns(win->hwndCanvas, contentRc.dx - 2 * pad);
    int pinnedRows = TouchCardRows(len(pinned), columns);
    int recentRows = TouchCardRows(len(recent), columns);
    int contentDy = pad + folderBlockDy +
                    (pinnedRows ? DpiScale(hdc, 31) + pinnedRows * cardBlockDy + (pinnedRows - 1) * gap + gap : 0) +
                    DpiScale(hdc, 31) + recentRows * cardBlockDy + std::max(0, recentRows - 1) * gap + pad;
    win->libraryFilesScrollMaxY = std::max(0, contentDy - contentRc.dy);
    win->libraryFilesScrollY = std::clamp(win->libraryFilesScrollY, 0, win->libraryFilesScrollMaxY);

    int contentX = contentRc.x + pad;
    int contentDx = std::max(0, contentRc.dx - 2 * pad);
    int contentBottom = contentRc.y + contentRc.dy;
    int saved = SaveDC(hdc);
    IntersectClipRect(hdc, contentRc.x, contentRc.y, contentRc.x + contentRc.dx, contentBottom);
    int y = contentRc.y + pad - win->libraryFilesScrollY;
    auto drawGroup = [&](Str label, const Vec<FileState*>& group, bool showProgress) {
        if (len(group) == 0 && str::Eq(label, StrL("PINNED"))) {
            return;
        }
        Rect labelRc{contentX, y, contentDx, DpiScale(hdc, 18)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, label, labelRc, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
        y += DpiScale(hdc, 31);
        for (int i = 0; i < len(group); i++) {
            int col = i % columns;
            int row = i / columns;
            Rect card{contentX + col * (cardDx + gap), y + row * (cardBlockDy + gap), cardDx, cardDy};
            if (card.y + cardBlockDy >= contentRc.y && card.y < contentBottom) {
                // clip the card's links to the scrolling pane, or a badge on a
                // half-scrolled card stays clickable outside it
                DrawTouchFileCard(win, hdc, group[i], card, showProgress, &contentRc);
            }
        }
        int rows = TouchCardRows(len(group), columns);
        y += rows * cardBlockDy + std::max(0, rows - 1) * gap + gap;
    };
    // where a pin flight should land: the PINNED header's slot, whether or not
    // the section is currently drawn (it is skipped when nothing is pinned)
    HomePageSetPinAnchor(win, false, Rect{contentX, y, contentDx, DpiScale(hdc, 18)});
    drawGroup(StrL("PINNED"), pinned, false);

    // folder pills sit between the pinned files and the recent ones
    if (len(pillRects) > 0) {
        Rect labelRc{contentX, y, contentDx, DpiScale(hdc, 18)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("RECENT FOLDERS"), labelRc, DT_SINGLELINE | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
        y += DpiScale(hdc, 31);
        HFONT pillFont = HdcGetUiFont(hdc, 13);
        ScopedSelectObject selPill(hdc, pillFont);
        COLORREF pillBg = ThemeControlBackgroundColor();
        for (int i = 0; i < len(pillRects) && i < len(recentFolders); i++) {
            Rect r = pillRects[i];
            Rect pill{contentX + r.x, y + r.y, r.dx, r.dy};
            if (pill.y + pill.dy < contentRc.y || pill.y > contentBottom) {
                continue; // scrolled out of the pane
            }
            FillHomeRoundRect(hdc, pill, pill.dy / 2, pillBg, ThemeEdgeColor());
            SetTextColor(hdc, ThemeWindowTextColor());
            TempStr name = path::GetBaseNameTemp(recentFolders[i]);
            Rect textRc{pill.x + pillPadX, pill.y, pill.dx - 2 * pillPadX, pill.dy};
            HdcDrawText(hdc, name, textRc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            Rect linkRect = pill.Intersect(contentRc);
            if (!linkRect.IsEmpty()) {
                // opens that folder in the Library, same as its sidebar row
                TempStr target = str::JoinTemp(kLinkLibraryFolderCardPrefix, recentFolders[i]);
                win->staticLinks.Append(new StaticLink(linkRect, target, recentFolders[i]));
            }
        }
        y += pillBlockDy + gap;
    }

    drawGroup(StrL("RECENT"), recent, true);
    if (len(pinned) == 0 && len(recent) == 0) {
        Rect empty{contentX, y, contentDx, DpiScale(hdc, 40)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        Str message = len(filterWords) > 0 ? StrL("No matching files.") : StrL("No recent files yet.");
        HdcDrawText(hdc, message, empty, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 14));
    }
    RestoreDC(hdc, saved);
}

struct TouchLibraryFolderData {
    int parent = -1;
    int depth = 0;
    int directCount = 0;
    int directFolderCount = 0;
    bool hasChildren = false;
};

// size is a cache of file::GetSize(): the card grid and list rows show it on
// every paint, and stat-ing every visible file per hover frame is slow on
// OneDrive/network folders.
struct TouchLibraryFileData {
    RenderedBitmap* thumbnail = nullptr;
    bool thumbnailRequested = false;
    int folderIdx = -1;
    i64 size = kSizeNotFetched;
    u32 sizeFetchedAt = 0;
};

static StrVecWithData<TouchLibraryFolderData> gTouchLibraryFolders;
static StrVecWithData<TouchLibraryFileData> gTouchLibraryFiles;
static bool gTouchLibraryValid = false;

// The disk walk runs on a background thread so the first Library paint (and
// every folder add/remove) doesn't freeze the window for seconds on a large
// library. The scan is generation-stamped: an invalidate while a scan is in
// flight bumps the generation, and the stale result is discarded when it lands.
static int gTouchLibraryScanGen = 0;
static bool gTouchLibraryScanning = false;

struct TouchLibraryScan {
    int gen = 0;
    StrVec roots;
    StrVecWithData<TouchLibraryFolderData> folders;
    StrVecWithData<TouchLibraryFileData> files;
};

static void TouchLibraryInvalidateWindows() {
    for (MainWindow* win : gWindows) {
        if (win->hwndCanvas && win->touchView == TouchView::Library) {
            HwndInvalidate(win->hwndCanvas, false);
        }
    }
}

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
        TouchLibraryInvalidateWindows();
    }
    delete thumbnail;
    delete request;
}

static bool TouchLibraryPathWithin(Str path, Str root);

// shutdown: a scan still in flight is dropped by its generation check
void FreeTouchLibraryModel() {
    gTouchLibraryScanGen++;
    gTouchLibraryValid = false;
    gTouchLibraryFolders.Reset();
    for (int i = 0; i < len(gTouchLibraryFiles); i++) {
        delete gTouchLibraryFiles.AtData(i)->thumbnail;
    }
    gTouchLibraryFiles.Reset();
}

// Keeps the current model on screen (minus folders under removed roots, so a
// removal is reflected immediately) and lets the next paint kick off a rescan.
void HomePageInvalidateLibrary() {
    gTouchLibraryScanGen++;
    gTouchLibraryValid = false;
    Vec<Str>* roots = gGlobalPrefs ? gGlobalPrefs->libraryFolders : nullptr;
    for (int i = len(gTouchLibraryFolders) - 1; i >= 0; i--) {
        bool keep = false;
        if (roots) {
            for (Str root : *roots) {
                TempStr normalized = path::NormalizeTemp(root);
                if (normalized && TouchLibraryPathWithin(gTouchLibraryFolders[i], normalized)) {
                    keep = true;
                    break;
                }
            }
        }
        if (keep) {
            continue;
        }
        for (int j = len(gTouchLibraryFiles) - 1; j >= 0; j--) {
            if (gTouchLibraryFiles.AtData(j)->folderIdx == i) {
                delete gTouchLibraryFiles.AtData(j)->thumbnail;
                gTouchLibraryFiles.RemoveAt(j);
            }
        }
        gTouchLibraryFolders.RemoveAt(i);
        // indexes above i shifted down by one
        for (int j = 0; j < len(gTouchLibraryFolders); j++) {
            int* parent = &gTouchLibraryFolders.AtData(j)->parent;
            if (*parent == i) {
                *parent = -1;
            } else if (*parent > i) {
                (*parent)--;
            }
        }
        for (int j = 0; j < len(gTouchLibraryFiles); j++) {
            int* folderIdx = &gTouchLibraryFiles.AtData(j)->folderIdx;
            if (*folderIdx > i) {
                (*folderIdx)--;
            }
        }
    }
    TouchLibraryInvalidateWindows();
}

static bool TouchLibraryCanOpenFile(Str filePath) {
    FileType kind = GuessFileTypeFromName(filePath, true);
    return IsSupportedFileType(kind, true) || DocIsSupportedFileType(kind);
}

// Dedupe is only needed when a root overlaps one scanned before it (one root
// nested inside another); the common case appends straight through.
static void ScanTouchLibraryRoot(Str root, StrVec& scannedRoots, StrVecWithData<TouchLibraryFolderData>& folders,
                                 StrVecWithData<TouchLibraryFileData>& files) {
    TempStr normalized = path::NormalizeTemp(root);
    if (!normalized || !dir::Exists(normalized)) {
        return;
    }
    bool overlaps = false;
    for (Str prev : scannedRoots) {
        if (TouchLibraryPathWithin(normalized, prev) || TouchLibraryPathWithin(prev, normalized)) {
            overlaps = true;
            break;
        }
    }
    scannedRoots.Append(normalized);
    if (!overlaps || folders.FindI(normalized) < 0) {
        folders.Append(normalized, TouchLibraryFolderData{});
    }
    DirIter di{normalized};
    di.includeFiles = true;
    di.includeDirs = true;
    di.recurse = true;
    for (DirIterEntry* de : di) {
        Str path = de->filePath;
        if (len(path) == 0) {
            continue;
        }
        if (IsDirectory(de)) {
            if (!overlaps || folders.FindI(path) < 0) {
                folders.Append(path, TouchLibraryFolderData{});
            }
        } else if (IsRegularFile(de) && TouchLibraryCanOpenFile(path)) {
            if (!overlaps || files.FindI(path) < 0) {
                files.Append(path, TouchLibraryFileData{});
            }
        }
    }
}

static int TouchLibraryFolderLookup(dict::MapStrToInt& byPath, StrVecWithData<TouchLibraryFolderData>& folders,
                                    Str path) {
    int idx = -1;
    if (byPath.Get(path, &idx)) {
        return idx;
    }
    // paths come from the same directory walk so a case mismatch is unusual
    return folders.FindI(path);
}

static void FinishTouchLibraryModel(StrVecWithData<TouchLibraryFolderData>& folders,
                                    StrVecWithData<TouchLibraryFileData>& files) {
    SortNatural(&folders);
    SortNatural(&files);
    dict::MapStrToInt byPath(std::max(len(folders) * 2, 64));
    for (int i = 0; i < len(folders); i++) {
        *folders.AtData(i) = TouchLibraryFolderData{};
        byPath.Insert(folders[i], i);
    }
    for (int i = 0; i < len(files); i++) {
        int folder = TouchLibraryFolderLookup(byPath, folders, path::GetDirTemp(files[i]));
        files.AtData(i)->folderIdx = folder;
        if (folder >= 0) {
            folders.AtData(folder)->directCount++;
        }
    }
    for (int i = 0; i < len(folders); i++) {
        TempStr parentPath = path::GetDirTemp(folders[i]);
        int parent = TouchLibraryFolderLookup(byPath, folders, parentPath);
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

static void SwapStrVec(StrVec* a, StrVec* b) {
    std::swap(a->first, b->first);
    std::swap(a->sortIndexes, b->sortIndexes);
    std::swap(a->nextPageSize, b->nextPageSize);
    std::swap(a->size, b->size);
}

// UI thread. Swaps the freshly scanned model in, carrying over thumbnails
// already rendered for files that are still present.
static void TouchLibraryScanFinished(TouchLibraryScan* scan) {
    gTouchLibraryScanning = false;
    if (scan->gen != gTouchLibraryScanGen) {
        // stale: the next Library paint starts a fresh scan
        delete scan;
        TouchLibraryInvalidateWindows();
        return;
    }
    for (int i = 0; i < len(gTouchLibraryFiles); i++) {
        TouchLibraryFileData* old = gTouchLibraryFiles.AtData(i);
        if (!old->thumbnail && !old->thumbnailRequested) {
            continue;
        }
        int idx = scan->files.FindI(gTouchLibraryFiles[i]);
        if (idx < 0) {
            continue;
        }
        TouchLibraryFileData* fresh = scan->files.AtData(idx);
        fresh->thumbnail = old->thumbnail;
        fresh->thumbnailRequested = old->thumbnailRequested;
        fresh->size = old->size;
        fresh->sizeFetchedAt = old->sizeFetchedAt;
        old->thumbnail = nullptr;
    }
    for (int i = 0; i < len(gTouchLibraryFiles); i++) {
        delete gTouchLibraryFiles.AtData(i)->thumbnail;
    }
    SwapStrVec(&gTouchLibraryFolders, &scan->folders);
    SwapStrVec(&gTouchLibraryFiles, &scan->files);
    delete scan;
    gTouchLibraryValid = true;
    TouchLibraryInvalidateWindows();
}

// The last scan is kept next to the thumbnail cache so a launch shows the
// Library immediately (walking a cloud-synced folder takes seconds) while
// the real scan refreshes it in the background. The roots are stored with
// it: an index made for a different set of roots is ignored.
#define kTouchLibraryIndexHeader "SumatraPDF library index v1"

static TempStr TouchLibraryIndexPathTemp() {
    TempStr dir = GetThumbnailCacheDirTemp();
    if (!dir) {
        return {};
    }
    return path::JoinTemp(dir, StrL("library-index.txt"));
}

static void SaveTouchLibraryIndex(TouchLibraryScan* scan) {
    TempStr path = TouchLibraryIndexPathTemp();
    if (!path) {
        return;
    }
    str::Builder out;
    out.Append(StrL(kTouchLibraryIndexHeader));
    out.AppendChar('\n');
    for (Str root : scan->roots) {
        out.Append(StrL("root:"));
        out.Append(root);
        out.AppendChar('\n');
    }
    out.Append(StrL("folders:\n"));
    for (Str folder : scan->folders) {
        out.Append(folder);
        out.AppendChar('\n');
    }
    out.Append(StrL("files:\n"));
    for (Str file : scan->files) {
        out.Append(file);
        out.AppendChar('\n');
    }
    int err = 0;
    if (dir::CreateForFile(path, &err)) {
        file::WriteFile(path, ToStrTemp(out));
    }
}

// UI thread, before the first scan. false if there is no usable index.
static bool LoadTouchLibraryIndex() {
    TempStr path = TouchLibraryIndexPathTemp();
    if (!path) {
        return false;
    }
    Str data = file::ReadFile(path);
    if (!data) {
        return false;
    }
    StrVec lines;
    Split(&lines, data, StrL("\n"));
    str::Free(data);
    Vec<Str>* roots = gGlobalPrefs->libraryFolders;
    int nRoots = roots ? len(*roots) : 0;
    int nRootsInIndex = 0;
    StrVecWithData<TouchLibraryFolderData> folders;
    StrVecWithData<TouchLibraryFileData> files;
    int section = 0; // 0: roots, 1: folders, 2: files
    for (int i = 0; i < len(lines); i++) {
        Str line = lines[i];
        str::TrimWSInPlace(line, str::TrimOpt::Right);
        if (len(line) == 0) {
            continue;
        }
        if (i == 0) {
            if (!str::Eq(line, StrL(kTouchLibraryIndexHeader))) {
                return false;
            }
        } else if (str::Eq(line, StrL("folders:"))) {
            section = 1;
        } else if (str::Eq(line, StrL("files:"))) {
            section = 2;
        } else if (section == 0) {
            Str root = line;
            if (!str::TrimPrefix(root, StrL("root:")) || nRootsInIndex >= nRoots ||
                !str::Eq(root, (*roots)[nRootsInIndex])) {
                return false;
            }
            nRootsInIndex++;
        } else if (section == 1) {
            folders.Append(line, TouchLibraryFolderData{});
        } else {
            files.Append(line, TouchLibraryFileData{});
        }
    }
    if (nRootsInIndex != nRoots || len(folders) == 0) {
        return false;
    }
    FinishTouchLibraryModel(folders, files);
    for (int i = 0; i < len(gTouchLibraryFiles); i++) {
        delete gTouchLibraryFiles.AtData(i)->thumbnail;
    }
    SwapStrVec(&gTouchLibraryFolders, &folders);
    SwapStrVec(&gTouchLibraryFiles, &files);
    // shown as-is until the scan started right after replaces it
    gTouchLibraryValid = true;
    return true;
}

static void TouchLibraryScanThread(TouchLibraryScan* scan) {
    StrVec scannedRoots;
    for (Str root : scan->roots) {
        ScanTouchLibraryRoot(root, scannedRoots, scan->folders, scan->files);
    }
    FinishTouchLibraryModel(scan->folders, scan->files);
    SaveTouchLibraryIndex(scan);
    uitask::Post(MkFunc0<TouchLibraryScan>(TouchLibraryScanFinished, scan), "TouchLibraryScanFinished");
}

static void StartTouchLibraryScan() {
    if (gTouchLibraryScanning || !gGlobalPrefs) {
        return;
    }
    static bool triedIndex = false;
    if (!triedIndex) {
        triedIndex = true;
        LoadTouchLibraryIndex();
    }
    gTouchLibraryScanning = true;
    auto* scan = new TouchLibraryScan();
    scan->gen = gTouchLibraryScanGen;
    Vec<Str>* roots = gGlobalPrefs->libraryFolders;
    if (roots) {
        for (Str root : *roots) {
            scan->roots.Append(root);
        }
    }
    RunAsync(MkFunc0<TouchLibraryScan>(TouchLibraryScanThread, scan), "TouchLibraryScan");
}

// True while the model on screen is being (re)built in the background.
static bool TouchLibraryLoading() {
    if (!gTouchLibraryValid) {
        StartTouchLibraryScan();
    }
    return !gTouchLibraryValid;
}

static i64 TouchLibraryFileSize(int fileIdx, Str filePath) {
    if (fileIdx < 0) {
        return file::GetSize(filePath);
    }
    TouchLibraryFileData* data = gTouchLibraryFiles.AtData(fileIdx);
    u32 now = GetTickCount();
    if (data->size == kSizeNotFetched || now - data->sizeFetchedAt > (u32)kHomeFileSizeCacheMs) {
        data->size = file::GetSize(filePath);
        data->sizeFetchedAt = now;
    }
    return data->size;
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

static void LibraryNavNoteSearch(MainWindow* win);
static void ShowFolderFromTouchSearch(MainWindow* win, Str folderPath);

bool HandleTouchLibraryLink(MainWindow* win, Str url) {
    if (!win || !gGlobalPrefs) {
        return false;
    }
    if (str::Eq(url, kLinkLibraryRecent)) {
        LibraryNavNoteSearch(win);
        LibraryNavPush(win, Str(kLibraryNavRecent));
        win->libraryRecentSelected = true;
        win->librarySearchFilesSelected = false;
        win->libraryFilesScrollY = 0;
        str::FreePtr(&win->libraryHighlightFilePath);
        str::FreePtr(&win->libraryRowMenuPath);
    } else if (str::Eq(url, kLinkLibrarySearchFiles)) {
        win->librarySearchFilesSelected = true;
        win->libraryRecentSelected = false;
        win->libraryFilesScrollY = 0;
        str::FreePtr(&win->libraryRowMenuPath);
    } else if (str::TrimPrefix(url, kLinkLibraryFolderFromSearchPrefix)) {
        ShowFolderFromTouchSearch(win, url);
        return true;
    } else if (str::Eq(url, kLinkLibraryBack)) {
        LibraryNavGo(win, -1);
    } else if (str::Eq(url, kLinkLibraryForward)) {
        LibraryNavGo(win, 1);
    } else if (str::TrimPrefix(url, kLinkLibraryMenuPinnedPrefix)) {
        str::ReplaceWithCopy(&win->libraryRowMenuPath, url);
        win->libraryRowMenuFromPinned = true;
    } else if (str::TrimPrefix(url, kLinkLibraryMenuPrefix)) {
        str::ReplaceWithCopy(&win->libraryRowMenuPath, url);
        win->libraryRowMenuFromPinned = false;
    } else if (str::TrimPrefix(url, kLinkLibraryPinPrefix)) {
        bool nowPinned = false;
        if (!TouchLibraryRemovePath(gGlobalPrefs->libraryPinnedFolders, url)) {
            TouchLibraryAddPath(&gGlobalPrefs->libraryPinnedFolders, url);
            nowPinned = true;
        }
        HomePageStartPinFlight(win, str::JoinTemp(kLinkLibraryPinPrefix, url), true, nowPinned);
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
    } else if (str::Eq(url, kLinkLibrarySearchScopeOpen)) {
        win->librarySearchScopePickerOpen = true;
    } else if (str::Eq(url, kLinkLibrarySearchScopeDone)) {
        win->librarySearchScopePickerOpen = false;
    } else if (str::Eq(url, kLinkLibrarySearchScopeClear)) {
        win->librarySearchFolderScope.Reset();
    } else if (str::TrimPrefix(url, kLinkLibrarySearchScopeTogglePrefix)) {
        int idx = win->librarySearchFolderScope.FindI(url);
        if (idx >= 0) {
            win->librarySearchFolderScope.RemoveAt(idx);
        } else {
            win->librarySearchFolderScope.Append(url);
        }
    } else if (str::TrimPrefix(url, kLinkLibrarySearchScopeExpandPrefix)) {
        int idx = win->librarySearchScopeExpanded.FindI(url);
        if (idx >= 0) {
            win->librarySearchScopeExpanded.RemoveAt(idx);
        } else {
            win->librarySearchScopeExpanded.Append(url);
        }
    } else {
        return false;
    }
    win->RedrawAll(true);
    return true;
}

// --- Library back/forward -----------------------------------------------
// The Recent surface and every folder the user opens are one history, so Back
// walks out of a folder and on into Recent the same way a browser would.

void LibraryNavPush(MainWindow* win, Str entry) {
    if (!win || !entry || win->libraryNavReplaying) {
        return;
    }
    // re-selecting where we already are is not a new destination
    if (win->libraryNavPos >= 0 && win->libraryNavPos < len(win->libraryNavStack) &&
        str::Eq(win->libraryNavStack[win->libraryNavPos], entry)) {
        return;
    }
    // The Library always opens on Recent, but nothing navigates *to* it at
    // startup, so the first push would otherwise leave Back with nowhere to go.
    // Seed that implicit starting point.
    if (len(win->libraryNavStack) == 0 && !str::Eq(entry, Str(kLibraryNavRecent))) {
        win->libraryNavStack.Append(Str(kLibraryNavRecent));
        win->libraryNavPos = 0;
    }
    // choosing a new destination after going back drops the forward entries
    while (len(win->libraryNavStack) > win->libraryNavPos + 1) {
        win->libraryNavStack.RemoveAt(len(win->libraryNavStack) - 1);
    }
    win->libraryNavStack.Append(entry);
    win->libraryNavPos = len(win->libraryNavStack) - 1;
}

bool LibraryNavCanGo(MainWindow* win, int delta) {
    if (!win) {
        return false;
    }
    int next = win->libraryNavPos + delta;
    return next >= 0 && next < len(win->libraryNavStack);
}

void LibraryNavGo(MainWindow* win, int delta) {
    if (!LibraryNavCanGo(win, delta)) {
        return;
    }
    win->libraryNavPos += delta;
    Str entry = win->libraryNavStack[win->libraryNavPos];
    // replaying, so the destination we land on must not be pushed again
    win->libraryNavReplaying = true;
    Str query = entry;
    if (str::Eq(entry, Str(kLibraryNavRecent))) {
        win->libraryRecentSelected = true;
        win->libraryFilesScrollY = 0;
        str::FreePtr(&win->libraryHighlightFilePath);
        str::FreePtr(&win->libraryRowMenuPath);
        win->RedrawAll(true);
    } else if (str::TrimPrefix(query, kLibraryNavSearchPrefix)) {
        // putting the query back in the box is what re-runs the search; the
        // box's change notification selects the file results
        str::ReplaceWithCopy(&win->librarySearchQuery, query);
        if (win->hwndHomeSearch) {
            HwndSetText(win->hwndHomeSearch, query);
        }
        win->librarySearchFilesSelected = true;
        win->libraryFilesScrollY = 0;
        str::FreePtr(&win->libraryHighlightFilePath);
        win->RedrawAll(true);
    } else {
        SelectTouchLibraryFolder(win, entry);
    }
    win->libraryNavReplaying = false;
}

// Leaving a search's results for a folder or Recent: record the results as
// the place we came from, so Back re-runs the query instead of skipping it.
static void LibraryNavNoteSearch(MainWindow* win) {
    if (!win || !win->librarySearchFilesSelected || win->libraryNavReplaying) {
        return;
    }
    TempStr query = win->hwndHomeSearch ? HwndGetTextTemp(win->hwndHomeSearch) : str::DupTemp(win->librarySearchQuery);
    if (len(query) == 0) {
        return;
    }
    LibraryNavPush(win, str::JoinTemp(kLibraryNavSearchPrefix, query));
}

void HomePageOnSearchQueryChanged(MainWindow* win) {
    if (!win || !IsTouchChrome(win) || !win->IsCurrentTabAbout()) {
        return;
    }
    // a new query is after files first; the sidebar's folder matches are
    // there for the reader who wants a folder instead
    win->librarySearchFilesSelected = true;
    win->libraryTreeScrollY = 0;
    win->libraryFilesScrollY = 0;
    str::FreePtr(&win->libraryHighlightFilePath);
    str::FreePtr(&win->libraryRowMenuPath);
}

bool TouchLibraryContainsFile(Str filePath) {
    if (len(filePath) == 0) {
        return false;
    }
    if (gTouchLibraryFiles.FindI(filePath) >= 0) {
        return true;
    }
    TempStr dir = path::GetDirTemp(filePath);
    return dir && gTouchLibraryFolders.FindI(dir) >= 0;
}

// A folder reached from a search result. The folder view wants the sidebar's
// tree, not the search's matches, so the query is cleared; Back restores it
// (see LibraryNavGo).
static void ShowFolderFromTouchSearch(MainWindow* win, Str folderPath) {
    if (!win || len(folderPath) == 0) {
        return;
    }
    LibraryNavNoteSearch(win);
    if (win->hwndHomeSearch) {
        HwndSetText(win->hwndHomeSearch, "");
    }
    str::FreePtr(&win->librarySearchQuery);
    SelectTouchLibraryFolder(win, folderPath);
}

void ShowFileInTouchLibrary(MainWindow* win, Str filePath) {
    if (!win || len(filePath) == 0) {
        return;
    }
    TempStr dir = path::GetDirTemp(filePath);
    if (!dir) {
        return;
    }
    ShowFolderFromTouchSearch(win, dir);
    str::ReplaceWithCopy(&win->libraryHighlightFilePath, filePath);
    win->libraryHighlightScrollPending = true;
    win->RedrawAll(true);
}

void SelectTouchLibraryFolder(MainWindow* win, Str folderPath) {
    if (!win || !folderPath) {
        return;
    }
    LibraryNavNoteSearch(win);
    LibraryNavPush(win, folderPath);
    str::ReplaceWithCopy(&win->librarySelectedFolderPath, folderPath);
    win->libraryRecentSelected = false;
    win->librarySearchFilesSelected = false;
    win->libraryFilesScrollY = 0;
    str::FreePtr(&win->libraryHighlightFilePath);

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
    if (win->librarySearchScopePickerOpen) {
        win->librarySearchScopePickerOpen = false;
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

// true if idx (or an ancestor of it) is one of the folders search is
// restricted to - or the scope is empty, meaning unrestricted
static bool TouchLibraryFolderInSearchScope(MainWindow* win, const StrVecWithData<TouchLibraryFolderData>& folders,
                                            int idx) {
    if (win->librarySearchFolderScope.IsEmpty()) {
        return true;
    }
    int at = idx;
    while (at >= 0) {
        if (win->librarySearchFolderScope.FindI(folders[at]) >= 0) {
            return true;
        }
        at = folders.AtData(at)->parent;
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

    TouchLibraryLoading();
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
                              nullptr, true, true, TouchLibraryFileSize(fileIdx, filePath));
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

// Tapping the scrim outside a modal card closes it, same as tapping Done -
// standard modal behavior this pair of dialogs was missing. Four strips
// tiling whatever of rc isn't the card, rather than one big rect under it:
// registered after the card's own controls, so this doesn't matter for hit
// order, but it means a card near an edge doesn't need a strip of zero or
// negative size handled specially - Append no-ops on an empty rect via the
// StaticLink's own emptiness, same as every other conditional link here.
static void CloseModalOnScrimTap(MainWindow* win, const Rect& rc, const Rect& card, Str closeLink) {
    Rect top{rc.x, rc.y, rc.dx, card.y - rc.y};
    Rect bottom{rc.x, card.y + card.dy, rc.dx, (rc.y + rc.dy) - (card.y + card.dy)};
    Rect left{rc.x, card.y, card.x - rc.x, card.dy};
    Rect right{card.x + card.dx, card.y, (rc.x + rc.dx) - (card.x + card.dx), card.dy};
    for (Rect strip : {top, bottom, left, right}) {
        if (!strip.IsEmpty()) {
            win->staticLinks.Append(new StaticLink(strip, closeLink));
        }
    }
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
    CloseModalOnScrimTap(win, rc, card, Str(kLinkLibraryManageDone));
}

// Opened from the search box's filter button. A flat, indented list rather
// than the sidebar's own collapsible tree: picking a scope is a rare,
// deliberate action, so trading away expand/collapse for "see every folder
// at once, no clicking to find the one you want" is the right call here.
static void DrawTouchLibrarySearchScopeModal(MainWindow* win, HDC hdc, const Rect& rc,
                                             const StrVecWithData<TouchLibraryFolderData>& folders) {
    DeleteVecMembers(win->staticLinks);
    Gdiplus::Graphics graphics(hdc);
    Gdiplus::SolidBrush scrim(Gdiplus::Color(90, 28, 26, 23));
    graphics.FillRectangle(&scrim, rc.x, rc.y, rc.dx, rc.dy);

    int visibleCount = 0;
    for (int i = 0; i < len(folders); i++) {
        if (!TouchLibraryFolderHidden(folders, i)) {
            visibleCount++;
        }
    }
    int desiredCardDy = 128 + visibleCount * 36 + 64;
    int cardDx = std::min(DpiScale(hdc, 420), rc.dx - DpiScale(hdc, 32));
    int cardDy = std::min(DpiScale(hdc, std::min(desiredCardDy, 520)), rc.dy - DpiScale(hdc, 32));
    Rect card{(rc.dx - cardDx) / 2, (rc.dy - cardDy) / 2, cardDx, cardDy};
    FillHomeRoundRect(hdc, card, DpiScale(hdc, 14), ThemeWindowControlBackgroundColor());
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    Rect title{card.x + DpiScale(hdc, 24), card.y + DpiScale(hdc, 18), card.dx - DpiScale(hdc, 48), DpiScale(hdc, 28)};
    HdcDrawText(hdc, StrL("Search in folders"), title, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 17, FW_SEMIBOLD));
    HdcFillRect(hdc, Rect{card.x, card.y + DpiScale(hdc, 60), card.dx, 1}, ThemeEdgeColor());

    auto drawCheck = [&](Rect box, bool checked) {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        COLORREF col = checked ? RgbToCOLORREF(0xb4530a) : ThemeEdgeColor();
        Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), 1.6f);
        int rad = DpiScale(hdc, 5);
        Gdiplus::GraphicsPath path;
        int d = rad * 2;
        path.AddArc(box.x, box.y, d, d, 180.0f, 90.0f);
        path.AddArc(box.x + box.dx - d, box.y, d, d, 270.0f, 90.0f);
        path.AddArc(box.x + box.dx - d, box.y + box.dy - d, d, d, 0.0f, 90.0f);
        path.AddArc(box.x, box.y + box.dy - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
        if (checked) {
            Gdiplus::SolidBrush br(GdiRgbFromCOLORREF(col));
            g.FillPath(&br, &path);
            Gdiplus::Pen checkPen(Gdiplus::Color(255, 255, 255, 255), 1.8f);
            checkPen.SetStartCap(Gdiplus::LineCapRound);
            checkPen.SetEndCap(Gdiplus::LineCapRound);
            checkPen.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::PointF pts[] = {
                {(float)box.x + box.dx * 0.22f, (float)box.y + box.dy * 0.52f},
                {(float)box.x + box.dx * 0.42f, (float)box.y + box.dy * 0.74f},
                {(float)box.x + box.dx * 0.80f, (float)box.y + box.dy * 0.28f},
            };
            g.DrawLines(&checkPen, pts, dimofi(pts));
        } else {
            g.DrawPath(&pen, &path);
        }
    };

    // The list is a tree collapsed to the roots (a whole library flat was
    // hundreds of rows with no way to reach most of them), with a chevron to
    // open a folder, and it scrolls: by wheel, or by finger (pan area 6 in
    // HomePageOnPointerEvent) - which used to close the picker instead.
    int listTop = card.y + DpiScale(hdc, 72);
    int listBottom = card.y + card.dy - DpiScale(hdc, 64);
    int rowDy = DpiScale(hdc, 36);
    Vec<u8> shown;
    int nShown = 0;
    for (int i = 0; i < len(folders); i++) {
        u8 visible = 0;
        if (!TouchLibraryFolderHidden(folders, i)) {
            int parent = folders.AtData(i)->parent;
            if (parent < 0) {
                visible = 1;
            } else if (parent < i && shown[parent] && win->librarySearchScopeExpanded.FindI(folders[parent]) >= 0) {
                visible = 1;
            }
        }
        shown.Append(visible);
        nShown += visible;
    }
    int listDy = std::max(0, listBottom - listTop);
    win->librarySearchScopeScrollMaxY = std::max(0, nShown * rowDy - listDy);
    win->librarySearchScopeScrollY = std::clamp(win->librarySearchScopeScrollY, 0, win->librarySearchScopeScrollMaxY);
    Rect listClip{card.x, listTop, card.dx, listDy};
    int listDc = SaveDC(hdc);
    IntersectClipRect(hdc, listClip.x, listClip.y, listClip.x + listClip.dx, listClip.y + listClip.dy);
    int y = listTop - win->librarySearchScopeScrollY;
    for (int i = 0; i < len(folders); i++) {
        if (!shown[i]) {
            continue;
        }
        Str folder = folders[i];
        int depth = folders.AtData(i)->depth;
        Rect row{card.x + DpiScale(hdc, 24) + depth * DpiScale(hdc, 18), y,
                 card.dx - DpiScale(hdc, 48) - depth * DpiScale(hdc, 18), rowDy};
        y += row.dy;
        Rect visibleRow = row.Intersect(listClip);
        if (visibleRow.IsEmpty()) {
            continue;
        }
        Rect chevron{row.x, row.y, DpiScale(hdc, 18), row.dy};
        bool hasChildren = folders.AtData(i)->hasChildren;
        if (hasChildren) {
            bool expanded = win->librarySearchScopeExpanded.FindI(folder) >= 0;
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, expanded ? StrL("⌄") : StrL("›"), chevron,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 15, FW_MEDIUM));
            Rect chevronLink =
                Rect{chevron.x - DpiScale(hdc, 6), row.y, chevron.dx + DpiScale(hdc, 12), row.dy}.Intersect(listClip);
            win->staticLinks.Append(
                new StaticLink(chevronLink, fmt("%s%s", Str(kLinkLibrarySearchScopeExpandPrefix), folder)));
        }
        Rect box{chevron.x + chevron.dx + DpiScale(hdc, 4), row.y + (row.dy - DpiScale(hdc, 18)) / 2, DpiScale(hdc, 18),
                 DpiScale(hdc, 18)};
        bool checked = win->librarySearchFolderScope.FindI(folder) >= 0;
        drawCheck(box, checked);
        Rect nameRect{box.x + box.dx + DpiScale(hdc, 10), row.y,
                      std::max(0, row.x + row.dx - box.x - box.dx - DpiScale(hdc, 10)), row.dy};
        SetTextColor(hdc, ThemeWindowTextColor());
        HdcDrawText(hdc, path::GetBaseNameTemp(folder), nameRect,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_MEDIUM));
        Rect toggleLink = Rect{box.x, row.y, row.x + row.dx - box.x, row.dy}.Intersect(listClip);
        win->staticLinks.Append(
            new StaticLink(toggleLink, fmt("%s%s", Str(kLinkLibrarySearchScopeTogglePrefix), folder), folder));
    }
    RestoreDC(hdc, listDc);
    if (len(folders) == 0) {
        Rect empty{card.x + DpiScale(hdc, 24), y, card.dx - DpiScale(hdc, 48), DpiScale(hdc, 40)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("No folders in your Library yet."), empty, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13));
    }

    int footerY = card.y + card.dy - DpiScale(hdc, 64);
    HdcFillRect(hdc, Rect{card.x, footerY, card.dx, 1}, ThemeEdgeColor());
    bool scopeActive = !win->librarySearchFolderScope.IsEmpty();
    Rect allBtn{card.x + DpiScale(hdc, 24), footerY + DpiScale(hdc, 14), DpiScale(hdc, 130), DpiScale(hdc, 36)};
    SetTextColor(hdc, scopeActive ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, StrL("Search all folders"), allBtn, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_MEDIUM));
    if (scopeActive) {
        win->staticLinks.Append(new StaticLink(allBtn, Str(kLinkLibrarySearchScopeClear)));
    }
    Rect done{card.x + card.dx - DpiScale(hdc, 112), footerY + DpiScale(hdc, 14), DpiScale(hdc, 88), DpiScale(hdc, 36)};
    FillHomeRoundRect(hdc, done, DpiScale(hdc, 8), ThemeDisabledEdgeColor());
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, StrL("Done"), done, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    win->staticLinks.Append(new StaticLink(done, Str(kLinkLibrarySearchScopeDone)));
    CloseModalOnScrimTap(win, rc, card, Str(kLinkLibrarySearchScopeDone));
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
    // Folders are square tiles, smaller than the portrait file cards, so a
    // listing reads as "some folders, then documents" at a glance and a
    // folder-heavy level does not fill the screen with blank cards.
    bool compact = card.dx < DpiScale(hdc, 130);
    DrawHomeShadow(hdc, card, DpiScale(hdc, 10), pageBg);
    FillHomeRoundRect(hdc, card, DpiScale(hdc, 10), RGB(255, 255, 255), ThemeEdgeColor());
    int iconDy = DpiScale(hdc, compact ? 36 : 48);
    HIMAGELIST iml = GetTintedToolbarImageList(iconDy, ThemeWindowDarkerTextColor(), RGB(255, 255, 255));
    if (iml) {
        ImageList_Draw(iml, (int)TbIcon::Folder, hdc, card.x + (card.dx - iconDy) / 2, card.y + (card.dy - iconDy) / 2,
                       ILD_NORMAL);
    }

    TempStr name = path::GetBaseNameTemp(folderPath);
    Rect nameRect{card.x, card.y + card.dy + DpiScale(hdc, 9), card.dx, DpiScale(hdc, compact ? 19 : 38)};
    SetTextColor(hdc, ThemeWindowTextColor());
    UINT nameFlags = DT_END_ELLIPSIS | DT_NOPREFIX | (compact ? DT_SINGLELINE : DT_WORDBREAK);
    HdcDrawText(hdc, name, nameRect, nameFlags, HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
    Rect metaRect{nameRect.x, nameRect.y + nameRect.dy + DpiScale(hdc, 1), nameRect.dx, DpiScale(hdc, 17)};
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, TouchLibraryFolderSummaryTemp(data), metaRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX,
                HdcGetUiFont(hdc, 12));

    // Pin badge, same idiom as a file card's: folders could be pinned from the
    // sidebar row's "..." but not from the card you are actually looking at.
    // Appended before the whole-card link, because GetStaticLinkAtTemp's hit
    // test is first-match-wins by append order (not by what's drawn on top) -
    // registering the badge second, inside the card's own rect, meant every
    // tap on it resolved to the card link first and the badge was never
    // reachable.
    bool isPinned = TouchLibraryPathIn(gGlobalPrefs->libraryPinnedFolders, folderPath);
    int badgeDx = DpiScale(hdc, compact ? 24 : 28);
    Rect badge{card.x + card.dx - badgeDx - DpiScale(hdc, 6), card.y + DpiScale(hdc, 6), badgeDx, badgeDx};
    Rect badgeLink = badge.Intersect(clip);
    if (!badgeLink.IsEmpty()) {
        COLORREF badgeBg = ThemeControlBackgroundColor();
        FillHomeRoundRect(hdc, badge, badge.dy / 2, badgeBg);
        COLORREF pinCol = isPinned ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor();
        int pinDy = DpiScale(hdc, 16);
        HIMAGELIST pinIml = GetTintedToolbarImageList(pinDy, pinCol, badgeBg);
        if (pinIml) {
            ImageList_Draw(pinIml, (int)TbIcon::Pin, hdc, badge.x + ((badge.dx - pinDy) / 2),
                           badge.y + ((badge.dy - pinDy) / 2), ILD_NORMAL);
        }
        TempStr pinTarget = str::JoinTemp(kLinkLibraryPinPrefix, folderPath);
        Str pinTip = isPinned ? _TRA("Unpin") : _TRA("Pin");
        win->staticLinks.Append(new StaticLink(badgeLink, pinTarget, pinTip));
    }

    Rect linkRect = card.Union(metaRect).Intersect(clip);
    if (!linkRect.IsEmpty()) {
        win->staticLinks.Append(
            new StaticLink(linkRect, fmt("%s%s", Str(kLinkLibraryFolderCardPrefix), folderPath), folderPath));
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

// A folder as the reader knows it: from its library root down, "Books ›
// Μελοδός › Kontakia", not the drive and account folders above the root.
static TempStr TouchLibraryFolderDisplayTemp(Str folder) {
    Str shown = folder;
    Vec<Str>* roots = gGlobalPrefs ? gGlobalPrefs->libraryFolders : nullptr;
    if (roots) {
        for (Str root : *roots) {
            TempStr normalized = path::NormalizeTemp(root);
            if (!normalized || !TouchLibraryPathWithin(folder, normalized)) {
                continue;
            }
            TempStr rootParent = path::GetDirTemp(normalized);
            if (rootParent && len(rootParent) < len(folder) && !path::IsSame(rootParent, normalized)) {
                shown = Str(folder.s + len(rootParent), len(folder) - len(rootParent));
                while (len(shown) > 0 && (shown.s[0] == '\\' || shown.s[0] == '/')) {
                    shown = Str(shown.s + 1, len(shown) - 1);
                }
            }
            break;
        }
    }
    TempStr res = str::ReplaceTemp(shown, StrL("\\"), StrL(" › "));
    return str::ReplaceTemp(res, StrL("/"), StrL(" › "));
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
    // With no document open, the Favorites panel is showing in place of a
    // document's sidebar. Drawing the Library's own sidebar as well would put
    // three columns on screen and squeeze the content, so the panel stands in
    // for it: Favorites on the left, Library content on the right.
    if (win->uiState.tocVisible && !win->IsDocLoaded()) {
        leftDx = 0;
    }
    int headerDy = DpiScale(hdc, 64);
    HdcFillRect(hdc, rc, ThemeWindowControlBackgroundColor());
    HdcFillRect(hdc, Rect{0, 0, leftDx, rc.dy}, ThemeHotBackgroundColor());
    int splitterDx = DpiScale(hdc, kSidebarSplitterVisualDx);
    HdcFillRect(hdc, Rect{leftDx - splitterDx / 2, 0, splitterDx, rc.dy}, ThemeEdgeColor());
    HdcFillRect(hdc, Rect{leftDx, headerDy - 1, rc.dx - leftDx, 1}, ThemeEdgeColor());
    SetBkMode(hdc, TRANSPARENT);

    bool libraryLoading = TouchLibraryLoading();
    auto& folders = gTouchLibraryFolders;
    auto& files = gTouchLibraryFiles;
    // the sidebar's "Recent" row: the content pane then shows the recent files
    // instead of a folder's contents
    bool recentSelected = win->libraryRecentSelected;

    EnsureHomeSearchCreated(win);
    // no sidebar (the Favorites panel is standing in for it): its search field
    // and Manage-folders row have nowhere to go, and drawn at zero width they
    // leave slivers behind
    bool hasSidebar = leftDx > 0;
    if (!hasSidebar) {
        HwndSetVisible(win->hwndHomeSearch, false);
    }
    // The pill's edges line up with the row highlights below it (both 8px in).
    // Inside, packed from the right: the filter chevron, a divider, the clear
    // × (drawn only with a query, but its slot is always reserved so the text
    // doesn't shift when it appears), then the text field.
    Rect search{DpiScale(hdc, 8), DpiScale(hdc, 12), leftDx - DpiScale(hdc, 16), DpiScale(hdc, 40)};
    int searchBtnDy = DpiScale(hdc, 32);
    COLORREF searchBg = ThemeTouchSurfaceColor();
    Rect filterBtn{search.x + search.dx - DpiScale(hdc, 6) - searchBtnDy, search.y + DpiScale(hdc, 4), searchBtnDy,
                   searchBtnDy};
    Rect divider{filterBtn.x - DpiScale(hdc, 5), search.y + DpiScale(hdc, 12), 1, DpiScale(hdc, 16)};
    Rect clearBtn{divider.x - DpiScale(hdc, 4) - searchBtnDy, filterBtn.y, searchBtnDy, searchBtnDy};
    bool scopeActive = !win->librarySearchFolderScope.IsEmpty();
    if (hasSidebar) {
        FillHomeRoundRect(hdc, search, search.dy / 2, searchBg, ThemeEdgeColor());
        int searchIconDy = DpiScale(hdc, 16);
        HIMAGELIST searchIcons = GetTintedToolbarImageList(searchIconDy, ThemeWindowDarkerTextColor(), searchBg);
        if (searchIcons) {
            ImageList_Draw(searchIcons, (int)TbIcon::Search, hdc, search.x + DpiScale(hdc, 14),
                           search.y + (search.dy - searchIconDy) / 2, ILD_NORMAL);
        }
        // a single-line edit top-aligns its text, so the control is sized to
        // the font and centered in the pill rather than filling it
        int editX = search.x + DpiScale(hdc, 38);
        int editDy = HdcMeasureText(hdc, StrL("Xg"), HdcGetUiFont(hdc, kFontSizeLabel)).dy + DpiScale(hdc, 2);
        MoveWindow(win->hwndHomeSearch, editX, search.y + (search.dy - editDy) / 2,
                   std::max(0, clearBtn.x - DpiScale(hdc, 2) - editX), editDy, TRUE);
        HwndShow(win->hwndHomeSearch);
    } // hasSidebar
    TempStr query = HwndGetTextTemp(win->hwndHomeSearch);
    // A query searches the whole Library, whichever row was selected before
    // typing: Recent steps aside for the results and is back once the box is
    // cleared. The file matches are the default result (the box's change
    // notification selects them, see HomePageOnSearchQueryChanged); a folder
    // match tapped in the sidebar shows that folder instead.
    bool searching = len(query) > 0;
    bool showingSearchFiles = searching && win->librarySearchFilesSelected;
    if (searching) {
        recentSelected = false;
    }
    if (hasSidebar) {
        // Always available, not just while there is a query: choosing folders
        // ahead of typing is as reasonable as narrowing an existing search.
        HdcFillRect(hdc, divider, ThemeEdgeColor());
        COLORREF filterFg = scopeActive ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor();
        int chevronDy = DpiScale(hdc, 14);
        HIMAGELIST chevronIcons = GetTintedToolbarImageList(chevronDy, filterFg, searchBg);
        if (chevronIcons) {
            ImageList_Draw(chevronIcons, (int)TbIcon::ChevronDown, hdc, filterBtn.x + (filterBtn.dx - chevronDy) / 2,
                           filterBtn.y + (filterBtn.dy - chevronDy) / 2, ILD_NORMAL);
        }
        if (scopeActive) {
            // a small filled dot, the same idiom the tab strip uses for "this
            // has state you should know about" - a number would need either a
            // wider button or type too small to read
            int dotDy = DpiScale(hdc, 7);
            Rect dot{filterBtn.x + filterBtn.dx - dotDy - DpiScale(hdc, 2), filterBtn.y - DpiScale(hdc, 1), dotDy,
                     dotDy};
            FillHomeRoundRect(hdc, dot, dot.dy / 2, ThemeWindowLinkColor());
        }
        Str filterTip = scopeActive ? fmt("Searching %d folder%s", len(win->librarySearchFolderScope),
                                          len(win->librarySearchFolderScope) == 1 ? StrL("") : StrL("s"))
                                    : StrL("Search all folders");
        win->staticLinks.Append(new StaticLink(filterBtn, Str(kLinkLibrarySearchScopeOpen), filterTip));
    }
    if (hasSidebar && searching) {
        // a chip one step above the surface in every theme (the edge color),
        // with the icon set's own × rather than a font glyph whose weight and
        // baseline vary with the UI font
        int circleDy = DpiScale(hdc, 20);
        Rect clearCircle{clearBtn.x + (clearBtn.dx - circleDy) / 2, clearBtn.y + (clearBtn.dy - circleDy) / 2, circleDy,
                         circleDy};
        COLORREF chipBg = ThemeEdgeColor();
        FillHomeRoundRect(hdc, clearCircle, circleDy / 2, chipBg);
        int closeDy = DpiScale(hdc, 10);
        HIMAGELIST closeIcons = GetTintedToolbarImageList(closeDy, ThemeWindowTextColor(), chipBg);
        if (closeIcons) {
            ImageList_Draw(closeIcons, (int)TbIcon::Close, hdc, clearCircle.x + (circleDy - closeDy) / 2,
                           clearCircle.y + (circleDy - closeDy) / 2, ILD_NORMAL);
        }
        win->staticLinks.Append(new StaticLink(clearBtn, Str(kLinkLibraryClearSearch), StrL("Clear search")));
    }

    int selected = win->librarySelectedFolderPath ? folders.FindI(win->librarySelectedFolderPath) : -1;
    // a just-added root isn't in the model until the rescan lands; keep its
    // path selected rather than falling back to the first folder
    bool selectionPending = selected < 0 && libraryLoading && win->librarySelectedFolderPath;
    if (!selectionPending &&
        (selected < 0 || (selected < len(folders) && TouchLibraryFolderHidden(folders, selected)))) {
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
    // the shared selected-row treatment (also used by the rail and the panels)
    COLORREF selBg = 0;
    COLORREF selFg = 0;
    ThemeAccentSurfaceColors(&selBg, &selFg);
    auto drawSidebarRowBg = [&](const Rect& row) { FillHomeRoundRect(hdc, row, DpiScale(hdc, 8), selBg); };
    Rect openMenuAnchor{};
    // Digits end 12px inside the row, past the highlight's corner radius, and
    // the same column serves every row (Recent, Files, folders) so they share
    // one right edge. The "⋯" menu sits 6px left of it with a touch-sized
    // link rect.
    int countDx = DpiScale(hdc, 28);
    int countInset = DpiScale(hdc, 12);
    auto rowCountRect = [&](const Rect& row) {
        return Rect{row.x + row.dx - countInset - countDx, row.y, countDx, row.dy};
    };
    auto drawCount = [&](const Rect& row, int count) {
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawTextTabular(hdc, fmt("%d", count), rowCountRect(row),
                           DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 12));
    };
    // flat: no tree indent or expand chevron (the PINNED section and search
    // matches); pinnedRow: drawn in the PINNED section, so its menu link says
    // which of the folder's two rows opened it and its icon takes the accent
    auto drawFolderRow = [&](int idx, bool flat, bool pinnedRow) {
        if (idx < 0 || idx >= len(folders)) {
            return;
        }
        Str folder = folders[idx];
        TouchLibraryFolderData* data = folders.AtData(idx);
        bool isSelected = !recentSelected && !showingSearchFiles && idx == selected;
        bool isPinned = TouchLibraryPathIn(gGlobalPrefs->libraryPinnedFolders, folder);
        Rect row{DpiScale(hdc, 8), y, leftDx - DpiScale(hdc, 16), DpiScale(hdc, 38)};
        y += row.dy;
        Rect visibleRow = row.Intersect(treeClip);
        if (visibleRow.IsEmpty()) {
            return;
        }
        COLORREF rowBg = ThemeHotBackgroundColor();
        if (isSelected) {
            rowBg = selBg;
            drawSidebarRowBg(row);
        }
        int indent = flat ? 10 : 10 + data->depth * 20;
        int x = row.x + DpiScale(hdc, indent);
        Rect chevron{x, row.y, DpiScale(hdc, 18), row.dy};
        if (!flat && data->hasChildren) {
            bool collapsed = win->libraryExpandedFolderPaths.FindI(folder) < 0;
            SetTextColor(hdc, isSelected ? selFg : ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, collapsed ? StrL("›") : StrL("⌄"), chevron,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 15, FW_MEDIUM));
            Rect chevronLink = chevron.Intersect(treeClip);
            if (!chevronLink.IsEmpty()) {
                win->staticLinks.Append(
                    new StaticLink(chevronLink, fmt("%s%s", Str(kLinkLibraryTogglePrefix), folder)));
            }
        }
        Rect folderIcon{chevron.x + chevron.dx + DpiScale(hdc, 4), row.y, DpiScale(hdc, 18), row.dy};
        COLORREF fg = isSelected ? selFg : (pinnedRow ? ThemeWindowLinkColor() : ThemeWindowTextColor());
        DrawTouchLibraryFolderIcon(hdc, folderIcon, fg, isSelected ? rowBg : ThemeHotBackgroundColor());
        Rect countRect = rowCountRect(row);
        Rect menuRect{countRect.x - DpiScale(hdc, 6) - DpiScale(hdc, 24), row.y + DpiScale(hdc, 7), DpiScale(hdc, 24),
                      DpiScale(hdc, 24)};
        int nameX = folderIcon.x + folderIcon.dx + DpiScale(hdc, 7);
        int pinDx = isPinned && !pinnedRow ? DpiScale(hdc, 14) : 0;
        Rect nameRect{nameX, row.y, std::max(0, menuRect.x - nameX - DpiScale(hdc, 6) - pinDx), row.dy};
        TempStr name = path::GetBaseNameTemp(folder);
        SetTextColor(hdc, isSelected ? selFg : ThemeWindowTextColor());
        HdcDrawText(hdc, name, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_MEDIUM));
        if (pinDx > 0) {
            DrawTouchLibraryPin(
                hdc,
                Rect{nameRect.x + nameRect.dx + DpiScale(hdc, 2), row.y + DpiScale(hdc, 11), pinDx, DpiScale(hdc, 16)},
                RgbToCOLORREF(0xb4530a));
        }
        // The "..." menu (Pin/Unpin, Hide) is available here too, not just on
        // the folder's row further down the tree - unpinning otherwise meant
        // scrolling to find (and maybe expanding parents to reach) the same
        // folder in its ordinary place, just to reach the one control that
        // could undo what this section put it here for. A pinned folder is
        // drawn twice (here and at its ordinary place), sharing one path, so
        // the link target says which instance this is.
        {
            // three drawn dots rather than a "⋯" glyph, whose size and
            // baseline drift with the UI font next to the vector icons
            int dotDy = DpiScale(hdc, 3);
            int pitch = DpiScale(hdc, 5);
            int dotsX = menuRect.x + (menuRect.dx - (2 * pitch + dotDy)) / 2;
            int dotY = menuRect.y + (menuRect.dy - dotDy) / 2;
            for (int d = 0; d < 3; d++) {
                FillHomeRoundRect(hdc, Rect{dotsX + d * pitch, dotY, dotDy, dotDy}, dotDy / 2,
                                  ThemeWindowDarkerTextColor());
            }
            Rect menuLink = Rect{menuRect.x - DpiScale(hdc, 4), row.y, DpiScale(hdc, 32), row.dy}.Intersect(treeClip);
            if (!menuLink.IsEmpty()) {
                Str prefix = pinnedRow ? Str(kLinkLibraryMenuPinnedPrefix) : Str(kLinkLibraryMenuPrefix);
                win->staticLinks.Append(new StaticLink(menuLink, fmt("%s%s", prefix, folder)));
            }
        }
        drawCount(row, data->directCount);
        win->staticLinks.Append(new StaticLink(visibleRow, fmt("%s%s", Str(kLinkLibraryFolderPrefix), folder), folder));
        if (win->libraryRowMenuPath && path::IsSame(win->libraryRowMenuPath, folder) &&
            win->libraryRowMenuFromPinned == pinnedRow) {
            openMenuAnchor = visibleRow;
        }
    };

    // A row with a tinted icon from the icon set (Recent, Files): the icon
    // sits where the folder rows' icons do, after the chevron slot.
    auto drawIconRow = [&](TbIcon iconId, Str label, int count, bool isSelected, Str target, Str tip) {
        Rect row{DpiScale(hdc, 8), y, leftDx - DpiScale(hdc, 16), DpiScale(hdc, 38)};
        y += row.dy;
        Rect visibleRow = row.Intersect(treeClip);
        if (visibleRow.IsEmpty()) {
            return;
        }
        COLORREF rowBg = ThemeHotBackgroundColor();
        if (isSelected) {
            rowBg = selBg;
            drawSidebarRowBg(row);
        }
        COLORREF fg = isSelected ? selFg : ThemeWindowTextColor();
        int iconDy = DpiScale(hdc, 18);
        Rect icon{row.x + DpiScale(hdc, 10) + DpiScale(hdc, 18) + DpiScale(hdc, 4), row.y, iconDy, row.dy};
        HIMAGELIST icons = GetTintedToolbarImageList(iconDy, fg, rowBg);
        if (icons) {
            ImageList_Draw(icons, (int)iconId, hdc, icon.x, icon.y + (row.dy - iconDy) / 2, ILD_NORMAL);
        }
        Rect countRect = rowCountRect(row);
        int nameX = icon.x + icon.dx + DpiScale(hdc, 7);
        Rect nameRect{nameX, row.y, std::max(0, countRect.x - nameX - DpiScale(hdc, 6)), row.dy};
        SetTextColor(hdc, fg);
        HdcDrawText(hdc, label, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_MEDIUM));
        drawCount(row, count);
        win->staticLinks.Append(new StaticLink(visibleRow, target, tip));
    };

    if (!searching) {
        // "Recent" sits above the folder sections and is the default destination
        drawIconRow(TbIcon::Recent, StrL("Recent"), CountHomePageFiles(), recentSelected, Str(kLinkLibraryRecent),
                    StrL("Recently opened files"));
        y += DpiScale(hdc, 8);
        HdcFillRect(hdc, Rect{DpiScale(hdc, 16), y, leftDx - DpiScale(hdc, 32), 1}, ThemeEdgeColor());
        y += DpiScale(hdc, 10);
    }

    if (searching) {
        // Every match across the whole library - one row, not one per file,
        // so a broad query does not turn the sidebar into a second list view
        // crammed into a ~380px column. It comes first and is selected by
        // default: the files are what a query is usually after, and the
        // content pane has the room to show them as cards.
        int fileMatchCount = 0;
        for (int i = 0; i < len(files); i++) {
            Str filePath = files[i];
            if (!str::ContainsI(path::GetBaseNameTemp(filePath), query)) {
                continue;
            }
            int folderIdx = files.AtData(i)->folderIdx;
            if (folderIdx < 0 || TouchLibraryFolderHidden(folders, folderIdx) ||
                !TouchLibraryFolderInSearchScope(win, folders, folderIdx)) {
                continue;
            }
            fileMatchCount++;
        }
        bool anyFile = fileMatchCount > 0;
        if (anyFile) {
            sectionLabel(StrL("FILES"));
            drawIconRow(TbIcon::Document, StrL("Files"), fileMatchCount, showingSearchFiles,
                        Str(kLinkLibrarySearchFiles), StrL("Show matching files"));
        }
        bool anyFolder = false;
        for (int i = 0; i < len(folders); i++) {
            if (!TouchLibraryFolderHidden(folders, i) && TouchLibraryFolderInSearchScope(win, folders, i) &&
                str::ContainsI(path::GetBaseNameTemp(folders[i]), query)) {
                if (!anyFolder) {
                    if (anyFile) {
                        y += DpiScale(hdc, 8);
                    }
                    sectionLabel(StrL("FOLDERS"));
                    anyFolder = true;
                }
                drawFolderRow(i, true, false);
            }
        }
        if (!anyFolder && !anyFile) {
            Rect empty{DpiScale(hdc, 20), y + DpiScale(hdc, 16), leftDx - DpiScale(hdc, 40), DpiScale(hdc, 48)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, fmt("No matches for “%s”", query), empty, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 13));
        }
    } else {
        HomePageSetPinAnchor(win, true, Rect{DpiScale(hdc, 16), y, leftDx - DpiScale(hdc, 32), DpiScale(hdc, 24)});
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
                    drawFolderRow(idx, true, true);
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
                drawFolderRow(i, false, false);
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
    if (hasSidebar) {
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
    } // hasSidebar

    Str selectedPath = selected >= 0 && selected < len(folders) ? folders[selected] : Str{};
    TempStr selectedName = selectedPath ? path::GetBaseNameTemp(selectedPath) : str::DupTemp("Library");
    int viewButtonDy = DpiScale(hdc, 36);
    // right end of the header: the grid/list toggle for a folder, "Open file"
    // for Recent (the recent cards have no list variant)
    Rect headerAction{rc.dx - DpiScale(hdc, 24) - 2 * viewButtonDy, (headerDy - viewButtonDy) / 2, 2 * viewButtonDy,
                      viewButtonDy};
    if (recentSelected) {
        int openDx = DpiScale(hdc, 118);
        Rect open{rc.dx - DpiScale(hdc, 24) - openDx, (headerDy - DpiScale(hdc, 40)) / 2, openDx, DpiScale(hdc, 40)};
        headerAction = open;
        FillHomeRoundRect(hdc, open, open.dy / 2, selBg);
        SetTextColor(hdc, selFg);
        int openIconDy = DpiScale(hdc, 16);
        HIMAGELIST openIcons = GetTintedToolbarImageList(openIconDy, selFg, selBg);
        if (openIcons) {
            ImageList_Draw(openIcons, (int)TbIcon::Open, hdc, open.x + DpiScale(hdc, 15),
                           open.y + (open.dy - openIconDy) / 2, ILD_NORMAL);
        }
        Rect openText{open.x + DpiScale(hdc, 38), open.y, open.dx - DpiScale(hdc, 48), open.dy};
        HdcDrawText(hdc, StrL("Open file"), openText, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
        win->staticLinks.Append(new StaticLink(open, Str(kLinkOpenFile), StrL("Open a document")));
    } else {
        // Pin/unpin the folder being browsed, right next to how its contents
        // are shown. Reaching this before required leaving the folder first -
        // back to the sidebar row's "..." menu, or the card on its parent's
        // own listing - neither of which is available while looking at the
        // folder's own contents. Search results span whichever folders the
        // matches happen to live in, so there is no single folder to pin.
        if (selectedPath && !showingSearchFiles) {
            int pinBtnDy = viewButtonDy;
            int pinGap = DpiScale(hdc, 8);
            Rect pinBtn{headerAction.x - pinGap - pinBtnDy, headerAction.y, pinBtnDy, pinBtnDy};
            bool isPinned = TouchLibraryPathIn(gGlobalPrefs->libraryPinnedFolders, selectedPath);
            COLORREF pinBg = isPinned ? ThemeWindowLinkColor() : ThemeHotBackgroundColor();
            FillHomeRoundRect(hdc, pinBtn, DpiScale(hdc, 11), pinBg);
            COLORREF pinFg = isPinned ? RGB(255, 255, 255) : ThemeWindowDarkerTextColor();
            int pinIconDy = DpiScale(hdc, 18);
            HIMAGELIST pinIcons = GetTintedToolbarImageList(pinIconDy, pinFg, pinBg);
            if (pinIcons) {
                ImageList_Draw(pinIcons, (int)TbIcon::Pin, hdc, pinBtn.x + (pinBtn.dx - pinIconDy) / 2,
                               pinBtn.y + (pinBtn.dy - pinIconDy) / 2, ILD_NORMAL);
            }
            TempStr pinTarget = str::JoinTemp(kLinkLibraryPinPrefix, selectedPath);
            Str pinTip = isPinned ? _TRA("Unpin folder") : _TRA("Pin folder");
            win->staticLinks.Append(new StaticLink(pinBtn, pinTarget, pinTip));
        }

        Rect viewToggle = headerAction;
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
    }

    // Back / Forward at the head of the row, ahead of the title. Greyed when
    // there is nowhere to go, so the pair is always in the same place rather
    // than appearing and disappearing under the finger.
    int navDy = DpiScale(hdc, 34);
    int navGap = DpiScale(hdc, 4);
    int navX = leftDx + DpiScale(hdc, 20);
    Rect backRc{navX, (headerDy - navDy) / 2, navDy, navDy};
    Rect fwdRc{navX + navDy + navGap, backRc.y, navDy, navDy};
    {
        Gdiplus::Graphics gfx(hdc);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (int i = 0; i < 2; i++) {
            bool isBack = (i == 0);
            Rect r = isBack ? backRc : fwdRc;
            bool enabled = LibraryNavCanGo(win, isBack ? -1 : 1);
            COLORREF fg = enabled ? ThemeWindowTextColor() : ThemeWindowDarkerTextColor();
            if (enabled) {
                FillHomeRoundRect(hdc, r, r.dy / 2, ThemeHotBackgroundColor());
            }
            // a chevron drawn as two strokes: no icon asset needed and it
            // scales cleanly with dpi
            Gdiplus::Pen pen(GdiRgbFromCOLORREF(fg), (Gdiplus::REAL)std::max(1, DpiScale(hdc, 2)));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            int cx = r.x + r.dx / 2;
            int cy = r.y + r.dy / 2;
            int arm = DpiScale(hdc, 5);
            int tipX = isBack ? cx + arm / 2 : cx - arm / 2;
            int backX = isBack ? cx - arm / 2 : cx + arm / 2;
            gfx.DrawLine(&pen, tipX, cy - arm, backX, cy);
            gfx.DrawLine(&pen, backX, cy, tipX, cy + arm);
            if (enabled) {
                Str target = isBack ? Str(kLinkLibraryBack) : Str(kLinkLibraryForward);
                Str tip = isBack ? StrL("Back") : StrL("Forward");
                win->staticLinks.Append(new StaticLink(r, target, tip));
            }
        }
    }

    int titleX = fwdRc.x + fwdRc.dx + DpiScale(hdc, 12);
    Rect header{titleX, 0, std::max(0, headerAction.x - titleX - DpiScale(hdc, 16)), headerDy};
    SetTextColor(hdc, ThemeWindowTextColor());
    Str headerTitle = selectedName;
    if (recentSelected) {
        headerTitle = StrL("Recent files");
    } else if (showingSearchFiles) {
        headerTitle = fmt("Files matching “%s”", query);
    }
    HdcDrawText(hdc, headerTitle, header, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                HdcGetUiFont(hdc, 18, FW_SEMIBOLD));

    {
        // "\x01recent"/"\x01search:..." cannot collide with a path, so Recent
        // and search results are always distinct destinations from a folder
        // (or from each other, across different queries)
        Str swapKey = recentSelected       ? StrL("\x01recent")
                      : showingSearchFiles ? fmt("\x01search:%s", query)
                                           : selectedPath;
        Rect contentRc{leftDx, headerDy, std::max(0, rc.dx - leftDx), std::max(0, rc.dy - headerDy)};
        NoteLibraryContentSwap(win, hdc, contentRc, swapKey);
    }
    if (recentSelected) {
        Rect content{leftDx, headerDy, std::max(0, rc.dx - leftDx), std::max(0, rc.dy - headerDy)};
        DrawTouchRecentCards(win, hdc, content);
    } else {
        Vec<int> selectedFolders;
        Vec<int> selectedFiles;
        if (showingSearchFiles) {
            // no folder cards here - the point of this view is the files
            // themselves, across whichever folders they happen to live in
            for (int i = 0; i < len(files); i++) {
                if (!str::ContainsI(path::GetBaseNameTemp(files[i]), query)) {
                    continue;
                }
                int folderIdx = files.AtData(i)->folderIdx;
                if (folderIdx < 0 || TouchLibraryFolderHidden(folders, folderIdx) ||
                    !TouchLibraryFolderInSearchScope(win, folders, folderIdx)) {
                    continue;
                }
                selectedFiles.Append(i);
            }
        } else {
            for (int i = 0; i < len(folders); i++) {
                if (folders.AtData(i)->parent == selected && !TouchLibraryFolderHidden(folders, i)) {
                    selectedFolders.Append(i);
                }
            }
            if (selectedPath && selected >= 0) {
                for (int i = 0; i < len(files); i++) {
                    if (files.AtData(i)->folderIdx == selected) {
                        selectedFiles.Append(i);
                    }
                }
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
        // search results carry a third line under each card / a second line
        // in each row: the folder the match lives in
        int cardStepY = cardDy + DpiScale(hdc, showingSearchFiles ? 88 : 70);
        int listPadY = DpiScale(hdc, 8);
        int listRowDy = DpiScale(hdc, showingSearchFiles ? 58 : 44);
        // folders: a denser grid of square tiles above the file cards (see
        // DrawTouchLibraryFolderCard), with a one-line name and count under each
        int tileDx = DpiScale(hdc, 112);
        int tileGap = DpiScale(hdc, 16);
        int tileColumns = std::max(1, (rc.dx - leftDx - 2 * pad + tileGap) / (tileDx + tileGap));
        int tileStepY = tileDx + DpiScale(hdc, 60);
        int foldersBlockDy = TouchCardRows(len(selectedFolders), tileColumns) * tileStepY;
        if (foldersBlockDy > 0 && len(selectedFiles) > 0) {
            foldersBlockDy += DpiScale(hdc, 8);
        }
        if (win->libraryListView) {
            filesContentDy = 2 * listPadY + itemCount * listRowDy;
        } else {
            filesContentDy = pad + foldersBlockDy + TouchCardRows(len(selectedFiles), columns) * cardStepY + pad;
        }
        Str highlightPath = win->libraryHighlightFilePath;
        if (win->libraryHighlightScrollPending && highlightPath) {
            // once, when "Show in Library folder" lands: center the file
            win->libraryHighlightScrollPending = false;
            for (int i = 0; i < len(selectedFiles); i++) {
                if (!path::IsSame(files[selectedFiles[i]], highlightPath)) {
                    continue;
                }
                int itemTop = win->libraryListView ? listPadY + (len(selectedFolders) + i) * listRowDy
                                                   : pad + foldersBlockDy + (i / columns) * cardStepY;
                int itemDy = win->libraryListView ? listRowDy : cardStepY;
                win->libraryFilesScrollY = itemTop - (filesViewportDy - itemDy) / 2;
                break;
            }
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
                    target = fmt("%s%s", Str(kLinkLibraryFolderCardPrefix), itemPath);
                    DrawTouchLibrarySolidFolderIcon(hdc, icon, RgbToCOLORREF(0xc56a1a));
                } else {
                    itemPath = files[idx];
                    name = path::GetBaseNameTemp(itemPath);
                    i64 size = TouchLibraryFileSize(idx, itemPath);
                    meta = size >= 0 ? str::FormatSizeShortTemp(size, nullptr) : str::DupTemp("");
                    target = str::DupTemp(itemPath);
                    Rect pdfIcon{icon.x + DpiScale(hdc, 1), icon.y + DpiScale(hdc, 1), DpiScale(hdc, 16),
                                 DpiScale(hdc, 16)};
                    DrawTouchLibraryPdfIcon(hdc, pdfIcon);
                }
                bool highlighted = !isFolder && highlightPath && path::IsSame(itemPath, highlightPath);
                if (highlighted) {
                    FillHomeRoundRect(hdc, row, DpiScale(hdc, 8), selBg);
                }
                Rect metaRect{row.x + row.dx - DpiScale(hdc, 170), row.y, DpiScale(hdc, 170), row.dy};
                Rect nameRect{icon.x + icon.dx + DpiScale(hdc, 10), row.y,
                              std::max(0, metaRect.x - icon.x - icon.dx - DpiScale(hdc, 22)), row.dy};
                bool withFolder = !isFolder && showingSearchFiles;
                if (withFolder) {
                    nameRect.y += DpiScale(hdc, 6);
                    nameRect.dy = DpiScale(hdc, 22);
                }
                SetTextColor(hdc, highlighted ? selFg : ThemeWindowTextColor());
                HdcDrawText(hdc, name, nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                            HdcGetUiFont(hdc, 14));
                if (withFolder) {
                    int folderIdx = files.AtData(idx)->folderIdx;
                    if (folderIdx >= 0) {
                        HFONT folderFont = HdcGetUiFont(hdc, 11);
                        Rect folderRect{nameRect.x, nameRect.y + nameRect.dy, nameRect.dx, DpiScale(hdc, 18)};
                        TempStr folderLine = TouchLibraryFolderDisplayTemp(folders[folderIdx]);
                        TempStr shown = FitPathTailTemp(hdc, folderLine, folderRect.dx, folderFont);
                        SetTextColor(hdc, ThemeWindowLinkColor());
                        HdcDrawText(hdc, shown, folderRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, folderFont);
                        Rect folderLink = folderRect.Intersect(filesClip);
                        if (!folderLink.IsEmpty()) {
                            win->staticLinks.Append(new StaticLink(
                                folderLink, fmt("%s%s", Str(kLinkLibraryFolderFromSearchPrefix), folders[folderIdx]),
                                folderLine));
                        }
                    }
                }
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
            int gridTop = headerDy + pad - win->libraryFilesScrollY;
            for (int j = 0; j < len(selectedFolders); j++) {
                Rect tile{leftDx + pad + (j % tileColumns) * (tileDx + tileGap),
                          gridTop + (j / tileColumns) * tileStepY, tileDx, tileDx};
                Rect tileBlock = tile;
                tileBlock.dy = tileStepY;
                if (tileBlock.Intersect(filesClip).IsEmpty()) {
                    continue;
                }
                int folderIdx = selectedFolders[j];
                DrawTouchLibraryFolderCard(win, hdc, folders[folderIdx], folders.AtData(folderIdx), tile, filesClip);
            }
            for (int i = 0; i < len(selectedFiles); i++) {
                int col = i % columns;
                int row = i / columns;
                Rect card{leftDx + pad + col * (cardDx + gap), gridTop + foldersBlockDy + row * cardStepY, cardDx,
                          cardDy};
                Rect cardBlock = card;
                cardBlock.dy = cardStepY;
                if (cardBlock.Intersect(filesClip).IsEmpty()) {
                    continue;
                }
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
                TempStr folderLine;
                TempStr folderTarget;
                if (showingSearchFiles && fileData->folderIdx >= 0) {
                    folderLine = TouchLibraryFolderDisplayTemp(folders[fileData->folderIdx]);
                    folderTarget = fmt("%s%s", Str(kLinkLibraryFolderFromSearchPrefix), folders[fileData->folderIdx]);
                }
                bool highlighted = highlightPath && path::IsSame(filePath, highlightPath);
                DrawTouchFileCardPath(win, hdc, filePath, gFileHistory.FindByPath(filePath), fileData->thumbnail, card,
                                      false, &filesClip, true, true, TouchLibraryFileSize(fileIdx, filePath),
                                      folderLine, folderTarget, highlighted);
            }
        }
        if (itemCount == 0) {
            Rect empty{leftDx + DpiScale(hdc, 40), headerDy + DpiScale(hdc, 40), rc.dx - leftDx - DpiScale(hdc, 80),
                       DpiScale(hdc, 40)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            Str message;
            if (libraryLoading && (len(folders) == 0 || selectionPending)) {
                message = StrL("Loading your Library…");
            } else if (showingSearchFiles) {
                message = fmt("No files match “%s”.", query);
            } else if (len(folders) == 0) {
                message = StrL("Add a folder to build your Library.");
            } else {
                message = StrL("This folder is empty.");
            }
            HdcDrawText(hdc, message, empty, DT_SINGLELINE | DT_NOPREFIX, HdcGetUiFont(hdc, 14));
        }
        RestoreDC(hdc, filesDc);
    }

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

    // the search field is a child window the scrim cannot dim; it would sit
    // on the modal as a bright box
    bool modalOpen = win->libraryManageFoldersOpen || win->librarySearchScopePickerOpen;
    if (hasSidebar && win->hwndHomeSearch) {
        HwndSetVisible(win->hwndHomeSearch, !modalOpen);
    }
    if (win->libraryManageFoldersOpen) {
        DrawTouchLibraryManageModal(win, hdc, rc);
    } else if (win->librarySearchScopePickerOpen) {
        DrawTouchLibrarySearchScopeModal(win, hdc, rc, folders);
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
        // the Library is the only browsing destination; Recent is one of its
        // sidebar rows (win->libraryRecentSelected)
        DrawTouchLibraryPageV2(win, hdc);
        DrawLibraryContentSwap(win, hdc);
        // after the content, so it sits on top of whatever the link covers
        DrawLibraryFeedback(win, hdc);
        DrawPinFlight(win, hdc);
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

// Keeps the timer running exactly as long as something is still moving, so an
// idle Library costs nothing.
static void UpdateLibraryScrollTimer(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    bool moving = KsIsMoving(win->libraryTreeKs) || KsIsMoving(win->libraryFilesKs);
    if (moving) {
        SetTimer(win->hwndCanvas, kLibraryScrollTimerID, kAnimTickMs, nullptr);
    } else {
        KillTimer(win->hwndCanvas, kLibraryScrollTimerID);
    }
}

// --- Library hover / press feedback -------------------------------------
// Base-level (AnimateUI) feedback: until now the Library answered a click with
// nothing but a cursor change, so tapping a folder felt dead until the whole
// view swapped.

// Owned here rather than on MainWindow (see the note in MainWindow.h). Only
// one Library surface is interacted with at a time, so a single set of state
// plus the window that owns it is enough; switching windows resets it.
static MainWindow* gLibSwapWin = nullptr;
static Str gLibSwapKey;
static Rect gLibSwapRect;
static AnimVal gLibSwapVal;

static MainWindow* gFeedbackWin = nullptr;
static Str gLibraryHotTarget;
static Str gLibraryPressedTarget;
static AnimVal gLibraryHotVal;
static AnimVal gLibraryPressVal;

static void ResetLibraryFeedbackIfOtherWindow(MainWindow* win) {
    if (gFeedbackWin == win) {
        return;
    }
    gFeedbackWin = win;
    str::ReplaceWithCopy(&gLibraryHotTarget, Str{});
    str::ReplaceWithCopy(&gLibraryPressedTarget, Str{});
    gLibraryHotVal.Set(0.0f);
    gLibraryPressVal.Set(0.0f);
}

static void UpdateLibraryFeedbackTimer(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    bool moving = gLibraryHotVal.IsAnimating() || gLibraryPressVal.IsAnimating() || gLibSwapVal.IsAnimating();
    if (moving) {
        SetTimer(win->hwndCanvas, kLibraryFeedbackTimerID, kAnimTickMs, nullptr);
    } else {
        KillTimer(win->hwndCanvas, kLibraryFeedbackTimerID);
    }
}

void HomePageFeedbackTick(MainWindow* win) {
    if (!win) {
        return;
    }
    HwndInvalidate(win->hwndCanvas, false);
    UpdateLibraryFeedbackTimer(win);
}

void HomePageSetHotLink(MainWindow* win, Str target) {
    if (!win) {
        return;
    }
    ResetLibraryFeedbackIfOtherWindow(win);
    if (str::Eq(gLibraryHotTarget, target)) {
        return;
    }
    str::ReplaceWithCopy(&gLibraryHotTarget, target);
    if (AnimEnabled()) {
        // ease from wherever it is, so sweeping across rows trails rather than
        // flicking between them
        gLibraryHotVal.SetTarget(target ? 1.0f : 0.0f, kAnimHoverMs);
        UpdateLibraryFeedbackTimer(win);
    } else {
        gLibraryHotVal.Set(target ? 1.0f : 0.0f);
    }
    HwndInvalidate(win->hwndCanvas, false);
}

void HomePageSetPressedLink(MainWindow* win, Str target) {
    if (!win) {
        return;
    }
    ResetLibraryFeedbackIfOtherWindow(win);
    if (str::Eq(gLibraryPressedTarget, target)) {
        return;
    }
    str::ReplaceWithCopy(&gLibraryPressedTarget, target);
    bool down = !!target; // Str has an explicit bool test, not a null compare
    if (AnimEnabled()) {
        gLibraryPressVal.SetTarget(down ? 1.0f : 0.0f, down ? kAnimPressMs : kAnimPressReleaseMs);
        UpdateLibraryFeedbackTimer(win);
    } else {
        gLibraryPressVal.Set(down ? 1.0f : 0.0f);
    }
    HwndInvalidate(win->hwndCanvas, false);
}

// Switching what the content pane shows - Recent, then a folder, then another
// folder - swaps the whole right-hand side in one frame. In elaborate mode the
// new contents rise out of the background instead, which reads as "this
// changed because you clicked" rather than as a flicker. Cheap because it is
// one alpha fill over the finished content, not a second render.
static void NoteLibraryContentSwap(MainWindow* win, HDC hdc, Rect content, Str key) {
    gLibSwapRect = content;
    if (!AnimElaborate()) {
        return;
    }
    bool sameWin = gLibSwapWin == win;
    if (sameWin && str::Eq(gLibSwapKey, key)) {
        return;
    }
    bool first = !sameWin || !gLibSwapKey;
    gLibSwapWin = win;
    str::ReplaceWithCopy(&gLibSwapKey, key);
    if (first) {
        gLibSwapVal.Set(1.0f); // opening the Library is not a swap
        return;
    }
    gLibSwapVal.Set(0.0f);
    gLibSwapVal.SetTarget(1.0f, kAnimContentSwapMs);
    SetTimer(win->hwndCanvas, kLibraryFeedbackTimerID, kAnimTickMs, nullptr);
}

static void DrawLibraryContentSwap(MainWindow* win, HDC hdc) {
    if (gLibSwapWin != win || gLibSwapRect.IsEmpty()) {
        return;
    }
    float v = gLibSwapVal.Value();
    if (v >= 1.0f) {
        return;
    }
    u8 alpha = (u8)(255.0f * (1.0f - v));
    Gdiplus::Graphics gfx(hdc);
    COLORREF bg = ThemeMainWindowBackgroundColor();
    Gdiplus::SolidBrush br(Gdiplus::Color(alpha, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
    gfx.FillRectangle(&br, gLibSwapRect.x, gLibSwapRect.y, gLibSwapRect.dx, gLibSwapRect.dy);
}

// look the link's CURRENT rect up by target: static links are rebuilt every
// paint and their rects move as the pane scrolls
static bool FindLinkRect(MainWindow* win, Str target, Rect* out) {
    if (!target) {
        return false;
    }
    for (StaticLink* l : win->staticLinks) {
        if (l && str::Eq(l->target, target)) {
            *out = l->rect;
            return true;
        }
    }
    return false;
}

// One overlay for the whole surface, drawn after the content: hover lightens,
// press darkens and sinks slightly. Keyed by target so every interactive
// element gets it without touching its own draw code.
static void DrawLibraryFeedback(MainWindow* win, HDC hdc) {
    float hotAmt = gLibraryHotVal.Value();
    float pressAmt = gLibraryPressVal.Value();
    struct Layer {
        Str target;
        float amt;
        bool press;
    };
    Layer layers[2] = {{gLibraryHotTarget, hotAmt, false}, {gLibraryPressedTarget, pressAmt, true}};
    for (const Layer& layer : layers) {
        if (layer.amt <= 0.01f || !layer.target) {
            continue;
        }
        Rect r;
        if (!FindLinkRect(win, layer.target, &r) || r.IsEmpty()) {
            continue;
        }
        if (layer.press) {
            int inset = (int)((float)DpiScale(hdc, 2) * layer.amt + 0.5f);
            r.x += inset;
            r.y += inset;
            r.dx -= inset * 2;
            r.dy -= inset * 2;
        }
        if (r.dx <= 0 || r.dy <= 0) {
            continue;
        }
        // alpha-blended so it reads on both the light and dark themes without
        // needing to know what is underneath
        u8 alpha = (u8)((layer.press ? 38.0f : 20.0f) * layer.amt);
        COLORREF col = ThemeWindowTextColor();
        int radius = std::min(DpiScale(hdc, 10), std::min(r.dx, r.dy) / 2);
        FillHomeRoundRectAlpha(hdc, r, radius, col, alpha);
    }
}

// Pinning moves an item somewhere else on the screen, and without a hint the
// card simply vanishes from where it was. A small pin glyph flies from the
// badge that was tapped to the PINNED section header, arcing up so the eye can
// follow it. Unpinning flies the other way, back to the card.
static MainWindow* gPinFlyWin = nullptr;
static Point gPinFlyFrom;
static Point gPinFlyTo;
static AnimVal gPinFlyVal;
static Rect gPinFilesAnchor;
static Rect gPinFoldersAnchor;
// A copy of the page taken once, right after the real pin/unpin has been
// painted, so every frame after that is a blit of this plus the moving glyph
// instead of a full DrawHomePage() pass - measured at 80-100ms on a modest
// library, which alone would make a 380ms flight choppy at best.
static HBITMAP gPinFlySnapshot = nullptr;
static Size gPinFlySnapshotSize;

void HomePageSetPinAnchor(MainWindow* win, bool isFolder, Rect r) {
    if (!win) {
        return;
    }
    (isFolder ? gPinFoldersAnchor : gPinFilesAnchor) = r;
}

static void FreePinFlySnapshot() {
    if (gPinFlySnapshot) {
        DeleteObject(gPinFlySnapshot);
        gPinFlySnapshot = nullptr;
    }
    gPinFlySnapshotSize = Size{};
}

static void DrawPinFlight(MainWindow* win, HDC hdc);

// Composites one frame: the static snapshot plus the glyph at its current
// position, blitted straight to the screen.
static void PaintPinFlightFrame(MainWindow* win, HWND hwnd) {
    HDC hdcBuf = win->buffer->GetDC();
    HDC hdcSnap = CreateCompatibleDC(hdcBuf);
    HGDIOBJ old = SelectObject(hdcSnap, gPinFlySnapshot);
    BitBlt(hdcBuf, 0, 0, gPinFlySnapshotSize.dx, gPinFlySnapshotSize.dy, hdcSnap, 0, 0, SRCCOPY);
    SelectObject(hdcSnap, old);
    DeleteDC(hdcSnap);
    DrawPinFlight(win, hdcBuf);
    HDC hdcScreen = GetDC(hwnd);
    if (hdcScreen) {
        win->buffer->Flush(hdcScreen);
        ReleaseDC(hwnd, hdcScreen);
    }
}

void HomePageStartPinFlight(MainWindow* win, Str target, bool isFolder, bool nowPinned) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    // The pin badge and the PINNED section both live only on this canvas, so
    // this is the one call the caller needs regardless of whether the flight
    // itself can run - a plain toggle still has to repaint. Callers rely on
    // this and do not invalidate anything themselves.
    if (!AnimEnabled()) {
        HwndInvalidate(win->hwndCanvas, false);
        return;
    }
    Rect badge;
    if (!FindLinkRect(win, target, &badge) || badge.IsEmpty()) {
        HwndInvalidate(win->hwndCanvas, false);
        return;
    }
    Rect anchor = isFolder ? gPinFoldersAnchor : gPinFilesAnchor;
    if (anchor.IsEmpty()) {
        HwndInvalidate(win->hwndCanvas, false);
        return;
    }
    Point onCard{badge.x + badge.dx / 2, badge.y + badge.dy / 2};
    Point onShelf{anchor.x + DpiScale(win->hwndCanvas, 9), anchor.y + anchor.dy / 2};
    // pinning travels card -> shelf; unpinning is the same path run backwards
    gPinFlyFrom = nowPinned ? onCard : onShelf;
    gPinFlyTo = nowPinned ? onShelf : onCard;
    gPinFlyWin = win;
    gPinFlyVal.Set(0.0f);
    gPinFlyVal.SetTarget(1.0f, kAnimPinFlightMs);

    // Paint the real, already-updated state once - the badge and PINNED
    // section already reflect the new pin state - and snapshot it so every
    // following frame is a cheap composite instead of a rebuild.
    HWND hwnd = win->hwndCanvas;
    HwndInvalidate(hwnd, false);
    UpdateWindow(hwnd);
    FreePinFlySnapshot();
    if (win->buffer) {
        HDC hdcBuf = win->buffer->GetDC();
        HDC hdcScreen = GetDC(hwnd);
        if (hdcScreen) {
            Size sz = win->buffer->rect.Size();
            if (sz.dx > 0 && sz.dy > 0) {
                gPinFlySnapshot = CreateCompatibleBitmap(hdcScreen, sz.dx, sz.dy);
                if (gPinFlySnapshot) {
                    HDC hdcSnap = CreateCompatibleDC(hdcBuf);
                    HGDIOBJ old = SelectObject(hdcSnap, gPinFlySnapshot);
                    BitBlt(hdcSnap, 0, 0, sz.dx, sz.dy, hdcBuf, 0, 0, SRCCOPY);
                    SelectObject(hdcSnap, old);
                    DeleteDC(hdcSnap);
                    gPinFlySnapshotSize = sz;
                }
            }
            ReleaseDC(hwnd, hdcScreen);
        }
    }
    if (!gPinFlySnapshot) {
        gPinFlyWin = nullptr;
        return; // nothing to composite onto; the real state is already shown
    }

    // Driven directly rather than through a WM_TIMER: that timer is the
    // lowest-priority message Windows will synthesize, so even one other
    // window with a pending repaint is enough to push it past this flight's
    // whole 380ms and skip the animation outright - measured directly, the
    // first tick sometimes did not arrive until well after the flight should
    // have finished. Each frame here is two bitmap blits, so a plain Sleep
    // loop is cheap, and unlike a timer it cannot be starved by anything else
    // in the queue. Messages are pumped between frames so the app stays
    // responsive; a second pin tap during the loop starts its own flight
    // (same globals), and this loop notices and steps aside for it.
    while (gPinFlyVal.IsAnimating() && gPinFlyWin == win && IsMainWindowValid(win) && IsWindow(hwnd)) {
        u64 tickStart = GetTickCount64();
        PaintPinFlightFrame(win, hwnd);
        MSG msg;
        // Cap the drain, or a burst of queued input (a flood of mouse-move
        // messages is the common case) can keep this pumping well past the
        // next frame's due time - the loop would still call Sleep(kAnimTickMs)
        // afterwards, so that one frame runs long and every frame after it is
        // late by the same amount, which reads as a stutter partway through
        // the flight rather than a uniformly slower one.
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            // a message in this batch may have closed the window (or the
            // whole app); win itself can be gone, not just the HWND
            if (!IsMainWindowValid(win) || !IsWindow(hwnd)) {
                break;
            }
            if (GetTickCount64() - tickStart >= kAnimTickMs) {
                break;
            }
        }
        if (!IsMainWindowValid(win) || !IsWindow(hwnd)) {
            break;
        }
        // Sleep only what is left of this tick - the paint and pump above
        // already spent some of it - so frames land at a steady kAnimTickMs
        // apart instead of that interval stacking on top of however long
        // this iteration's work took.
        u64 elapsed = GetTickCount64() - tickStart;
        if (elapsed < kAnimTickMs) {
            Sleep((DWORD)(kAnimTickMs - elapsed));
        }
    }
    // Only clean up if this call still owns the shared state: a second pin
    // tap during the loop above starts its own flight (same globals) and
    // takes over gPinFlyWin/gPinFlySnapshot, and that flight's own loop is
    // responsible for freeing them, not this one.
    if (gPinFlyWin == win) {
        gPinFlyWin = nullptr;
        FreePinFlySnapshot();
        if (IsMainWindowValid(win) && IsWindow(hwnd)) {
            // one real repaint to replace the last composited frame - which
            // still shows the glyph - with the plain, finished page
            HwndInvalidate(hwnd, false);
        }
    }
}

static void DrawPinFlight(MainWindow* win, HDC hdc) {
    if (gPinFlyWin != win) {
        return;
    }
    float t = gPinFlyVal.Value();
    if (t <= 0.0f || t >= 1.0f) {
        return;
    }
    // quadratic bezier with the control point lifted above the straight line,
    // so the glyph arcs rather than sliding
    float mx = (float)(gPinFlyFrom.x + gPinFlyTo.x) / 2.0f;
    float my = (float)std::min(gPinFlyFrom.y, gPinFlyTo.y) - (float)DpiScale(hdc, 46);
    float u = 1.0f - t;
    float x = u * u * (float)gPinFlyFrom.x + 2.0f * u * t * mx + t * t * (float)gPinFlyTo.x;
    float y = u * u * (float)gPinFlyFrom.y + 2.0f * u * t * my + t * t * (float)gPinFlyTo.y;
    // shrinks as it arrives, and fades over the last third
    int full = DpiScale(hdc, 22);
    int dy = (int)((float)full * (1.0f - 0.45f * t) + 0.5f);
    Rect glyph{(int)(x + 0.5f) - dy / 2, (int)(y + 0.5f) - dy / 2, dy, dy};
    float alpha = t > 0.66f ? (1.0f - t) / 0.34f : 1.0f;

    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    COLORREF col = ThemeWindowLinkColor();
    // a soft disc behind the glyph so it stays legible over cards and thumbnails
    Gdiplus::SolidBrush halo(Gdiplus::Color((u8)(46.0f * alpha), GetRValue(col), GetGValue(col), GetBValue(col)));
    int pad = DpiScale(hdc, 5);
    gfx.FillEllipse(&halo, glyph.x - pad, glyph.y - pad, glyph.dx + 2 * pad, glyph.dy + 2 * pad);
    Gdiplus::SolidBrush brush(Gdiplus::Color((u8)(255.0f * alpha), GetRValue(col), GetGValue(col), GetBValue(col)));
    Gdiplus::PointF pts[] = {
        {(float)glyph.x + glyph.dx * 0.27f, (float)glyph.y + glyph.dy * 0.16f},
        {(float)glyph.x + glyph.dx * 0.73f, (float)glyph.y + glyph.dy * 0.16f},
        {(float)glyph.x + glyph.dx * 0.73f, (float)glyph.y + glyph.dy * 0.84f},
        {(float)glyph.x + glyph.dx * 0.50f, (float)glyph.y + glyph.dy * 0.65f},
        {(float)glyph.x + glyph.dx * 0.27f, (float)glyph.y + glyph.dy * 0.84f},
    };
    gfx.FillPolygon(&brush, pts, dimofi(pts));
}

// Tap-and-hold fired: treat it exactly like a right-click at the point the
// finger went down, so touch and mouse reach the same menu. The pan is
// cancelled first, or releasing the finger afterwards would also tap the card.
void HomePageOnHoldTimer(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    KillTimer(win->hwndCanvas, kAboutHoldTimerID);
    if (!IsTouchChrome(win) || win->touchAboutPanMoved || win->touchAboutPointerId == 0) {
        return;
    }
    Point pt = win->touchAboutPanStart;
    win->touchAboutPointerId = 0;
    win->touchAboutPanArea = 0;
    win->touchAboutSuppressMouseUp = true;
    OnAboutContextMenu(win, pt.x, pt.y);
}

// One tick of the Library's scroll momentum: advance both columns, publish the
// result into the ScrollY fields the paint code reads, repaint.
void HomePageKineticTick(MainWindow* win) {
    if (!win) {
        return;
    }
    bool moving = KsTick(win->libraryTreeKs);
    moving |= KsTick(win->libraryFilesKs);
    win->libraryTreeScrollY = KsPos(win->libraryTreeKs);
    win->libraryFilesScrollY = KsPos(win->libraryFilesKs);
    HwndInvalidate(win->hwndCanvas, false);
    if (!moving) {
        UpdateLibraryScrollTimer(win);
    }
}

// Called before feeding a scroller: the content it scrolls over is remeasured
// on every layout, so bounds have to be resynced or a fling runs past the end.
static void SyncLibraryScrollBounds(MainWindow* win) {
    KsSetBounds(win->libraryTreeKs, 0, win->libraryTreeScrollMaxY);
    KsSetBounds(win->libraryFilesKs, 0, win->libraryFilesScrollMaxY);
    // another code path may have set the position directly (view change, filter
    // applied, folder opened); adopt it rather than fighting it
    if (KsPos(win->libraryTreeKs) != win->libraryTreeScrollY && !KsIsMoving(win->libraryTreeKs)) {
        KsSetPos(win->libraryTreeKs, win->libraryTreeScrollY);
    }
    if (KsPos(win->libraryFilesKs) != win->libraryFilesScrollY && !KsIsMoving(win->libraryFilesKs)) {
        KsSetPos(win->libraryFilesKs, win->libraryFilesScrollY);
    }
}

void HomePageOnMouseWheel(MainWindow* win, int delta, Point canvasPt) {
    // Match the PAINT gate exactly (see the IsTouchChrome branch in the about
    // page draw): under the touch chrome this canvas is always the Library
    // surface. Testing touchView == Library here instead meant that opening
    // with no document - where the Library draws but touchView is still Doc -
    // fell through to the classic home-page scroller, which moves a different
    // variable, so the Recent screen could not be scrolled at all.
    if (IsTouchChrome(win)) {
        if (win->libraryManageFoldersOpen) {
            return;
        }
        if (win->librarySearchScopePickerOpen) {
            int step = DpiScale(win->hwndCanvas, 72);
            int dy = delta > 0 ? -step : step;
            win->librarySearchScopeScrollY =
                std::clamp(win->librarySearchScopeScrollY + dy, 0, win->librarySearchScopeScrollMaxY);
            HwndInvalidate(win->hwndCanvas);
            return;
        }
        // a wheel notch eases to its new position instead of jumping a fixed
        // number of pixels, which is what made this feel unfinished
        int step = DpiScale(win->hwndCanvas, 72);
        int dy = delta > 0 ? -step : step;
        SyncLibraryScrollBounds(win);
        if (canvasPt.x < TouchLibrarySidebarDx(win)) {
            KsScrollBy(win->libraryTreeKs, dy);
            win->libraryTreeScrollY = KsPos(win->libraryTreeKs);
        } else {
            KsScrollBy(win->libraryFilesKs, dy);
            win->libraryFilesScrollY = KsPos(win->libraryFilesKs);
        }
        UpdateLibraryScrollTimer(win);
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
    if (!IsTouchChrome(win) || win->touchView != TouchView::Library || win->homeOpenScrollMaxX <= 0) {
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
        // No touchView test: this handler only runs for the touch chrome's about
        // canvas, which always draws the Library surface (same gate the paint
        // path uses). Requiring touchView == Library here broke drag-scrolling
        // whenever the app opened with no document, where the Library is on
        // screen but touchView is still Doc.
        if (win->librarySearchScopePickerOpen) {
            // a finger on the picker scrolls its list; it must not start a
            // pan of the Library underneath, which closed the picker
            area = 6;
        } else if (TouchLibrarySplitterHitRect(win).Contains(pt)) {
            area = 5;
        } else if (pt.y >= headerDy) {
            int leftDx = TouchLibrarySidebarDx(win);
            int manageTop = canvas.dy - DpiScale(win->hwndCanvas, 64);
            if (pt.x < leftDx) {
                area = pt.y < manageTop ? 3 : 0;
            } else {
                area = 4;
            }
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
        if (area == 3) {
            win->touchAboutPanStartY = win->libraryTreeScrollY;
        } else if (area == 6) {
            win->touchAboutPanStartY = win->librarySearchScopeScrollY;
        } else {
            win->touchAboutPanStartY = win->libraryFilesScrollY;
        }
        // a touch on a coasting list catches it, like every other touch surface
        SyncLibraryScrollBounds(win);
        KsStop(win->libraryTreeKs);
        KsStop(win->libraryFilesKs);
        UpdateLibraryScrollTimer(win);
        KsDragBegin(area == 3 ? win->libraryTreeKs : win->libraryFilesKs, pt.y);
        win->touchAboutPanMoved = false;
        win->touchAboutSuppressMouseUp = false;
        // a finger has no right button: hold still on a card for ~500ms and the
        // same context menu opens (Open Another Copy, Pin, ...)
        SetTimer(win->hwndCanvas, kAboutHoldTimerID, 500, nullptr);
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
            // it became a drag, so it is not a hold
            KillTimer(win->hwndCanvas, kAboutHoldTimerID);
            str::FreePtr(&win->urlOnLastButtonDown);
            if (win->touchAboutPanArea != 6) {
                CloseTouchLibraryTransientUi(win);
            }
        }
        if (win->touchAboutPanArea == 6) {
            win->librarySearchScopeScrollY =
                std::clamp(win->touchAboutPanStartY - dy, 0, win->librarySearchScopeScrollMaxY);
            HwndInvalidate(win->hwndCanvas, false);
            return true;
        }
        if (win->touchAboutPanArea == 2 && win->touchAboutPanAxis == 0) {
            win->touchAboutPanAxis = abs(dx) > abs(dy) ? 1 : 2;
        }
        if (win->touchAboutPanArea == 5) {
            ResizeTouchLibrarySidebar(win, pt.x);
        } else if (win->touchAboutPanArea == 2 && win->touchAboutPanAxis == 1) {
            win->homeOpenScrollX = std::clamp(win->touchAboutPanStartX - dx, 0, win->homeOpenScrollMaxX);
        } else if (win->touchAboutPanArea == 3) {
            KsDragUpdate(win->libraryTreeKs, pt.y);
            win->libraryTreeScrollY = KsPos(win->libraryTreeKs);
        } else if (win->touchAboutPanArea == 2 || win->touchAboutPanArea == 4) {
            KsDragUpdate(win->libraryFilesKs, pt.y);
            win->libraryFilesScrollY = KsPos(win->libraryFilesKs);
        }
        HwndInvalidate(win->hwndCanvas, false);
        return true;
    }
    if (msg == WM_POINTERUP) {
        Point pt = TouchAboutPointerPos(win, lp);
        KillTimer(win->hwndCanvas, kAboutHoldTimerID);
        int area = win->touchAboutPanArea;
        bool moved = win->touchAboutPanMoved;
        // let go of a flick and the list keeps going, then coasts to a stop
        KsDragEnd(win->libraryTreeKs);
        KsDragEnd(win->libraryFilesKs);
        UpdateLibraryScrollTimer(win);
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
