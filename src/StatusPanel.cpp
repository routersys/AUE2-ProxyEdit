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
int g_max_scroll = 0;
int g_hover = -1;
int g_hover_button = -1;
int g_press_button = -1;
int g_header_height = 0;
int g_row_pitch = 0;
RECT g_thumb{};
bool g_tracking = false;
bool g_dragging = false;
int g_drag_offset = 0;
BuilderSummary g_shown;
bool g_shown_valid = false;
unsigned long long g_asked = 0;

int Scaled(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;
    return MulDiv(value, (int)dpi, 96);
}

HFONT PanelFont(HWND window) {
    if (g_font) return g_font;
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) dpi = 96;
    LOGFONTW description{};
    description.lfCharSet = DEFAULT_CHARSET;
    description.lfQuality = CLEARTYPE_QUALITY;
    description.lfHeight = -MulDiv(13, (int)dpi, 96);
    wcscpy_s(description.lfFaceName, L"Yu Gothic UI");
    FONT_INFO* info = HostFont("Control");
    if (info) {
        if (info->size > 0.0f) description.lfHeight = -MulDiv((int)(info->size + 0.5f), (int)dpi, 96);
        if (info->name && info->name[0]) wcscpy_s(description.lfFaceName, info->name);
    }
    g_font = CreateFontIndirectW(&description);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    return g_font;
}

void ReleaseFont() {
    if (g_font && g_font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_font);
    g_font = nullptr;
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

int TextWidth(HDC dc, const wchar_t* text) {
    RECT box{0, 0, 0, 0};
    DrawTextW(dc, text, -1, &box, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    return box.right - box.left;
}

const wchar_t* WidestThatFits(HDC dc, const wchar_t* const* candidates, int count, int room) {
    for (int index = 0; index < count; index++) {
        if (TextWidth(dc, candidates[index]) <= room) return candidates[index];
    }
    return candidates[count - 1];
}

void DrawButton(HDC dc, const RECT& area, const wchar_t* text, bool active, bool hover, bool pressed) {
    COLORREF body;
    if (pressed) {
        body = ThemeColor("ButtonBodyPress", RGB(0xA0, 0xA0, 0xA0));
    } else if (active) {
        body = ThemeColor("ButtonBodySelect", RGB(0x70, 0x70, 0xC0));
    } else if (hover) {
        body = ThemeColor("ButtonBodyHover", RGB(0x80, 0x80, 0x80));
    } else {
        body = ThemeColor("ButtonBody", RGB(0x60, 0x60, 0x60));
    }
    FillRectangle(dc, area, body);
    SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));
    RECT text_area = area;
    DrawTextW(dc, text, -1, &text_area,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void DrawChunks(HDC dc, const RECT& area, const std::vector<unsigned char>& chunks) {
    FillRectangle(dc, area, ThemeColor("TrackBarRange", RGB(0x28, 0x28, 0x28)));
    const int width = area.right - area.left;
    if (chunks.empty() || width <= 0) return;
    HBRUSH working = CreateSolidBrush(ThemeColor("Border", RGB(0x90, 0x90, 0x90)));
    HBRUSH ready = CreateSolidBrush(ThemeColor("BorderFocus", RGB(0x80, 0x80, 0xE0)));
    auto state_at = [&chunks, width](int x) -> unsigned char {
        size_t index = (size_t)((long long)x * (long long)chunks.size() / width);
        if (index >= chunks.size()) index = chunks.size() - 1;
        return chunks[index];
    };
    int run_start = 0;
    unsigned char current = state_at(0);
    for (int x = 1; x <= width; x++) {
        unsigned char next = x < width ? state_at(x) : (unsigned char)0xFF;
        if (next == current) continue;
        if (current != kChunkMissing) {
            RECT run{area.left + run_start, area.top, area.left + x, area.bottom};
            FillRect(dc, &run, current == kChunkReady ? ready : working);
        }
        run_start = x;
        current = next;
    }
    DeleteObject(working);
    DeleteObject(ready);
}

void DrawScrollBar(HDC dc, const RECT& track, int view_height, int content_height) {
    FillRectangle(dc, track, ThemeColor("TrackBarRange", RGB(0x28, 0x28, 0x28)));
    if (content_height <= view_height) {
        g_thumb = RECT{};
        return;
    }
    const int span = track.bottom - track.top;
    int height = std::max((int)((long long)span * view_height / content_height), 24);
    if (height > span) height = span;
    const int room = span - height;
    const int offset = g_max_scroll > 0 ? (int)((long long)room * g_scroll / g_max_scroll) : 0;
    g_thumb = RECT{track.left, track.top + offset, track.right, track.top + offset + height};
    FillRectangle(dc, g_thumb, ThemeColor("SliderThumbBody", RGB(0x70, 0x70, 0x70)));
}

void Render(HWND window, HDC dc, const RECT& client) {
    FillRectangle(dc, client, ThemeColor("Background", RGB(0x20, 0x20, 0x20)));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = SelectObject(dc, PanelFont(window));
    SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));

    g_buttons.clear();
    const int padding = std::max(LayoutSize("SettingItemMarginWidth"), Scaled(window, 6));
    const int gap = Scaled(window, 4);
    const int unit = std::max(LayoutSize("SettingItemHeight"), Scaled(window, 24));
    const int line = Scaled(window, 18);
    const int bar = Scaled(window, 8);
    const int row_height = line * 2 + bar + padding;
    const int available = std::max((int)(client.right - client.left) - padding * 2, Scaled(window, 40));

    BuilderSummary summary = BuilderState();

    struct Definition {
        const wchar_t* text;
        int action;
        bool active;
    };
    const Definition definitions[3] = {
        {L"プロキシへ", kActionApply, false},
        {L"元へ戻す", kActionRestore, false},
        {summary.paused ? L"再開" : L"一時停止", kActionPause, summary.paused},
    };

    const int button_height = unit - Scaled(window, 6);
    int widths[3];
    int group = gap * 2;
    for (int index = 0; index < 3; index++) {
        widths[index] = std::clamp(TextWidth(dc, definitions[index].text) + Scaled(window, 24),
                                   Scaled(window, 56), available);
        group += widths[index];
    }

    wchar_t full[256];
    wchar_t middle[128];
    wchar_t brief[64];
    _snwprintf_s(full, _TRUNCATE, L"素材 %d 件   進捗 %.0f%%   容量 %s / %s", summary.jobs,
                 summary.overall * 100.0, SizeText(summary.bytes).c_str(),
                 SizeText(summary.capacity).c_str());
    _snwprintf_s(middle, _TRUNCATE, L"素材 %d 件   進捗 %.0f%%", summary.jobs, summary.overall * 100.0);
    _snwprintf_s(brief, _TRUNCATE, L"%d 件   %.0f%%", summary.jobs, summary.overall * 100.0);
    const wchar_t* captions[3] = {full, middle, brief};

    const bool one_row = TextWidth(dc, full) + Scaled(window, 12) + group <= available;
    const wchar_t* caption = one_row ? full : WidestThatFits(dc, captions, 3, available);

    RECT areas[3];
    RECT caption_area;
    int header_bottom;
    if (one_row) {
        const int top = client.top + (unit - button_height) / 2;
        int x = client.right - padding - group;
        for (int index = 0; index < 3; index++) {
            areas[index] = {x, top, x + widths[index], top + button_height};
            x += widths[index] + gap;
        }
        header_bottom = client.top + unit;
        caption_area = {client.left + padding, client.top, areas[0].left - Scaled(window, 12),
                        header_bottom};
    } else {
        int x = client.left + padding;
        int y = client.top + unit;
        for (int index = 0; index < 3; index++) {
            if (x > client.left + padding && x + widths[index] > client.right - padding) {
                x = client.left + padding;
                y += button_height + gap;
            }
            areas[index] = {x, y, x + widths[index], y + button_height};
            x += widths[index] + gap;
        }
        header_bottom = y + button_height + Scaled(window, 4);
        caption_area = {client.left + padding, client.top, client.right - padding, client.top + unit};
    }

    RECT header{client.left, client.top, client.right, header_bottom};
    FillRectangle(dc, header, ThemeColor("Grouping", RGB(0x38, 0x38, 0x38)));
    RECT edge{client.left, header_bottom - 1, client.right, header_bottom};
    FillRectangle(dc, edge, ThemeColor("GroupingSeparator", RGB(0x4C, 0x4C, 0x4C)));
    DrawTextW(dc, caption, -1, &caption_area,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    for (int index = 0; index < 3; index++) {
        DrawButton(dc, areas[index], definitions[index].text, definitions[index].active,
                   g_hover_button == index, g_press_button == index);
        g_buttons.push_back({areas[index], definitions[index].action});
    }

    g_header_height = header_bottom - client.top;
    g_row_pitch = row_height + Scaled(window, 4);

    g_jobs = BuilderSnapshot();
    const int view_height = std::max((int)(client.bottom - header_bottom), 0);
    const int content_height = (int)g_jobs.size() * g_row_pitch + padding;
    g_max_scroll = std::max(content_height - view_height, 0);
    g_scroll = std::clamp(g_scroll, 0, g_max_scroll);

    const int scrollbar = g_max_scroll > 0
                              ? std::max(LayoutSize("ScrollBarSize"), Scaled(window, 12))
                              : 0;
    const int right_edge = client.right - scrollbar;

    SaveDC(dc);
    IntersectClipRect(dc, client.left, header_bottom, right_edge, client.bottom);
    int y = header_bottom + padding - g_scroll;
    for (size_t index = 0; index < g_jobs.size(); index++) {
        const JobProgress& job = g_jobs[index];
        RECT row{client.left + padding, y, right_edge - padding, y + row_height};
        if (row.bottom > header_bottom && row.top < client.bottom) {
            COLORREF background = (int)index == g_hover
                                      ? ThemeColor("GroupingHover", RGB(0x40, 0x40, 0x40))
                                      : ThemeColor("Grouping", RGB(0x38, 0x38, 0x38));
            FillRectangle(dc, row, background);

            RECT title{row.left + padding, row.top + Scaled(window, 2), row.right - padding,
                       row.top + Scaled(window, 2) + line};
            std::wstring name = ShortName(job.source);
            SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));
            DrawTextW(dc, name.c_str(), -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);

            RECT detail_area{row.left + padding, title.bottom, row.right - padding, title.bottom + line};
            const int room = detail_area.right - detail_area.left;
            wchar_t wide[256];
            wchar_t compact[160];
            wchar_t tight[96];
            const wchar_t* detail = nullptr;
            if (job.failed) {
                detail = job.message.c_str();
            } else {
                _snwprintf_s(wide, _TRUNCATE, L"%dx%d → %dx%d   %d / %d   %.1f fps   %s",
                             job.source_width, job.source_height, job.proxy_width, job.proxy_height,
                             job.ready_chunks, job.total_chunks, job.frames_per_second,
                             SizeText(job.bytes).c_str());
                _snwprintf_s(compact, _TRUNCATE, L"%dx%d   %d / %d   %s", job.proxy_width,
                             job.proxy_height, job.ready_chunks, job.total_chunks,
                             SizeText(job.bytes).c_str());
                _snwprintf_s(tight, _TRUNCATE, L"%d / %d", job.ready_chunks, job.total_chunks);
                const wchar_t* details[3] = {wide, compact, tight};
                detail = WidestThatFits(dc, details, 3, room);
            }
            SetTextColor(dc, job.failed ? RGB(0xFF, 0x90, 0x90)
                                        : ThemeColor("TextDisable", RGB(0x90, 0x90, 0x90)));
            DrawTextW(dc, detail, -1, &detail_area,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));

            RECT chunk_area{row.left + padding, detail_area.bottom, row.right - padding,
                            detail_area.bottom + bar};
            DrawChunks(dc, chunk_area, job.chunks);
        }
        y = row.bottom + Scaled(window, 4);
    }

    if (g_jobs.empty()) {
        RECT empty{client.left + padding, header_bottom + padding, right_edge - padding, client.bottom};
        SetTextColor(dc, ThemeColor("TextDisable", RGB(0x90, 0x90, 0x90)));
        DrawTextW(dc, L"対象の素材がありません。「プロキシへ」を押すと現在のシーンを調べます。", -1, &empty,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SetTextColor(dc, ThemeColor("Text", RGB(0xFF, 0xFF, 0xFF)));
    }
    RestoreDC(dc, -1);

    if (scrollbar > 0) {
        RECT track{right_edge, header_bottom, client.right, client.bottom};
        DrawScrollBar(dc, track, view_height, content_height);
    } else {
        g_thumb = RECT{};
    }

    SelectObject(dc, previous);
}

void Paint(HWND window) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);
    if (client.right > client.left && client.bottom > client.top) {
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP surface = CreateCompatibleBitmap(dc, client.right - client.left,
                                                 client.bottom - client.top);
        HGDIOBJ old_surface = SelectObject(memory, surface);
        Render(window, memory, client);
        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_surface);
        DeleteObject(surface);
        DeleteDC(memory);
    }
    EndPaint(window, &paint);
}

int HitRow(HWND window, POINT point) {
    if (g_row_pitch <= 0) return -1;
    const int padding = std::max(LayoutSize("SettingItemMarginWidth"), Scaled(window, 6));
    if (point.y <= g_header_height) return -1;
    if (g_thumb.right > g_thumb.left && point.x >= g_thumb.left) return -1;
    int offset = point.y - (g_header_height + padding) + g_scroll;
    if (offset < 0) return -1;
    int index = offset / g_row_pitch;
    if (index < 0 || index >= (int)g_jobs.size()) return -1;
    return index;
}

int HitButton(POINT point) {
    for (size_t index = 0; index < g_buttons.size(); index++) {
        if (PtInRect(&g_buttons[index].area, point)) return (int)index;
    }
    return -1;
}

void RunAction(HWND window, int action) {
    if (action == kActionApply) {
        ScanResult result = ApplyProxies();
        Say(L"プロキシへ差し替えました: 対象 %d 件、差し替え %d 件", result.eligible, result.swapped);
    } else if (action == kActionRestore) {
        ScanResult result = RestoreOriginals();
        Say(L"元素材へ戻しました: %d 件", result.restored);
    } else if (action == kActionPause) {
        SetBuilderPaused(!BuilderPaused());
    }
    InvalidateRect(window, nullptr, FALSE);
}

bool SummaryChanged(const BuilderSummary& summary) {
    if (!g_shown_valid) return true;
    return summary.jobs != g_shown.jobs || summary.queued_chunks != g_shown.queued_chunks ||
           summary.bytes != g_shown.bytes || summary.paused != g_shown.paused ||
           summary.working != g_shown.working || summary.overall != g_shown.overall;
}

LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM first, LPARAM second) {
    switch (message) {
        case WM_CREATE:
            SetTimer(window, kTimerId, 250, nullptr);
            return 0;
        case WM_TIMER: {
            if (!IsWindowVisible(window)) return 0;
            if (g_jobs.empty() && CurrentSettings().enabled) {
                const unsigned long long now = GetTickCount64();
                if (now - g_asked >= 4000) {
                    EDIT_INFO info = EditInfo();
                    if (info.width > 0 && (info.frame_max > 0 || info.layer_max > 0)) {
                        g_asked = now;
                        RequestAutomaticScan();
                    }
                }
            }
            BuilderSummary summary = BuilderState();
            if (SummaryChanged(summary)) {
                g_shown = summary;
                g_shown_valid = true;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(window);
            return 0;
        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            ReleaseFont();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            if (!g_tracking) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
                g_tracking = true;
            }
            if (g_dragging) {
                const int span = std::max((int)(g_thumb.bottom - g_thumb.top), 1);
                RECT client;
                GetClientRect(window, &client);
                const int room = std::max((int)client.bottom - g_header_height - span, 1);
                const int position = point.y - g_header_height - g_drag_offset;
                g_scroll = std::clamp((int)((long long)position * g_max_scroll / room), 0, g_max_scroll);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            int row = HitRow(window, point);
            int button = HitButton(point);
            if (row != g_hover || button != g_hover_button) {
                g_hover = row;
                g_hover_button = button;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = -1;
            g_hover_button = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            if (g_thumb.right > g_thumb.left && PtInRect(&g_thumb, point)) {
                g_dragging = true;
                g_drag_offset = point.y - g_thumb.top;
                SetCapture(window);
                return 0;
            }
            int button = HitButton(point);
            if (button >= 0) {
                g_press_button = button;
                SetCapture(window);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(second), GET_Y_LPARAM(second)};
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                return 0;
            }
            const int pressed = g_press_button;
            if (pressed >= 0) {
                g_press_button = -1;
                ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                if (pressed < (int)g_buttons.size() && PtInRect(&g_buttons[pressed].area, point)) {
                    RunAction(window, g_buttons[pressed].action);
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_dragging = false;
            if (g_press_button >= 0) {
                g_press_button = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
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
            g_scroll = std::clamp(g_scroll - delta / 4, 0, g_max_scroll);
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
    ReleaseFont();
}

}
