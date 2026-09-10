#include "SettingsDialog.h"

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "FlatButton.h"
#include "HostContext.h"
#include "Log.h"
#include "ProxyStore.h"
#include "Settings.h"
#include "StoreDialog.h"

namespace pe {

namespace {

const wchar_t* kClassName = L"ProxyEditSettingsWindow";

enum ControlId {
    kIdEnabled = 1000,
    kIdMinWidth,
    kIdMinMbps,
    kIdScale,
    kIdQuality,
    kIdChunk,
    kIdReadAhead,
    kIdWorkers,
    kIdCapacity,
    kIdStorePath,
    kIdBrowse,
    kIdStoreInfo,
    kIdOpenStore,
    kIdAccept,
    kIdCancel,
};

struct Field {
    int id;
    const wchar_t* label;
    const wchar_t* suffix;
};

const Field kFields[] = {
    {kIdMinWidth, L"対象にする最小の横幅", L"画素"},
    {kIdMinMbps, L"対象にする最小のビットレート", L"Mbps"},
    {kIdScale, L"プロキシの解像度比率", L"%"},
    {kIdQuality, L"プロキシの画質", L"30-98"},
    {kIdChunk, L"まとめて作る長さ", L"フレーム"},
    {kIdReadAhead, L"先読みに使う量", L"MB"},
    {kIdWorkers, L"生成の並列数", L"0で自動"},
    {kIdCapacity, L"保存の上限", L"GB"},
};

HWND g_window = nullptr;
HFONT g_font = nullptr;
bool g_closed = false;
long long g_usage = 0;

std::wstring SizeText(long long bytes) {
    wchar_t buffer[64];
    if (bytes >= 1024LL * 1024 * 1024) {
        _snwprintf_s(buffer, _TRUNCATE, L"%.1f GB", (double)bytes / 1024.0 / 1024.0 / 1024.0);
    } else {
        _snwprintf_s(buffer, _TRUNCATE, L"%.0f MB", (double)bytes / 1024.0 / 1024.0);
    }
    return buffer;
}

bool PickFolder(HWND owner, std::wstring& selected) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    bool picked = false;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    dialog->SetTitle(L"プロキシの保存先");
    if (!selected.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(selected.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dialog->SetFolder(start);
            start->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR name = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                selected = name;
                CoTaskMemFree(name);
                picked = true;
            }
            result->Release();
        }
    }
    dialog->Release();
    return picked;
}

void UpdateStoreInfo(HWND window) {
    wchar_t entered[MAX_PATH]{};
    GetDlgItemTextW(window, kIdStorePath, entered, MAX_PATH);
    std::wstring target = entered[0] ? std::wstring(entered) : DefaultStorePath();

    ULARGE_INTEGER free_bytes{};
    ULARGE_INTEGER total_bytes{};
    std::wstring probe = target;
    bool measured = false;
    for (int guard = 0; guard < 16 && !probe.empty(); guard++) {
        if (GetDiskFreeSpaceExW(probe.c_str(), &free_bytes, &total_bytes, nullptr)) {
            measured = true;
            break;
        }
        size_t separator = probe.find_last_of(L"\\/");
        if (separator == std::wstring::npos || separator < 2) break;
        probe = probe.substr(0, separator);
    }

    wchar_t line[256];
    if (measured) {
        _snwprintf_s(line, _TRUNCATE, L"空き %s　　使用量 %s", SizeText((long long)free_bytes.QuadPart).c_str(),
                     SizeText(g_usage).c_str());
    } else {
        _snwprintf_s(line, _TRUNCATE, L"この場所の空き容量を取得できません");
    }
    SetDlgItemTextW(window, kIdStoreInfo, line);
}

int Scaled(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;
    return MulDiv(value, (int)dpi, 96);
}

void ApplyFont(HWND control) {
    if (g_font) SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
}

HWND MakeControl(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style, int x, int y,
                 int width, int height, int id) {
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                   parent, (HMENU)(INT_PTR)id, ModuleInstance(), nullptr);
    ApplyFont(control);
    return control;
}

void SetNumber(HWND parent, int id, int value) {
    wchar_t buffer[32];
    _snwprintf_s(buffer, _TRUNCATE, L"%d", value);
    SetDlgItemTextW(parent, id, buffer);
}

int GetNumber(HWND parent, int id, int fallback) {
    wchar_t buffer[32];
    if (GetDlgItemTextW(parent, id, buffer, 32) == 0) return fallback;
    return _wtoi(buffer);
}

void Build(HWND window) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        LOGFONTW description = metrics.lfMessageFont;
        UINT dpi = GetDpiForWindow(window);
        if (dpi == 0) dpi = 96;
        description.lfHeight = MulDiv(description.lfHeight, (int)dpi, 96);
        g_font = CreateFontIndirectW(&description);
    }

    const Settings& settings = CurrentSettings();
    const int margin = Scaled(window, 12);
    const int row = Scaled(window, 26);
    const int label_width = Scaled(window, 190);
    const int edit_width = Scaled(window, 80);
    const int suffix_width = Scaled(window, 70);
    const int height = Scaled(window, 20);
    int y = margin;

    MakeControl(window, L"BUTTON", L"対象の素材を自動でプロキシへ差し替える", BS_AUTOCHECKBOX, margin, y,
                label_width + edit_width + suffix_width, height, kIdEnabled);
    CheckDlgButton(window, kIdEnabled, settings.enabled ? BST_CHECKED : BST_UNCHECKED);
    y += row;

    for (const Field& field : kFields) {
        MakeControl(window, L"STATIC", field.label, SS_LEFT, margin, y + Scaled(window, 3), label_width,
                    height, 0);
        MakeControl(window, L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_RIGHT, margin + label_width, y,
                    edit_width, height, field.id);
        MakeControl(window, L"STATIC", field.suffix, SS_LEFT, margin + label_width + edit_width +
                    Scaled(window, 6), y + Scaled(window, 3), suffix_width, height, 0);
        y += row;
    }

    SetNumber(window, kIdMinWidth, settings.target_min_width);
    SetNumber(window, kIdMinMbps, settings.target_min_mbps);
    SetNumber(window, kIdScale, settings.scale_percent);
    SetNumber(window, kIdQuality, settings.quality);
    SetNumber(window, kIdChunk, settings.chunk_frames);
    SetNumber(window, kIdReadAhead, settings.read_ahead_bytes / (1024 * 1024));
    SetNumber(window, kIdWorkers, settings.worker_count);
    SetNumber(window, kIdCapacity, (int)(settings.capacity_bytes / (1024LL * 1024 * 1024)));

    const int total_width = label_width + edit_width + suffix_width;
    const int browse_width = FlatButtonWidth(window, g_font, L"参照...");
    const int path_height = Scaled(window, 26);
    MakeControl(window, L"STATIC", L"プロキシの保存先", SS_LEFT, margin, y + Scaled(window, 3),
                total_width, height, 0);
    y += Scaled(window, 20);
    MakeControl(window, L"EDIT", EffectiveStorePath().c_str(), WS_BORDER | ES_AUTOHSCROLL, margin, y,
                total_width - browse_width - Scaled(window, 6), path_height, kIdStorePath);
    MakeFlatButton(window, L"参照...", kIdBrowse, g_font, false);
    SetWindowPos(GetDlgItem(window, kIdBrowse), nullptr, margin + total_width - browse_width, y,
                 browse_width, path_height, SWP_NOZORDER);
    y += path_height + Scaled(window, 6);
    MakeControl(window, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, margin, y, total_width, height,
                kIdStoreInfo);
    y += row;

    const int button_height = Scaled(window, 26);
    const int total = margin + label_width + edit_width + suffix_width;
    const int manage_width = FlatButtonWidth(window, g_font, L"保存先を管理");
    const int cancel_width = FlatButtonWidth(window, g_font, L"取消");
    const int accept_width = FlatButtonWidth(window, g_font, L"適用");
    MakeFlatButton(window, L"保存先を管理", kIdOpenStore, g_font, false);
    SetWindowPos(GetDlgItem(window, kIdOpenStore), nullptr, margin, y, manage_width, button_height,
                 SWP_NOZORDER);
    MakeFlatButton(window, L"取消", kIdCancel, g_font, false);
    SetWindowPos(GetDlgItem(window, kIdCancel), nullptr, total - cancel_width, y, cancel_width,
                 button_height, SWP_NOZORDER);
    MakeFlatButton(window, L"適用", kIdAccept, g_font, true);
    SetWindowPos(GetDlgItem(window, kIdAccept), nullptr,
                 total - cancel_width - accept_width - Scaled(window, 6), y, accept_width,
                 button_height, SWP_NOZORDER);
    y += button_height + margin;

    RECT client{0, 0, total + margin, y};
    AdjustWindowRectEx(&client, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE, 0);
    SetWindowPos(window, nullptr, 0, 0, client.right - client.left, client.bottom - client.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    g_usage = StoreUsage();
    UpdateStoreInfo(window);
}

void Accept(HWND window) {
    Settings settings = CurrentSettings();
    settings.enabled = IsDlgButtonChecked(window, kIdEnabled) == BST_CHECKED;
    settings.target_min_width = GetNumber(window, kIdMinWidth, settings.target_min_width);
    settings.target_min_mbps = GetNumber(window, kIdMinMbps, settings.target_min_mbps);
    settings.scale_percent = GetNumber(window, kIdScale, settings.scale_percent);
    settings.quality = GetNumber(window, kIdQuality, settings.quality);
    settings.chunk_frames = GetNumber(window, kIdChunk, settings.chunk_frames);
    settings.read_ahead_bytes = GetNumber(window, kIdReadAhead, 96) * 1024 * 1024;
    settings.worker_count = GetNumber(window, kIdWorkers, settings.worker_count);
    settings.capacity_bytes = (long long)GetNumber(window, kIdCapacity, 20) * 1024LL * 1024 * 1024;
    wchar_t path[MAX_PATH]{};
    GetDlgItemTextW(window, kIdStorePath, path, MAX_PATH);
    settings.store_path = path;
    if (settings.store_path == DefaultStorePath()) settings.store_path.clear();
    StoreSettings(settings);
    LoadSettings();
    EnsureStoreDirectory();
    Say(L"設定を保存しました");
}

LRESULT CALLBACK SettingsProc(HWND window, UINT message, WPARAM first, LPARAM second) {
    switch (message) {
        case WM_COMMAND: {
            const int id = LOWORD(first);
            if (id == kIdAccept) {
                Accept(window);
                DestroyWindow(window);
            } else if (id == kIdCancel) {
                DestroyWindow(window);
            } else if (id == kIdBrowse) {
                wchar_t current[MAX_PATH]{};
                GetDlgItemTextW(window, kIdStorePath, current, MAX_PATH);
                std::wstring selected = current[0] ? std::wstring(current) : EffectiveStorePath();
                if (PickFolder(window, selected)) {
                    SetDlgItemTextW(window, kIdStorePath, selected.c_str());
                    UpdateStoreInfo(window);
                }
            } else if (id == kIdStorePath && HIWORD(first) == EN_CHANGE) {
                UpdateStoreInfo(window);
            } else if (id == kIdOpenStore) {
                EnsureStoreDirectory();
                OpenStoreDialog(window);
            }
            return 0;
        }
        case WM_DRAWITEM:
            DrawFlatButton((const DRAWITEMSTRUCT*)second);
            return TRUE;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_closed = true;
            g_window = nullptr;
            if (g_font) {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, first, second);
}

void OnConfigMenu(HWND owner, HINSTANCE) {
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = SettingsProc;
    description.hInstance = ModuleInstance();
    description.lpszClassName = kClassName;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&description);

    g_closed = false;
    g_window = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"プロキシ編集の設定",
                               WS_POPUPWINDOW | WS_CAPTION, CW_USEDEFAULT, CW_USEDEFAULT, 400, 400, owner,
                               nullptr, ModuleInstance(), nullptr);
    if (!g_window) return;
    Build(g_window);

    RECT owner_area{};
    RECT self_area{};
    if (owner && GetWindowRect(owner, &owner_area) && GetWindowRect(g_window, &self_area)) {
        int x = owner_area.left + ((owner_area.right - owner_area.left) -
                                   (self_area.right - self_area.left)) / 2;
        int y = owner_area.top + ((owner_area.bottom - owner_area.top) -
                                  (self_area.bottom - self_area.top)) / 3;
        SetWindowPos(g_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
}

}

void RegisterSettingsMenu(HOST_APP_TABLE* host) {
    host->register_config_menu(L"プロキシ編集", OnConfigMenu);
}

}
