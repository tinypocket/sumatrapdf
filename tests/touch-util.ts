// Shared helpers for the touch-chrome tests (tests/touch-*.ts).
//
// They all need the same three things: a document that provably has no
// bookmarks, a settings directory of their own so the user's real settings are
// never touched, and the window geometry of the touch chrome's own children.

import { mkdirSync, rmSync, writeFileSync } from "fs";
import { join } from "path";
import { tmpPath } from "./util.ts";
import { enumChildWindows, getClassName, getWindowRect, isWindowVisible } from "./winapi.ts";

// A minimal PDF, with or without an outline. No fixture in tests/ has
// bookmarks, and the touch pane's behavior differs precisely on whether a
// document has them, so both shapes are generated here.
export function makePdf(nPages: number, withToc: boolean, label = "Page"): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const body: Record<number, Buffer> = {};
  // outline objects live past the page objects; see maxN below
  const nPageObjs = 3 + nPages * 2;
  const outlinesRoot = nPageObjs + 1;
  const item1 = nPageObjs + 2;
  const item2 = nPageObjs + 3;
  body[1] = withToc
    ? enc(`<< /Type /Catalog /Pages 2 0 R /Outlines ${outlinesRoot} 0 R /PageMode /UseOutlines >>`)
    : enc("<< /Type /Catalog /Pages 2 0 R >>");
  const kids: string[] = [];
  for (let i = 0; i < nPages; i++) {
    kids.push(`${4 + i * 2} 0 R`);
  }
  body[2] = enc(`<< /Type /Pages /Kids [${kids.join(" ")}] /Count ${nPages} >>`);
  body[3] = enc("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  for (let i = 0; i < nPages; i++) {
    const po = 4 + i * 2;
    const co = po + 1;
    const stream = `BT /F1 24 Tf 72 720 Td (${label} ${i + 1}) Tj ET`;
    body[po] = enc(
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] ` +
        `/Resources << /Font << /F1 3 0 R >> >> /Contents ${co} 0 R >>`,
    );
    body[co] = enc(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
  }
  if (withToc) {
    const secondPage = nPages > 1 ? 6 : 4; // object number of page 2, else page 1
    body[outlinesRoot] = enc(`<< /Type /Outlines /First ${item1} 0 R /Last ${item2} 0 R /Count 2 >>`);
    body[item1] = enc(`<< /Title (First section) /Parent ${outlinesRoot} 0 R /Next ${item2} 0 R /Dest [4 0 R /Fit] >>`);
    body[item2] = enc(
      `<< /Title (Second section) /Parent ${outlinesRoot} 0 R /Prev ${item1} 0 R /Dest [${secondPage} 0 R /Fit] >>`,
    );
  }
  const maxN = withToc ? item2 : nPageObjs;
  const parts: Buffer[] = [enc("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")];
  const offsets: Record<number, number> = {};
  let pos = parts[0].length;
  for (let n = 1; n <= maxN; n++) {
    offsets[n] = pos;
    const obj = Buffer.concat([enc(`${n} 0 obj\n`), body[n], enc("\nendobj\n")]);
    parts.push(obj);
    pos += obj.length;
  }
  let xref = `xref\n0 ${maxN + 1}\n0000000000 65535 f \n`;
  for (let n = 1; n <= maxN; n++) {
    xref += `${String(offsets[n]).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${maxN + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  return Buffer.concat(parts);
}

function writePdf(name: string, nPages: number, withToc: boolean): string {
  const dir = tmpPath("touch-data");
  mkdirSync(dir, { recursive: true });
  const path = join(dir, name);
  writeFileSync(path, makePdf(nPages, withToc));
  return path;
}

export function writePdfNoToc(name: string, nPages = 3): string {
  return writePdf(name, nPages, false);
}

export function writePdfWithToc(name: string, nPages = 3): string {
  return writePdf(name, nPages, true);
}

// a saved page in a document, as the Favorites panel lists it
export type TouchFavorite = {
  path: string;
  pageNo: number;
  name: string;
};

export type TouchPrefs = {
  sidebarOpen?: boolean;
  twoRowTabs?: boolean;
  largerTabs?: boolean;
  libraryFolders?: string[];
  favorites?: TouchFavorite[];
};

// A settings directory of this test's own, so a run never reads or writes the
// settings of whoever is running it. Pass it to the app as -appdata <dir>.
export function makeAppdata(name: string, prefs: TouchPrefs = {}): string {
  const dir = tmpPath(name);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const lines = [
    "TouchChrome = true",
    // maximized, so every run lays the chrome out the same way: the tab strip
    // is a window of its own only in this state, and the geometry the tests
    // measure does not depend on where the window happened to land
    "WindowState = 2",
    "RestoreSession = false",
    "CheckForUpdates = false",
    "ShowStartPage = true",
    `TouchSidebarOpen = ${prefs.sidebarOpen ? "true" : "false"}`,
    `TwoRowTabs = ${prefs.twoRowTabs ? "true" : "false"}`,
    `LargerTabs = ${prefs.largerTabs ? "true" : "false"}`,
  ];
  if (prefs.libraryFolders && prefs.libraryFolders.length > 0) {
    lines.push(`LibraryFolders = ${prefs.libraryFolders.map((f) => `"${f}"`).join(" ")}`);
  }
  if (prefs.favorites && prefs.favorites.length > 0) {
    // favorites live in the per-file state, one block per document
    lines.push("FavoritesInToolbar = true");
    lines.push("FileStates [");
    for (const fav of prefs.favorites) {
      lines.push(
        "\t[",
        `\t\tFilePath = ${fav.path}`,
        "\t\tFavorites [",
        "\t\t\t[",
        `\t\t\t\tName = ${fav.name}`,
        `\t\t\t\tPageNo = ${fav.pageNo}`,
        "\t\t\t]",
        "\t\t]",
        "\t\tIsMissing = false",
        "\t\tOpenCount = 3",
        "\t]",
      );
    }
    lines.push("]");
  }
  lines.push("");
  writeFileSync(join(dir, "SumatraPDF-settings.txt"), lines.join("\n"));
  return dir;
}

export type TouchWindows = {
  frame: number;
  rail: number;
  panel: number;
  canvas: number;
};

// The touch chrome's own children, found by shape rather than by a fixed
// position: the rail is the narrow full-height strip on the left edge and the
// side pane the wide one beside it.
export function findTouchWindows(frame: number): TouchWindows {
  const fr = getWindowRect(frame);
  const out: TouchWindows = { frame, rail: 0, panel: 0, canvas: 0 };
  enumChildWindows(frame, (h) => {
    if (!isWindowVisible(h)) {
      return true;
    }
    const r = getWindowRect(h);
    const dx = r.right - r.left;
    const dy = r.bottom - r.top;
    const cls = getClassName(h);
    const left = r.left - fr.left;
    if (cls === "SumatraWgDefaultWinClass" && left < 40 && dx >= 50 && dx <= 140 && dy > 300) {
      out.rail = h;
    }
    if (cls === "Static" && dx > 250 && dx < 1400 && dy > 300) {
      out.panel = h;
    }
    if (cls === "SUMATRA_PDF_CANVAS") {
      out.canvas = h;
    }
    return true;
  });
  return out;
}

export async function waitFor<T>(what: string, fn: () => T | 0 | undefined, timeoutMs = 15000): Promise<T> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = fn();
    if (v) {
      return v as T;
    }
    if (Date.now() > deadline) {
      throw new Error(`timed out waiting for ${what}`);
    }
    await new Promise((r) => setTimeout(r, 250));
  }
}
