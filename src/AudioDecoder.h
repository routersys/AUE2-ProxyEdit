#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct IMFSourceReader;

namespace pe {

class AudioDecoder {
public:
    ~AudioDecoder();
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const;

    int SampleRate() const { return sample_rate_; }
    int Channels() const { return channels_; }
    long long SampleCount() const { return sample_count_; }

    int Read(long long start, int length, void* buffer);

private:
    void CloseLocked();

    bool Seek(long long sample);
    bool Extend();

    IMFSourceReader* reader_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    long long sample_count_ = 0;
    long long buffer_start_ = 0;
    std::vector<short> buffer_;
    std::mutex mutex_;
};

}
