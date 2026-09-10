#include "ExportGuard.h"

#include <commctrl.h>

#include <atomic>
#include <string>

#include "HostContext.h"
#include "Log.h"
#include "ScanController.h"

namespace pe {

namespace {

const UINT_PTR kSubclassId = 0x50450003;

enum GuardKind {
    kGuardNone = 0,
    kGuardResume = 1,
    kGuardLeave = 2,
};

HWND g_host = nullptr;
bool g_hooked = false;
UINT g_ready_message = 0;
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

bool FindItem(HMENU menu, int id, const std::wstring& output, bool under_output,
              std::wstring& label, bool& inside) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; index++) {
        HMENU sub = GetSubMenu(menu, index);
        if (sub) {
            const bool deeper = under_output || LabelAt(menu, index) == output;
            if (FindItem(sub, id, output, deeper, label, inside)) return true;
            continue;
        }
        if (GetMenuItemID(menu, index) != (UINT)id) continue;
        label = LabelAt(menu, index);
        inside = under_output;
        return true;
    }
    return false;
}

GuardKind Classify(int id) {
    if (id <= 0) return kGuardNone;
    HMENU menu = GetMenu(g_host);
    if (!menu) return kGuardNone;
    std::wstring label;
    bool inside = false;
    if (!FindItem(menu, id, MenuName(L"ファイル出力"), false, label, inside)) return kGuardNone;
    if (inside) return kGuardResume;
    if (label == MenuName(L"バッチ出力")) return kGuardResume;
    if (label == MenuName(L"プロジェクトを保存")) return kGuardResume;
    if (label == MenuName(L"プロジェクトを別名で保存")) return kGuardResume;
    if (label == MenuName(L"プロジェクトを保存して終了")) return kGuardLeave;
    return kGuardNone;
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
        const int id = (int)LOWORD(first);
        if (id > 0 && g_passthrough.load() == id) {
            g_passthrough.store(0);
            const GuardKind kind = Classify(id);
            LRESULT result = DefSubclassProc(window, message, first, second);
            if (kind != kGuardLeave && EditState() == EDIT_HANDLE::EDIT_STATE_EDIT) {
                SuspendAutomaticScan(false);
                if (g_restored.exchange(false)) RequestApply();
            }
            return result;
        }
        if (Classify(id) != kGuardNone) {
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
    if (!g_hooked) {
        Warn(L"出力と保存の監視を始められませんでした。出力と保存の前に元素材へ戻りません");
    }
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
