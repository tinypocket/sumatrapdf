/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

// "Trim headers & footers": pick how much comes off the top and bottom of every
// page by pointing at a few sample pages. Unlike the automatic Smart header &
// footer this needs nothing readable on the page, so it works on scans.
// Returns true if the trim was applied.
bool ShowTrimHeaderFooterDialog(MainWindow*);
