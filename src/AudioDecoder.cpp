#include "AudioDecoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>

#include "MediaDecoder.h"

namespace pe {

namespace {

const size_t kMaximumBuffer = 1 << 22;

}

AudioDecoder::~AudioDecoder() {
    Close();
}

bool AudioDecoder::Open(const std::wstring& path) {
    Close();
    if (!StartMediaFoundation()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 2))) return false;
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT result = MFCreateSourceReaderFromURL(path.c_str(), attributes, &reader_);
    attributes->Release();
    if (FAILED(result) || !reader_) return false;

    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
        CloseLocked();
        return false;
    }
    IMFMediaType* wanted = nullptr;
    if (FAILED(MFCreateMediaType(&wanted))) {
        CloseLocked();
        return false;
    }
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    result = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, wanted);
    wanted->Release();
    if (FAILED(result)) {
        CloseLocked();
        return false;
    }
    IMFMediaType* current = nullptr;
    if (SUCCEEDED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current)) && current) {
        UINT32 rate = 0, channels = 0;
        current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        sample_rate_ = (int)rate;
        channels_ = (int)channels;
        current->Release();
    }
    if (sample_rate_ <= 0 || channels_ <= 0) {
        CloseLocked();
        return false;
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION,
                                                    &duration)) &&
        duration.vt == VT_UI8) {
        double seconds = (double)duration.uhVal.QuadPart / 10000000.0;
        sample_count_ = (long long)(seconds * sample_rate_);
    }
    PropVariantClear(&duration);
    buffer_start_ = 0;
    buffer_.clear();
    return true;
}

void AudioDecoder::CloseLocked() {
    if (reader_) {
        reader_->Release();
        reader_ = nullptr;
    }
    buffer_.clear();
    buffer_start_ = 0;
    sample_rate_ = 0;
    channels_ = 0;
    sample_count_ = 0;
}

void AudioDecoder::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
}

bool AudioDecoder::IsOpen() const {
    return reader_ != nullptr;
}

bool AudioDecoder::Seek(long long sample) {
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = (long long)((double)sample / sample_rate_ * 10000000.0);
    HRESULT result = reader_->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    buffer_.clear();
    buffer_start_ = sample;
    return SUCCEEDED(result);
}

bool AudioDecoder::Extend() {
    DWORD stream = 0, flags = 0;
    LONGLONG stamp = 0;
    IMFSample* sample = nullptr;
    if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream, &flags, &stamp,
                                   &sample))) {
        return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (sample) sample->Release();
        return false;
    }
    if (!sample) return true;
    IMFMediaBuffer* buffer = nullptr;
    bool done = false;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
        BYTE* data = nullptr;
        DWORD maximum = 0, length = 0;
        if (SUCCEEDED(buffer->Lock(&data, &maximum, &length))) {
            const short* samples = (const short*)data;
            size_t count = length / sizeof(short);
            buffer_.insert(buffer_.end(), samples, samples + count);
            buffer->Unlock();
            done = true;
        }
        buffer->Release();
    }
    sample->Release();
    return done;
}

int AudioDecoder::Read(long long start, int length, void* destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reader_ || length <= 0 || channels_ <= 0) return 0;
    short* target = (short*)destination;
    long long available = (long long)buffer_.size() / channels_;
    if (start < buffer_start_ || start > buffer_start_ + available) {
        if (!Seek(start)) return 0;
    }
    for (int guard = 0; guard < 4096; guard++) {
        available = (long long)buffer_.size() / channels_;
        if (start + length <= buffer_start_ + available) break;
        if (!Extend()) break;
        if (buffer_.size() > kMaximumBuffer) {
            size_t drop = buffer_.size() - kMaximumBuffer / 2;
            drop -= drop % (size_t)channels_;
            buffer_.erase(buffer_.begin(), buffer_.begin() + (ptrdiff_t)drop);
            buffer_start_ += (long long)(drop / channels_);
        }
    }
    available = (long long)buffer_.size() / channels_;
    long long offset = start - buffer_start_;
    if (offset < 0 || offset >= available) return 0;
    long long usable = std::min<long long>(length, available - offset);
    memcpy(target, buffer_.data() + offset * channels_, (size_t)usable * channels_ * sizeof(short));
    if (usable < length) {
        memset(target + usable * channels_, 0,
               (size_t)(length - usable) * channels_ * sizeof(short));
    }
    return (int)length;
}

}
