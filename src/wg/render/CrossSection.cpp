#include "wg/render/CrossSection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "wg/core/WorldState.hpp"
#include "wg/island/Geology.hpp"
#include "wg/island/IslandLayers.hpp"

namespace wg {

namespace {

struct Line {
    double tMin = 0.0, tMax = 0.0, dx = 1.0, dy = 0.0;
};

// The line, clipped to the map rectangle (slab method).
Line clipLine(const CrossSection& section, double widthM, double lengthM) {
    Line line;
    const double a = section.angleDeg * std::numbers::pi / 180.0;
    line.dx = std::cos(a), line.dy = std::sin(a);
    double tMin = -std::numeric_limits<double>::infinity(), tMax = std::numeric_limits<double>::infinity();
    const auto clip = [&](double origin, double direction, double half) {
        if (std::abs(direction) < 1e-12)
            return;
        const double t0 = (-half - origin) / direction, t1 = (half - origin) / direction;
        tMin = std::max(tMin, std::min(t0, t1));
        tMax = std::min(tMax, std::max(t0, t1));
    };
    clip(section.x, line.dx, widthM / 2), clip(section.y, line.dy, lengthM / 2);
    if (tMax > tMin)
        line.tMin = tMin, line.tMax = tMax; // else: the center is off the map
    return line;
}

} // namespace

double fitCrossSectionExaggeration(const WorldState& state, const CrossSection& section, int width, int height) {
    const double widthM = state.info().widthM(), lengthM = state.info().lengthM();
    const double sea = state.info().seaLevelM();
    const auto& h = state.layer(layers::kHeight);
    const Line line = clipLine(section, widthM, lengthM);
    double lowest = sea, highest = sea;
    for (int c = 0; c < width; ++c) {
        const double t = line.tMin + (c + 0.5) * (line.tMax - line.tMin) / width;
        const double z = sampleCentered(h, section.x + t * line.dx, section.y + t * line.dy, widthM, lengthM);
        lowest = std::min(lowest, z), highest = std::max(highest, z);
    }
    // The relief, plus a quarter of it below the lowest point to show the rock.
    const double range = std::max(1.0, (highest - lowest) * 1.25) / 0.92; // the top row keeps an 8 % margin
    const double metersPerPixelX = std::max(line.tMax - line.tMin, 1e-6) / width;
    return metersPerPixelX / (range / height);
}

bool canRenderCrossSection(const WorldState& state) {
    return state.hasParams() && state.hasLayer(layers::kHeight);
}

CrossSectionImage renderCrossSection(const WorldState& state, const CrossSection& section, int width, int height) {
    const auto* hInitial = state.hasLayer(layers::kHeightInitial) ? &state.layer(layers::kHeightInitial) : nullptr;
    const Geology geology(state.params().values, hInitial);
    const double widthM = state.info().widthM(), lengthM = state.info().lengthM();
    const double sea = state.info().seaLevelM();
    const auto& h = state.layer(layers::kHeight);

    const Line line = clipLine(section, widthM, lengthM);
    const double tMin = line.tMin, tMax = line.tMax, dx = line.dx, dy = line.dy;

    CrossSectionImage out;
    out.materialNames = geology.materialNames();
    out.startX = section.x + tMin * dx, out.startY = section.y + tMin * dy;
    out.endX = section.x + tMax * dx, out.endY = section.y + tMax * dy;
    out.metersPerPixelX = std::max(tMax - tMin, 1e-6) / width;
    out.metersPerPixelZ = out.metersPerPixelX / std::max(section.exaggeration, 1e-6);
    out.image = {width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 0)};
    out.materials.assign(static_cast<std::size_t>(width) * height, -1);

    // Surface along the line; the top row leaves a margin above the highest point.
    std::vector<double> surface(width), px(width), py(width);
    double highest = sea;
    for (int c = 0; c < width; ++c) {
        const double t = tMin + (c + 0.5) * out.metersPerPixelX;
        px[c] = section.x + t * dx, py[c] = section.y + t * dy;
        surface[c] = sampleCentered(h, px[c], py[c], widthM, lengthM);
        highest = std::max(highest, surface[c]);
    }
    out.zTop = highest + 0.08 * height * out.metersPerPixelZ;

    const Rgb water = blockInfo(Block::Water).color;
    for (int c = 0; c < width; ++c) {
        for (int r = 0; r < height; ++r) {
            const double z = out.zTop - (r + 0.5) * out.metersPerPixelZ;
            const std::size_t k = static_cast<std::size_t>(r) * width + c;
            Rgb color;
            if (z > surface[c]) {
                if (z > sea)
                    continue; // air: transparent
                color = water;
            } else {
                const int id = geology.material(px[c], py[c], z);
                out.materials[k] = static_cast<std::int16_t>(id);
                color = materialColor(out.materialNames[id]);
                if (surface[c] - z < out.metersPerPixelZ * 1.5) // outline the ground surface
                    color = {static_cast<std::uint8_t>(color.r / 2), static_cast<std::uint8_t>(color.g / 2),
                             static_cast<std::uint8_t>(color.b / 2)};
            }
            out.image.pixels[k * 4 + 0] = color.r;
            out.image.pixels[k * 4 + 1] = color.g;
            out.image.pixels[k * 4 + 2] = color.b;
            out.image.pixels[k * 4 + 3] = 255;
        }
    }
    return out;
}

} // namespace wg
