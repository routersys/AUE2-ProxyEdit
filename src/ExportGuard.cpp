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
std::set<int> g_output_commands;
bool g_collected = false;
std::atomic<int> g_waiting{0};
std::atomic<int> g_passthrough{0};
std::atomic<bool> g_restored{false};

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
    if (g_ready_message != 0 && message == g_ready_message) {
        const int id = g_waiting.exchange(0);
        if (id != 0) {
            g_passthrough.store(id);
            PostMessageW(window, WM_COMMAND, (WPARAM)id, 0);
        }
        return 0;
    }
    if (message == WM_COMMAND && second == 0 && HIWORD(first) <= 1) {
        CollectOutputCommands();
        const int id = (int)LOWORD(first);
        if (g_output_commands.count(id) > 0) {
            if (g_passthrough.load() == id) {
                g_passthrough.store(0);
                LRESULT result = DefSubclassProc(window, message, first, second);
                if (EditState() == EDIT_HANDLE::EDIT_STATE_EDIT) {
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
