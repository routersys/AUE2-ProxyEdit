#include "MediaDecoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>

namespace pe {

namespace {

std::atomic<int> g_started{0};
const unsigned kFourccNv12 = 0x3231564E;
const unsigned kFourccYuy2 = 0x32595559;

void CopyNv12(const BYTE* source, int pitch, int source_width, int source_height,
              unsigned char* destination, int stride, int rows) {
    const BYTE* luma = source;
    const BYTE* chroma = source + (ptrdiff_t)pitch * source_height;
    int usable = source_width;
    if (usable * 2 > stride) usable = stride / 2;
    for (int y = 0; y < rows; y++) {
        const BYTE* luma_row = luma + (ptrdiff_t)y * pitch;
        const BYTE* chroma_row = chroma + (ptrdiff_t)(y / 2) * pitch;
        unsigned char* target = destination + (size_t)y * stride;
        for (int x = 0; x + 1 < usable; x += 2) {
            target[x * 2 + 0] = luma_row[x];
            target[x * 2 + 1] = chroma_row[x];
            target[x * 2 + 2] = luma_row[x + 1];
            target[x * 2 + 3] = chroma_row[x + 1];
        }
    }
}

void CopyP010(const BYTE* source, int pitch, int source_width, int source_height,
              unsigned char* destination, int stride, int rows) {
    const BYTE* luma = source;
    const BYTE* chroma = source + (ptrdiff_t)pitch * source_height;
    int usable = source_width;
    if (usable * 2 > stride) usable = stride / 2;
    for (int y = 0; y < rows; y++) {
        const unsigned short* luma_row = (const unsigned short*)(luma + (ptrdiff_t)y * pitch);
        const unsigned short* chroma_row = (const unsigned short*)(chroma + (ptrdiff_t)(y / 2) * pitch);
        unsigned char* target = destination + (size_t)y * stride;
        for (int x = 0; x + 1 < usable; x += 2) {
            target[x * 2 + 0] = (unsigned char)(luma_row[x] >> 8);
            target[x * 2 + 1] = (unsigned char)(chroma_row[x] >> 8);
            target[x * 2 + 2] = (unsigned char)(luma_row[x + 1] >> 8);
            target[x * 2 + 3] = (unsigned char)(chroma_row[x + 1] >> 8);
        }
    }
}

void CopyYuy2(const BYTE* source, int pitch, int source_width, unsigned char* destination, int stride,
              int rows) {
    int bytes = source_width * 2 < stride ? source_width * 2 : stride;
    for (int y = 0; y < rows; y++) {
        memcpy(destination + (size_t)y * stride, source + (ptrdiff_t)y * pitch, (size_t)bytes);
    }
}

}

bool StartMediaFoundation() {
    if (g_started.load() > 0) return true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    g_started.store(1);
    return true;
}

void StopMediaFoundation() {
    if (g_started.exchange(0) > 0) MFShutdown();
}

MediaDecoder::~MediaDecoder() {
    Close();
}

bool MediaDecoder::Open(const std::wstring& path) {
    Close();
    if (!StartMediaFoundation()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 4))) return false;
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    HRESULT result = MFCreateSourceReaderFromURL(path.c_str(), attributes, &reader_);
    attributes->Release();
    if (FAILED(result) || !reader_) return false;

    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    IMFMediaType* native = nullptr;
    if (SUCCEEDED(reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)) && native) {
        GUID subtype{};
        if (SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            ten_bit_ = subtype.Data1 == 0x30313050 || subtype.Data1 == 0x30313259;
        }
        native->Release();
    }

    IMFMediaType* wanted = nullptr;
    if (FAILED(MFCreateMediaType(&wanted))) return false;
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    result = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted);
    wanted->Release();
    if (FAILED(result)) return false;

    IMFMediaType* current = nullptr;
    if (SUCCEEDED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) && current) {
        UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
        MFGetAttributeRatio(current, MF_MT_FRAME_RATE, &numerator, &denominator);
        GUID subtype{};
        current->GetGUID(MF_MT_SUBTYPE, &subtype);
        fourcc_ = subtype.Data1;
        width_ = (int)width;
        height_ = (int)height;
        rate_ = (int)numerator;
        scale_ = (int)denominator;
        current->Release();
    }
    if (width_ <= 0 || height_ <= 0 || rate_ <= 0 || scale_ <= 0) return false;

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION,
                                                    &duration)) &&
        duration.vt == VT_UI8) {
        double seconds = (double)duration.uhVal.QuadPart / 10000000.0;
        frame_count_ = (long long)(seconds * (double)rate_ / (double)scale_ + 0.5);
    }
    PropVariantClear(&duration);
    if (frame_count_ <= 0) frame_count_ = 1;
    next_index_ = -1;
    return true;
}

void MediaDecoder::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reader_) {
        reader_->Release();
        reader_ = nullptr;
    }
    next_index_ = -1;
}

bool MediaDecoder::IsOpen() const {
    return reader_ != nullptr;
}

long long MediaDecoder::FrameToTime(long long index) const {
    if (rate_ <= 0) return 0;
    return (long long)((double)index * (double)scale_ / (double)rate_ * 10000000.0 + 0.5);
}

bool MediaDecoder::Seek(long long index) {
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = FrameToTime(index);
    HRESULT result = reader_->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    next_index_ = index;
    return SUCCEEDED(result);
}

bool MediaDecoder::ReadFrameYuy2(long long index, unsigned char* destination, int stride, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reader_) return false;
    if (index != next_index_ && !Seek(index)) return false;
    const long long wanted = FrameToTime(index);
    const long long step = FrameToTime(1);
    for (int guard = 0; guard < 900; guard++) {
        DWORD stream = 0, flags = 0;
        LONGLONG stamp = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &stamp,
                                       &sample))) {
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) sample->Release();
            return false;
        }
        if (!sample) continue;
        if (stamp + step <= wanted - step / 2) {
            sample->Release();
            continue;
        }
        IMFMediaBuffer* buffer = nullptr;
        bool done = false;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* scan = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            IMF2DBuffer* planar = nullptr;
            bool locked = false;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&planar)))) {
                if (SUCCEEDED(planar->Lock2D(&scan, &pitch))) locked = true;
            }
            if (!locked) {
                DWORD maximum = 0;
                if (SUCCEEDED(buffer->Lock(&scan, &maximum, &length))) {
                    pitch = (fourcc_ == kFourccYuy2) ? width_ * 2 : width_;
                } else {
                    scan = nullptr;
                }
            }
            if (length == 0) buffer->GetCurrentLength(&length);
            if (scan) {
                int rows = height_ < height ? height_ : height;
                if (fourcc_ == kFourccYuy2) {
                    CopyYuy2(scan, pitch, width_, destination, stride, rows);
                } else {
                    long long expected = (long long)(pitch < 0 ? -pitch : pitch) * height_ * 3 / 2;
                    if (expected > 0 && (long long)length >= expected * 7 / 4) {
                        CopyP010(scan, pitch, width_, height_, destination, stride, rows);
                    } else {
                        CopyNv12(scan, pitch, width_, height_, destination, stride, rows);
                    }
                }
                done = true;
                if (locked) planar->Unlock2D();
                else buffer->Unlock();
            }
            if (planar) planar->Release();
            buffer->Release();
        }
        sample->Release();
        if (done) next_index_ = index + 1;
        return done;
    }
    return false;
}

}
