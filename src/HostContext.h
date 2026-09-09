#pragma once

#include <windows.h>

#include <cstdint>

#include "plugin2.h"
#include "config2.h"
#include "cache2.h"

namespace pe {

void SetEditHandle(EDIT_HANDLE* handle);
void SetConfigHandle(CONFIG_HANDLE* handle);
void SetCacheHandle(CACHE_HANDLE* handle);
void SetHostWindow(HWND window);
void SetModuleInstance(HINSTANCE instance);

EDIT_HANDLE* Edit();
CONFIG_HANDLE* Config();
CACHE_HANDLE* Cache();
HWND HostWindow();
HINSTANCE ModuleInstance();

EDIT_INFO EditInfo();
int EditState();
bool Exporting();

int LayoutSize(const char* key);
COLORREF ThemeColor(const char* key, COLORREF fallback);
FONT_INFO* HostFont(const char* key);
const wchar_t* Translate(const wchar_t* text);
const wchar_t* LanguageText(const wchar_t* section, const wchar_t* text);

bool CallEditSection(void* param, void (*proc)(void* param, EDIT_SECTION* edit));
bool CallReadSection(void* param, void (*proc)(void* param, EDIT_SECTION* edit));

}
