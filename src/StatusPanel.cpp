#include "StatusPanel.h"

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "HostContext.h"
#include "Log.h"
#include "ProxyBuilder.h"
#include "ScanController.h"
#include "Settings.h"

namespace pe {

namespace {

const wchar_t* kClassName = L"ProxyEditStatusPanel";
const UINT_PTR kTimerId = 0x50450002;

struct Button {
    RECT area{};
    int action = 0;
};

enum Action {
    kActionNone = 0,
    kActionApply = 1,
    kActionRestore = 2,
    kActionPause = 3,
    kActionRowAll = 100,
    kActionRowDiscard = 200,
};

HWND g_panel = nullptr;
HFONT g_font = nullptr;
std::vector<Button> g_buttons;
std::vector<JobProgress> g_jobs;
int g_scroll = 0;
int g_hover = -1;

int Scaled(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;
    return MulDiv(value, (int)dpi, 96);
}

HFONT PanelFont(HWND window) {
    if (g_font) return g_font;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        LOGFONTW description = metrics.lfMessageFont;
        UINT dpi = GetDpiForWindow(window);
        if (dpi == 0) dpi = 96;
        description.lfHeight = MulDiv(description.lfHeight, (int)dpi, 96);
        g_font = CreateFontIndirectW(&description);
    }
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    return g_font;
}

std::wstring ShortName(const std::wstring& path) {
    size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
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

void FillRectangle(HDC dc, const RECT& area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

void DrawButton(HDC dc, const RECT& area, const wchar_t* text, bool active) {
    COLORREF body = active ? ThemeColor("ButtonBodySelect", RGB(0x70, 0x70, 0xC0))
                           : ThemeColor("ButtonBody", RGB(0x60, 0x60, 0x60));
    FillRectangle(dc, area, body);
    SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));
    RECT text_area = area;
    DrawTextW(dc, text, -1, &text_area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawChunks(HDC dc, const RECT& area, const std::vector<unsigned char>& chunks) {
    FillRectangle(dc, area, ThemeColor("Background", RGB(0x20, 0x20, 0x20)));
    if (chunks.empty()) return;
    const int width = area.right - area.left;
    if (width <= 0) return;
    COLORREF ready = ThemeColor("BorderFocus", RGB(0x80, 0x80, 0xE0));
    COLORREF working = ThemeColor("GroupingSelect", RGB(0x48, 0x48, 0x48));
    for (int x = 0; x < width; x++) {
        size_t index = (size_t)((long long)x * chunks.size() / width);
        if (index >= chunks.size()) index = chunks.size() - 1;
        unsigned char state = chunks[index];
        if (state == kChunkMissing) continue;
        RECT column{area.left + x, area.top, area.left + x + 1, area.bottom};
        FillRectangle(dc, column, state == kChunkReady ? ready : working);
    }
}

void Paint(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);

    FillRectangle(dc, client, ThemeColor("Background", RGB(0x20, 0x20, 0x20)));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = SelectObject(dc, PanelFont(window));
    SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));

    g_buttons.clear();
    const int padding = Scaled(window, 6);
    const int header_height = std::max(LayoutSize("SettingItemHeight"), Scaled(window, 26));
    const int line = Scaled(window, 18);
    const int bar = Scaled(window, 8);
    const int row_height = line * 2 + bar + padding;

    BuilderSummary summary = BuilderState();

    RECT header{client.left, client.top, client.right, client.top + header_height};
    FillRectangle(dc, header, ThemeColor("Grouping", RGB(0x38, 0x38, 0x38)));

    const int button_width = Scaled(window, 92);
    const int button_height = header_height - Scaled(window, 6);
    int right = client.right - padding;
    struct Definition {
        const wchar_t* text;
        int action;
        bool active;
    };
    Definition definitions[3] = {
        {summary.paused ? L"再開" : L"一時停止", kActionPause, summary.paused},
        {L"元へ戻す", kActionRestore, false},
        {L"プロキシへ", kActionApply, false},
    };
    for (const Definition& definition : definitions) {
        RECT area{right - button_width, header.top + Scaled(window, 3), right,
                  header.top + Scaled(window, 3) + button_height};
        DrawButton(dc, area, definition.text, definition.active);
        g_buttons.push_back({area, definition.action});
        right = area.left - Scaled(window, 4);
    }

    wchar_t caption[256];
    _snwprintf_s(caption, _TRUNCATE, L"素材 %d 件   進捗 %.0f%%   容量 %s / %s", summary.jobs,
                 summary.overall * 100.0, SizeText(summary.bytes).c_str(),
                 SizeText(summary.capacity).c_str());
    RECT caption_area{header.left + padding, header.top, right, header.bottom};
    DrawTextW(dc, caption, -1, &caption_area, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    g_jobs = BuilderSnapshot();
    int y = header.bottom + padding - g_scroll;
    for (size_t index = 0; index < g_jobs.size(); index++) {
        const JobProgress& job = g_jobs[index];
        RECT row{client.left + padding, y, client.right - padding, y + row_height};
        if (row.bottom > header.bottom && row.top < client.bottom) {
            COLORREF background = (int)index == g_hover ? ThemeColor("GroupingHover", RGB(0x40, 0x40, 0x40))
                                                        : ThemeColor("Grouping", RGB(0x38, 0x38, 0x38));
            FillRectangle(dc, row, background);

            RECT title{row.left + padding, row.top + Scaled(window, 2), row.right - padding,
                       row.top + Scaled(window, 2) + line};
            std::wstring name = ShortName(job.source);
            DrawTextW(dc, name.c_str(), -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);

            wchar_t detail[256];
            if (job.failed) {
                _snwprintf_s(detail, _TRUNCATE, L"%s", job.message.c_str());
            } else {
                _snwprintf_s(detail, _TRUNCATE, L"%dx%d → %dx%d   %d / %d   %.1f fps   %s",
                             job.source_width, job.source_height, job.proxy_width, job.proxy_height,
                             job.ready_chunks, job.total_chunks, job.frames_per_second,
                             SizeText(job.bytes).c_str());
            }
            RECT detail_area{row.left + padding, title.bottom, row.right - padding, title.bottom + line};
            SetTextColor(dc, job.failed ? RGB(0xFF, 0x90, 0x90)
                                        : ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));
            DrawTextW(dc, detail, -1, &detail_area, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));

            RECT chunk_area{row.left + padding, detail_area.bottom, row.right - padding,
                            detail_area.bottom + bar};
            DrawChunks(dc, chunk_area, job.chunks);
        }
        y = row.bottom + Scaled(window, 4);
    }

    if (g_jobs.empty()) {
        RECT empty{client.left + padding, header.bottom + padding, client.right - padding,
                   header.bottom + padding + line * 2};
        DrawTextW(dc, L"対象の素材がありません。「プロキシへ」を押すと現在のシーンを調べます。", -1, &empty,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }

    SelectObject(dc, previous);
    EndPaint(window, &paint);
}

int HitRow(HWND window, POINT point) {
    RECT client;
    GetClientRect(window, &client);
    const int padding = Scaled(window, 6);
    const int header_height = std::max(LayoutSize("SettingItemHeight"), Scaled(window, 26));
    const int line = Scaled(window, 18);
    const int bar = Scaled(window, 8);
    const int row_height = line * 2 + bar + padding;
    if (point.y <= header_height) return -1;
    int offset = point.y - (header_height + padding) + g_scroll;
    if (offset < 0) return -1;
    int index = offset / (row_height + Scaled(window, 4));
    if (index < 0 || index >= (int)g_jobs.size()) return -1;
    return index;
}

LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM first, LPARAM second) {
    switch (message) {
        case WM_CREATE:
            SetTimer(window, kTimerId, 600, nullptr);
            return 0;
        case WM_TIMER:
            if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(window);
            return 0;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            int row = HitRow(window, point);
            if (row != g_hover) {
                g_hover = row;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hover = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            for (const Button& button : g_buttons) {
                if (!PtInRect(&button.area, point)) continue;
                if (button.action == kActionApply) {
                    ScanResult result = ApplyProxies();
                    Say(L"プロキシへ差し替えました: 対象 %d 件、差し替え %d 件", result.eligible,
                        result.swapped);
                } else if (button.action == kActionRestore) {
                    ScanResult result = RestoreOriginals();
                    Say(L"元素材へ戻しました: %d 件", result.restored);
                } else if (button.action == kActionPause) {
                    SetBuilderPaused(!BuilderPaused());
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            int row = HitRow(window, point);
            if (row >= 0 && row < (int)g_jobs.size()) {
                RequestWholeSource(g_jobs[(size_t)row].source);
                Say(L"全体の生成を要求しました: %s", g_jobs[(size_t)row].source.c_str());
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            int row = HitRow(window, point);
            if (row < 0 || row >= (int)g_jobs.size()) return 0;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kActionRowAll, L"この素材を全部作る");
            AppendMenuW(menu, MF_STRING, kActionRowDiscard, L"この素材のプロキシを破棄する");
            POINT screen = point;
            ClientToScreen(window, &screen);
            int command = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0,
                                              window, nullptr);
            DestroyMenu(menu);
            if (command == kActionRowAll) {
                RequestWholeSource(g_jobs[(size_t)row].source);
            } else if (command == kActionRowDiscard) {
                DiscardSource(g_jobs[(size_t)row].source);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(first);
            g_scroll -= delta / 4;
            if (g_scroll < 0) g_scroll = 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kTimerId);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, first, second);
}

}

void RegisterStatusPanel(HOST_APP_TABLE* host) {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.style = CS_DBLCLKS;
    description.lpfnWndProc = PanelProc;
    description.hInstance = ModuleInstance();
    description.lpszClassName = kClassName;
    description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&description);
    g_panel = CreateWindowExW(0, kClassName, L"プロキシ編集", WS_POPUP, 0, 0, 480, 320, nullptr, nullptr,
                              description.hInstance, nullptr);
    if (!g_panel) return;
    host->register_window_client(L"プロキシ編集", g_panel);
}

void DestroyStatusPanel() {
    if (g_panel) {
        DestroyWindow(g_panel);
        g_panel = nullptr;
    }
    if (g_font && g_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
}

}
