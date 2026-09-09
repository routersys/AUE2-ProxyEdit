#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace pe {

bool EncodeJpeg(const unsigned char* bgr, int width, int height, int out_width, int out_height,
                int quality, std::vector<unsigned char>& out);

bool DecodeJpegToYuy2(const unsigned char* data, size_t size, int proxy_width, int proxy_height,
                      unsigned char* destination, int width, int height, int stride);

}
