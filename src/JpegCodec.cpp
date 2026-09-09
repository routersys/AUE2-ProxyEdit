#include "JpegCodec.h"

#include <objbase.h>
#include <wincodec.h>

#include "ColorConvert.h"

namespace pe {

namespace {

struct ImagingContext {
    IWICImagingFactory* factory = nullptr;
    bool ready = false;
};

thread_local ImagingContext t_imaging;

IWICImagingFactory* Imaging() {
    if (!t_imaging.ready) {
        t_imaging.ready = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&t_imaging.factory));
    }
    return t_imaging.factory;
}

bool DecodePlanar(IWICBitmapFrameDecode* frame, int proxy_width, int proxy_height,
                  unsigned char* destination, int width, int height, int stride,
                  std::vector<unsigned char>& scratch) {
    IWICPlanarBitmapSourceTransform* planar = nullptr;
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&planar)))) return false;
    bool done = false;
    WICPixelFormatGUID formats[3] = {GUID_WICPixelFormat8bppY, GUID_WICPixelFormat8bppCb,
                                     GUID_WICPixelFormat8bppCr};
    WICBitmapPlaneDescription description[3]{};
    UINT requested_width = (UINT)proxy_width;
    UINT requested_height = (UINT)proxy_height;
    BOOL supported = FALSE;
    if (SUCCEEDED(planar->DoesSupportTransform(&requested_width, &requested_height,
                                               WICBitmapTransformRotate0, WICPlanarOptionsDefault,
                                               formats, description, 3, &supported)) &&
        supported && (int)requested_width == proxy_width && (int)requested_height == proxy_height) {
        UINT luma_stride = (description[0].Width + 15) & ~15u;
        UINT chroma_stride = (description[1].Width + 15) & ~15u;
        size_t needed = (size_t)luma_stride * description[0].Height +
                        (size_t)chroma_stride * description[1].Height +
                        (size_t)chroma_stride * description[2].Height;
        scratch.resize(needed);
        WICBitmapPlane planes[3]{};
        planes[0].Format = formats[0];
        planes[0].pbBuffer = scratch.data();
        planes[0].cbStride = luma_stride;
        planes[0].cbBufferSize = (UINT)((size_t)luma_stride * description[0].Height);
        planes[1].Format = formats[1];
        planes[1].pbBuffer = planes[0].pbBuffer + planes[0].cbBufferSize;
        planes[1].cbStride = chroma_stride;
        planes[1].cbBufferSize = (UINT)((size_t)chroma_stride * description[1].Height);
        planes[2].Format = formats[2];
        planes[2].pbBuffer = planes[1].pbBuffer + planes[1].cbBufferSize;
        planes[2].cbStride = chroma_stride;
        planes[2].cbBufferSize = (UINT)((size_t)chroma_stride * description[2].Height);
        if (SUCCEEDED(planar->CopyPixels(nullptr, requested_width, requested_height,
                                         WICBitmapTransformRotate0, WICPlanarOptionsDefault, planes, 3))) {
            PlanarToYuy2(planes[0].pbBuffer, (int)luma_stride, planes[1].pbBuffer, planes[2].pbBuffer,
                         (int)chroma_stride, (int)description[1].Width, (int)description[1].Height,
                         proxy_width, proxy_height, destination, width, height, stride);
            done = true;
        }
    }
    planar->Release();
    return done;
}

bool DecodeThroughBgra(IWICImagingFactory* imaging, IWICBitmapFrameDecode* frame, int proxy_width,
                       int proxy_height, unsigned char* destination, int width, int height, int stride,
                       std::vector<unsigned char>& scratch) {
    IWICBitmapSource* source = frame;
    IWICBitmapScaler* scaler = nullptr;
    UINT actual_width = 0, actual_height = 0;
    frame->GetSize(&actual_width, &actual_height);
    if ((int)actual_width != proxy_width || (int)actual_height != proxy_height) {
        if (SUCCEEDED(imaging->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, proxy_width, proxy_height,
                                         WICBitmapInterpolationModeLinear))) {
            source = scaler;
        }
    }
    bool done = false;
    IWICBitmapSource* converted = nullptr;
    if (SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source, &converted))) {
        const int row = proxy_width * 4;
        scratch.resize((size_t)row * proxy_height);
        if (SUCCEEDED(converted->CopyPixels(nullptr, row, (UINT)scratch.size(), scratch.data()))) {
            BgraToYuy2(scratch.data(), proxy_width, proxy_height, row, destination, width, height, stride);
            done = true;
        }
        converted->Release();
    }
    if (scaler) scaler->Release();
    return done;
}

}

bool EncodeJpeg(const unsigned char* bgr, int width, int height, int out_width, int out_height,
                int quality, std::vector<unsigned char>& out) {
    IWICImagingFactory* imaging = Imaging();
    if (!imaging) return false;
    IWICBitmap* bitmap = nullptr;
    if (FAILED(imaging->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat24bppBGR, width * 3,
                                               (UINT)((size_t)width * height * 3), (BYTE*)bgr, &bitmap))) {
        return false;
    }
    IWICBitmapSource* source = bitmap;
    IWICBitmapScaler* scaler = nullptr;
    if (out_width != width || out_height != height) {
        if (SUCCEEDED(imaging->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(bitmap, out_width, out_height, WICBitmapInterpolationModeFant))) {
            source = scaler;
        }
    }
    bool done = false;
    IStream* stream = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        IWICBitmapEncoder* encoder = nullptr;
        if (SUCCEEDED(imaging->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* options = nullptr;
            if (SUCCEEDED(encoder->CreateNewFrame(&frame, &options))) {
                PROPBAG2 option{};
                option.pstrName = (LPOLESTR)L"ImageQuality";
                VARIANT value{};
                value.vt = VT_R4;
                value.fltVal = (float)quality / 100.0f;
                options->Write(1, &option, &value);
                if (SUCCEEDED(frame->Initialize(options)) &&
                    SUCCEEDED(frame->SetSize(out_width, out_height))) {
                    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
                    frame->SetPixelFormat(&format);
                    if (SUCCEEDED(frame->WriteSource(source, nullptr)) && SUCCEEDED(frame->Commit()) &&
                        SUCCEEDED(encoder->Commit())) {
                        STATSTG status{};
                        HGLOBAL memory = nullptr;
                        if (SUCCEEDED(stream->Stat(&status, STATFLAG_NONAME)) &&
                            SUCCEEDED(GetHGlobalFromStream(stream, &memory))) {
                            SIZE_T used = (SIZE_T)status.cbSize.QuadPart;
                            if (used > 0 && used <= GlobalSize(memory)) {
                                void* address = GlobalLock(memory);
                                out.assign((unsigned char*)address, (unsigned char*)address + used);
                                GlobalUnlock(memory);
                                done = true;
                            }
                        }
                    }
                }
                if (options) options->Release();
                if (frame) frame->Release();
            }
        }
        if (encoder) encoder->Release();
        stream->Release();
    }
    if (scaler) scaler->Release();
    bitmap->Release();
    return done;
}

bool DecodeJpegToYuy2(const unsigned char* data, size_t size, int proxy_width, int proxy_height,
                      unsigned char* destination, int width, int height, int stride) {
    IWICImagingFactory* imaging = Imaging();
    if (!imaging) return false;
    IWICStream* stream = nullptr;
    if (FAILED(imaging->CreateStream(&stream))) return false;
    bool done = false;
    if (SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size))) {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(imaging->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                       &decoder))) {
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                thread_local std::vector<unsigned char> scratch;
                done = DecodePlanar(frame, proxy_width, proxy_height, destination, width, height, stride,
                                    scratch);
                if (!done) {
                    done = DecodeThroughBgra(imaging, frame, proxy_width, proxy_height, destination, width,
                                             height, stride, scratch);
                }
                frame->Release();
            }
            decoder->Release();
        }
    }
    stream->Release();
    return done;
}

}
