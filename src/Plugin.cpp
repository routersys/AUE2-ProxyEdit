#include <windows.h>

#include "HostContext.h"
#include "Log.h"

using namespace pe;

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"プロキシ編集",
    L"プロキシ編集 version 1.0.0",
};

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}
EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2010000; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) { SetLogHandle(handle); }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) { SetConfigHandle(handle); }
EXTERN_C __declspec(dllexport) void InitializeCache(CACHE_HANDLE* handle) { SetCacheHandle(handle); }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    SetEditHandle(host->create_edit_handle());
    SetHostWindow(Edit()->get_host_app_window());
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        SetModuleInstance((HINSTANCE)module);
    }
    return TRUE;
}
