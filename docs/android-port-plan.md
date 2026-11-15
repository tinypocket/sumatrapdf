# Android version — plan (parked)

Status: **parked** until the Windows version (SumatraPDF+) is where we want it.
Next step when we pick this up: the prototype below.

## What it would be

Not a port: a new Android app with the same ideas. Nothing of the Windows UI
carries over — the rail, panes, painting, tabs and the in-app browser are all
Win32 / GDI / GDI+ / WebView2. What carries over:

- the designs and behaviour we've worked out (and tested on a real user), and
- optionally the PDF engine (MuPDF runs on Android; see licensing below).

Features are re-implemented, not copied. Ideas aren't copyrightable, so an app
written from scratch isn't bound by SumatraPDF's GPL.

## Engine choice (decides whether it can be sold as a normal app)

| Engine | Rendering / search | License | Selling |
|---|---|---|---|
| **PDFium** (Google's, in Chrome) | Good; text search fine | BSD-3 / Apache bindings | Closed source OK — sell normally |
| MuPDF (what SumatraPDF uses) | Best, handles odd files | AGPLv3 | Must publish source; others can rebuild and give it away (or buy an Artifex commercial license) |
| Android `PdfRenderer` (built in) | Basic; weak text/search on older Android | Platform API | No license concerns, but too limited |

**Leaning: PDFium.** Revisit if prototype rendering quality falls short on the
chant PDFs (Byzantine notation, big scanned books).

## Prototype (first step, ~1–2 weeks)

Goal: see how reading feels on a phone and a tablet before committing.

- Open a PDF (Storage Access Framework picker), vertical scroll, pinch zoom
- **Smart margins**: trim page margins, per-gap expand/collapse
  (Windows: `src/Canvas.cpp` MarginGap*, `src/DisplayModel.cpp` PageTrimmedBox /
  MarginExpansion)
- **Night light**: warm multiply, red kept, green ×(1 − 0.20·s), blue
  ×(1 − 0.55·s) — on Android a `ColorMatrix` on the page bitmaps
  (Windows: `WarmFactors` / `WarmColor` in `src/base/Pixmap_win.cpp`)
- Remember page / zoom per document

Stack to decide at start: Kotlin + Jetpack Compose (likely) vs Flutter.

## Later, towards parity (rough: +1–2 months, then more for the browser)

- Library: folders, recent, pinned (Windows: `src/HomePage.cpp`)
- Favorites across documents (`src/Favorites.cpp`)
- Bookmarks pane: per-document open/closed, compact when narrow
- Thumbnails pane
- Search with whole-word suggestions (word index: `src/TableOfContents.cpp`
  `TouchWordIndexThread` / `UpdateTouchSuggestions`)
- Open documents: tabs or a switcher, drag to reorder
- In-app browser with signed-in PDF downloads (cookies handed to the
  downloader) — Android `WebView` + `CookieManager`
- Updates (Play Store handles this)

## Android chores

- File access: Storage Access Framework / persisted URI permissions for library
  folders
- Phone vs tablet layouts, rotation
- Play Store: $25 one-time developer account, app signing, review
- Test devices: the Surface is Windows; need an Android tablet for the
  chant-reading case

## Naming / selling notes

- Own name and icon — don't use "Sumatra".
- With PDFium: normal commercial app, closed source allowed.
- With MuPDF: AGPL obligations as above.
- Get a quick legal check before charging money.

## Effort summary (me writing most code, you testing on a device)

| Stage | Rough time |
|---|---|
| Prototype (reading + smart margins + night light) | 1–2 weeks |
| Basic viewer (bookmarks, thumbnails, search, recents) | 2–4 weeks |
| Close to current Windows features | +1–2 months |
| Full parity incl. browser downloads | +several weeks |
