/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Crash recovery for unsaved annotations.
//
// Shortly after each change to a PDF's annotations (drawings, highlights,
// notes, form edits) the whole document as edited is written to
// <app data>/recovery/<key>.pdf, with <key>.info naming the PDF, the process
// that owns it and the file's size and time then. Saving or discarding the
// changes deletes both. So a pair still there when no running SumatraPDF+ owns
// it means the app crashed or was killed with unsaved work: at startup, and
// whenever that PDF is opened, the user is offered to restore it. Restoring
// opens the recovery copy in place of the file (under the file's own name),
// marked as unsaved, so Save writes it to the file.

#include "base/Base.h"
#include "base/File.h"
#include "base/ScopedWin.h"
#include "base/Win.h"
#include "base/UITask.h"
#include "base/GuessFileType.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "AppTools.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "TopBar.h"
#include "Translations.h"
#include "Recovery.h"

class EngineMupdf;
extern Str gRecoveryCopyToLoad;
extern Str gRecoveryCopyForFile;
static bool gRecoveryEnabled = false;
extern void (*gOnAnnotationsModified)(EngineMupdf*);

// write the copy this long after the last change (changes come in bursts)
constexpr uint kRecoveryWriteDelayMs = 1500;

static Vec<EngineBase*> gDirtyEngines;
static UINT_PTR gRecoveryTimer = 0;

static TempStr RecoveryDirTemp() {
    return GetPathInAppDataDirTemp(StrL("recovery"));
}

// the key for a PDF: a hash of its normalized, lower-cased path
static TempStr RecoveryKeyTemp(Str path) {
    TempStr norm = path::NormalizeTemp(path);
    TempStr lower = str::DupTemp(norm);
    str::ToLowerInPlace(lower);
    return fmt("%08x", (int)MurmurHash2(lower));
}

static TempStr CopyPathTemp(Str path) {
    return path::JoinTemp(RecoveryDirTemp(), fmt("%s.pdf", RecoveryKeyTemp(path)));
}

static TempStr InfoPathTemp(Str path) {
    return path::JoinTemp(RecoveryDirTemp(), fmt("%s.info", RecoveryKeyTemp(path)));
}

struct RecoveryInfo {
    Str filePath; // owned
    DWORD pid = 0;
    // the file's size and modification time when the copy was made
    Str fileStamp; // owned
    ~RecoveryInfo() {
        str::Free(filePath);
        str::Free(fileStamp);
    }
};

static TempStr FileStampTemp(Str path) {
    FILETIME ft = file::GetModificationTime(path);
    return fmt("%d %u %u", (int)file::GetSize(path), (uint)ft.dwHighDateTime, (uint)ft.dwLowDateTime);
}

// info file, one value per line: the PDF's path, the owning process id, the
// PDF's FileStampTemp when the copy was made
static bool ReadInfo(Str infoPath, RecoveryInfo& info) {
    Str data = file::ReadFile(infoPath);
    if (!data.s) {
        return false;
    }
    StrVec lines;
    Split(&lines, data, StrL("\n"));
    str::Free(data);
    if (len(lines) < 2 || len(lines[0]) == 0) {
        return false;
    }
    info.filePath = str::Dup(lines[0]);
    info.pid = (DWORD)atoi(CStrTemp(lines[1]));
    if (len(lines) > 2) {
        info.fileStamp = str::Dup(lines[2]);
    }
    return true;
}

static bool WriteInfo(Str path) {
    TempStr content = fmt("%s\n%d\n%s\n", path, (int)GetCurrentProcessId(), FileStampTemp(path));
    return file::WriteFile(InfoPathTemp(path), content);
}

// Another running SumatraPDF+ owns this entry: its document is still open
// there, nothing crashed.
static bool IsOwnedByLiveProcess(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    if (pid == GetCurrentProcessId()) {
        return true;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return false;
    }
    bool alive = false;
    DWORD code = 0;
    if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) {
        // a reused pid belongs to some other program
        WCHAR exe[MAX_PATH]{};
        DWORD n = dimof(exe);
        if (QueryFullProcessImageNameW(h, 0, exe, &n)) {
            alive = str::ContainsI(ToUtf8Temp(WStr(exe, (int)n)), StrL("SumatraPDF"));
        }
    }
    CloseHandle(h);
    return alive;
}

void RecoveryForgetFile(Str path) {
    if (len(path) == 0) {
        return;
    }
    TempStr copy = CopyPathTemp(path);
    if (file::Exists(copy)) {
        logf("recovery: dropping the recovery copy of '%s'\n", path);
    }
    file::Delete(copy);
    file::Delete(InfoPathTemp(path));
}

static bool IsEngineAlive(EngineBase* engine, WindowTab** tabOut) {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            DisplayModel* dm = tab->AsFixed();
            if (dm && dm->GetEngine() == engine) {
                *tabOut = tab;
                return true;
            }
        }
    }
    return false;
}

static void WriteRecoveryCopies() {
    for (EngineBase* engine : gDirtyEngines) {
        WindowTab* tab = nullptr;
        if (!IsEngineAlive(engine, &tab)) {
            continue; // closed meanwhile; closing saved or discarded
        }
        Str path = engine->FilePath();
        if (!EngineMupdfHasUnsavedAnnotations(engine)) {
            RecoveryForgetFile(path);
            continue;
        }
        dir::CreateAll(RecoveryDirTemp());
        bool ok = EngineMupdfSaveRecoveryCopy(engine, CopyPathTemp(path)) && WriteInfo(path);
        logf("recovery: wrote the recovery copy of '%s': %s\n", path, ok ? StrL("ok") : StrL("failed"));
    }
    gDirtyEngines.Reset();
}

static void CALLBACK RecoveryTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    gRecoveryTimer = 0;
    WriteRecoveryCopies();
}

static void RefreshUnsavedIndicators() {
    for (MainWindow* win : gWindows) {
        UpdateTopBarForWindow(win);
    }
}

static void OnAnnotationsModifiedUI(void* engine) {
    if (!gDirtyEngines.Contains((EngineBase*)engine)) {
        gDirtyEngines.Append((EngineBase*)engine);
    }
    if (gRecoveryTimer) {
        KillTimer(nullptr, gRecoveryTimer);
    }
    gRecoveryTimer = SetTimer(nullptr, 0, kRecoveryWriteDelayMs, RecoveryTimerProc);
    RefreshUnsavedIndicators();
}

// called by the engine on whatever thread changed an annotation (in practice
// the UI thread); everything else happens on the UI thread
static void OnAnnotationsModified(EngineMupdf* e) {
    auto fn = MkFunc0<void>(OnAnnotationsModifiedUI, (void*)e);
    uitask::Post(fn, "RecoveryAnnotationsModified");
}

void InitRecovery() {
    gRecoveryEnabled = true;
    gOnAnnotationsModified = OnAnnotationsModified;
}

enum class RestoreChoice {
    Restore,
    Discard,
    Later,
};

static RestoreChoice AskRestore(HWND hwndParent, Str mainInstr, Str content) {
    constexpr int kBtnRestore = 100;
    constexpr int kBtnDiscard = 101;
    TASKDIALOG_BUTTON buttons[3];
    Str s = _TRA("&Restore changes");
    buttons[0].nButtonID = kBtnRestore;
    buttons[0].pszButtonText = CWStrTemp(s);
    s = _TRA("&Discard them");
    buttons[1].nButtonID = kBtnDiscard;
    buttons[1].pszButtonText = CWStrTemp(s);
    s = _TRA("&Not now");
    buttons[2].nButtonID = IDCANCEL;
    buttons[2].pszButtonText = CWStrTemp(s);

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    DWORD flags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    if (trans::IsCurrLangRtl()) {
        flags |= TDF_RTL_LAYOUT;
    }
    cfg.dwFlags = (TASKDIALOG_FLAGS)flags;
    s = _TRA("Recover unsaved changes");
    cfg.pszWindowTitle = CWStrTemp(s);
    cfg.pszMainInstruction = CWStrTemp(mainInstr);
    cfg.pszContent = CWStrTemp(content);
    cfg.cButtons = dimof(buttons);
    cfg.pButtons = buttons;
    cfg.nDefaultButton = kBtnRestore;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.hwndParent = hwndParent;
    int pressed = 0;
    HRESULT hr = TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (hr != S_OK || pressed == IDCANCEL) {
        return RestoreChoice::Later;
    }
    return pressed == kBtnRestore ? RestoreChoice::Restore : RestoreChoice::Discard;
}

// set while the startup prompt opens the documents it was told to restore, so
// opening them doesn't ask a second time
static bool gRestoringWithoutAsking = false;

// Called on the UI thread before a document is loaded. If a crash left
// unsaved changes for it, asks whether to restore them and, if so, arranges
// for the engine to open the recovery copy in place of the file.
void RecoveryBeforeLoad(HWND hwndParent, Str path) {
    if (!gRecoveryEnabled || len(path) == 0) {
        return;
    }
    TempStr copy = CopyPathTemp(path);
    TempStr infoPath = InfoPathTemp(path);
    if (!file::Exists(copy) || !file::Exists(infoPath)) {
        return;
    }
    RecoveryInfo info;
    if (!ReadInfo(infoPath, info) || !path::IsSame(info.filePath, path)) {
        return; // a hash collision with another file: not ours to touch
    }
    if (IsOwnedByLiveProcess(info.pid)) {
        return; // open in a running instance (or reloading here): not a crash
    }
    RestoreChoice choice = RestoreChoice::Restore;
    if (!gRestoringWithoutAsking) {
        TempStr name = path::GetBaseNameTemp(path);
        TempStr instr = fmt(_TRA("Restore unsaved changes to '%s'?").s, name);
        Str content = _TRA(
            "SumatraPDF+ closed before the changes made to this PDF (drawings, highlights and other "
            "annotations) were saved. They were kept and can be put back now.");
        if (len(info.fileStamp) > 0 && !str::Eq(info.fileStamp, FileStampTemp(path))) {
            // something else changed the file since: restoring brings back the
            // version that was being edited, and saving it replaces the newer file
            content = str::JoinTemp(content, StrL("\n\n"),
                                    _TRA("The file has been changed since then. Restoring brings back the "
                                         "version you were editing; saving it would replace the newer file."));
        }
        choice = AskRestore(hwndParent, instr, content);
    }
    logf("recovery: '%s' has unsaved changes from a session that did not close; choice %d\n", path, (int)choice);
    if (choice == RestoreChoice::Discard) {
        RecoveryForgetFile(path);
        return;
    }
    if (choice == RestoreChoice::Later) {
        return;
    }
    // claim the entry so another instance doesn't offer it too
    WriteInfo(path);
    str::ReplaceWithCopy(&gRecoveryCopyToLoad, copy);
    str::ReplaceWithCopy(&gRecoveryCopyForFile, path);
}

// Entries left by a crashed or killed session whose PDFs still exist.
static void CollectOrphans(StrVec& paths) {
    TempStr pattern = path::JoinTemp(RecoveryDirTemp(), StrL("*.info"));
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(CWStrTemp(ToWStrTemp(pattern)), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        TempStr infoPath = path::JoinTemp(RecoveryDirTemp(), ToUtf8Temp(WStr(fd.cFileName)));
        RecoveryInfo info;
        if (!ReadInfo(infoPath, info) || IsOwnedByLiveProcess(info.pid)) {
            continue;
        }
        if (!file::Exists(info.filePath) || !file::Exists(CopyPathTemp(info.filePath))) {
            // the PDF is gone (or the copy is): nothing to restore
            file::Delete(infoPath);
            continue;
        }
        paths.Append(info.filePath);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// After startup: if the last session ended without saving changes, offer to
// open those documents with the changes restored.
void RecoveryOfferAtStartup(MainWindow* win) {
    if (!gRecoveryEnabled || !win) {
        return;
    }
    StrVec paths;
    CollectOrphans(paths);
    if (len(paths) == 0) {
        return;
    }
    str::Builder names;
    for (int i = 0; i < len(paths) && i < 5; i++) {
        names.Append(StrL("\n    "));
        names.Append(path::GetBaseNameTemp(paths[i]));
    }
    if (len(paths) > 5) {
        names.Append(fmt("\n    ... (%d more)", len(paths) - 5));
    }
    Str instr = len(paths) == 1 ? _TRA("Restore unsaved changes from last time?")
                                : _TRA("Restore unsaved changes to these PDFs from last time?");
    TempStr content = fmt("%s\n%s", _TRA("SumatraPDF+ closed before these changes were saved:"), ToStr(names));
    RestoreChoice choice = AskRestore(win->hwndFrame, instr, content);
    logf("recovery: offered %d document(s) at startup, choice %d\n", len(paths), (int)choice);
    if (choice == RestoreChoice::Later) {
        return;
    }
    for (Str path : paths) {
        if (choice == RestoreChoice::Discard) {
            RecoveryForgetFile(path);
            continue;
        }
        gRestoringWithoutAsking = true;
        LoadArgs args(path, win);
        args.activateExisting = true;
        StartLoadDocument(&args);
        gRestoringWithoutAsking = false;
    }
}

// A PDF that should be saved under a new name rather than in place: a copy
// the browser downloaded into the temp folder (it would be cleaned up, taking
// the changes with it) or a read-only file.
bool IsPathSaveAsOnly(Str path) {
    if (len(path) == 0) {
        return false;
    }
    DWORD attrs = file::GetAttributes(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        return true;
    }
    WCHAR tmp[MAX_PATH + 1]{};
    DWORD n = GetTempPathW(dimof(tmp), tmp);
    if (n == 0 || n > MAX_PATH) {
        return false;
    }
    TempStr tmpDir = path::NormalizeTemp(ToUtf8Temp(WStr(tmp, (int)n)));
    TempStr norm = path::NormalizeTemp(path);
    return str::StartsWithI(norm, tmpDir);
}
