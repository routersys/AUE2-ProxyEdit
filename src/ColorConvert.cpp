#include "ColorConvert.h"

#include <string.h>

namespace pe {

namespace {

inline unsigned char Clamp(int value) {
    value >>= 8;
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (unsigned char)value;
}

std::vector<int> BuildColumnMap(int source_width, int width) {
    std::vector<int> map((size_t)(width / 2));
    for (int index = 0; index < width / 2; index++) {
        long long position = (long long)index * 2 * source_width / (width > 0 ? width : 1);
        if (position >= source_width) position = source_width - 1;
        map[(size_t)index] = (int)position;
    }
    return map;
}

}

void PlanarToYuy2(const unsigned char* luma, int luma_stride, const unsigned char* blue,
                  const unsigned char* red, int chroma_stride, int chroma_width, int chroma_height,
                  int source_width, int source_height, unsigned char* destination, int width,
                  int height, int stride) {
    if (source_width <= 0 || source_height <= 0 || width <= 1 || height <= 0) return;
    if (chroma_width <= 0 || chroma_height <= 0) return;
    std::vector<int> columns = BuildColumnMap(source_width, width);
    const int pairs = (int)columns.size();
    std::vector<int> luma_columns((size_t)pairs * 2);
    for (int index = 0; index < pairs * 2; index++) {
        long long position = (long long)index * source_width / (width > 0 ? width : 1);
        if (position >= source_width) position = source_width - 1;
        luma_columns[(size_t)index] = (int)position;
    }
    std::vector<int> chroma_columns((size_t)pairs);
    for (int index = 0; index < pairs; index++) {
        long long position = (long long)columns[(size_t)index] * chroma_width / source_width;
        if (position >= chroma_width) position = chroma_width - 1;
        chroma_columns[(size_t)index] = (int)position;
    }
    int built = -1;
    unsigned char* built_row = nullptr;
    for (int y = 0; y < height; y++) {
        long long mapped = (long long)y * source_height / height;
        if (mapped >= source_height) mapped = source_height - 1;
        unsigned char* target = destination + (size_t)y * stride;
        if ((int)mapped == built && built_row) {
            memcpy(target, built_row, (size_t)width * 2);
            continue;
        }
        long long chroma_row = mapped * chroma_height / source_height;
        if (chroma_row >= chroma_height) chroma_row = chroma_height - 1;
        const unsigned char* luma_line = luma + (ptrdiff_t)mapped * luma_stride;
        const unsigned char* blue_line = blue + (ptrdiff_t)chroma_row * chroma_stride;
        const unsigned char* red_line = red + (ptrdiff_t)chroma_row * chroma_stride;
        for (int index = 0; index < pairs; index++) {
            const int chroma_column = chroma_columns[(size_t)index];
            target[index * 4 + 0] = luma_line[luma_columns[(size_t)index * 2]];
            target[index * 4 + 1] = blue_line[chroma_column];
            target[index * 4 + 2] = luma_line[luma_columns[(size_t)index * 2 + 1]];
            target[index * 4 + 3] = red_line[chroma_column];
        }
        built = (int)mapped;
        built_row = target;
    }
}

void Yuy2ToPlanarScaled(const unsigned char* source, int source_width, int source_height,
                        int source_stride, unsigned char* luma, unsigned char* blue,
                        unsigned char* red, int width, int height) {
    if (source_width <= 1 || source_height <= 0 || width <= 1 || height <= 0) return;
    const int chroma_width = width / 2;
    const int source_pairs = source_width / 2;
    for (int y = 0; y < height; y++) {
        int top = (int)((long long)y * source_height / height);
        int bottom = (int)((long long)(y + 1) * source_height / height);
        if (bottom <= top) bottom = top + 1;
        if (bottom > source_height) bottom = source_height;
        unsigned char* luma_row = luma + (size_t)y * width;
        unsigned char* blue_row = blue + (size_t)y * chroma_width;
        unsigned char* red_row = red + (size_t)y * chroma_width;

        for (int x = 0; x < width; x++) {
            int left = (int)((long long)x * source_width / width);
            int right = (int)((long long)(x + 1) * source_width / width);
            if (right <= left) right = left + 1;
            if (right > source_width) right = source_width;
            int total = 0;
            int count = 0;
            for (int row = top; row < bottom; row++) {
                const unsigned char* line = source + (size_t)row * source_stride;
                for (int column = left; column < right; column++) {
                    total += line[(size_t)column * 2];
                    count++;
                }
            }
            luma_row[x] = count > 0 ? (unsigned char)(total / count) : 0;
        }

        for (int x = 0; x < chroma_width; x++) {
            int left = (int)((long long)x * source_pairs / chroma_width);
            int right = (int)((long long)(x + 1) * source_pairs / chroma_width);
            if (right <= left) right = left + 1;
            if (right > source_pairs) right = source_pairs;
            int blue_total = 0;
            int red_total = 0;
            int count = 0;
            for (int row = top; row < bottom; row++) {
                const unsigned char* line = source + (size_t)row * source_stride;
                for (int pair = left; pair < right; pair++) {
                    blue_total += line[(size_t)pair * 4 + 1];
                    red_total += line[(size_t)pair * 4 + 3];
                    count++;
                }
            }
            blue_row[x] = count > 0 ? (unsigned char)(blue_total / count) : 128;
            red_row[x] = count > 0 ? (unsigned char)(red_total / count) : 128;
        }
    }
}

void FillBlackYuy2(unsigned char* destination, int width, int height, int stride) {
    for (int y = 0; y < height; y++) {
        unsigned char* target = destination + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            target[x * 2 + 0] = 16;
            target[x * 2 + 1] = 128;
        }
    }
}

}
