#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace pe {

bool EncodeJpegPlanar(const unsigned char* luma, const unsigned char* blue, const unsigned char* red,
                      int width, int height, int quality, std::vector<unsigned char>& out);

bool DecodeJpegToYuy2(const unsigned char* data, size_t size, int proxy_width, int proxy_height,
                      unsigned char* destination, int width, int height, int stride);

}
