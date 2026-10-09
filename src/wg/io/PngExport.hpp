#pragma once

#include <filesystem>

#include "wg/render/MapRenderer.hpp"

namespace wg {

void writePng(const RgbaImage& image, const std::filesystem::path& path);

} // namespace wg
