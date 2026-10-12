#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wg {

// How a 2D layer should be displayed by default.
enum class LayerStyle : std::uint8_t {
    Scalar,      // generic value, shown from its min to its max
    Height,      // elevation in meters: hypsometric colors, hillshade, sea below sea level
    Diverging,   // signed value centered on 0 (blue negative, red positive)
    Categorical, // integer ids indexing `categories`
    Direction,   // angle in radians (atan2(y, x)) shown as a hue; `categories[0]`, if
                 // any, names a layer of the same size giving the intensity (a norm)
};

struct LayerMeta {
    LayerStyle style = LayerStyle::Scalar;
    std::vector<std::string> categories; // labels of the ids (Categorical), intensity layer (Direction)
};

} // namespace wg
