// Opening a saved page from the Favorites panel, with nothing open to begin
// with - the Library is showing, the panel lists favorites from every document.
//
// Two things went wrong on that route and both are checked here:
//   - the document arrived with no toolbar. The toolbar is switched off over
//     the Library (it carries its own header) and nothing switched it back on
//     when a document took the right-hand side.
//   - going back to the Library afterwards crashed. The Favorites panel is
//     allowed to stay open with no document, and the bookmarks tree was loaded
//     for it anyway, through the tab's null controller.

import { launchSumatra, waitForFrame, clickAt } from "./win-automation.ts";
import {
  enumChildWindows,
  getClassName,
  getWindowRect,
  isWindowVisible,
  sendMessage,
  setProcessDpiAware,
  sleep,
  WM_CLOSE,
} from "./winapi.ts";
import { runStandalone } from "./util.ts";
import { findTouchWindows, makeAppdata, waitFor, writePdfNoToc, writePdfWithToc } from "./touch-util.ts";

// The rail's own metrics, from src/TouchMetrics.h. Everything in it is DPI
// scaled, so the test derives the scale from the rail's width rather than
// assuming the display it was written on.
const kRailDx = 64;
const kRailBtnDy = 52;
const kRailBtnGap = 6;
const kRailPadY = 10;
// the bottom group, laid out upwards from the bottom edge: Favorites, Library,
// the browser, the document switcher
const kFavoritesBelow = 3;
const kLibraryBelow = 2;
// the first favorite's row, below the panel header and the "Show in toolbar"
// strip, in unscaled pixels from the top of the panel
const kFirstFavoriteRowY = 173;

type RailMetrics = { scale: number; x: number; favoritesY: number; libraryY: number };

function railMetrics(railRect: { left: number; top: number; right: number; bottom: number }): RailMetrics {
  const dx = railRect.right - railRect.left;
  const dy = railRect.bottom - railRect.top;
  const scale = dx / kRailDx;
  const btnDy = kRailBtnDy * scale;
  const step = btnDy + kRailBtnGap * scale;
  const padY = kRailPadY * scale;
  // centre of the nth button up from the bottom
  const centerFromBottom = (nBelow: number) => dy - padY - btnDy - nBelow * step + btnDy / 2;
  return {
    scale,
    x: Math.round(dx / 2),
    favoritesY: Math.round(centerFromBottom(kFavoritesBelow)),
    libraryY: Math.round(centerFromBottom(kLibraryBelow)),
  };
}

// The document toolbar is the band directly above the canvas and as wide as
// it. The panel has a header of the same height beside it, so the canvas is
// what tells them apart.
function documentToolbar(frame: number): number {
  const canvas = findTouchWindows(frame).canvas;
  if (!canvas) {
    return 0;
  }
  const cr = getWindowRect(canvas);
  let found = 0;
  enumChildWindows(frame, (h) => {
    if (!isWindowVisible(h) || getClassName(h) !== "SumatraWgDefaultWinClass") {
      return true;
    }
    const r = getWindowRect(h);
    if (r.left === cr.left && r.right === cr.right && r.bottom === cr.top && r.bottom > r.top) {
      found = h;
    }
    return true;
  });
  return found;
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const withToc = writePdfWithToc("touch-fav-open-with-toc.pdf", 4);
  const noToc = writePdfNoToc("touch-fav-open-no-toc.pdf", 3);
  const appdata = makeAppdata("touch-fav-open", {
    sidebarOpen: true,
    favorites: [
      { path: withToc, pageNo: 3, name: "Mark in the first file" },
      { path: noToc, pageNo: 2, name: "Mark in the second file" },
    ],
  });

  // no document on the command line: the Library is what comes up
  const proc = launchSumatra(["-appdata", appdata]);
  try {
    const frame = await waitForFrame(proc.pid, 30000);
    if (!frame) {
      throw new Error("no main window");
    }
    const rail = await waitFor("touch rail", () => findTouchWindows(frame).rail);
    await sleep(1500);
    const m = railMetrics(getWindowRect(rail));

    await clickAt(rail, m.x, m.favoritesY);
    await sleep(1500);
    if (proc.exitCode !== null) {
      throw new Error("crashed opening the Favorites panel with no document");
    }
    const panel = await waitFor("favorites panel", () => findTouchWindows(frame).panel);

    await clickAt(panel, Math.round(200 * m.scale), Math.round(kFirstFavoriteRowY * m.scale));
    await sleep(2500);
    if (proc.exitCode !== null) {
      throw new Error("crashed opening a favorite");
    }
    // the tap opened a document, so the canvas is beside the panel and the
    // toolbar belongs above it
    const toolbar = await waitFor("document toolbar", () => documentToolbar(frame), 8000);
    const tr = getWindowRect(toolbar);
    if (tr.bottom - tr.top < 20) {
      throw new Error(`document toolbar has no height: ${JSON.stringify(tr)}`);
    }

    // and back to the Library, with the panel still open
    await clickAt(rail, m.x, m.libraryY);
    await sleep(2000);
    if (proc.exitCode !== null) {
      throw new Error("crashed going back to the Library with the Favorites panel open");
    }
    if (documentToolbar(frame)) {
      throw new Error("document toolbar is still up over the Library");
    }
  } finally {
    try {
      const frame = await waitForFrame(proc.pid, 2000);
      if (frame) {
        sendMessage(frame, WM_CLOSE, 0, 0);
      }
    } catch {
      // closing is best effort; the kill below is the backstop
    }
    await sleep(1500);
    try {
      proc.kill();
    } catch {
      // already gone
    }
    await proc.exited;
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
