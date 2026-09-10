#include "StoreDialog.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "FlatButton.h"
#include "HostContext.h"
#include "Notify.h"
#include "ProxyBuilder.h"
#include "ProxyStore.h"
#include "ScanController.h"
#include "Settings.h"

namespace pe {

namespace {

const wchar_t* kClassName = L"ProxyEditStoreWindow";
const UINT kStateMessage = WM_APP + 2;

enum ControlId {
    kIdList = 1200,
    kIdRefresh,
    kIdDeleteSelected,
    kIdDeleteOrphans,
    kIdOpenStore,
    kIdClose,
    kIdSummary,
};

struct ButtonSpec {
    int id;
    const wchar_t* label;
};

const ButtonSpec kLeftButtons[4] = {
    {kIdRefresh, L"更新"},
    {kIdDeleteSelected, L"選んだものを削除"},
    {kIdDeleteOrphans, L"元素材が無いものを削除"},
    {kIdOpenStore, L"保存先を開く"},
};

struct Row {
    StoreItem item;
    bool in_use = false;
    std::wstring name;
    std::wstring state;
};

HWND g_window = nullptr;
HWND g_list = nullptr;
HFONT g_font = nullptr;
std::vector<Row> g_rows;
int g_sort_column = 2;
bool g_sort_descending = true;

int Scaled(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;
    return MulDiv(value, (int)dpi, 96);
}

std::wstring SizeText(long long bytes) {
    wchar_t buffer[64];
    if (bytes >= 1024LL * 1024 * 1024) {
        _snwprintf_s(buffer, _TRUNCATE, L"%.1f GB", (double)bytes / 1024.0 / 1024.0 / 1024.0);
    } else {
        _snwprintf_s(buffer, _TRUNCATE, L"%.0f MB", (double)bytes / 1024.0 / 1024.0);
    }
    return buffer;
}

std::wstring TimeText(long long stamp) {
    if (stamp <= 0) return L"-";
    FILETIME raw;
    raw.dwLowDateTime = (DWORD)(stamp & 0xFFFFFFFF);
    raw.dwHighDateTime = (DWORD)(stamp >> 32);
    FILETIME local{};
    SYSTEMTIME when{};
    if (!FileTimeToLocalFileTime(&raw, &local) || !FileTimeToSystemTime(&local, &when)) return L"-";
    wchar_t buffer[64];
    _snwprintf_s(buffer, _TRUNCATE, L"%04d/%02d/%02d %02d:%02d", when.wYear, when.wMonth, when.wDay,
                 when.wHour, when.wMinute);
    return buffer;
}

std::wstring ShortName(const std::wstring& path) {
    size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring ProgressText(const StoreItem& item) {
    if (!item.readable || item.frame_count <= 0) return L"-";
    const int percent = (int)((long long)item.ready_frames * 100 / item.frame_count);
    wchar_t buffer[32];
    _snwprintf_s(buffer, _TRUNCATE, L"%d%%", percent);
    return buffer;
}

std::wstring StateText(const Row& row) {
    if (!row.item.readable) return L"壊れています";
    if (!row.item.source_exists) return L"元素材がありません";
    if (row.item.source_changed) return L"元素材が更新されています";
    if (row.in_use) return L"使用中";
    return L"未使用";
}

bool Orphan(const Row& row) {
    return !row.item.readable || !row.item.source_exists;
}

void Sort() {
    std::stable_sort(g_rows.begin(), g_rows.end(), [](const Row& left, const Row& right) {
        bool less = false;
        switch (g_sort_column) {
            case 0: less = _wcsicmp(left.name.c_str(), right.name.c_str()) < 0; break;
            case 1: less = left.state < right.state; break;
            case 2: less = left.item.size < right.item.size; break;
            case 3: less = (long long)left.item.ready_frames * 1000 /
                               std::max(left.item.frame_count, 1) <
                           (long long)right.item.ready_frames * 1000 /
                               std::max(right.item.frame_count, 1);
                    break;
            default: less = left.item.used < right.item.used; break;
        }
        return g_sort_descending ? !less : less;
    });
}

void Fill() {
    ListView_DeleteAllItems(g_list);
    for (size_t index = 0; index < g_rows.size(); index++) {
        Row& row = g_rows[index];
        LVITEMW entry{};
        entry.mask = LVIF_TEXT | LVIF_PARAM;
        entry.iItem = (int)index;
        entry.pszText = (LPWSTR)row.name.c_str();
        entry.lParam = (LPARAM)index;
        ListView_InsertItem(g_list, &entry);
        ListView_SetItemText(g_list, (int)index, 1, (LPWSTR)row.state.c_str());
        std::wstring size = SizeText(row.item.size);
        ListView_SetItemText(g_list, (int)index, 2, (LPWSTR)size.c_str());
        std::wstring progress = ProgressText(row.item);
        ListView_SetItemText(g_list, (int)index, 3, (LPWSTR)progress.c_str());
        std::wstring used = TimeText(row.item.used);
        ListView_SetItemText(g_list, (int)index, 4, (LPWSTR)used.c_str());
    }
}

void Refresh() {
    std::vector<JobProgress> jobs = BuilderSnapshot();
    g_rows.clear();
    for (StoreItem& item : StoreContents()) {
        Row row;
        row.item = item;
        row.name = item.readable ? ShortName(item.source) : ShortName(item.proxy);
        for (const JobProgress& job : jobs) {
            if (_wcsicmp(job.proxy.c_str(), item.proxy.c_str()) == 0) {
                row.in_use = true;
                break;
            }
        }
        row.state = StateText(row);
        g_rows.push_back(std::move(row));
    }
    Sort();
    Fill();

    long long total = 0;
    int orphans = 0;
    for (const Row& row : g_rows) {
        total += row.item.size;
        if (Orphan(row)) orphans++;
    }
    wchar_t line[256];
    _snwprintf_s(line, _TRUNCATE, L"%d 件　%s / 上限 %s　　元素材が無いもの %d 件", (int)g_rows.size(),
                 SizeText(total).c_str(), SizeText(CurrentSettings().capacity_bytes).c_str(), orphans);
    SetDlgItemTextW(g_window, kIdSummary, line);
}

void Erase(const std::vector<Row*>& targets) {
    if (targets.empty()) return;
    std::vector<std::wstring> proxies;
    proxies.reserve(targets.size());
    for (const Row* row : targets) proxies.push_back(row->item.proxy);
    RequestDeleteProxies(proxies);
}

std::vector<Row*> Selected() {
    std::vector<Row*> targets;
    int index = -1;
    while ((index = ListView_GetNextItem(g_list, index, LVNI_SELECTED)) >= 0) {
        if (index >= 0 && index < (int)g_rows.size()) targets.push_back(&g_rows[(size_t)index]);
    }
    return targets;
}

bool Confirm(HWND window, int count, const wchar_t* what) {
    wchar_t message[256];
    _snwprintf_s(message, _TRUNCATE, L"%s %d 件を削除します。元素材は消えません。よろしいですか。", what,
                 count);
    return MessageBoxW(window, message, L"プロキシの削除", MB_OKCANCEL | MB_ICONWARNING) == IDOK;
}

HWND MakeControl(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style, int x, int y,
                 int width, int height, int id) {
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                   parent, (HMENU)(INT_PTR)id, ModuleInstance(), nullptr);
    if (g_font) SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
    return control;
}

void Layout(HWND window) {
    RECT client;
    GetClientRect(window, &client);
    const int margin = Scaled(window, 10);
    const int button_height = Scaled(window, 26);
    const int line = Scaled(window, 20);
    const int bottom = client.bottom - margin - button_height;
    const int summary_top = bottom - Scaled(window, 6) - line;
    SetWindowPos(g_list, nullptr, margin, margin, client.right - margin * 2, summary_top - margin * 2,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window, kIdSummary), nullptr, margin, summary_top,
                 client.right - margin * 2, line, SWP_NOZORDER);

    const int gap = Scaled(window, 6);
    int x = margin;
    for (const ButtonSpec& spec : kLeftButtons) {
        const int width = FlatButtonWidth(window, g_font, spec.label);
        SetWindowPos(GetDlgItem(window, spec.id), nullptr, x, bottom, width, button_height,
                     SWP_NOZORDER);
        x += width + gap;
    }
    const int close_width = FlatButtonWidth(window, g_font, L"閉じる");
    SetWindowPos(GetDlgItem(window, kIdClose), nullptr, client.right - margin - close_width, bottom,
                 close_width, button_height, SWP_NOZORDER);
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

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 10, 10,
                             window, (HMENU)(INT_PTR)kIdList, ModuleInstance(), nullptr);
    if (g_font) SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                  LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP);

    struct Column {
        const wchar_t* title;
        int width;
    };
    const Column columns[5] = {
        {L"素材", 330}, {L"状態", 170}, {L"容量", 90}, {L"進捗", 70}, {L"最終使用", 130},
    };
    int columns_width = 0;
    for (int index = 0; index < 5; index++) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = (LPWSTR)columns[index].title;
        column.cx = Scaled(window, columns[index].width);
        column.iSubItem = index;
        ListView_InsertColumn(g_list, index, &column);
        columns_width += column.cx;
    }

    const int margin = Scaled(window, 10);
    int buttons_width = margin * 2 + FlatButtonWidth(window, g_font, L"閉じる") + Scaled(window, 24);
    for (const ButtonSpec& spec : kLeftButtons) {
        buttons_width += FlatButtonWidth(window, g_font, spec.label) + Scaled(window, 6);
    }
    const int list_width =
        margin * 2 + columns_width + GetSystemMetrics(SM_CXVSCROLL) + Scaled(window, 8);
    RECT wanted{0, 0, std::max(list_width, buttons_width), Scaled(window, 460)};
    AdjustWindowRectEx(&wanted, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE, 0);
    SetWindowPos(window, nullptr, 0, 0, wanted.right - wanted.left, wanted.bottom - wanted.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    MakeControl(window, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10, kIdSummary);
    for (const ButtonSpec& spec : kLeftButtons) {
        MakeFlatButton(window, spec.label, spec.id, g_font, false);
    }
    MakeFlatButton(window, L"閉じる", kIdClose, g_font, false);

    Layout(window);
    Refresh();
}

LRESULT CALLBACK StoreProc(HWND window, UINT message, WPARAM first, LPARAM second) {
    switch (message) {
        case WM_CREATE:
            AddStateListener(window, kStateMessage);
            return 0;
        case kStateMessage:
            AcknowledgeStateChange(window);
            if (IsWindowVisible(window)) Refresh();
            return 0;
        case WM_SIZE:
            if (g_list) Layout(window);
            return 0;
        case WM_DRAWITEM:
            DrawFlatButton((const DRAWITEMSTRUCT*)second);
            return TRUE;
        case WM_NOTIFY: {
            LPNMHDR header = (LPNMHDR)second;
            if (header && header->idFrom == kIdList && header->code == LVN_COLUMNCLICK) {
                LPNMLISTVIEW view = (LPNMLISTVIEW)second;
                if (view->iSubItem == g_sort_column) {
                    g_sort_descending = !g_sort_descending;
                } else {
                    g_sort_column = view->iSubItem;
                    g_sort_descending = true;
                }
                Sort();
                Fill();
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(first);
            if (id == kIdRefresh) {
                Refresh();
            } else if (id == kIdDeleteSelected) {
                std::vector<Row*> targets = Selected();
                if (targets.empty()) {
                    MessageBoxW(window, L"削除するものが選ばれていません。", L"プロキシの削除",
                                MB_OK | MB_ICONINFORMATION);
                } else if (Confirm(window, (int)targets.size(), L"選んだプロキシ")) {
                    Erase(targets);
                }
            } else if (id == kIdDeleteOrphans) {
                std::vector<Row*> targets;
                for (Row& row : g_rows) {
                    if (Orphan(row)) targets.push_back(&row);
                }
                if (targets.empty()) {
                    MessageBoxW(window, L"元素材が無いプロキシはありません。", L"プロキシの削除",
                                MB_OK | MB_ICONINFORMATION);
                } else if (Confirm(window, (int)targets.size(), L"元素材が無いプロキシ")) {
                    Erase(targets);
                }
            } else if (id == kIdOpenStore) {
                EnsureStoreDirectory();
                ShellExecuteW(window, L"open", EffectiveStorePath().c_str(), nullptr, nullptr, SW_SHOW);
            } else if (id == kIdClose) {
                DestroyWindow(window);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            RemoveStateListener(window);
            g_rows.clear();
            g_window = nullptr;
            g_list = nullptr;
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

}

void OpenStoreDialog(HWND owner) {
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = StoreProc;
    description.hInstance = ModuleInstance();
    description.lpszClassName = kClassName;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    description.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&description);

    g_window = CreateWindowExW(0, kClassName, L"プロキシの保存先", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 960, 560, owner, nullptr, ModuleInstance(), nullptr);
    if (!g_window) return;
    Build(g_window);
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
}

}
