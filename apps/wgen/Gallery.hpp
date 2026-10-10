#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Gallery mode: generates many seeds of a config and assembles hillshaded
// thumbnails into one PNG sheet, captioned with seed and origin, to judge the
// variety of the islands at a glance.
struct GalleryOptions {
    std::filesystem::path configPath;
    std::filesystem::path outputPath;
    int count = 16;                   // seeds (per origin with "all")
    int firstSeed = 1;
    std::optional<std::string> origin; // forced origin.type, or "all" for one group per origin
    std::optional<int> resolution;    // overrides simulation.coarse_resolution (faster previews)
    int columns = 8;
    int thumbnail = 256;              // thumbnail width in pixels
};

// Returns 0 on success, prints errors and returns 1 otherwise.
int runGallery(const GalleryOptions& options);
