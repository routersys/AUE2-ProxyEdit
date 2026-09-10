#include "ProxyStore.h"

#include <shlwapi.h>

#include <algorithm>
#include <map>
#include <mutex>

#include "Log.h"
#include "ProxyBuilder.h"
#include "ProxyFormat.h"
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

std::mutex g_index_lock;

std::wstring IndexPathFor(const std::wstring& proxy) {
    size_t separator = proxy.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return EffectiveStorePath() + L"\\sources.txt";
    return proxy.substr(0, separator) + L"\\sources.txt";
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                     nullptr, nullptr);
    std::string result((size_t)length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), result.data(), length, nullptr,
                        nullptr);
    return result;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring result((size_t)length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), result.data(), length);
    return result;
}

std::map<std::wstring, std::wstring> ReadIndex(const std::wstring& path) {
    std::map<std::wstring, std::wstring> table;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return table;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
        text.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(file, text.data(), (DWORD)text.size(), &read, nullptr)) text.clear();
        else text.resize(read);
    }
    CloseHandle(file);
    size_t start = 0;
    while (start < text.size()) {
        size_t stop = text.find('\n', start);
        if (stop == std::string::npos) stop = text.size();
        std::string line = text.substr(start, stop - start);
        start = stop + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        table[Wide(line.substr(0, tab))] = Wide(line.substr(tab + 1));
    }
    return table;
}

void WriteIndex(const std::wstring& path,
                const std::map<std::wstring, std::wstring>& table) {
    std::string text;
    for (const auto& entry : table) {
        text += Utf8(entry.first);
        text += '\t';
        text += Utf8(entry.second);
        text += "\r\n";
    }
    const std::wstring staging = path + L".new";
    HANDLE file = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool stored = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) &&
                        written == text.size();
    CloseHandle(file);
    if (!stored || !MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(staging.c_str());
    }
}

std::wstring IndexKey(const std::wstring& proxy) {
    size_t separator = proxy.find_last_of(L"\\/");
    return separator == std::wstring::npos ? proxy : proxy.substr(separator + 1);
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

std::vector<StoreItem> StoreContents() {
    std::vector<StoreItem> items;
    for (const StoreEntry& entry : CollectEntries()) {
        StoreItem item;
        item.proxy = entry.path;
        item.size = entry.size;
        item.used = entry.used;
        ProxyReader reader;
        if (reader.Open(entry.path)) {
            const ProxyHeader& header = reader.Header();
            item.readable = true;
            item.source = header.source_path;
            item.proxy_width = header.proxy_width;
            item.proxy_height = header.proxy_height;
            item.frame_count = header.frame_count;
            item.ready_frames = reader.ReadyFrames();
            SourceKey key;
            if (QuerySource(item.source, key)) {
                item.source_exists = true;
                item.source_changed = !SameSource(key, header.source_size, header.source_time);
            }
            reader.Close();
        }
        items.push_back(std::move(item));
    }
    return items;
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
        if (ProxyInUse(entry.path)) continue;
        if (DeleteFileW(entry.path.c_str())) {
            total -= entry.size;
            Say(L"容量を空けるためプロキシを破棄しました: %s", entry.path.c_str());
        }
    }
}

void RememberSource(const std::wstring& proxy, const std::wstring& source) {
    if (proxy.empty() || source.empty()) return;
    const std::wstring path = IndexPathFor(proxy);
    std::lock_guard<std::mutex> lock(g_index_lock);
    std::map<std::wstring, std::wstring> table = ReadIndex(path);
    const std::wstring key = IndexKey(proxy);
    auto found = table.find(key);
    if (found != table.end() && found->second == source) return;
    table[key] = source;
    WriteIndex(path, table);
}

std::wstring RecallSource(const std::wstring& proxy) {
    if (proxy.empty()) return std::wstring();
    const std::wstring path = IndexPathFor(proxy);
    std::lock_guard<std::mutex> lock(g_index_lock);
    std::map<std::wstring, std::wstring> table = ReadIndex(path);
    auto found = table.find(IndexKey(proxy));
    return found == table.end() ? std::wstring() : found->second;
}

void RemoveProxy(const std::wstring& proxy_path) {
    DeleteFileW(proxy_path.c_str());
}

}
