#pragma once

#include <cstdint>
#include <string_view>

// Names of the island state layers shared between stages, all on the
// simulation grid (see IslandGrid). Heights are absolute altitudes in meters
// (sea level at WorldInfo::seaLevelM()).
namespace wg::layers {

inline constexpr std::string_view kHeight = "h";                 // altitude (m)
inline constexpr std::string_view kHeightInitial = "h_initial";  // h at the end of the initialization
inline constexpr std::string_view kMask = "mask";                // land mask, [0, 1]
inline constexpr std::string_view kCoastDist = "coast_dist";     // signed distance to the coast (m), > 0 on land
inline constexpr std::string_view kLagoon = "lagoon";            // 1 inside an atoll lagoon, else 0
inline constexpr std::string_view kSurfaceMaterial = "surface_material"; // material id at the surface
inline constexpr std::string_view kFoldOffset = "fold_offset"; // vertical displacement of the strata by folds (m)
inline constexpr std::string_view kFlags = "flags";              // byte layer of CellFlags

enum CellFlags : std::uint8_t {
    kFixed = 1 << 0,  // boundary cell: height held at simulation.boundary_depth_m
    kInLagoon = 1 << 1, // water enclosed by an atoll ring (passes included)
};

} // namespace wg::layers
