#include "ProxyStore.h"

#include <shlwapi.h>

#include <algorithm>

#include "Log.h"
#include "Settings.h"

namespace pe {

namespace {

struct StoreEntry {
    std::wstring path;
    long long size = 0;
    long long used = 0;
};

unsigned long long HashPath(const std::wstring& path) {
    unsigned long long hash = 1469598103934665603ULL;
    for (wchar_t character : path) {
        wchar_t lowered = (wchar_t)towlower(character);
        hash ^= (unsigned long long)(lowered & 0xFF);
        hash *= 1099511628211ULL;
        hash ^= (unsigned long long)((lowered >> 8) & 0xFF);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring BaseName(const std::wstring& path) {
    size_t separator = path.find_last_of(L"\\/");
    std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    std::wstring cleaned;
    for (wchar_t character : name) {
        if (wcschr(L"\\/:*?\"<>|", character)) continue;
        cleaned += character;
        if (cleaned.size() >= 48) break;
    }
    if (cleaned.empty()) cleaned = L"source";
    return cleaned;
}

std::vector<StoreEntry> CollectEntries() {
    std::vector<StoreEntry> entries;
    std::wstring pattern = EffectiveStorePath() + L"\\*.pxy";
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return entries;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        StoreEntry entry;
        entry.path = EffectiveStorePath() + L"\\" + found.cFileName;
        entry.size = ((long long)found.nFileSizeHigh << 32) | (long long)found.nFileSizeLow;
        ULARGE_INTEGER used;
        used.LowPart = found.ftLastAccessTime.dwLowDateTime;
        used.HighPart = found.ftLastAccessTime.dwHighDateTime;
        entry.used = (long long)used.QuadPart;
        entries.push_back(entry);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return entries;
}

}

bool QuerySource(const std::wstring& path, SourceKey& key) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    key.path = path;
    key.size = ((long long)data.nFileSizeHigh << 32) | (long long)data.nFileSizeLow;
    ULARGE_INTEGER stamp;
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    key.time = (long long)stamp.QuadPart;
    return true;
}

bool SameSource(const SourceKey& key, long long size, long long time) {
    return key.size == size && key.time == time;
}

bool EnsureStoreDirectory() {
    std::wstring path = EffectiveStorePath();
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ProxyPathFor(const SourceKey& key) {
    wchar_t suffix[32];
    _snwprintf_s(suffix, _TRUNCATE, L"_%016llX.pxy", HashPath(key.path));
    return EffectiveStorePath() + L"\\" + BaseName(key.path) + suffix;
}

long long StoreUsage() {
    long long total = 0;
    for (const StoreEntry& entry : CollectEntries()) total += entry.size;
    return total;
}

void ReleaseCapacity(long long incoming, const std::wstring& keep) {
    const long long capacity = CurrentSettings().capacity_bytes;
    std::vector<StoreEntry> entries = CollectEntries();
    long long total = 0;
    for (const StoreEntry& entry : entries) total += entry.size;
    if (total + incoming <= capacity) return;
    std::sort(entries.begin(), entries.end(),
              [](const StoreEntry& left, const StoreEntry& right) { return left.used < right.used; });
    for (const StoreEntry& entry : entries) {
        if (total + incoming <= capacity) break;
        if (_wcsicmp(entry.path.c_str(), keep.c_str()) == 0) continue;
        if (DeleteFileW(entry.path.c_str())) {
            total -= entry.size;
            Say(L"容量を空けるためプロキシを破棄しました: %s", entry.path.c_str());
        }
    }
}

void RemoveProxy(const std::wstring& proxy_path) {
    DeleteFileW(proxy_path.c_str());
}

}
