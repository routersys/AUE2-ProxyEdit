#include "Settings.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <thread>

#include "HostContext.h"
#include "Log.h"

namespace pe {

namespace {

Settings g_settings;
bool g_loaded = false;

const wchar_t* kSection = L"ProxyEdit";

std::wstring AppDataPath() {
    CONFIG_HANDLE* config = Config();
    if (config && config->app_data_path) return std::wstring(config->app_data_path);
    wchar_t buffer[MAX_PATH];
    if (SHGetSpecialFolderPathW(nullptr, buffer, CSIDL_COMMON_APPDATA, FALSE)) {
        std::wstring path(buffer);
        path += L"\\aviutl2";
        return path;
    }
    return std::wstring(L".");
}

int ReadInt(const wchar_t* key, int fallback) {
    return (int)GetPrivateProfileIntW(kSection, key, fallback, SettingsFilePath().c_str());
}

long long ReadLongLong(const wchar_t* key, long long fallback) {
    wchar_t buffer[64];
    wchar_t initial[64];
    _snwprintf_s(initial, _TRUNCATE, L"%lld", fallback);
    GetPrivateProfileStringW(kSection, key, initial, buffer, 64, SettingsFilePath().c_str());
    return _wtoi64(buffer);
}

std::wstring ReadText(const wchar_t* key) {
    wchar_t buffer[MAX_PATH];
    GetPrivateProfileStringW(kSection, key, L"", buffer, MAX_PATH, SettingsFilePath().c_str());
    return std::wstring(buffer);
}

void WriteInt(const wchar_t* key, int value) {
    wchar_t buffer[64];
    _snwprintf_s(buffer, _TRUNCATE, L"%d", value);
    WritePrivateProfileStringW(kSection, key, buffer, SettingsFilePath().c_str());
}

void WriteLongLong(const wchar_t* key, long long value) {
    wchar_t buffer[64];
    _snwprintf_s(buffer, _TRUNCATE, L"%lld", value);
    WritePrivateProfileStringW(kSection, key, buffer, SettingsFilePath().c_str());
}

}

const Settings& CurrentSettings() {
    if (!g_loaded) LoadSettings();
    return g_settings;
}

void LoadSettings() {
    g_loaded = true;
    Settings fallback;
    g_settings.enabled = ReadInt(L"Enabled", fallback.enabled ? 1 : 0) != 0;
    g_settings.target_min_width = ReadInt(L"TargetMinWidth", fallback.target_min_width);
    g_settings.target_ten_bit = ReadInt(L"TargetTenBit", fallback.target_ten_bit ? 1 : 0) != 0;
    g_settings.target_min_mbps = ReadInt(L"TargetMinMbps", fallback.target_min_mbps);
    g_settings.scale_percent = std::clamp(ReadInt(L"ScalePercent", fallback.scale_percent), 20, 100);
    g_settings.quality = std::clamp(ReadInt(L"Quality", fallback.quality), 30, 98);
    g_settings.chunk_frames = std::clamp(ReadInt(L"ChunkFrames", fallback.chunk_frames), 24, 3600);
    g_settings.read_ahead_bytes = std::clamp(ReadInt(L"ReadAheadBytes", fallback.read_ahead_bytes),
                                             8 * 1024 * 1024, 512 * 1024 * 1024);
    g_settings.worker_count = std::clamp(ReadInt(L"WorkerCount", fallback.worker_count), 0, 32);
    g_settings.capacity_bytes = ReadLongLong(L"CapacityBytes", fallback.capacity_bytes);
    if (g_settings.capacity_bytes < 1024LL * 1024 * 1024) g_settings.capacity_bytes = 1024LL * 1024 * 1024;
    g_settings.store_path = ReadText(L"StorePath");
}

void StoreSettings(const Settings& settings) {
    g_settings = settings;
    g_loaded = true;
    WriteInt(L"Enabled", settings.enabled ? 1 : 0);
    WriteInt(L"TargetMinWidth", settings.target_min_width);
    WriteInt(L"TargetTenBit", settings.target_ten_bit ? 1 : 0);
    WriteInt(L"TargetMinMbps", settings.target_min_mbps);
    WriteInt(L"ScalePercent", settings.scale_percent);
    WriteInt(L"Quality", settings.quality);
    WriteInt(L"ChunkFrames", settings.chunk_frames);
    WriteInt(L"ReadAheadBytes", settings.read_ahead_bytes);
    WriteInt(L"WorkerCount", settings.worker_count);
    WriteLongLong(L"CapacityBytes", settings.capacity_bytes);
    WritePrivateProfileStringW(kSection, L"StorePath", settings.store_path.c_str(),
                               SettingsFilePath().c_str());
}

std::wstring SettingsFilePath() {
    std::wstring path = AppDataPath();
    path += L"\\ProxyEdit.ini";
    return path;
}

std::wstring DefaultStorePath() {
    std::wstring path = AppDataPath();
    path += L"\\ProxyEdit";
    return path;
}

std::wstring EffectiveStorePath() {
    const Settings& settings = CurrentSettings();
    if (!settings.store_path.empty()) return settings.store_path;
    return DefaultStorePath();
}

int EffectiveWorkerCount() {
    const Settings& settings = CurrentSettings();
    if (settings.worker_count > 0) return settings.worker_count;
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;
    int count = (int)(hardware / 2);
    return std::clamp(count, 1, 4);
}

int ReadAheadFrames(int width, int height) {
    if (width <= 0 || height <= 0) return 4;
    long long bytes = (long long)width * height * 2;
    if (bytes <= 0) return 4;
    int frames = (int)(CurrentSettings().read_ahead_bytes / bytes);
    return std::clamp(frames, 2, 8);
}

}
