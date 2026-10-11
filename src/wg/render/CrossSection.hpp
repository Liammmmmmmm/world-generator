#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wg/render/MapRenderer.hpp"

namespace wg {

class WorldState;

// A vertical line through the map: center (meters from the map center) and
// direction. The section spans the whole map along that line.
struct CrossSection {
    double x = 0.0, y = 0.0;
    double angleDeg = 0.0;
    double exaggeration = 4.0; // vertical scale relative to the horizontal one
};

struct CrossSectionImage {
    RgbaImage image;                     // materials below the surface, water, transparent air
    std::vector<std::int16_t> materials; // material id per pixel, -1 for water or air
    std::vector<std::string> materialNames;
    double startX = 0.0, startY = 0.0;   // ends of the line, clipped to the map (meters)
    double endX = 0.0, endY = 0.0;
    double metersPerPixelX = 0.0;        // along the line
    double metersPerPixelZ = 0.0;        // vertical (= metersPerPixelX / exaggeration)
    double zTop = 0.0;                   // altitude of the top row (m)
};

// Whether the state has what a section needs: parameters and a heightmap.
bool canRenderCrossSection(const WorldState& state);

// Exaggeration that fits the relief along the line (with sea level and some
// depth below the lowest point) in an image of this size.
double fitCrossSectionExaggeration(const WorldState& state, const CrossSection& section, int width, int height);

// Rock in place along the section, from the geology lookup (strata and
// folds), below the surface "h".
CrossSectionImage renderCrossSection(const WorldState& state, const CrossSection& section, int width, int height);

} // namespace wg
