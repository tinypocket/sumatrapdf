/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// @gen-start cmd-enum
// clang-format off
enum {
    // commands are integers sent with WM_COMMAND so start them
    // at some number higher than 0
    CmdFirst = 200,
    CmdSeparator = CmdFirst,

    CmdOpenFile = 201,
    CmdClose = 202,
    CmdCloseCurrentDocument = 203,
    CmdCloseOtherTabs = 204,
    CmdCloseTabsToTheRight = 205,
    CmdCloseTabsToTheLeft = 206,
    CmdCloseAllTabs = 207,
    CmdSaveAs = 208,
    CmdPrint = 209,
    CmdShowInFolder = 210,
    CmdRenameFile = 211,
    CmdDeleteFile = 212,
    CmdExit = 213,
    CmdReloadDocument = 214,
    CmdCreateShortcutToFile = 215,
    CmdSendByEmail = 216,
    CmdProperties = 217,
    CmdSinglePageView = 218,
    CmdFacingView = 219,
    CmdBookView = 220,
    CmdToggleContinuousView = 221,
    CmdToggleMangaMode = 222,
    CmdTrimHeaderFooter = 223,
    CmdRotateLeft = 224,
    CmdRotateRight = 225,
    CmdToggleBookmarks = 226,
    CmdToggleTableOfContents = 227,
    CmdToggleFullscreen = 228,
    CmdPresentationWhiteBackground = 229,
    CmdPresentationBlackBackground = 230,
    CmdTogglePresentationMode = 231,
    CmdToggleToolbar = 232,
    CmdChangeScrollbar = 233,
    CmdToggleMenuBar = 234,
    CmdCopySelection = 235,
    CmdTranslateSelectionWithGoogle = 236,
    CmdTranslateSelectionWithDeepL = 237,
    CmdSearchSelectionWithGoogle = 238,
    CmdSearchSelectionWithBing = 239,
    CmdSearchSelectionWithWikipedia = 240,
    CmdSearchSelectionWithGoogleScholar = 241,
    CmdSelectAll = 242,
    CmdNewWindow = 243,
    CmdDuplicateInNewWindow = 244,
    CmdDuplicateInNewTab = 245,
    CmdCopyImage = 246,
    CmdCopyLinkTarget = 247,
    CmdCopyComment = 248,
    CmdCopyFilePath = 249,
    CmdScrollUp = 250,
    CmdScrollDown = 251,
    CmdScrollLeft = 252,
    CmdScrollRight = 253,
    CmdScrollLeftPage = 254,
    CmdScrollRightPage = 255,
    CmdScrollUpPage = 256,
    CmdScrollDownPage = 257,
    CmdScrollDownHalfPage = 258,
    CmdScrollUpHalfPage = 259,
    CmdGoToNextPage = 260,
    CmdGoToPrevPage = 261,
    CmdGoToFirstPage = 262,
    CmdGoToLastPage = 263,
    CmdGoToPage = 264,
    CmdFindFirst = 265,
    CmdFindNext = 266,
    CmdFindPrev = 267,
    CmdFindNextSel = 268,
    CmdFindPrevSel = 269,
    CmdFindToggleMatchCase = 270,
    CmdSaveAnnotations = 271,
    CmdSaveAnnotationsNewFile = 272,
    CmdDiscardChanges = 273,
    CmdEditAnnotations = 274,
    CmdDeleteAnnotation = 275,
    CmdZoomFitPage = 276,
    CmdZoomActualSize = 277,
    CmdZoomFitWidth = 278,
    CmdZoomFitByOrientation = 279,
    CmdZoom6400 = 280,
    CmdZoom3200 = 281,
    CmdZoom1600 = 282,
    CmdZoom800 = 283,
    CmdZoom400 = 284,
    CmdZoom200 = 285,
    CmdZoom150 = 286,
    CmdZoom125 = 287,
    CmdZoom100 = 288,
    CmdZoom50 = 289,
    CmdZoom25 = 290,
    CmdZoom12_5 = 291,
    CmdZoom8_33 = 292,
    CmdZoomFitContent = 293,
    CmdZoomShrinkToFit = 294,
    CmdZoomCustom = 295,
    CmdZoomIn = 296,
    CmdZoomOut = 297,
    CmdZoomFitWidthAndContinuous = 298,
    CmdZoomFitPageAndSinglePage = 299,
    CmdContributeTranslation = 300,
    CmdOpenWithKnownExternalViewerFirst = 301,
    CmdOpenWithExplorer = 302,
    CmdOpenWithDirectoryOpus = 303,
    CmdOpenWithTotalCommander = 304,
    CmdOpenWithDoubleCommander = 305,
    CmdOpenWithAcrobat = 306,
    CmdOpenWithFoxIt = 307,
    CmdOpenWithFoxItPhantom = 308,
    CmdOpenWithPdfXchange = 309,
    CmdOpenWithXpsViewer = 310,
    CmdOpenWithHtmlHelp = 311,
    CmdOpenWithPdfDjvuBookmarker = 312,
    CmdOpenWithKnownExternalViewerLast = 313,
    CmdOpenSelectedDocument = 314,
    CmdOpenSelectedDocumentNewCopy = 315,
    CmdPinSelectedDocument = 316,
    CmdForgetSelectedDocument = 317,
    CmdExpandAll = 318,
    CmdCollapseAll = 319,
    CmdSaveEmbeddedFile = 320,
    CmdOpenEmbeddedPDF = 321,
    CmdSaveAttachment = 322,
    CmdOpenAttachment = 323,
    CmdOptions = 324,
    CmdAdvancedOptions = 325,
    CmdAdvancedSettings = 326,
    CmdChangeLanguage = 327,
    CmdCheckUpdate = 328,
    CmdInstallPrereleaseUpdate = 329,
    CmdTogglePdfPreviewLogging = 330,
    CmdHelpOpenManual = 331,
    CmdHelpOpenManualOnWebsite = 332,
    CmdHelpOpenKeyboardShortcuts = 333,
    CmdHelpVisitWebsite = 334,
    CmdHelpAbout = 335,
    CmdMoveFrameFocus = 336,
    CmdFavoriteAdd = 337,
    CmdFavoriteDel = 338,
    CmdFavoriteToggle = 339,
    CmdToggleLinks = 340,
    CmdToggleShowAnnotations = 341,
    CmdShowAnnotations = 342,
    CmdHideAnnotations = 343,
    CmdCreateAnnotText = 344,
    CmdCreateAnnotLink = 345,
    CmdCreateAnnotFreeText = 346,
    CmdCreateAnnotLine = 347,
    CmdCreateAnnotSquare = 348,
    CmdCreateAnnotCircle = 349,
    CmdCreateAnnotPolygon = 350,
    CmdCreateAnnotPolyLine = 351,
    CmdCreateAnnotHighlight = 352,
    CmdCreateAnnotUnderline = 353,
    CmdCreateAnnotSquiggly = 354,
    CmdCreateAnnotStrikeOut = 355,
    CmdCreateAnnotRedact = 356,
    CmdCreateAnnotStamp = 357,
    CmdCreateAnnotCaret = 358,
    CmdCreateAnnotInk = 359,
    CmdCreateAnnotPopup = 360,
    CmdCreateAnnotFileAttachment = 361,
    CmdInvertColors = 362,
    CmdTogglePageInfo = 363,
    CmdToggleZoom = 364,
    CmdNavigateBack = 365,
    CmdNavigateForward = 366,
    CmdToggleCursorPosition = 367,
    CmdOpenNextFileInFolder = 368,
    CmdOpenPrevFileInFolder = 369,
    CmdCommandPalette = 370,
    CmdShowLog = 371,
    CmdShowErrors = 372,
    CmdClearHistory = 373,
    CmdReopenLastClosedFile = 374,
    CmdNextTab = 375,
    CmdPrevTab = 376,
    CmdNextTabSmart = 377,
    CmdPrevTabSmart = 378,
    CmdMoveTabLeft = 379,
    CmdMoveTabRight = 380,
    CmdInvokeInverseSearch = 381,
    CmdExec = 382,
    CmdViewWithExternalViewer = 383,
    CmdSelectionHandler = 384,
    CmdSetTheme = 385,
    CmdToggleInverseSearch = 386,
    CmdDebugCorruptMemory = 387,
    CmdDebugCrashMe = 388,
    CmdDebugDownloadSymbols = 389,
    CmdDebugTestApp = 390,
    CmdDebugShowNotif = 391,
    CmdDebugStartStressTest = 392,
    CmdDebugTogglePredictiveRender = 393,
    CmdDebugToggleRtl = 394,
    CmdListPrinters = 395,
    CmdToggleWindowsPreviewer = 396,
    CmdToggleWindowsSearchFilter = 397,
    CmdScreenshot = 398,
    CmdCropImage = 399,
    CmdResizeImage = 400,
    CmdSaveImage = 401,
    CmdPasteClipboardImage = 402,
    CmdTabGroupSave = 403,
    CmdTabGroupRestore = 404,
    CmdChangeBackgroundColor = 405,
    CmdSetTabColor = 406,
    CmdPdfCompress = 407,
    CmdPdfDecompress = 408,
    CmdPdfDeletePages = 409,
    CmdPdfExtractPages = 410,
    CmdPdfEncrypt = 411,
    CmdPdfDecrypt = 412,
    CmdPdfBake = 413,
    CmdPdShowInfo = 414,
    CmdDocumentExtractText = 415,
    CmdDocumentShowOutline = 416,
    CmdSetScreenshotHotkey = 417,
    CmdReadAloud = 418,
    CmdPauseReadAloud = 419,
    CmdContinueReadAloud = 420,
    CmdStopReadAloud = 421,
    CmdReadAloudFromTopPage = 422,
    CmdReadAloudSelection = 423,
    CmdToggleToolbarShowReadAloud = 424,
    CmdRemoveDeletedFilesFromHistory = 425,
    CmdCommandPaletteTOC = 426,
    CmdDebugToggleRenderInfo = 427,
    CmdConvertImageToPdf = 428,
    CmdExpandToCurrentPage = 429,
    CmdStartAutoScroll = 430,
    CmdAIChatWithClaudeCode = 431,
    CmdAIChatWithGrokBuild = 432,
    CmdAIChatWithOpenAICodex = 433,
    CmdTranslateSelectionWithGrokBuild = 434,
    CmdTranslateSelectionWithClaudeCode = 435,
    CmdTranslateSelectionWithOpenAICodex = 436,
    CmdFindToggleMatchWholeWord = 437,
    CmdGoToNextFavorite = 438,
    CmdGoToPrevFavorite = 439,
    CmdCreateAnnotImageFromClipboard = 440,
    CmdSetInverseSearch = 441,
    CmdCommandPaletteFavorites = 442,
    CmdNavigateFilesInFolder = 443,
    CmdDebugToggleCacheInfo = 444,
    CmdToggleEngineeringDrawingEnhance = 445,
    CmdSetDocumentColorsFollowTheme = 446,
    CmdTogglePreservePdfImages = 447,
    CmdToggleLightDarkTheme = 448,
    CmdChangeTheme = 449,
    CmdTranslateSelection = 450,
    CmdFavoriteShowInTab = 451,
    CmdTocExpandToLevel1 = 452,
    CmdTocExpandToLevel2 = 453,
    CmdTocExpandToLevel3 = 454,
    CmdTocCollapseSameLevel = 455,
    CmdToggleFavoritesSort = 456,
    CmdZoomFitHeight = 457,
    CmdDeleteFileAndOpenNext = 458,
    CmdShowGeneratedHTML = 459,
    CmdDeleteCachedFiles = 460,
    CmdToggleKeyboardLinkFollowing = 461,
    CmdDebugToggleDpiOverride = 462,
    CmdToggleImages = 463,
    CmdSelectTextViaKeyboard = 464,
    CmdToggleTabs = 465,
    CmdTouchSidebarDensityCondensed = 466,
    CmdTouchSidebarDensityNormal = 467,
    CmdTouchSidebarDensityExpanded = 468,
    CmdNone = 469,

    /* range for file history */
    CmdFileHistoryFirst,
    CmdFileHistoryLast = CmdFileHistoryFirst + 32,

    /* range for favorites */
    CmdFavoriteFirst,
    CmdFavoriteLast = CmdFavoriteFirst + 256,

    CmdLast = CmdFavoriteLast,
    CmdFirstCustom = CmdLast + 100,

    // aliases, at the end to not mess ordering
    CmdViewLayoutFirst = CmdSinglePageView,
    CmdViewLayoutLast = CmdToggleMangaMode,

    CmdZoomFirst = CmdZoomFitPage,
    CmdZoomLast = CmdZoomCustom,

    CmdCreateAnnotFirst = CmdCreateAnnotText,
    CmdCreateAnnotLast = CmdCreateAnnotFileAttachment,
};
// clang-format on
// @gen-end cmd-enum

// order of CreateAnnot* must be the same as enum AnnotationType
/*
TOOD: maybe add commands for those annotations
Sound,
Movie,
Widget,
Screen,
PrinterMark,
TrapNet,
Watermark,
ThreeD,
*/

struct CommandArg {
    enum class Type : u16 {
        None,
        Bool,
        Int,
        Float,
        String,
        Color,
    };

    // arguments are a linked list for simplicity
    struct CommandArg* next = nullptr;

    Type type = Type::None;

    // TODO: we have a fixed number of argument names
    // we could use SeqStrings and use u16 for arg name id
    Str name;

    // TODO: could be a union
    Str strVal;
    bool boolVal = false;
    int intVal = 0;
    float floatVal = 0.0;
    ParsedColor colorVal;
};

CommandArg* AllocCommandArg(Str name, Str strVal);
void FreeCommandArgs(CommandArg* first);

struct CustomCommand {
    // all commands are stored as linked list
    struct CustomCommand* next = nullptr;

    // the command id like CmdOpenFile
    int origId = 0;

    // for debugging, the full definition of the command
    // as given by the user
    Str definition;

    // optional name, if given this shows up in command palette
    Str name;

    // optional keyboard shortcut
    Str key;

    // a unique command id generated by us, starting with CmdFirstCustom
    // it identifies a command with their fixed set of arguments
    int id = 0;

    CommandArg* firstArg = nullptr;
};

CustomCommand* AllocCustomCommand(Str definition, Str name, Str key);
void FreeCustomCommand(CustomCommand* cmd);

extern CustomCommand* gFirstCustomCommand;
extern SeqStrings gCommandDescriptions;

int GetCommandIdByName(Str);
int GetCommandIdByDesc(Str);

CustomCommand* CreateCustomCommand(Str definition, int origCmdId, CommandArg* args, Str name = {}, Str key = {});
CustomCommand* CloneCustomCommand(CustomCommand* cmd, Str name = {}, Str key = {});
CustomCommand* FindCustomCommand(int cmdId);
void FreeCustomCommands();
CommandArg* NewStringArg(Str name, Str val);
CommandArg* NewFloatArg(Str name, float val);
void InsertArg(CommandArg** firstPtr, CommandArg* arg);

CustomCommand* CreateCommandFromDefinition(Str definition);
CommandArg* GetCommandArg(CustomCommand*, Str argName);
int GetCommandIntArg(CustomCommand* cmd, Str name, int defValue);
bool GetCommandBoolArg(CustomCommand* cmd, Str name, bool defValue);
Str GetCommandStringArg(CustomCommand* cmd, Str name, Str defValue);
void GetCommandsWithOrigId(Vec<CustomCommand*>& commands, int origId);

#define kCmdArgColor StrL("color")
#define kCmdArgBgColor StrL("bgcolor")
#define kCmdArgOpacity StrL("opacity")
#define kCmdArgOpenEdit StrL("openedit")
#define kCmdArgTextSize StrL("textsize")
#define kCmdArgBorderWidth StrL("borderwidth")
#define kCmdArgAlignment StrL("alignment")
#define kCmdArgInteriorColor StrL("interiorcolor")

#define kCmdArgCopyToClipboard StrL("copytoclipboard")
#define kCmdArgSetContent StrL("setcontent")
#define kCmdArgExe StrL("exe")
#define kCmdArgURL StrL("url")
// SelectionHandlers: how and what to send (see Customize-search-translation-services.md)
#define kCmdArgMethod StrL("method")
#define kCmdArgBody StrL("body")
#define kCmdArgContentType StrL("contenttype")
#define kCmdArgHeaders StrL("headers")
#define kCmdArgLevel StrL("level")
#define kCmdArgFilter StrL("filter")
#define kCmdArgN StrL("n")
#define kCmdArgMode StrL("mode")
#define kCmdArgTheme StrL("theme")
#define kCmdArgCommandLine StrL("cmdline")
#define kCmdArgToolbarText StrL("toolbartext")
#define kCmdArgToolbarSvgIcon StrL("toolbarsvgicon")
#define kCmdArgFocusEdit StrL("focusedit")
#define kCmdArgFocusList StrL("focuslist")
// optional bool to force a state on a toggle command instead of flipping it (#5067)
#define kCmdArgState StrL("state")
