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

long long StoreUsage();
void ReleaseCapacity(long long incoming, const std::wstring& keep);
void RemoveProxy(const std::wstring& proxy_path);

}
