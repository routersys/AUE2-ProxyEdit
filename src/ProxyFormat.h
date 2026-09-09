#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pe {

#pragma pack(push, 1)
struct ProxyHeader {
    char magic[8];
    int32_t version;
    int32_t source_width;
    int32_t source_height;
    int32_t rate;
    int32_t scale;
    int32_t frame_count;
    int32_t proxy_width;
    int32_t proxy_height;
    int32_t quality;
    int32_t chunk_frames;
    int64_t source_size;
    int64_t source_time;
    int64_t data_offset;
    int32_t reserved[14];
};

struct ProxyIndexEntry {
    int64_t offset;
    int32_t size;
    int32_t flags;
};
#pragma pack(pop)

extern const char kProxyMagic[8];
const int kProxyVersion = 1;

bool ProxyHeaderIsValid(const ProxyHeader& header);
long long ProxyIndexOffset(int frame);

class ProxyWriter {
public:
    ~ProxyWriter();
    bool Create(const std::wstring& path, const ProxyHeader& header);
    bool OpenExisting(const std::wstring& path);
    void Close();
    bool IsOpen() const;
    const ProxyHeader& Header() const { return header_; }
    bool WriteFrame(int frame, const void* data, int size);
    bool HasFrame(int frame);
    long long FileSize();

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    ProxyHeader header_{};
    long long append_ = 0;
    std::mutex mutex_;
};

class ProxyReader {
public:
    ~ProxyReader();
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const;
    const ProxyHeader& Header() const { return header_; }
    bool HasFrame(int frame);
    bool ReadFrame(int frame, std::vector<unsigned char>& out);

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    ProxyHeader header_{};
    std::mutex mutex_;
};

}
