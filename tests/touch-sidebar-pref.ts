// The side pane opens because the user opened it, not because a document has
// bookmarks.
//
// It used to be decided per document: a file with an outline popped the pane
// open, one without closed it, and the state was saved per file. It is now one
// setting (TouchSidebarOpen) that applies to every document. This checks both
// halves: off means no pane on a document that does have bookmarks, on means a
// pane on a document that has none.

import { launchSumatra, waitForFrame } from "./win-automation.ts";
import { getWindowRect, sendMessage, setProcessDpiAware, sleep, WM_CLOSE } from "./winapi.ts";
import { runStandalone } from "./util.ts";
import { findTouchWindows, makeAppdata, waitFor, writePdfNoToc, writePdfWithToc } from "./touch-util.ts";

async function paneVisibleFor(appdata: string, pdf: string): Promise<boolean> {
  const proc = launchSumatra(["-appdata", appdata, pdf]);
  try {
    const frame = await waitForFrame(proc.pid, 30000);
    if (!frame) {
      throw new Error("no main window");
    }
    // the rail is always there in the touch chrome; wait for it so the layout
    // has settled before deciding whether the panel beside it is up
    await waitFor("touch rail", () => findTouchWindows(frame).rail);
    await sleep(1500);
    const w = findTouchWindows(frame);
    const panel = w.panel;
    if (!panel) {
      return false;
    }
    const r = getWindowRect(panel);
    return r.right - r.left > 0 && r.bottom - r.top > 0;
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

export async function testit(): Promise<void> {
  setProcessDpiAware();
  // one document with bookmarks, one without: the pane must ignore both
  const withToc = writePdfWithToc("touch-sidebar-pref-with-toc.pdf", 3);
  const noToc = writePdfNoToc("touch-sidebar-pref-no-toc.pdf", 2);

  const closedAppdata = makeAppdata("touch-sidebar-pref-closed", { sidebarOpen: false });
  const openAppdata = makeAppdata("touch-sidebar-pref-open", { sidebarOpen: true });

  // a document with bookmarks must not open the pane by itself
  const bookmarksDidNotOpenIt = await paneVisibleFor(closedAppdata, withToc);
  if (bookmarksDidNotOpenIt) {
    throw new Error("pane opened on a document with bookmarks while TouchSidebarOpen = false");
  }

  // and the setting must hold it open on a document that has none
  const openOnPlainDoc = await paneVisibleFor(openAppdata, noToc);
  if (!openOnPlainDoc) {
    throw new Error("pane did not open on a document without bookmarks while TouchSidebarOpen = true");
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
