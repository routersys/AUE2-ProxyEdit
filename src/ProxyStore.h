#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pe {

struct SourceKey {
    std::wstring path;
    long long size = 0;
    long long time = 0;
};

bool QuerySource(const std::wstring& path, SourceKey& key);
bool SameSource(const SourceKey& key, long long size, long long time);

bool EnsureStoreDirectory();
std::wstring ProxyPathFor(const SourceKey& key);

struct StoreItem {
    std::wstring proxy;
    std::wstring source;
    long long size = 0;
    long long used = 0;
    int proxy_width = 0;
    int proxy_height = 0;
    int frame_count = 0;
    int ready_frames = 0;
    bool readable = false;
    bool source_exists = false;
    bool source_changed = false;
};

std::vector<StoreItem> StoreContents();

long long StoreUsage();
void ReleaseCapacity(long long incoming, const std::wstring& keep);
void RemoveProxy(const std::wstring& proxy_path);

}
