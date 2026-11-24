/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

void InitRecovery();
void RecoveryForgetFile(Str path);
void RecoveryBeforeLoad(HWND hwndParent, Str path);
void RecoveryOfferAtStartup(MainWindow*);
bool IsPathSaveAsOnly(Str path);
