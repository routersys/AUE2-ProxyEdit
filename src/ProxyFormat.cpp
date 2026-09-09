#include "ProxyFormat.h"

#include <string.h>

namespace pe {

const char kProxyMagic[8] = {'P', 'X', 'P', 'R', 'O', 'X', 'Y', '2'};

namespace {

bool ReadAt(HANDLE file, long long offset, void* buffer, DWORD size) {
    LARGE_INTEGER position;
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    if (!ReadFile(file, buffer, size, &read, nullptr)) return false;
    return read == size;
}

bool WriteAt(HANDLE file, long long offset, const void* buffer, DWORD size) {
    LARGE_INTEGER position;
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD written = 0;
    if (!WriteFile(file, buffer, size, &written, nullptr)) return false;
    return written == size;
}

}

bool ProxyHeaderIsValid(const ProxyHeader& header) {
    if (memcmp(header.magic, kProxyMagic, sizeof(kProxyMagic)) != 0) return false;
    if (header.version != kProxyVersion) return false;
    if (header.frame_count <= 0) return false;
    if (header.source_width <= 0 || header.source_height <= 0) return false;
    if (header.proxy_width <= 0 || header.proxy_height <= 0) return false;
    return true;
}

long long ProxyIndexOffset(int frame) {
    return (long long)sizeof(ProxyHeader) + (long long)frame * (long long)sizeof(ProxyIndexEntry);
}

ProxyWriter::~ProxyWriter() {
    Close();
}

bool ProxyWriter::Create(const std::wstring& path, const ProxyHeader& header) {
    Close();
    std::lock_guard<std::mutex> lock(mutex_);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    header_ = header;
    memcpy(header_.magic, kProxyMagic, sizeof(kProxyMagic));
    header_.version = kProxyVersion;
    header_.data_offset = ProxyIndexOffset(header_.frame_count);
    if (!WriteAt(file, 0, &header_, sizeof(header_))) {
        CloseHandle(file);
        return false;
    }
    std::vector<ProxyIndexEntry> blank((size_t)header_.frame_count);
    memset(blank.data(), 0, blank.size() * sizeof(ProxyIndexEntry));
    if (!WriteAt(file, ProxyIndexOffset(0), blank.data(),
                 (DWORD)(blank.size() * sizeof(ProxyIndexEntry)))) {
        CloseHandle(file);
        return false;
    }
    file_ = file;
    append_ = header_.data_offset;
    return true;
}

bool ProxyWriter::OpenExisting(const std::wstring& path) {
    Close();
    std::lock_guard<std::mutex> lock(mutex_);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    ProxyHeader header{};
    if (!ReadAt(file, 0, &header, sizeof(header)) || !ProxyHeaderIsValid(header)) {
        CloseHandle(file);
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return false;
    }
    header_ = header;
    file_ = file;
    append_ = size.QuadPart > header_.data_offset ? size.QuadPart : header_.data_offset;
    return true;
}

void ProxyWriter::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

bool ProxyWriter::IsOpen() const {
    return file_ != INVALID_HANDLE_VALUE;
}

bool ProxyWriter::WriteFrame(int frame, const void* data, int size) {
    if (size <= 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    if (frame < 0 || frame >= header_.frame_count) return false;
    ProxyIndexEntry existing{};
    if (ReadAt(file_, ProxyIndexOffset(frame), &existing, sizeof(existing)) && existing.size > 0) {
        return true;
    }
    long long offset = append_;
    if (!WriteAt(file_, offset, data, (DWORD)size)) return false;
    append_ = offset + size;
    ProxyIndexEntry entry{};
    entry.offset = offset;
    entry.size = size;
    entry.flags = 1;
    if (!WriteAt(file_, ProxyIndexOffset(frame), &entry, sizeof(entry))) return false;
    return true;
}

bool ProxyWriter::HasFrame(int frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    if (frame < 0 || frame >= header_.frame_count) return false;
    ProxyIndexEntry entry{};
    if (!ReadAt(file_, ProxyIndexOffset(frame), &entry, sizeof(entry))) return false;
    return entry.size > 0;
}

long long ProxyWriter::FileSize() {
    std::lock_guard<std::mutex> lock(mutex_);
    return append_;
}

ProxyReader::~ProxyReader() {
    Close();
}

bool ProxyReader::Open(const std::wstring& path) {
    Close();
    std::lock_guard<std::mutex> lock(mutex_);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    ProxyHeader header{};
    if (!ReadAt(file, 0, &header, sizeof(header)) || !ProxyHeaderIsValid(header)) {
        CloseHandle(file);
        return false;
    }
    header_ = header;
    file_ = file;
    return true;
}

void ProxyReader::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

bool ProxyReader::IsOpen() const {
    return file_ != INVALID_HANDLE_VALUE;
}

bool ProxyReader::HasFrame(int frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    if (frame < 0 || frame >= header_.frame_count) return false;
    ProxyIndexEntry entry{};
    if (!ReadAt(file_, ProxyIndexOffset(frame), &entry, sizeof(entry))) return false;
    return entry.size > 0;
}

bool ProxyReader::ReadFrame(int frame, std::vector<unsigned char>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    if (frame < 0 || frame >= header_.frame_count) return false;
    ProxyIndexEntry entry{};
    if (!ReadAt(file_, ProxyIndexOffset(frame), &entry, sizeof(entry))) return false;
    if (entry.size <= 0) return false;
    out.resize((size_t)entry.size);
    return ReadAt(file_, entry.offset, out.data(), (DWORD)entry.size);
}

}
