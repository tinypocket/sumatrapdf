// Members-only documents opened from the in-app browser.
//
// When a page turns out to be a document, the browser hands it to its own
// downloader with the webview's cookies. WinINet replaced those cookies with
// its own store's, so a download that needed the user's sign-in came back as
// the site's sign-in page - saved as "document.pdf" and opened as a broken
// document. A local site stands in for the real one: it sets an HttpOnly
// session cookie, serves the file only to requests that carry it and sends the
// rest to a sign-in page, and keeps the file name in the query the way such
// sites do.
//
// Checked: the download carries the cookie and opens under the file's real
// name, from a link in the same tab and from a target=_blank link (whose tab is
// closed again); and when the site still answers with a web page, nothing is
// opened and nothing is left in Downloads.

import { launchSumatra, waitForFrame, clickAt, pressEnter } from "./win-automation.ts";
import {
  enumChildWindows,
  getClassName,
  getWindowRect,
  getWindowText,
  isWindowVisible,
  sendMessage,
  sendText,
  setProcessDpiAware,
  showWindow,
  sleep,
  WM_CLOSE,
} from "./winapi.ts";
import { runStandalone } from "./util.ts";
import { findTouchWindows, makeAppdata, makePdf, waitFor } from "./touch-util.ts";
import { rmSync } from "fs";
import { join } from "path";

const kFileName = "Touch-Browser-Download-Test.pdf";

type Mode = "same-tab" | "new-tab" | "refused";
type Seen = { path: string; cookie: boolean; sumatra: boolean };

async function run(mode: Mode): Promise<{ seen: Seen[]; title: string; tabs: number }> {
  const pdf = makePdf(2, false, "Protected");
  const seen: Seen[] = [];
  const server = Bun.serve({
    port: 0,
    // loopback only: listening on every interface raises a firewall prompt
    hostname: "127.0.0.1",
    fetch(req) {
      const u = new URL(req.url);
      const cookie = /(^|;\s*)member=yes/.test(req.headers.get("cookie") ?? "");
      const sumatra = (req.headers.get("user-agent") ?? "").startsWith("SumatraPdf");
      seen.push({ path: u.pathname + u.search, cookie, sumatra });
      if (u.pathname === "/start") {
        const href = `/?download_protected=${kFileName}`;
        const go =
          mode === "new-tab"
            ? `<a id=l target=_blank href="${href}">pdf</a><script>setTimeout(()=>document.getElementById('l').click(),600)</script>`
            : `<a id=l href="${href}">pdf</a><script>setTimeout(()=>document.getElementById('l').click(),600)</script>`;
        return new Response(`<html><head><title>Start</title></head><body>${go}</body></html>`, {
          headers: { "content-type": "text/html", "set-cookie": "member=yes; Path=/; HttpOnly" },
        });
      }
      if (u.searchParams.has("download_protected")) {
        // "refused": the browser still gets the file, the app's download does not
        if (!cookie || (mode === "refused" && sumatra)) {
          return new Response(null, { status: 302, headers: { location: "/my-account/" } });
        }
        return new Response(pdf, { headers: { "content-type": "application/pdf" } });
      }
      if (u.pathname === "/my-account/") {
        return new Response("<!DOCTYPE html><html><head><title>My Account</title></head><body>Log In</body></html>", {
          headers: { "content-type": "text/html" },
        });
      }
      return new Response("not found", { status: 404 });
    },
  });

  const proc = launchSumatra(["-appdata", makeAppdata(`touch-browser-download-${mode}`, { sidebarOpen: false })]);
  try {
    const frame = await waitForFrame(proc.pid, 30000);
    if (!frame) {
      throw new Error("no main window");
    }
    showWindow(frame, 3);
    const rail = await waitFor("touch rail", () => findTouchWindows(frame).rail);
    await sleep(2000);
    // the browser: the rail's bottom group, one up from the document switcher
    const rr = getWindowRect(rail);
    const dx = rr.right - rr.left;
    const s = dx / 64;
    const webY = Math.round(rr.bottom - rr.top - 10 * s - 52 * s - 58 * s + 26 * s);
    await clickAt(rail, Math.round(dx / 2), webY);
    const urlEdit = await waitFor(
      "address bar",
      () => {
        let h = 0;
        enumChildWindows(frame, (c) => {
          if (getClassName(c) === "Edit" && isWindowVisible(c)) {
            h = c;
          }
          return true;
        });
        return h;
      },
      20000,
    );
    await sleep(3000);
    sendText(urlEdit, `http://127.0.0.1:${server.port}/start`);
    await pressEnter(urlEdit);
    if (mode === "refused") {
      // the file never opens; give the download time to be turned away
      await waitFor("the app's download", () => seen.some((x) => x.sumatra && x.path === "/my-account/"), 20000);
      await sleep(1500);
    } else {
      await waitFor("the document", () => getWindowText(frame).includes(kFileName), 20000);
    }
    const title = getWindowText(frame);
    // back to the browser, to count what its tab strip was left with
    await clickAt(rail, Math.round(dx / 2), webY);
    await sleep(1500);
    let tabs = 0;
    enumChildWindows(frame, (c) => {
      const r = getWindowRect(c);
      if (getClassName(c) === "Chrome_WidgetWin_0" && r.right - r.left > 100) {
        tabs++;
      }
      return true;
    });
    return { seen, title, tabs };
  } finally {
    server.stop(true);
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

function downloadsDir(): string {
  return join(process.env.USERPROFILE ?? "", "Downloads");
}

function savedCopies(): string[] {
  return [...new Bun.Glob(kFileName.replace(".pdf", "*.pdf")).scanSync(downloadsDir())];
}

function clearDownloads() {
  // only the test's own uniquely named file: Downloads is the user's folder
  for (const f of savedCopies()) {
    rmSync(join(downloadsDir(), f), { force: true });
  }
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  for (const mode of ["same-tab", "new-tab"] as Mode[]) {
    clearDownloads();
    const r = await run(mode);
    const dl = r.seen.find((x) => x.sumatra && x.path.includes("download_protected"));
    if (!dl) {
      throw new Error(`${mode}: the app never downloaded the document (server saw ${JSON.stringify(r.seen)})`);
    }
    if (!dl.cookie) {
      throw new Error(`${mode}: the app's download went out without the site's cookie`);
    }
    if (!r.title.includes(kFileName)) {
      throw new Error(`${mode}: expected the document to open as ${kFileName}, the window says '${r.title}'`);
    }
    if (mode === "new-tab" && r.tabs > 1) {
      throw new Error(`new-tab: the tab opened for the document was left behind (${r.tabs} tabs)`);
    }
  }
  clearDownloads();
  const refused = await run("refused");
  if (refused.title.includes(".pdf")) {
    throw new Error(`refused: the site's sign-in page was opened as a document: '${refused.title}'`);
  }
  if (savedCopies().length > 0) {
    throw new Error(`refused: a web page was left in Downloads as ${savedCopies().join(", ")}`);
  }
  clearDownloads();
}

if (import.meta.main) {
  await runStandalone(testit);
}
