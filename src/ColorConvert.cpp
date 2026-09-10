#include "ColorConvert.h"

#include <string.h>

namespace pe {

namespace {

int g_luma_red[256], g_luma_green[256], g_luma_blue[256];
int g_blue_red[256], g_blue_green[256], g_blue_blue[256];
int g_red_red[256], g_red_green[256], g_red_blue[256];
int g_jpeg_luma[256], g_jpeg_luma_blue[256], g_jpeg_luma_red[256];
int g_jpeg_blue_blue[256], g_jpeg_blue_red[256];
int g_jpeg_red_blue[256], g_jpeg_red_red[256];
bool g_ready = false;

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

void InitColorTables() {
    if (g_ready) return;
    for (int index = 0; index < 256; index++) {
        g_luma_red[index] = (int)(0.1826 * 256.0 * index);
        g_luma_green[index] = (int)(0.6142 * 256.0 * index);
        g_luma_blue[index] = (int)(0.0620 * 256.0 * index) + (16 << 8);
        g_blue_red[index] = (int)(-0.1006 * 256.0 * index);
        g_blue_green[index] = (int)(-0.3386 * 256.0 * index);
        g_blue_blue[index] = (int)(0.4392 * 256.0 * index) + (128 << 8);
        g_red_red[index] = (int)(0.4392 * 256.0 * index);
        g_red_green[index] = (int)(-0.3989 * 256.0 * index);
        g_red_blue[index] = (int)(-0.0403 * 256.0 * index) + (128 << 8);
        double centered = index - 128.0;
        g_jpeg_luma[index] = (int)(0.8588 * 256.0 * index) + (16 << 8);
        g_jpeg_luma_blue[index] = (int)(-0.101503 * 256.0 * centered);
        g_jpeg_luma_red[index] = (int)(-0.182617 * 256.0 * centered);
        g_jpeg_blue_blue[index] = (int)(0.894787 * 256.0 * centered) + (128 << 8);
        g_jpeg_blue_red[index] = (int)(0.100766 * 256.0 * centered);
        g_jpeg_red_blue[index] = (int)(0.065864 * 256.0 * centered);
        g_jpeg_red_red[index] = (int)(0.900627 * 256.0 * centered) + (128 << 8);
    }
    g_ready = true;
}

void BgraToYuy2(const unsigned char* source, int source_width, int source_height, int source_stride,
                unsigned char* destination, int width, int height, int stride) {
    InitColorTables();
    if (source_width <= 0 || source_height <= 0 || width <= 1 || height <= 0) return;
    std::vector<int> columns = BuildColumnMap(source_width, width);
    const int pairs = (int)columns.size();
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
        const unsigned char* row = source + (size_t)mapped * source_stride;
        for (int index = 0; index < pairs; index++) {
            const unsigned char* pixel = row + (size_t)columns[(size_t)index] * 4;
            int blue = pixel[0], green = pixel[1], red = pixel[2];
            unsigned char luma = Clamp(g_luma_red[red] + g_luma_green[green] + g_luma_blue[blue]);
            target[index * 4 + 0] = luma;
            target[index * 4 + 1] = Clamp(g_blue_red[red] + g_blue_green[green] + g_blue_blue[blue]);
            target[index * 4 + 2] = luma;
            target[index * 4 + 3] = Clamp(g_red_red[red] + g_red_green[green] + g_red_blue[blue]);
        }
        built = (int)mapped;
        built_row = target;
    }
}

void PlanarToYuy2(const unsigned char* luma, int luma_stride, const unsigned char* blue,
                  const unsigned char* red, int chroma_stride, int chroma_width, int chroma_height,
                  int source_width, int source_height, unsigned char* destination, int width,
                  int height, int stride) {
    InitColorTables();
    if (source_width <= 0 || source_height <= 0 || width <= 1 || height <= 0) return;
    if (chroma_width <= 0 || chroma_height <= 0) return;
    std::vector<int> columns = BuildColumnMap(source_width, width);
    const int pairs = (int)columns.size();
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
            int column = columns[(size_t)index];
            int chroma_column = chroma_columns[(size_t)index];
            int blue_value = blue_line[chroma_column];
            int red_value = red_line[chroma_column];
            unsigned char converted = Clamp(g_jpeg_luma[luma_line[column]] +
                                            g_jpeg_luma_blue[blue_value] + g_jpeg_luma_red[red_value]);
            target[index * 4 + 0] = converted;
            target[index * 4 + 1] = Clamp(g_jpeg_blue_blue[blue_value] + g_jpeg_blue_red[red_value]);
            target[index * 4 + 2] = converted;
            target[index * 4 + 3] = Clamp(g_jpeg_red_blue[blue_value] + g_jpeg_red_red[red_value]);
        }
        built = (int)mapped;
        built_row = target;
    }
}

void Yuy2ToBgrScaled(const unsigned char* source, int source_width, int source_height,
                     int source_stride, unsigned char* destination, int width, int height) {
    if (source_width <= 1 || source_height <= 0 || width <= 0 || height <= 0) return;
    for (int y = 0; y < height; y++) {
        int top = (int)((long long)y * source_height / height);
        int bottom = (int)((long long)(y + 1) * source_height / height);
        if (bottom <= top) bottom = top + 1;
        if (bottom > source_height) bottom = source_height;
        unsigned char* target = destination + (size_t)y * width * 3;
        for (int x = 0; x < width; x++) {
            int left = (int)((long long)x * source_width / width);
            int right = (int)((long long)(x + 1) * source_width / width);
            if (right <= left) right = left + 1;
            if (right > source_width) right = source_width;
            int luma_total = 0, blue_total = 0, red_total = 0, luma_count = 0, chroma_count = 0;
            for (int row = top; row < bottom; row++) {
                const unsigned char* line = source + (size_t)row * source_stride;
                for (int column = left; column < right; column++) {
                    luma_total += line[column * 2];
                    luma_count++;
                    int pair = column & ~1;
                    blue_total += line[pair * 2 + 1];
                    red_total += line[pair * 2 + 3];
                    chroma_count++;
                }
            }
            if (luma_count == 0 || chroma_count == 0) continue;
            double luma = 1.1644 * ((double)luma_total / luma_count - 16.0);
            double blue = (double)blue_total / chroma_count - 128.0;
            double red = (double)red_total / chroma_count - 128.0;
            double to_red = 1.7927 * red;
            double to_green = -0.2132 * blue - 0.5329 * red;
            double to_blue = 2.1124 * blue;
            auto clamp = [](double value) {
                return (unsigned char)(value < 0 ? 0 : (value > 255 ? 255 : value));
            };
            target[x * 3 + 0] = clamp(luma + to_blue);
            target[x * 3 + 1] = clamp(luma + to_green);
            target[x * 3 + 2] = clamp(luma + to_red);
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
