#include "ExportGuard.h"

#include <commctrl.h>

#include <atomic>
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
UINT g_ready_message = 0;
std::set<int> g_guarded;
std::set<int> g_final;
bool g_collected = false;
std::atomic<int> g_waiting{0};
std::atomic<int> g_passthrough{0};
std::atomic<bool> g_restored{false};

std::wstring MenuLabel(const wchar_t* text) {
    std::wstring result;
    for (const wchar_t* cursor = text; cursor && *cursor; cursor++) {
        if (*cursor == L'\t') break;
        if (*cursor != L'&') result.push_back(*cursor);
    }
    const size_t size = result.size();
    if (size >= 3 && result[size - 1] == L')' && result[size - 3] == L'(') result.erase(size - 3);
    return result;
}

std::wstring MenuName(const wchar_t* key) {
    return MenuLabel(LanguageText(L"Menu", key));
}

std::wstring LabelAt(HMENU menu, int index) {
    wchar_t label[256]{};
    GetMenuStringW(menu, index, label, 256, MF_BYPOSITION);
    return MenuLabel(label);
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
        if (LabelAt(menu, index) == wanted) {
            CollectLeaves(sub, into);
            return true;
        }
        if (FindOutputMenu(sub, wanted, into)) return true;
    }
    return false;
}

int FindCommand(HMENU menu, const std::wstring& wanted) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; index++) {
        HMENU sub = GetSubMenu(menu, index);
        if (sub) {
            const int found = FindCommand(sub, wanted);
            if (found > 0) return found;
            continue;
        }
        if (LabelAt(menu, index) != wanted) continue;
        const int id = GetMenuItemID(menu, index);
        if (id > 0) return id;
    }
    return 0;
}

void Guard(HMENU menu, const wchar_t* key, bool leaves_edit) {
    const int id = FindCommand(menu, MenuName(key));
    if (id <= 0) return;
    g_guarded.insert(id);
    if (leaves_edit) g_final.insert(id);
}

void CollectCommands() {
    if (g_collected) return;
    HMENU menu = GetMenu(g_host);
    if (!menu) return;
    g_collected = true;
    FindOutputMenu(menu, MenuName(L"ファイル出力"), g_guarded);
    Guard(menu, L"バッチ出力", false);
    Guard(menu, L"プロジェクトを保存", false);
    Guard(menu, L"プロジェクトを別名で保存", false);
    Guard(menu, L"プロジェクトを保存して終了", true);
}

LRESULT CALLBACK GuardProc(HWND window, UINT message, WPARAM first, LPARAM second, UINT_PTR,
                           DWORD_PTR) {
    if (g_ready_message != 0 && message == g_ready_message) {
        const int id = g_waiting.exchange(0);
        if (id != 0) {
            g_passthrough.store(id);
            PostMessageW(window, WM_COMMAND, (WPARAM)id, 0);
        }
        return 0;
    }
    if (message == WM_COMMAND && second == 0 && HIWORD(first) <= 1) {
        CollectCommands();
        const int id = (int)LOWORD(first);
        if (g_guarded.count(id) > 0) {
            if (g_passthrough.load() == id) {
                g_passthrough.store(0);
                LRESULT result = DefSubclassProc(window, message, first, second);
                if (g_final.count(id) == 0 && EditState() == EDIT_HANDLE::EDIT_STATE_EDIT) {
                    SuspendAutomaticScan(false);
                    if (g_restored.exchange(false)) RequestApply();
                }
                return result;
            }
            if (g_waiting.load() != 0) return 0;
            g_waiting.store(id);
            BeginScanning();
            SuspendAutomaticScan(true);
            RequestExportRestore();
            return 0;
        }
    }
    return DefSubclassProc(window, message, first, second);
}

}

void StartExportGuard() {
    if (g_hooked) return;
    g_host = HostWindow();
    if (!g_host) return;
    g_ready_message = RegisterWindowMessageW(L"ProxyEditExportReady");
    g_hooked = SetWindowSubclass(g_host, GuardProc, kSubclassId, 0) != FALSE;
}

void StopExportGuard() {
    if (!g_hooked) return;
    RemoveWindowSubclass(g_host, GuardProc, kSubclassId);
    g_hooked = false;
    g_host = nullptr;
}

void ExportRestoreFinished(bool restored) {
    g_restored.store(restored);
    if (g_host && g_ready_message != 0) PostMessageW(g_host, g_ready_message, 0, 0);
}

void NoticeEditActivity() {
    if (g_waiting.load() != 0 || g_passthrough.load() != 0) return;
    if (!AutomaticScanSuspended()) return;
    if (EditState() != EDIT_HANDLE::EDIT_STATE_EDIT) return;
    SuspendAutomaticScan(false);
    if (g_restored.exchange(false)) RequestApply();
}

}
