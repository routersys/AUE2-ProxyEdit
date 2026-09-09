#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "cache2.h"

namespace pe {

enum PixelFormat {
    kFormatHalfFloat = 10,
    kFormatWide = 11,
    kFormatYc48 = 13,
    kFormatRgba = 28,
    kFormatBgra = 87,
    kFormatBgr = 88,
    kFormatYuy2 = 107,
};

void InitColorTables();

bool CacheImageToBgr(const CACHE_FILE_IMAGE& image, std::vector<unsigned char>& out, int& width,
                     int& height);

void BgraToYuy2(const unsigned char* source, int source_width, int source_height, int source_stride,
                unsigned char* destination, int width, int height, int stride);

void PlanarToYuy2(const unsigned char* luma, int luma_stride, const unsigned char* blue,
                  const unsigned char* red, int chroma_stride, int chroma_width, int chroma_height,
                  int source_width, int source_height, unsigned char* destination, int width,
                  int height, int stride);

void Yuy2ToBgrScaled(const unsigned char* source, int source_width, int source_height,
                     int source_stride, unsigned char* destination, int width, int height);

void FillBlackYuy2(unsigned char* destination, int width, int height, int stride);

}
