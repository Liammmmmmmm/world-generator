#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wg/core/Block.hpp"
#include "wg/core/Grid2D.hpp"
#include "wg/core/LayerMeta.hpp"

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
    enum class Kind { Layer, Voxels, Parameters };
    Kind kind = Kind::Layer;
    std::string layer; // when kind == Layer

    std::string label() const {
        return kind == Kind::Voxels ? "voxels" : kind == Kind::Parameters ? "parameters" : layer;
    }
    // Whether renderView() can turn it into an image (parameters are text).
    bool isImage() const { return kind != Kind::Parameters; }
    bool operator==(const View&) const = default;
};

// Parameters if resolved, layers in creation order, then the voxels if any.
std::vector<View> availableViews(const WorldState& state);
bool hasView(const WorldState& state, const View& view);
// What the stage produced: voxels if it changed them, else its first changed
// layer, else the parameters if it resolved them.
View defaultView(const Snapshot& snapshot);

// --- Colors -------------------------------------------------------------------

enum class ColorMap : std::uint8_t {
    Auto,      // from the layer style (see LayerStyle)
    Grayscale, // any layer, normalized to its min..max
    Count
};
std::string_view colorMapName(ColorMap colorMap);

struct ValueRange {
    float min = 0.0f;
    float max = 0.0f;
};
ValueRange valueRange(const Grid2D<float>& grid);

// Preview color of a material (geology.materials), stable per name.
Rgb materialColor(std::string_view name);

// Colors the values of one layer.
//   Height:      hypsometric tints above sea level, blue by depth below
//   Scalar:      grayscale, min..max
//   Diverging:   blue (negative) / white (0) / red (positive), each sign scaled
//                to its own extent
//   Categorical: one color per category (materials get materialColor)
class LayerPalette {
public:
    LayerPalette(const Grid2D<float>& layer, const LayerMeta& meta, ColorMap colorMap, double seaLevelM);
    Rgb color(float value) const;
    // Hillshade is applied to Height layers in Auto mode.
    bool shaded() const { return style_ == LayerStyle::Height; }

private:
    LayerStyle style_;
    ValueRange range_;
    double seaLevelM_;
    std::vector<Rgb> categories_;
};

// Lambert shading factor (~1 on flat ground) from a heightmap, light from the
// NW. `cellSize` is the horizontal size of a cell, in the unit of the heights.
float hillshade(const Grid2D<float>& heights, int x, int z, double cellSize = 1.0);

// --- Top-down images ----------------------------------------------------------

// Horizontal size of a layer cell in meters (layers span the whole map).
double cellSizeM(const WorldState& state, const Grid2D<float>& layer);

RgbaImage renderLayer(const WorldState& state, std::string_view layer, ColorMap colorMap);
RgbaImage renderVoxelsTopDown(const VoxelVolume& voxels);
RgbaImage renderView(const WorldState& state, const View& view, ColorMap colorMap);

} // namespace wg
