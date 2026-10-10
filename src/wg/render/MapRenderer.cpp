#include "wg/render/MapRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

#include "wg/core/Random.hpp"

#include "wg/core/VoxelVolume.hpp"
#include "wg/core/WorldState.hpp"
#include "wg/gen/Pipeline.hpp"

namespace wg {

namespace {

Rgb scale(Rgb c, float f) {
    const auto channel = [f](std::uint8_t v) { return static_cast<std::uint8_t>(std::clamp(v * f, 0.0f, 255.0f)); };
    return {channel(c.r), channel(c.g), channel(c.b)};
}

Rgb mix(Rgb a, Rgb b, float t) {
    const auto channel = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b)};
}

// Darker with depth; depths are in meters (from a few meters to kilometers).
Rgb waterColor(float depth) {
    return scale(blockInfo(Block::Water).color, static_cast<float>(1.0 - 0.7 * depth / (depth + 150.0)));
}

// Hypsometric tints, by elevation above sea level in meters.
Rgb terrainColor(float elevation) {
    struct Stop {
        float elevation;
        Rgb color;
    };
    static constexpr std::array<Stop, 6> kStops{{
        {0, {214, 201, 148}},
        {5, {96, 163, 72}},
        {150, {62, 124, 52}},
        {500, {122, 104, 74}},
        {1000, {132, 128, 124}},
        {1600, {245, 245, 250}},
    }};
    for (std::size_t i = 1; i < kStops.size(); ++i)
        if (elevation < kStops[i].elevation)
            return mix(kStops[i - 1].color, kStops[i].color,
                       (elevation - kStops[i - 1].elevation) / (kStops[i].elevation - kStops[i - 1].elevation));
    return kStops.back().color;
}

Rgb divergingColor(float t) { // t in [-1, 1]
    constexpr Rgb kNegative{59, 76, 192}, kZero{240, 240, 240}, kPositive{180, 4, 38};
    return t < 0.0f ? mix(kZero, kNegative, std::min(1.0f, -t)) : mix(kZero, kPositive, std::min(1.0f, t));
}

void setPixel(RgbaImage& image, int x, int y, Rgb c) {
    const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4;
    image.pixels[i + 0] = c.r;
    image.pixels[i + 1] = c.g;
    image.pixels[i + 2] = c.b;
    image.pixels[i + 3] = 255;
}

RgbaImage makeImage(int width, int height) {
    return {width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
}

} // namespace

std::vector<View> availableViews(const WorldState& state) {
    std::vector<View> views;
    if (state.hasParams())
        views.push_back({View::Kind::Parameters, {}});
    for (auto& name : state.layerNames())
        views.push_back({View::Kind::Layer, std::move(name)});
    if (state.hasVoxels())
        views.push_back({View::Kind::Voxels, {}});
    return views;
}

bool hasView(const WorldState& state, const View& view) {
    switch (view.kind) {
    case View::Kind::Voxels: return state.hasVoxels();
    case View::Kind::Parameters: return state.hasParams();
    default: return state.hasLayer(view.layer);
    }
}

View defaultView(const Snapshot& snapshot) {
    if (snapshot.voxelsChanged)
        return {View::Kind::Voxels, {}};
    if (!snapshot.changedLayers.empty())
        return {View::Kind::Layer, snapshot.changedLayers.front()};
    if (snapshot.paramsChanged)
        return {View::Kind::Parameters, {}};
    const auto views = availableViews(snapshot.state);
    return views.empty() ? View{} : views.back();
}

std::string_view colorMapName(ColorMap colorMap) {
    switch (colorMap) {
    case ColorMap::Auto: return "auto";
    case ColorMap::Grayscale: return "grayscale";
    default: return "?";
    }
}

ValueRange valueRange(const Grid2D<float>& grid) {
    const auto [min, max] = std::minmax_element(grid.data().begin(), grid.data().end());
    return grid.size() == 0 ? ValueRange{} : ValueRange{*min, *max};
}

Rgb materialColor(std::string_view name) {
    static const std::map<std::string_view, Rgb> kKnown{
        {"basalt", {62, 62, 68}},       {"granite", {205, 172, 160}}, {"gneiss", {150, 138, 150}},
        {"schist", {108, 128, 116}},    {"limestone", {222, 216, 194}}, {"sandstone", {214, 170, 108}},
        {"shale", {96, 90, 78}},        {"tuff", {192, 160, 108}},
    };
    if (const auto it = kKnown.find(name); it != kKnown.end())
        return it->second;
    const std::uint64_t h = hashString(name); // unknown material: any stable color
    return {static_cast<std::uint8_t>(60 + h % 160), static_cast<std::uint8_t>(60 + (h >> 8) % 160),
            static_cast<std::uint8_t>(60 + (h >> 16) % 160)};
}

LayerPalette::LayerPalette(const Grid2D<float>& layer, const LayerMeta& meta, ColorMap colorMap, double seaLevelM)
    : style_(colorMap == ColorMap::Grayscale ? LayerStyle::Scalar : meta.style), range_(valueRange(layer)),
      seaLevelM_(seaLevelM) {
    for (const auto& category : meta.categories)
        categories_.push_back(materialColor(category));
}

Rgb LayerPalette::color(float value) const {
    switch (style_) {
    case LayerStyle::Height:
        return value <= seaLevelM_ ? waterColor(static_cast<float>(seaLevelM_ - value))
                                   : terrainColor(static_cast<float>(value - seaLevelM_));
    case LayerStyle::Diverging: // each sign scaled to its own extent, 0 stays white
        if (value >= 0.0f)
            return divergingColor(range_.max > 0.0f ? value / range_.max : 0.0f);
        return divergingColor(range_.min < 0.0f ? -value / range_.min : 0.0f);
    case LayerStyle::Categorical: {
        const auto id = static_cast<std::size_t>(std::max(0.0f, std::round(value)));
        return id < categories_.size() ? categories_[id] : materialColor(std::to_string(id));
    }
    default: {
        const float span = range_.max - range_.min;
        const float t = span > 0.0f ? std::clamp((value - range_.min) / span, 0.0f, 1.0f) : 0.5f;
        const auto gray = static_cast<std::uint8_t>(std::lround(t * 255.0f));
        return {gray, gray, gray};
    }
    }
}

float hillshade(const Grid2D<float>& heights, int x, int z, double cellSize) {
    constexpr float kLx = -0.5f, kLy = 0.7f, kLz = -0.5f;
    const auto scaleFactor = static_cast<float>(1.0 / (2.0 * cellSize));
    const float dx = (heights.atClamped(x + 1, z) - heights.atClamped(x - 1, z)) * scaleFactor;
    const float dz = (heights.atClamped(x, z + 1) - heights.atClamped(x, z - 1)) * scaleFactor;
    // Normal (-dx, 1, -dz) / |n|, divided by kLy so flat ground gives 1.
    const float lambert = (-dx * kLx + kLy - dz * kLz) / (std::sqrt(dx * dx + 1.0f + dz * dz) * kLy);
    return std::clamp(lambert, 0.35f, 1.35f);
}

double cellSizeM(const WorldState& state, const Grid2D<float>& layer) {
    return state.info().widthM() / layer.width();
}

RgbaImage renderLayer(const WorldState& state, std::string_view name, ColorMap colorMap) {
    const auto& layer = state.layer(name);
    const LayerPalette palette(layer, state.layerMeta(name), colorMap, state.info().seaLevelM());
    const bool shade = colorMap == ColorMap::Auto && palette.shaded();
    const double cell = cellSizeM(state, layer);
    const float sea = static_cast<float>(state.info().seaLevelM());

    RgbaImage image = makeImage(layer.width(), layer.length());
    for (int z = 0; z < layer.length(); ++z) {
        for (int x = 0; x < layer.width(); ++x) {
            Rgb color = palette.color(layer.at(x, z));
            if (shade) // full relief shading on land, a hint of it under water to show the bathymetry
                color = scale(color, layer.at(x, z) > sea ? hillshade(layer, x, z, cell)
                                                          : 0.5f + 0.5f * hillshade(layer, x, z, cell));
            setPixel(image, x, z, color);
        }
    }
    return image;
}

RgbaImage renderVoxelsTopDown(const VoxelVolume& voxels) {
    Grid2D<float> top(voxels.width(), voxels.length());
    for (int z = 0; z < voxels.length(); ++z)
        for (int x = 0; x < voxels.width(); ++x)
            top.at(x, z) = static_cast<float>(voxels.topNonAir(x, z));

    RgbaImage image = makeImage(voxels.width(), voxels.length());
    for (int z = 0; z < voxels.length(); ++z) {
        for (int x = 0; x < voxels.width(); ++x) {
            const int y = static_cast<int>(top.at(x, z));
            const Block block = voxels.get(x, y, z);
            Rgb color;
            if (block == Block::Water) {
                int floor = y;
                while (floor > voxels.minY() && voxels.get(x, floor, z) == Block::Water)
                    --floor;
                color = waterColor(static_cast<float>(y - floor));
            } else {
                color = scale(blockInfo(block).color, hillshade(top, x, z, 1.0));
            }
            setPixel(image, x, z, color);
        }
    }
    return image;
}

RgbaImage renderView(const WorldState& state, const View& view, ColorMap colorMap) {
    if (!view.isImage())
        throw WorldError("'" + view.label() + "' cannot be rendered as an image");
    if (view.kind == View::Kind::Voxels)
        return renderVoxelsTopDown(state.voxels());
    return renderLayer(state, view.layer, colorMap);
}

} // namespace wg
