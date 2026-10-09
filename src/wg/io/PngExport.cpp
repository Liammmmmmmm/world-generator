#include "wg/io/PngExport.hpp"

#include <stdexcept>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

namespace wg {

void writePng(const RgbaImage& image, const std::filesystem::path& path) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    if (!stbi_write_png(path.string().c_str(), image.width, image.height, 4, image.pixels.data(), image.width * 4))
        throw std::runtime_error("cannot write PNG '" + path.string() + "'");
}

} // namespace wg
