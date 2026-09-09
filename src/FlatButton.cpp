#include "FlatButton.h"

#include <algorithm>

#include "HostContext.h"

namespace pe {

namespace {

int Scaled(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;
    return MulDiv(value, (int)dpi, 96);
}

void FillRectangle(HDC dc, const RECT& area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

void FrameRectangle(HDC dc, const RECT& area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &area, brush);
    DeleteObject(brush);
}

}

int FlatButtonWidth(HWND parent, HFONT font, const wchar_t* text) {
    HDC dc = GetDC(parent);
    HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    RECT box{0, 0, 0, 0};
    DrawTextW(dc, text, -1, &box, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    if (previous) SelectObject(dc, previous);
    ReleaseDC(parent, dc);
    const int width = box.right - box.left + Scaled(parent, 28);
    return std::max(width, Scaled(parent, 80));
}

HWND MakeFlatButton(HWND parent, const wchar_t* text, int id, HFONT font, bool defaulted) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    if (defaulted) style |= BS_DEFPUSHBUTTON;
    HWND button = CreateWindowExW(0, L"BUTTON", text, style, 0, 0, 10, 10, parent,
                                  (HMENU)(INT_PTR)id, ModuleInstance(), nullptr);
    if (button && font) SendMessageW(button, WM_SETFONT, (WPARAM)font, TRUE);
    return button;
}

void DrawFlatButton(const DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_BUTTON) return;
    HDC dc = item->hDC;
    const RECT area = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool hot = (item->itemState & ODS_HOTLIGHT) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const LONG_PTR style = GetWindowLongPtrW(item->hwndItem, GWL_STYLE);
    const bool primary = (style & BS_DEFPUSHBUTTON) == BS_DEFPUSHBUTTON;
    const bool accent = primary || (item->itemState & (ODS_DEFAULT | ODS_FOCUS)) != 0;

    COLORREF body = accent && !disabled ? RGB(0xE8, 0xF2, 0xFC) : GetSysColor(COLOR_BTNFACE);
    if (disabled) {
        body = RGB(0xF5, 0xF5, 0xF5);
    } else if (pressed) {
        body = RGB(0xCC, 0xE4, 0xF7);
    } else if (hot) {
        body = RGB(0xE5, 0xF1, 0xFB);
    }
    FillRectangle(dc, area, body);

    COLORREF border = RGB(0xAD, 0xAD, 0xAD);
    if (disabled) {
        border = RGB(0xD0, 0xD0, 0xD0);
    } else if (pressed) {
        border = RGB(0x00, 0x5A, 0x9E);
    } else if (hot || accent) {
        border = RGB(0x00, 0x78, 0xD4);
    }
    FrameRectangle(dc, area, border);

    wchar_t text[256]{};
    GetWindowTextW(item->hwndItem, text, 256);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_BTNTEXT));
    RECT text_area = area;
    DrawTextW(dc, text, -1, &text_area,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

}
