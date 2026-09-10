#include "FlatButton.h"

#include <commctrl.h>

#include <algorithm>

#include "HostContext.h"

namespace pe {

namespace {

const wchar_t* kProp = L"ProxyEditFlatButton";
const UINT_PTR kSubclassId = 0x50450004;

struct State {
    bool primary = false;
    bool hot = false;
};

State* StateOf(HWND button) {
    return (State*)GetPropW(button, kProp);
}

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

LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM first, LPARAM second, UINT_PTR,
                            DWORD_PTR) {
    State* state = StateOf(window);
    switch (message) {
        case WM_MOUSEMOVE:
            if (state && !state->hot) {
                state->hot = true;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (state && state->hot) {
                state->hot = false;
                InvalidateRect(window, nullptr, FALSE);
            }
            break;
        case WM_NCDESTROY: {
            State* dying = (State*)RemovePropW(window, kProp);
            RemoveWindowSubclass(window, ButtonProc, kSubclassId);
            delete dying;
            break;
        }
        default:
            break;
    }
    return DefSubclassProc(window, message, first, second);
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
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                  parent, (HMENU)(INT_PTR)id, ModuleInstance(), nullptr);
    if (!button) return nullptr;
    if (font) SendMessageW(button, WM_SETFONT, (WPARAM)font, TRUE);
    State* state = new State();
    state->primary = defaulted;
    SetPropW(button, kProp, (HANDLE)state);
    SetWindowSubclass(button, ButtonProc, kSubclassId, 0);
    return button;
}

void DrawFlatButton(const DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_BUTTON) return;
    HDC dc = item->hDC;
    const RECT area = item->rcItem;
    const State* state = StateOf(item->hwndItem);
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;
    const bool hot = state && state->hot;
    const bool primary = state && state->primary;

    COLORREF body = primary && !disabled ? RGB(0xE8, 0xF2, 0xFC) : GetSysColor(COLOR_BTNFACE);
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
    } else if (hot || primary || focused) {
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
