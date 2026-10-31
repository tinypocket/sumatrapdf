// Smart margins: the tab in the gap between two pages.
//
// Smart margins trim the blank bands above and below each page's content, and
// a small tab in the gap between two pages gives them back. Two things went
// wrong there, and both are checked:
//   - the page above came back scaled up and clipped at the right. Its height
//     changed at an unchanged zoom, so the render cache still held its tiles
//     for the old crop and blitted them into the taller slot.
//   - only the page above opened. The gap is its footer AND the header of the
//     page below, and the header stayed cut.
// And tapping the gap again has to put both back exactly as they were.

import { launchSumatra, waitForFrame, clickAt } from "./win-automation.ts";
import {
  getWindowRect,
  readWindowDCColumn,
  readWindowDCPoints,
  readWindowDCRow,
  sendMessage,
  setProcessDpiAware,
  sleep,
  WM_CLOSE,
} from "./winapi.ts";
import { runStandalone, tmpPath } from "./util.ts";
import { findTouchWindows, makeAppdata, waitFor } from "./touch-util.ts";
import { appendFileSync, mkdirSync, writeFileSync } from "fs";
import { join } from "path";

const WHITE = 0xffffff;
const BLACK = 0x000000;

// Pages whose content - a 400pt black bar of known width and a line of text -
// sits in the middle, with wide blank bands above and below for smart margins
// to trim.
function makePdf(nPages: number): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const body: Record<number, Buffer> = {};
  const kids: string[] = [];
  for (let i = 0; i < nPages; i++) {
    kids.push(`${4 + i * 2} 0 R`);
  }
  body[1] = enc("<< /Type /Catalog /Pages 2 0 R >>");
  body[2] = enc(`<< /Type /Pages /Kids [${kids.join(" ")}] /Count ${nPages} >>`);
  body[3] = enc("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  for (let i = 0; i < nPages; i++) {
    const po = 4 + i * 2;
    const stream = `0 0 0 rg 106 300 400 20 re f BT /F1 24 Tf 150 520 Td (Page ${i + 1}) Tj ET`;
    body[po] = enc(
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] ` +
        `/Resources << /Font << /F1 3 0 R >> >> /Contents ${po + 1} 0 R >>`,
    );
    body[po + 1] = enc(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
  }
  const maxN = 3 + nPages * 2;
  const parts: Buffer[] = [enc("%PDF-1.7\n")];
  const offsets: number[] = [];
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

type Run = { y0: number; y1: number };
type View = { pages: Run[]; barDx: number; midX: number; topToBar: number[] };

// What is on screen, read off the canvas: where each page is (a column inside
// the page's left margin sees white page and the gap lines between pages), and
// how wide the first page's bar came out.
function readView(canvas: number): View {
  const r = getWindowRect(canvas);
  const w = r.right - r.left;
  const h = r.bottom - r.top;
  const midX = Math.floor(w / 2);
  const center = readWindowDCColumn(canvas, midX, 0, h);
  const barY = center.findIndex((c) => c === BLACK);
  let barDx = 0;
  if (barY >= 0) {
    barDx = readWindowDCRow(canvas, 0, barY + 2, w).filter((c) => c === BLACK).length;
  }
  // the page's left edge, from a row just above the bar (a row across the
  // middle of a tall screen can pass below the last page)
  const pages: Run[] = [];
  const edgeRow = barY > 4 ? readWindowDCRow(canvas, 0, barY - 4, w) : [];
  const pageLeft = edgeRow.findIndex((c) => c === WHITE);
  if (pageLeft >= 0) {
    const col = readWindowDCColumn(canvas, pageLeft + 30, 0, h);
    let start = -1;
    for (let y = 0; y <= h; y++) {
      const onPage = y < h && col[y] === WHITE;
      if (onPage && start < 0) {
        start = y;
      }
      if (!onPage && start >= 0) {
        if (y - start > 40) {
          pages.push({ y0: start, y1: y });
        }
        start = -1;
      }
    }
  }
  // each page's top edge to its bar, down the middle - or to the bottom of the
  // screen when the bar is below it, which a page whose header just opened on a
  // short screen can be: either way the distance grows when the header opens
  const topToBar = pages.map((p) => {
    const i = center.slice(p.y0).findIndex((c) => c === BLACK);
    return i >= 0 ? i : h - p.y0;
  });
  return { pages, barDx, midX, topToBar };
}

const pageDy = (v: View, i: number) => v.pages[i].y1 - v.pages[i].y0;

// Just the first bar's width, cheaply enough to sample several times a second:
// every third pixel down the middle to find it, then one row across it.
function quickBarDx(canvas: number): number {
  const r = getWindowRect(canvas);
  const w = r.right - r.left;
  const h = r.bottom - r.top;
  const midX = Math.floor(w / 2);
  const pts: { x: number; y: number }[] = [];
  for (let y = 0; y < h; y += 3) {
    pts.push({ x: midX, y });
  }
  const col = readWindowDCPoints(canvas, pts);
  const i = col.findIndex((c) => c === BLACK);
  if (i < 0) {
    return 0;
  }
  return readWindowDCRow(canvas, 0, pts[i].y + 1, w).filter((c) => c === BLACK).length;
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  // not the appdata's name: makeAppdata starts by wiping its directory
  const dir = tmpPath("touch-smart-margins-data");
  mkdirSync(dir, { recursive: true });
  const pdf = join(dir, "touch-smart-margins.pdf");
  writeFileSync(pdf, makePdf(4));
  const appdata = makeAppdata("touch-smart-margins", { sidebarOpen: false });
  // a fixed zoom: the stale tiles only came back when the zoom did not change,
  // and at 50% both pages stay in view with the gap between them opened, even
  // on a short landscape screen
  appendFileSync(
    join(appdata, "SumatraPDF-settings.txt"),
    ["SmartMargins = true", "DefaultZoom = 50", "DefaultDisplayMode = continuous", ""].join("\n"),
  );

  const proc = launchSumatra(["-appdata", appdata, pdf]);
  try {
    const frame = await waitForFrame(proc.pid, 30000);
    if (!frame) {
      throw new Error("no main window");
    }
    const canvas = await waitFor("canvas", () => findTouchWindows(frame).canvas);
    // the trim lands when the background margin scan does, then the page renders
    await waitFor("first page rendered", () => readView(canvas).barDx > 0, 20000);
    await sleep(1000);

    const before = readView(canvas);
    if (before.pages.length < 2) {
      throw new Error(`expected two pages in view, found ${JSON.stringify(before.pages)}`);
    }
    // the middle of the gap toggles as well as the tab at its right
    const gapY = Math.floor((before.pages[0].y1 + before.pages[1].y0) / 2);
    await clickAt(canvas, before.midX, gapY, 0);
    // The stale tiles showed only until the page re-rendered, which on a fast
    // machine is a fraction of a second - so watch it throughout. A bar not
    // drawn yet is fine; a bar drawn at any other width is the bug.
    const watchUntil = Date.now() + 2500;
    while (Date.now() < watchUntil) {
      const dx = quickBarDx(canvas);
      if (dx !== 0 && dx !== before.barDx) {
        throw new Error(`the page above was drawn rescaled when its footer opened: bar ${before.barDx}px -> ${dx}px`);
      }
    }

    const open = readView(canvas);
    if (open.pages.length < 2) {
      throw new Error(`after opening the gap, expected two pages in view, found ${JSON.stringify(open.pages)}`);
    }
    if (open.barDx !== before.barDx) {
      throw new Error(`the page above changed scale when its footer opened: bar ${before.barDx}px -> ${open.barDx}px`);
    }
    // Positions are measured within each page, from its top edge: a document
    // shorter than the window is centred in it, so a page growing moves them all.
    // The page above grows at the bottom, and only there: its top edge belongs
    // to the gap above it, which was not tapped.
    if (pageDy(open, 0) <= pageDy(before, 0)) {
      throw new Error(`the page above did not open at its footer: ${pageDy(before, 0)}px -> ${pageDy(open, 0)}px tall`);
    }
    if (open.topToBar[0] !== before.topToBar[0]) {
      throw new Error(`the page above opened at its header too: top to bar ${before.topToBar[0]}px -> ${open.topToBar[0]}px`);
    }
    // and the page below opens at its top, which pushes its content down
    if (open.topToBar[1] <= before.topToBar[1]) {
      throw new Error(
        `the page below did not open at its header: top to bar ${before.topToBar[1]}px -> ${open.topToBar[1]}px`,
      );
    }

    // and closing it puts both back
    const gapY2 = Math.floor((open.pages[0].y1 + open.pages[1].y0) / 2);
    await clickAt(canvas, open.midX, gapY2);
    await sleep(2500);
    const closed = readView(canvas);
    if (pageDy(closed, 0) !== pageDy(before, 0) || pageDy(closed, 1) !== pageDy(before, 1)) {
      throw new Error(
        `closing the gap did not restore the pages: ${pageDy(before, 0)},${pageDy(before, 1)} -> ` +
          `${pageDy(closed, 0)},${pageDy(closed, 1)}`,
      );
    }
    if (closed.barDx !== before.barDx) {
      throw new Error(`closing the gap changed the page's scale: bar ${before.barDx}px -> ${closed.barDx}px`);
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
