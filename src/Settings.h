#pragma once

#include <windows.h>

#include <string>

namespace pe {

struct Settings {
    bool enabled = true;
    int target_min_width = 1921;
    bool target_ten_bit = true;
    int target_min_mbps = 60;
    int scale_percent = 50;
    int quality = 80;
    int chunk_frames = 240;
    int read_ahead_bytes = 96 * 1024 * 1024;
    int worker_count = 0;
    long long capacity_bytes = 20LL * 1024 * 1024 * 1024;
    std::wstring store_path;
};

const Settings& CurrentSettings();
void LoadSettings();
void StoreSettings(const Settings& settings);

std::wstring SettingsFilePath();
std::wstring DefaultStorePath();
std::wstring EffectiveStorePath();
int EffectiveWorkerCount();
int ReadAheadFrames(int width, int height);

}
