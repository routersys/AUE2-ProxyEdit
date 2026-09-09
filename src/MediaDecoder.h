#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct IMFSourceReader;

namespace pe {

bool StartMediaFoundation();
void StopMediaFoundation();

class MediaDecoder {
public:
    ~MediaDecoder();
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const;

    int Width() const { return width_; }
    int Height() const { return height_; }
    int Rate() const { return rate_; }
    int Scale() const { return scale_; }
    long long FrameCount() const { return frame_count_; }
    bool TenBit() const { return ten_bit_; }

    bool ReadFrameYuy2(long long index, unsigned char* destination, int stride, int height);

private:
    bool Seek(long long index);
    long long FrameToTime(long long index) const;

    IMFSourceReader* reader_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int rate_ = 0;
    int scale_ = 0;
    unsigned fourcc_ = 0;
    long long frame_count_ = 0;
    long long next_index_ = -1;
    bool ten_bit_ = false;
    std::mutex mutex_;
};

}
