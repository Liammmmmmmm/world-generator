#include "wg/render/ImageOps.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <stb_easy_font.h>

namespace wg {

RgbaImage makeRgbaImage(int width, int height, Rgb fill) {
    RgbaImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (std::size_t k = 0; k < image.pixels.size(); k += 4) {
        image.pixels[k + 0] = fill.r;
        image.pixels[k + 1] = fill.g;
        image.pixels[k + 2] = fill.b;
        image.pixels[k + 3] = 255;
    }
    return image;
}

RgbaImage resizeImage(const RgbaImage& image, int width, int height) {
    RgbaImage out{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (int y = 0; y < height; ++y) {
        const int y0 = y * image.height / height, y1 = std::max(y0 + 1, (y + 1) * image.height / height);
        for (int x = 0; x < width; ++x) {
            const int x0 = x * image.width / width, x1 = std::max(x0 + 1, (x + 1) * image.width / width);
            unsigned sum[4] = {};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += image.pixels[(static_cast<std::size_t>(sy) * image.width + sx) * 4 + c];
            const unsigned count = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c)
                out.pixels[(static_cast<std::size_t>(y) * width + x) * 4 + c] = static_cast<std::uint8_t>(sum[c] / count);
        }
    }
    return out;
}

void blit(RgbaImage& target, const RgbaImage& source, int x, int y) {
    for (int sy = 0; sy < source.height; ++sy) {
        const int ty = y + sy;
        if (ty < 0 || ty >= target.height)
            continue;
        for (int sx = 0; sx < source.width; ++sx) {
            const int tx = x + sx;
            if (tx < 0 || tx >= target.width)
                continue;
            std::copy_n(&source.pixels[(static_cast<std::size_t>(sy) * source.width + sx) * 4], 4,
                        &target.pixels[(static_cast<std::size_t>(ty) * target.width + tx) * 4]);
        }
    }
}

int textWidth(std::string_view text, int scale) {
    std::string buffer(text);
    return stb_easy_font_width(buffer.data()) * scale;
}

void drawText(RgbaImage& target, int x, int y, std::string_view text, Rgb color, int scale) {
    // stb_easy_font emits axis-aligned quads (4 vertices of 16 bytes: x, y, z, color).
    std::string buffer(text);
    std::vector<char> vertices(buffer.size() * 270 + 64);
    unsigned char rgba[4] = {color.r, color.g, color.b, 255};
    const int quads = stb_easy_font_print(0, 0, buffer.data(), rgba, vertices.data(), static_cast<int>(vertices.size()));
    for (int q = 0; q < quads; ++q) {
        const float* v = reinterpret_cast<const float*>(vertices.data() + q * 64);
        const float minX = std::min({v[0], v[4], v[8], v[12]}), maxX = std::max({v[0], v[4], v[8], v[12]});
        const float minY = std::min({v[1], v[5], v[9], v[13]}), maxY = std::max({v[1], v[5], v[9], v[13]});
        for (int py = static_cast<int>(minY * scale); py < static_cast<int>(maxY * scale); ++py) {
            for (int px = static_cast<int>(minX * scale); px < static_cast<int>(maxX * scale); ++px) {
                const int tx = x + px, ty = y + py;
                if (tx < 0 || ty < 0 || tx >= target.width || ty >= target.height)
                    continue;
                std::uint8_t* p = &target.pixels[(static_cast<std::size_t>(ty) * target.width + tx) * 4];
                p[0] = color.r, p[1] = color.g, p[2] = color.b, p[3] = 255;
            }
        }
    }
}

} // namespace wg
