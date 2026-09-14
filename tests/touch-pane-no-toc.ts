// The side pane must survive documents that have no bookmarks.
//
// Regression: from the version that let the pane stay open on any document,
// opening one without an outline marked its bookmark tree "loaded" while the
// tree itself stayed null. Saving that tab's sidebar state then walked the null
// tree and read its root, and the app died with an access violation before its
// window ever appeared. Two such documents plus an open pane was enough.
//
// withControlledSumatra fails the test on any non-zero exit, so a crash or a
// ReportIf during the run or the shutdown is caught here without needing to
// assert anything about the UI itself.

import { ControlCommand, withControlledSumatra } from "../cmd/control.ts";
import { EXE, runStandalone } from "./util.ts";
import { makeAppdata, writePdfNoToc } from "./touch-util.ts";

export async function testit(): Promise<void> {
  const a = writePdfNoToc("touch-no-toc-a.pdf", 3);
  const b = writePdfNoToc("touch-no-toc-b.pdf", 2);

  // the pane open is the case that crashed; the pref is what holds it open
  const appdata = makeAppdata("touch-pane-no-toc-appdata", { sidebarOpen: true });
  await withControlledSumatra(
    EXE,
    async (client) => {
      // the app answering at all means it got past loading both documents with
      // the pane open, which is where it used to die
      await client.request(ControlCommand.Ping);
    },
    ["-appdata", appdata, a, b],
  );

  // and again with the pane closed, so the test still covers the plain path
  const appdata2 = makeAppdata("touch-pane-no-toc-appdata2", { sidebarOpen: false });
  await withControlledSumatra(
    EXE,
    async (client) => {
      await client.request(ControlCommand.Ping);
    },
    ["-appdata", appdata2, a, b],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
