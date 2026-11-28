/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct LabelWithCloseWnd : Wnd {
    struct CreateArgs {
        HWND parent = nullptr;
        HFONT font = nullptr;
        int cmdId = 0;
        bool isRtl = false;
    };

    LabelWithCloseWnd() = default;
    ~LabelWithCloseWnd() override = default;

    HWND Create(const CreateArgs&);

    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

    void SetLabel(Str label);
    void SetFont(HFONT);
    void SetPaddingXY(int x, int y);
    void Layout();

    Size GetIdealSize();

    int cmdId = 0;
    // when false, no close button is drawn or hit-tested (the panel is toggled
    // elsewhere, e.g. the icon rail)
    bool showClose = true;

    Rect closeBtnPos{};

    // in points
    int padX = 0;
    int padY = 0;
};
