#include "ExportGuard.h"

#include <commctrl.h>

#include <set>
#include <string>

#include "HostContext.h"
#include "Log.h"
#include "ScanController.h"

namespace pe {

namespace {

const UINT_PTR kSubclassId = 0x50450003;

HWND g_host = nullptr;
bool g_hooked = false;
std::set<int> g_output_commands;
bool g_collected = false;
bool g_restored = false;
bool g_in_command = false;

std::wstring WithoutMarker(const wchar_t* text) {
    std::wstring result;
    for (const wchar_t* cursor = text; cursor && *cursor; cursor++) {
        if (*cursor != L'&') result.push_back(*cursor);
    }
    return result;
}

void CollectLeaves(HMENU menu, std::set<int>& into) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; index++) {
        HMENU sub = GetSubMenu(menu, index);
        if (sub) {
            CollectLeaves(sub, into);
            continue;
        }
        const int id = GetMenuItemID(menu, index);
        if (id > 0) into.insert(id);
    }
}

bool FindOutputMenu(HMENU menu, const std::wstring& wanted, std::set<int>& into) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; index++) {
        HMENU sub = GetSubMenu(menu, index);
        if (!sub) continue;
        wchar_t label[256]{};
        GetMenuStringW(menu, index, label, 256, MF_BYPOSITION);
        if (WithoutMarker(label) == wanted) {
            CollectLeaves(sub, into);
            return true;
        }
        if (FindOutputMenu(sub, wanted, into)) return true;
    }
    return false;
}

void CollectOutputCommands() {
    if (g_collected) return;
    HMENU menu = GetMenu(g_host);
    if (!menu) return;
    g_collected = true;
    const std::wstring wanted = WithoutMarker(LanguageText(L"Menu", L"ファイル出力(&U)"));
    if (!FindOutputMenu(menu, wanted, g_output_commands)) {
        FindOutputMenu(menu, WithoutMarker(L"ファイル出力(&U)"), g_output_commands);
    }
}

LRESULT CALLBACK GuardProc(HWND window, UINT message, WPARAM first, LPARAM second, UINT_PTR,
                           DWORD_PTR) {
    if (message == WM_COMMAND && second == 0 && HIWORD(first) <= 1) {
        CollectOutputCommands();
        if (g_output_commands.count((int)LOWORD(first)) > 0 && !g_in_command) {
            g_in_command = true;
            SuspendAutomaticScan(true);
            g_restored = RestoreForExport() > 0;
            LRESULT result = DefSubclassProc(window, message, first, second);
            g_in_command = false;
            NoticeEditActivity();
            return result;
        }
    }
    return DefSubclassProc(window, message, first, second);
}

}

void StartExportGuard() {
    if (g_hooked) return;
    g_host = HostWindow();
    if (!g_host) return;
    g_hooked = SetWindowSubclass(g_host, GuardProc, kSubclassId, 0) != FALSE;
}

void StopExportGuard() {
    if (!g_hooked) return;
    RemoveWindowSubclass(g_host, GuardProc, kSubclassId);
    g_hooked = false;
    g_host = nullptr;
}

void NoticeEditActivity() {
    if (g_in_command) return;
    if (!AutomaticScanSuspended()) return;
    if (EditState() != EDIT_HANDLE::EDIT_STATE_EDIT) return;
    SuspendAutomaticScan(false);
    if (g_restored) {
        g_restored = false;
        ApplyAfterExport();
    }
}

}
