#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wg/core/Block.hpp"
#include "wg/core/Grid2D.hpp"

namespace wg {

class VoxelVolume;
class WorldState;
struct Snapshot;

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // row-major RGBA8
};

// --- What to look at in a world state ----------------------------------------

struct View {
    enum class Kind { Layer, Voxels };
    Kind kind = Kind::Layer;
    std::string layer; // when kind == Layer

    std::string label() const { return kind == Kind::Voxels ? "voxels" : layer; }
    bool operator==(const View&) const = default;
};

// Layers in creation order, then the voxels if any.
std::vector<View> availableViews(const WorldState& state);
bool hasView(const WorldState& state, const View& view);
// What the stage produced: voxels if it changed them, else its first changed layer.
View defaultView(const Snapshot& snapshot);

// --- Colors -------------------------------------------------------------------

enum class ColorMap : std::uint8_t {
    Terrain,   // absolute heights: ocean by depth, then sand, grass, rock, snow
    Grayscale, // normalized to the layer's min..max
    Count
};
std::string_view colorMapName(ColorMap colorMap);

struct ValueRange {
    float min = 0.0f;
    float max = 0.0f;
};
ValueRange valueRange(const Grid2D<float>& grid);

Rgb layerColor(float value, ColorMap colorMap, ValueRange range, int seaLevel);

// Lambert shading factor (~1 on flat ground) from a heightmap, light from the NW.
float hillshade(const Grid2D<float>& heights, int x, int z);

// --- Top-down images ----------------------------------------------------------

RgbaImage renderLayer(const Grid2D<float>& layer, ColorMap colorMap, int seaLevel);
RgbaImage renderVoxelsTopDown(const VoxelVolume& voxels);
RgbaImage renderView(const WorldState& state, const View& view, ColorMap colorMap);

} // namespace wg
