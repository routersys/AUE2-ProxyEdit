#pragma once

#include <windows.h>

namespace pe {

int FlatButtonWidth(HWND parent, HFONT font, const wchar_t* text);

HWND MakeFlatButton(HWND parent, const wchar_t* text, int id, HFONT font, bool defaulted);

void DrawFlatButton(const DRAWITEMSTRUCT* item);

}
