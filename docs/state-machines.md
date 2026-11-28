# State machines in SumatraPDF+

Much of the fork's UI is a state machine that nobody named: a lifecycle spread
over a few bools, nullable pointers, `-1` sentinels and timers, set in one
function and checked in five others. That shape is where most of the fork's
"works until you do X in the middle of Y" bugs have come from. This file is the
map: what is already explicit, what is still implicit, and the rules for
touching either.

Scope is the Windows fork's own code and the upstream mechanisms it extends.
Compare the current worktree against `f64e7d41a77b` under `src/` when updating
this map. This is a source audit, not a claim that all event orders have been
reproduced. Independent preferences, derived layout flags, and straight-line
parsers are not separate machines. Engine internals, third-party code, and the
separate mobile reader are outside this inventory.

## Rules

- **One field per mutually exclusive phase.** Use an `enum class` with a
  comment that draws the transitions (`from --event--> to`) when it rules out
  invalid combinations. Phase-specific data can stay next to it. Do not fold
  independent facts (such as usable cached results and an active refresh) into
  one enum, or replace a well-owned two-state boolean just for consistency.
- **Transitions go through one function** when entering a phase has to reset
  something (`KsEnter` in `KineticScroll.cpp`). That is what stops stale
  leftovers such as "the ease finished but a fling is still flagged".
- **Derived state is computed, never stored twice.** If what is on screen
  follows from two remembered choices, write one function that answers it
  (`LibraryShownPane`) and make every reader call it.
- **Remember things by identity, not by pointer.** Anything that outlives a
  message (posted tasks, history entries) holds a serial (`WindowTab::serial`,
  `TbTab::serial`) and looks it up again. A freed pointer can be reused by the
  next allocation, and a `Find(ptr)` check then passes for the wrong object.
- **Every gesture has a reset**, called on capture loss (`WM_CAPTURECHANGED`,
  `WM_POINTERCAPTURECHANGED`) and on anything that changes what the gesture
  was acting on (mode or view switch). See `ResetTouchPanelGesture`.
- **Timer ids on one window are distinct**, enforced with a `static_assert`
  (see `TopBar.cpp`). A shared id makes the first `WM_TIMER` handler swallow
  the other's ticks.
- **Log transitions** that depend on async ordering (`logf("tb: hand-off exit
%s ...")`). `-log-to-file <path>` together with `-for-testing` turns a GUI
  repro into a readable trace.
- **Test the machine, not the pixels**, whenever it can be lifted out of the
  window code. `KineticScroll_ut.cpp` drives every transition with a fake clock
  (`KsSetClockForTesting`), which a screenshot harness cannot do.

## Explicit (done)

| Machine                       | Where                                   | States                                              |
| ----------------------------- | --------------------------------------- | --------------------------------------------------- |
| Kinetic scroll                | `KineticScroll.h` `KsPhase`             | Idle, Easing, Dragging, Flinging. Unit-tested.      |
| Browser tab load              | `SimpleBrowserWindow.cpp` `TbTab::Load` | Idle, Loading, HandedOff                            |
| Browser document hand-off     | `TbHandoffExit`, `TbPickHandoffExit`    | BackAfterLoad, GoBack, CloseTab, GoHome             |
| Browser load progress bar     | `TbProgState`                           | already explicit                                    |
| Library content pane          | `HomePage.cpp` `LibraryShownPane`       | Recent, Folder, SearchFiles (derived)               |
| Document tab load             | `WindowTab::LoadState`                  | explicit load phase; activation is separate         |
| Touch annotations, panel mode | `TouchAnnotsState`, `TouchPanelMode`    | already explicit                                    |
| Update check / download       | `UpdateCheck.cpp` `UpdatePhase`         | Idle, Checking, Downloading                         |
| Shared WebView environment    | `WebView.cpp` `SharedWebViewEnvState`   | explicit phase; retry timer and waiters are payload |

### Partly done

- **Which surface is showing** (`Rail.cpp` `SetTouchView`): a switch now checks
  that the target can be shown before leaving the old view, and rolls back
  (browser shown again) if selecting the target tab still fails. "Is there a
  document" has one answer, `HasTouchDocumentTab` (`Tabs.cpp`), used by the
  rail, the caption Back/Forward buttons and close-last-document. The view and
  panel choices are already enums; the implicit part is the transition across
  tab activation, browser visibility, and panel restoration.
- **Top-bar saved-page tray** (`TopBar.cpp`): a press remembers its page as
  well as its index and re-finds it before acting (`SavedPressIdx`); a rename
  remembers its file (`savedRenamePath`). Still a dozen loose fields, not one
  `TrayGesture` enum.
- **Document preview popup** (`TopBar.cpp`): the top bar owns the single hover
  delay for both switchers, the bar's own switcher sets an anchor, and `Hide()`
  clears `pointerId`. Still no `PreviewState` enum.

### Browser document hand-off, in full

A PDF can only be recognised by its response, which arrives **before** its
navigation commits. At that moment the link's page is still the current
history entry.

- Going Back then overshot to the page _before_ the link's. That was the bug.
- Cancelling the load (`Stop`) kept the right entry, but Chromium had already
  dropped that page's drawing, leaving it blank.

So the hand-off now:

1. switches the window to the document tab;
2. lets the load finish in the hidden browser tab;
3. steps Back once it has committed (`TbHandoffBackNow`).

`tab->url` is the address being loaded; `tab->committedUrl` is what is on
screen. Opening a URL whose document is still open in a tab switches to that
tab instead of saving another copy. That is also what makes the browser's own
Forward entry for the PDF lead back to it.

## Implicit and partially explicit inventory

Each row describes a transition owner or a distinct lifecycle within one owner.
The sequences are conceptual, not proposed enum declarations. A risk in this
table is a test target, not proof of a user-visible defect.

### Shell, Library, and controls

| Machine                              | Entry points / state                                                                                                                       | Transitions to protect                                                                                                                                                             |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Touch surface activation             | `Rail.cpp`: `SetTouchView`, `SetTouchPanelMode`, `SetTouchDocumentTab`; `SumatraPDF.cpp`: `LoadModelIntoTab`                               | Prepare target → activate → reconcile chrome; failed activation must leave the old surface usable.                                                                                 |
| Side-panel visibility                | `SetTouchSidebarCollapsed`, `touchSidebarCollapsed`, `uiState.tocVisible`, `WindowTab::showToc`, `touchSidebarOpen`                        | Collapse/open → leave Document → restore per-tab intent. Persisted default, tab intent, and actual visibility are different inputs, not necessarily duplicate bits.                |
| Place and Library navigation replay  | `navPlaceMoving`, `MainWindow::NavPlace`, `libraryNavReplaying`, `libraryNavPos`                                                           | Record → replay → resume recording. Keep replay from appending itself; a deleted tab is not a valid history target.                                                                |
| Legacy Home → Library entry          | `TouchView::Home` remaps; `homeSearchRestoring`, `homeSearchQuery`, `librarySearchQuery`                                                   | Restore → normalize view → show query without losing text entered during the handoff.                                                                                              |
| Library transient controls           | `CloseTouchLibraryTransientUi`, `libraryRowMenuPath`, `libraryManageFoldersOpen`, `librarySearchScopePickerOpen`                           | Open → action/dismiss → restore ordinary hit testing and focus; check switching surfaces while open.                                                                               |
| Library pointer gesture              | `HomePageOnPointerEvent`, `touchAboutPanArea`, `touchAboutPanAxis`, pointer IDs, hold timer, promoted-mouse suppression                    | Press → tap/hold/pan/pinch/resize → release or capture loss. Area 2 has a handler but no assignment in this entry point; verify all producers before deleting it.                  |
| Library splitter and shared splitter | `HomePageOnLibraryResizeMouse`, touch resize branch, `Splitter.cpp` `CancelSplitterDrag`                                                   | Armed → dragging → release/cancel. Test mouse and touch capture loss before sharing implementation.                                                                                |
| Library indexing                     | `StartTouchLibraryScan`, `TouchLibraryScanFinished`, `gTouchLibraryValid`, `gTouchLibraryScanning`, generation, disk-index attempt         | Load cached model → scan → publish or discard; root changes may invalidate the result while a worker is still running. Cached availability and worker activity are separate facts. |
| Library file thumbnails              | `TouchLibraryFileData::thumbnailRequested`, `TouchLibraryThumbnailFinished`, `CreateThumbnailFromFile`                                     | Unrequested → queued → ready/failed; current result matching is by path, and a null completion does not clear the request flag. Test replacement and retry before changing policy. |
| Library feedback and deferred reveal | `HomePageStartPinFlight`, `gPinFlyVal`, `libraryHighlightScrollPending`, Library feedback timer                                            | Animate or reveal → settle/cancel; switching windows, changing the model, or removing the target must not apply old feedback.                                                      |
| Saved-page tray                      | `TopBarWnd`, `savedTrayDragging`, `savedTrayDidDrag`, hold indexes/pages/timers, rename path                                               | Press → tap/hold/drag/rename → commit/cancel. Keep the identity fix separate from gesture phase cleanup.                                                                           |
| Document preview                     | `TopBarWnd::ShowPreview`, `PinPreview`, `TouchPreviewWnd::Hide`, preview timers/anchor, `PressStart/Move/End/Cancel`, thumbnail completion | Hover pending → shown → pinned or closing; separately press/reorder and deliver a thumbnail only to the intended card.                                                             |
| Night-light popup                    | `TouchNightLightWnd::WndProc`, `ScheduleApply`, `ApplyNow`, `Hide`                                                                         | Open → drag/defer apply → release or dismiss; preserve apply timing and release capture.                                                                                           |

### Reading and background work

| Machine                                   | Entry points / state                                                                                                                               | Transitions to protect                                                                                                                                                                              |
| ----------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Smart-margin analysis                     | `StartSmartMarginScan`, `InvalidateSmartMargins`, `SmartMarginScanFinishedUi`, `smartScanRunning`, `smartHfChecked`, generation                    | Queue → progress → apply/discard. Body-scan completion sets `smartHfChecked` only when it detected bands (fixed). Completion ownership uses a model pointer plus generation, not a lifetime serial. |
| Manual Trim dialog                        | `TrimDlgWnd`: `dragging`, `dragSample`, `TrimBand`, loupe/animation timer, `done`/`applied`                                                        | Suggest → adjust → apply/clear/cancel; capture loss ends the drag without committing the dialog.                                                                                                    |
| Canvas input                              | `OnPointerMessage`, `MouseAction`, `dragStartPending`, `textDragPending`, `imageDragPending`, `Selection.cpp`                                      | Press → click/pan/select/drag-drop → release/cancel. Action type and gesture phase are not the same thing.                                                                                          |
| Canvas scroll modes                       | `StartOrUpdateSmoothScrollY`, `StopSmoothScroll`, `ToggleAutoScroll`, `OnDocumentVerticalScrollIntent`                                             | Wheel easing, middle-click autoscroll, and end-of-document next-file intent each have separate start/stop rules. Do not merge them merely because they scroll.                                      |
| Side-panel input                          | `ResetTouchPanelGesture`, pointer ID, moved/coast flags, per-panel `KineticScroll`                                                                 | Press → coast catch/tap/drag → release; cancel on capture loss or panel/document change. Scroll physics are already explicit.                                                                       |
| Page thumbnail queue                      | `TouchThumbRenderFinishedUi`, `touchThumbnailDm`, `touchThumbQueue`, `touchThumbRendering`                                                         | Enqueue → render → deliver or discard → pump; old completion must neither publish to nor stall the new document.                                                                                    |
| Find request and result count             | `touchSearchPending`, `findDebouncePending`, `findThread`, `findCountThread`, `findCountEpoch`, pending query and count cache                      | Defer until loaded → debounce → first match → count/partial results → ready or superseded. Interactive find and counting can overlap; keep distinct ownership and query identity.                   |
| Word suggestion index                     | `TouchWordIndexFinishedUi`, `touchWordIndexBuilding`, document/index state                                                                         | Request → build → publish/discard; exercise switching documents during the build.                                                                                                                   |
| Find chrome                               | `ShowFindBar`, `HideFindBarForDocumentChange`, `ToggleFloatingFindUI`, resize handling in `FindBarWnd::WndProc`                                    | Choose presentation → show/focus → resize/hide/recreate. Do not conflate presentation with the search workers.                                                                                      |
| Auto-reload                               | `ScheduleReloadTab`, `ReloadTab`, `AutoReloadFileStillChanging`, `AUTO_RELOAD_TIMER_ID`, `reloadOnFocus`, `ignoreNextAutoReload`                   | Watcher event → wait for writer / defer to focus / consume skip → reload. Test tab close and switching while a timer is armed.                                                                      |
| Render request lifetime                   | `RenderCache`: queued/current requests, `CancelRenderingBlocking`, `darkModeEpoch`; pending controller deletes                                     | Queue → render → publish/abort → dispose. Keep queue locking and cancellation ordering; this is not a UI-enum cleanup.                                                                              |
| Fullscreen, overlays, and deferred layout | `EnterFullScreen`, `ExitFullScreen`, `toolbarOverlayHidePending`, `selectionToolbarShowPending`, `deferDpiChromeRefresh`/`dpiChromeRefreshPending` | Save/restore chrome; show and hide transient overlays; coalesce layout while it is deferred. These are cooperating but independent lifecycles.                                                      |

### Browser, chat, and deployment

| Machine                                       | Entry points / state                                                                                                             | Transitions to protect                                                                                                                                                                                             |
| --------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| WebView control startup and queued operations | `WebviewWnd`: `initStarted`, `initFailed`, control pointer, `pendingOps`, `FlushPendingOps`                                      | Unstarted → awaiting environment/controller → ready/failed → teardown; distinguish the control phase from the shared environment.                                                                                  |
| Browser response takeover and download        | `TbDocTakeoverNow`, `TbDocDownload`, `TbDocDownloadFinish`, `isDocPage`, recent takeover URL/time                                | Detect → post takeover → download/reuse → open/fail. The posted takeover and close-tab requests carry `TbTab::serial` and look the tab up again (fixed); the download request itself still holds a window pointer. |
| Browser tab hold overlay                      | `TbSetHold`, `TbEndHold`, `holdTab`, hold timer                                                                                  | Keep previous tab visible → new tab paints or timeout → release; close/switch must cancel the right hold.                                                                                                          |
| Browser tab and favorites gestures            | Browser chrome/sidebar press, drag and target indexes; favorites editor `editClosing`                                            | Press → activate/reorder/edit → release/cancel; re-resolve the target if the list changes mid-gesture.                                                                                                             |
| AI chat request and panel replay              | `AIChatTabState::process`, provider/session routing in `FindAIChatUpdateTab`, `aiChatWebViewReady`, session-select/replay timers | Start → stream → stop/finish; separately init panel → replay → switch/close. Session identity alone is not necessarily request identity.                                                                           |
| Update restart handoff                        | `UpdateCheck.cpp`: `reopenOnce` SessionData marker; startup consumption                                                          | Save one-shot intent → launch installer/restart → consume once. Check failed launch and ordinary restart separately from download state.                                                                           |
| Installer execution and completion            | `gInstallStarted`, `gInstallFailed`, worker handle, `OnInstallationFinished`; uninstaller relaunch/self-delete                   | Start/elevate → run → succeed/fail → exit. Exercise separately from reader changes; leave deployment behavior untouched without an installer-specific regression.                                                  |

## Decision and rollout

Land this inventory without changing runtime behavior. Refactor one owned
transition at a time, only after a regression proves the current failure and
captures the behavior that must stay unchanged.

1. **First: Smart-margin scan → Trim suggestion.** _Fix landed, regression
   still owed._ `SmartMarginScanFinishedUi` now sets `smartHfChecked` and the
   bands only when the scan ran with `detectRunningBands`, so a body-only scan
   neither hides nor overwrites the Trim suggestion's own detection. The
   remaining work below is the test. Originally: the completion handler
   unconditionally set `smartHfChecked`, including a scan configured with
   `detectRunningBands == false`, and the Trim suggestion then skipped
   synchronous detection. Separate body-scan progress from band-result availability
   (not checked / checked, even if empty). A completion that did not check
   bands must not overwrite a result from a manual suggestion. Start with a
   deterministic event-order test, then extend `tests/touch-smart-margins.ts`
   with a fixture that has repeated bands. Keep lifetime-serial hardening in a
   separate patch unless the regression requires it.
2. **Next: async target ownership.** Browser takeover and posted close-tab now
   resolve their tab by serial (`TbTabIdxBySerial`); `bun tests/tmp/nav-places.ts`
   (link mode) still passes. Cover stale completion after close/reopen for
   Smart margins, then audit auto-reload, thumbnails, and chat. Prefer a stable lifetime identity plus per-request generation.
   Preserve the existing worker and timer cadence. Trace event, owner serial,
   generation, and outcome, not document paths, URLs, or query text.
3. **Then: surface and gesture boundaries.** Characterize tab-close, failed
   activation, capture loss, and view-switch behavior with the existing touch
   tests before consolidating writers. Keep preview/tray/splitter refactors
   independent. Leave low-risk two-state booleans alone when one owner already
   controls entry and exit.

Acceptance for each patch: the user-visible failure is covered (UX), one
owner decides the transition and invariants (DX), and a named regression plus
an unambiguous trace or state assertion explains it without a screenshot-only
repro (AX). Do not run a full suite as a substitute for the targeted check.

## Testing

- Smart margins: `bun tests/touch-smart-margins.ts` covers opening and closing
  the gap without changing scale. It does not cover the Trim suggestion race.
- Touch shell: `bun tests/touch-pane-no-toc.ts`,
  `bun tests/touch-sidebar-pref.ts`, and `bun tests/touch-favorites-open.ts`.
- Other starting points: `bun tests/touch-browser-download.ts`,
  `bun tests/find-results-sorted.ts`, and `bun tests/reload-debounce.ts`.
- `bun tests/tmp/nav-places.ts` exercises the browser hand-off. It is ad hoc,
  so set its env vars (see the script). With `-for-testing`, browser downloads
  go to `<appdata>/Downloads`, never the user's.
- In Git Bash, set `MSYS2_ENV_CONV_EXCL=LINK` before passing a URL path such
  as `LINK=/dl`. Otherwise MSYS rewrites it into a Windows path.
- Unit tests: build `vs2022/test_util.vcxproj` (Release x64), then run
  `out/rel64/test_util.exe -for-ai`.
