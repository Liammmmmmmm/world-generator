#include "wg/render/MapRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

Rgb waterColor(float depth) {
    return scale(blockInfo(Block::Water).color, std::max(0.35f, 1.0f - depth / 48.0f));
}

Rgb terrainColor(float height, int seaLevel) {
    if (height <= static_cast<float>(seaLevel))
        return waterColor(static_cast<float>(seaLevel) - height);

    struct Stop {
        float elevation;
        Rgb color;
    };
    static constexpr std::array<Stop, 6> kStops{{
        {0, {214, 201, 148}},
        {4, {96, 163, 72}},
        {35, {62, 124, 52}},
        {70, {122, 104, 74}},
        {110, {132, 128, 124}},
        {150, {245, 245, 250}},
    }};
    const float e = height - static_cast<float>(seaLevel);
    for (std::size_t i = 1; i < kStops.size(); ++i)
        if (e < kStops[i].elevation)
            return mix(kStops[i - 1].color, kStops[i].color,
                       (e - kStops[i - 1].elevation) / (kStops[i].elevation - kStops[i - 1].elevation));
    return kStops.back().color;
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
    for (auto& name : state.layerNames())
        views.push_back({View::Kind::Layer, std::move(name)});
    if (state.hasVoxels())
        views.push_back({View::Kind::Voxels, {}});
    return views;
}

bool hasView(const WorldState& state, const View& view) {
    return view.kind == View::Kind::Voxels ? state.hasVoxels() : state.hasLayer(view.layer);
}

View defaultView(const Snapshot& snapshot) {
    if (snapshot.voxelsChanged)
        return {View::Kind::Voxels, {}};
    if (!snapshot.changedLayers.empty())
        return {View::Kind::Layer, snapshot.changedLayers.front()};
    const auto views = availableViews(snapshot.state);
    return views.empty() ? View{} : views.back();
}

std::string_view colorMapName(ColorMap colorMap) {
    switch (colorMap) {
    case ColorMap::Terrain: return "terrain";
    case ColorMap::Grayscale: return "grayscale";
    default: return "?";
    }
}

ValueRange valueRange(const Grid2D<float>& grid) {
    const auto [min, max] = std::minmax_element(grid.data().begin(), grid.data().end());
    return grid.size() == 0 ? ValueRange{} : ValueRange{*min, *max};
}

Rgb layerColor(float value, ColorMap colorMap, ValueRange range, int seaLevel) {
    if (colorMap == ColorMap::Terrain)
        return terrainColor(value, seaLevel);
    const float span = range.max - range.min;
    const float t = span > 0.0f ? std::clamp((value - range.min) / span, 0.0f, 1.0f) : 0.5f;
    const auto gray = static_cast<std::uint8_t>(std::lround(t * 255.0f));
    return {gray, gray, gray};
}

float hillshade(const Grid2D<float>& heights, int x, int z) {
    constexpr float kLx = -0.5f, kLy = 0.7f, kLz = -0.5f;
    const float dx = (heights.atClamped(x + 1, z) - heights.atClamped(x - 1, z)) * 0.5f;
    const float dz = (heights.atClamped(x, z + 1) - heights.atClamped(x, z - 1)) * 0.5f;
    // Normal (-dx, 1, -dz) / |n|, divided by kLy so flat ground gives 1.
    const float lambert = (-dx * kLx + kLy - dz * kLz) / (std::sqrt(dx * dx + 1.0f + dz * dz) * kLy);
    return std::clamp(lambert, 0.35f, 1.35f);
}

RgbaImage renderLayer(const Grid2D<float>& layer, ColorMap colorMap, int seaLevel) {
    RgbaImage image = makeImage(layer.width(), layer.length());
    const ValueRange range = valueRange(layer);
    for (int z = 0; z < layer.length(); ++z) {
        for (int x = 0; x < layer.width(); ++x) {
            const float value = layer.at(x, z);
            Rgb color = layerColor(value, colorMap, range, seaLevel);
            if (colorMap == ColorMap::Terrain && value > static_cast<float>(seaLevel))
                color = scale(color, hillshade(layer, x, z));
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
                color = scale(blockInfo(block).color, hillshade(top, x, z));
            }
            setPixel(image, x, z, color);
        }
    }
    return image;
}

RgbaImage renderView(const WorldState& state, const View& view, ColorMap colorMap) {
    if (view.kind == View::Kind::Voxels)
        return renderVoxelsTopDown(state.voxels());
    return renderLayer(state.layer(view.layer), colorMap, state.info().seaLevel);
}

} // namespace wg
