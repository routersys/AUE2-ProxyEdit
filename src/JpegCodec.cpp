#include "JpegCodec.h"

#include <objbase.h>
#include <wincodec.h>

#include "ColorConvert.h"

namespace pe {

namespace {

struct ImagingContext {
    IWICImagingFactory* factory = nullptr;
    bool ready = false;
    bool owns_com = false;

    ~ImagingContext() {
        if (factory) {
            factory->Release();
            factory = nullptr;
        }
        if (owns_com) CoUninitialize();
    }
};

thread_local ImagingContext t_imaging;

IWICImagingFactory* Imaging() {
    if (!t_imaging.ready) {
        t_imaging.ready = true;
        const HRESULT prepared = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        t_imaging.owns_com = SUCCEEDED(prepared);
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

}

bool EncodeJpegPlanar(const unsigned char* luma, const unsigned char* blue, const unsigned char* red,
                      int width, int height, int quality, std::vector<unsigned char>& out) {
    IWICImagingFactory* imaging = Imaging();
    if (!imaging || width <= 1 || height <= 0) return false;
    const int chroma_width = width / 2;
    bool done = false;
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    IWICBitmapEncoder* encoder = nullptr;
    if (SUCCEEDED(imaging->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* options = nullptr;
        if (SUCCEEDED(encoder->CreateNewFrame(&frame, &options))) {
            PROPBAG2 names[2]{};
            VARIANT values[2]{};
            names[0].pstrName = (LPOLESTR)L"ImageQuality";
            values[0].vt = VT_R4;
            values[0].fltVal = (float)quality / 100.0f;
            names[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";
            values[1].vt = VT_UI1;
            values[1].bVal = (BYTE)WICJpegYCrCbSubsampling422;
            options->Write(2, names, values);
            IWICPlanarBitmapFrameEncode* planar = nullptr;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(frame->Initialize(options)) && SUCCEEDED(frame->SetSize(width, height)) &&
                SUCCEEDED(frame->SetPixelFormat(&format)) &&
                SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&planar)))) {
                WICBitmapPlane planes[3]{};
                planes[0].Format = GUID_WICPixelFormat8bppY;
                planes[0].pbBuffer = (BYTE*)luma;
                planes[0].cbStride = (UINT)width;
                planes[0].cbBufferSize = (UINT)((size_t)width * height);
                planes[1].Format = GUID_WICPixelFormat8bppCb;
                planes[1].pbBuffer = (BYTE*)blue;
                planes[1].cbStride = (UINT)chroma_width;
                planes[1].cbBufferSize = (UINT)((size_t)chroma_width * height);
                planes[2].Format = GUID_WICPixelFormat8bppCr;
                planes[2].pbBuffer = (BYTE*)red;
                planes[2].cbStride = (UINT)chroma_width;
                planes[2].cbBufferSize = (UINT)((size_t)chroma_width * height);
                if (SUCCEEDED(planar->WritePixels((UINT)height, planes, 3)) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
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
            if (planar) planar->Release();
            if (options) options->Release();
            if (frame) frame->Release();
        }
    }
    if (encoder) encoder->Release();
    stream->Release();
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
                frame->Release();
            }
            decoder->Release();
        }
    }
    stream->Release();
    return done;
}

}
