#include "HostContext.h"

namespace pe {

namespace {

EDIT_HANDLE* g_edit = nullptr;
CONFIG_HANDLE* g_config = nullptr;
CACHE_HANDLE* g_cache = nullptr;
HWND g_host = nullptr;
HINSTANCE g_instance = nullptr;

}

void SetEditHandle(EDIT_HANDLE* handle) { g_edit = handle; }
void SetConfigHandle(CONFIG_HANDLE* handle) { g_config = handle; }
void SetCacheHandle(CACHE_HANDLE* handle) { g_cache = handle; }
void SetHostWindow(HWND window) { g_host = window; }
void SetModuleInstance(HINSTANCE instance) { g_instance = instance; }

EDIT_HANDLE* Edit() { return g_edit; }
CONFIG_HANDLE* Config() { return g_config; }
CACHE_HANDLE* Cache() { return g_cache; }
HWND HostWindow() { return g_host; }
HINSTANCE ModuleInstance() { return g_instance; }

EDIT_INFO EditInfo() {
    EDIT_INFO info{};
    if (g_edit) g_edit->get_edit_info(&info, sizeof(info));
    return info;
}

int EditState() {
    if (!g_edit) return EDIT_HANDLE::EDIT_STATE_EDIT;
    return g_edit->get_edit_state();
}

bool Exporting() {
    return EditState() == EDIT_HANDLE::EDIT_STATE_SAVE;
}

int LayoutSize(const char* key) {
    if (!g_config) return 0;
    return g_config->get_layout_size(g_config, key);
}

COLORREF ThemeColor(const char* key, COLORREF fallback) {
    if (!g_config) return fallback;
    int code = g_config->get_color_code(g_config, key);
    if (code < 0) return fallback;
    return RGB((code >> 16) & 0xFF, (code >> 8) & 0xFF, code & 0xFF);
}

FONT_INFO* HostFont(const char* key) {
    if (!g_config) return nullptr;
    return g_config->get_font_info(g_config, key);
}

const wchar_t* LanguageText(const wchar_t* section, const wchar_t* text) {
    if (!g_config || !g_config->get_language_text) return text;
    const wchar_t* found = g_config->get_language_text(g_config, section, text);
    return found ? found : text;
}

const wchar_t* Translate(const wchar_t* text) {
    if (!g_config) return text;
    const wchar_t* translated = g_config->translate(g_config, text);
    return translated ? translated : text;
}

bool CallEditSection(void* param, void (*proc)(void* param, EDIT_SECTION* edit)) {
    if (!g_edit) return false;
    return g_edit->call_edit_section_param(param, proc);
}

bool CallReadSection(void* param, void (*proc)(void* param, EDIT_SECTION* edit)) {
    if (!g_edit) return false;
    return g_edit->call_read_section_param(param, proc);
}

}
