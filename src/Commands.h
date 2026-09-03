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
    CmdShowInLibraryFolder = 318,
    CmdExpandAll = 319,
    CmdCollapseAll = 320,
    CmdSaveEmbeddedFile = 321,
    CmdOpenEmbeddedPDF = 322,
    CmdSaveAttachment = 323,
    CmdOpenAttachment = 324,
    CmdOptions = 325,
    CmdAdvancedOptions = 326,
    CmdAdvancedSettings = 327,
    CmdChangeLanguage = 328,
    CmdCheckUpdate = 329,
    CmdInstallPrereleaseUpdate = 330,
    CmdTogglePdfPreviewLogging = 331,
    CmdHelpOpenManual = 332,
    CmdHelpOpenManualOnWebsite = 333,
    CmdHelpOpenKeyboardShortcuts = 334,
    CmdHelpVisitWebsite = 335,
    CmdHelpAbout = 336,
    CmdMoveFrameFocus = 337,
    CmdFavoriteAdd = 338,
    CmdFavoriteDel = 339,
    CmdFavoriteToggle = 340,
    CmdToggleLinks = 341,
    CmdToggleShowAnnotations = 342,
    CmdShowAnnotations = 343,
    CmdHideAnnotations = 344,
    CmdCreateAnnotText = 345,
    CmdCreateAnnotLink = 346,
    CmdCreateAnnotFreeText = 347,
    CmdCreateAnnotLine = 348,
    CmdCreateAnnotSquare = 349,
    CmdCreateAnnotCircle = 350,
    CmdCreateAnnotPolygon = 351,
    CmdCreateAnnotPolyLine = 352,
    CmdCreateAnnotHighlight = 353,
    CmdCreateAnnotUnderline = 354,
    CmdCreateAnnotSquiggly = 355,
    CmdCreateAnnotStrikeOut = 356,
    CmdCreateAnnotRedact = 357,
    CmdCreateAnnotStamp = 358,
    CmdCreateAnnotCaret = 359,
    CmdCreateAnnotInk = 360,
    CmdCreateAnnotPopup = 361,
    CmdCreateAnnotFileAttachment = 362,
    CmdInvertColors = 363,
    CmdTogglePageInfo = 364,
    CmdToggleZoom = 365,
    CmdNavigateBack = 366,
    CmdNavigateForward = 367,
    CmdToggleCursorPosition = 368,
    CmdOpenNextFileInFolder = 369,
    CmdOpenPrevFileInFolder = 370,
    CmdCommandPalette = 371,
    CmdShowLog = 372,
    CmdShowErrors = 373,
    CmdClearHistory = 374,
    CmdReopenLastClosedFile = 375,
    CmdNextTab = 376,
    CmdPrevTab = 377,
    CmdNextTabSmart = 378,
    CmdPrevTabSmart = 379,
    CmdMoveTabLeft = 380,
    CmdMoveTabRight = 381,
    CmdInvokeInverseSearch = 382,
    CmdExec = 383,
    CmdViewWithExternalViewer = 384,
    CmdSelectionHandler = 385,
    CmdSetTheme = 386,
    CmdToggleInverseSearch = 387,
    CmdDebugCorruptMemory = 388,
    CmdDebugCrashMe = 389,
    CmdDebugDownloadSymbols = 390,
    CmdDebugTestApp = 391,
    CmdDebugShowNotif = 392,
    CmdDebugStartStressTest = 393,
    CmdDebugTogglePredictiveRender = 394,
    CmdDebugToggleRtl = 395,
    CmdListPrinters = 396,
    CmdToggleWindowsPreviewer = 397,
    CmdToggleWindowsSearchFilter = 398,
    CmdScreenshot = 399,
    CmdCropImage = 400,
    CmdResizeImage = 401,
    CmdSaveImage = 402,
    CmdPasteClipboardImage = 403,
    CmdTabGroupSave = 404,
    CmdTabGroupRestore = 405,
    CmdChangeBackgroundColor = 406,
    CmdSetTabColor = 407,
    CmdPdfCompress = 408,
    CmdPdfDecompress = 409,
    CmdPdfDeletePages = 410,
    CmdPdfExtractPages = 411,
    CmdPdfEncrypt = 412,
    CmdPdfDecrypt = 413,
    CmdPdfBake = 414,
    CmdPdShowInfo = 415,
    CmdDocumentExtractText = 416,
    CmdDocumentShowOutline = 417,
    CmdSetScreenshotHotkey = 418,
    CmdReadAloud = 419,
    CmdPauseReadAloud = 420,
    CmdContinueReadAloud = 421,
    CmdStopReadAloud = 422,
    CmdReadAloudFromTopPage = 423,
    CmdReadAloudSelection = 424,
    CmdToggleToolbarShowReadAloud = 425,
    CmdRemoveDeletedFilesFromHistory = 426,
    CmdCommandPaletteTOC = 427,
    CmdDebugToggleRenderInfo = 428,
    CmdConvertImageToPdf = 429,
    CmdExpandToCurrentPage = 430,
    CmdStartAutoScroll = 431,
    CmdAIChatWithClaudeCode = 432,
    CmdAIChatWithGrokBuild = 433,
    CmdAIChatWithOpenAICodex = 434,
    CmdTranslateSelectionWithGrokBuild = 435,
    CmdTranslateSelectionWithClaudeCode = 436,
    CmdTranslateSelectionWithOpenAICodex = 437,
    CmdFindToggleMatchWholeWord = 438,
    CmdGoToNextFavorite = 439,
    CmdGoToPrevFavorite = 440,
    CmdCreateAnnotImageFromClipboard = 441,
    CmdSetInverseSearch = 442,
    CmdCommandPaletteFavorites = 443,
    CmdNavigateFilesInFolder = 444,
    CmdDebugToggleCacheInfo = 445,
    CmdToggleEngineeringDrawingEnhance = 446,
    CmdSetDocumentColorsFollowTheme = 447,
    CmdTogglePreservePdfImages = 448,
    CmdToggleLightDarkTheme = 449,
    CmdChangeTheme = 450,
    CmdTranslateSelection = 451,
    CmdFavoriteShowInTab = 452,
    CmdTocExpandToLevel1 = 453,
    CmdTocExpandToLevel2 = 454,
    CmdTocExpandToLevel3 = 455,
    CmdTocCollapseSameLevel = 456,
    CmdToggleFavoritesSort = 457,
    CmdZoomFitHeight = 458,
    CmdDeleteFileAndOpenNext = 459,
    CmdShowGeneratedHTML = 460,
    CmdDeleteCachedFiles = 461,
    CmdToggleKeyboardLinkFollowing = 462,
    CmdDebugToggleDpiOverride = 463,
    CmdToggleImages = 464,
    CmdSelectTextViaKeyboard = 465,
    CmdToggleTabs = 466,
    CmdTouchSidebarDensityCondensed = 467,
    CmdTouchSidebarDensityNormal = 468,
    CmdTouchSidebarDensityExpanded = 469,
    CmdNone = 470,

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
