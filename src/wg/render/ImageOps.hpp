#pragma once

#include <string_view>

#include "wg/core/Block.hpp"
#include "wg/render/MapRenderer.hpp"

namespace wg {

// Small image utilities (gallery sheets, captions).

RgbaImage makeRgbaImage(int width, int height, Rgb fill);
// Box-filter downscale (or nearest upscale) to the given size.
RgbaImage resizeImage(const RgbaImage& image, int width, int height);
// Copies `source` into `target` with its top-left corner at (x, y), clipped.
void blit(RgbaImage& target, const RgbaImage& source, int x, int y);
// Draws ASCII text with a small built-in font (stb_easy_font), `scale` pixels per font unit.
void drawText(RgbaImage& target, int x, int y, std::string_view text, Rgb color, int scale = 2);
// Width in pixels of `text` drawn by drawText at that scale.
int textWidth(std::string_view text, int scale = 2);

} // namespace wg
