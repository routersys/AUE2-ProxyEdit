#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace pe {

void PlanarToYuy2(const unsigned char* luma, int luma_stride, const unsigned char* blue,
                  const unsigned char* red, int chroma_stride, int chroma_width, int chroma_height,
                  int source_width, int source_height, unsigned char* destination, int width,
                  int height, int stride);

void Yuy2ToPlanarScaled(const unsigned char* source, int source_width, int source_height,
                        int source_stride, unsigned char* luma, unsigned char* blue,
                        unsigned char* red, int width, int height);

void FillBlackYuy2(unsigned char* destination, int width, int height, int stride);

}
