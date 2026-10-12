#pragma once

#include <cstdint>
#include <string_view>

// Names of the island state layers shared between stages, on the simulation
// grid (see IslandGrid) unless noted. Heights are absolute altitudes in meters
// (sea level at WorldInfo::seaLevelM()).
namespace wg::layers {

inline constexpr std::string_view kHeight = "h";                 // altitude (m); at the block resolution after the upscale
inline constexpr std::string_view kHeightInitial = "h_initial";  // h at the end of the initialization
inline constexpr std::string_view kMask = "mask";                // land mask, [0, 1]
inline constexpr std::string_view kCoastDist = "coast_dist";     // signed distance to the coast (m), > 0 on land
inline constexpr std::string_view kLagoon = "lagoon";            // 1 inside an atoll lagoon, else 0
inline constexpr std::string_view kRelief = "relief";            // height added by the relief zones (m)
inline constexpr std::string_view kSurfaceMaterial = "surface_material"; // material id at the surface
inline constexpr std::string_view kFoldOffset = "fold_offset"; // vertical displacement of the strata by folds (m)
inline constexpr std::string_view kFlags = "flags";              // byte layer of CellFlags

// Produced by the erosion.
inline constexpr std::string_view kSediment = "sediment";             // loose sediment thickness (m)
inline constexpr std::string_view kDischargeNorm = "discharge_norm";  // normalized water flow, [0, 1)
inline constexpr std::string_view kEroded = "eroded";                 // h_initial - h (m): > 0 eroded, < 0 deposited
inline constexpr std::string_view kMomentum = "momentum";             // direction of the time-averaged water momentum (rad)
inline constexpr std::string_view kMomentumNorm = "momentum_norm";    // its norm

// Produced by the hydrology, at the block resolution (like "h" after the upscale).
inline constexpr std::string_view kWaterSurface = "water_surface"; // top of the water (m); the ground where dry
inline constexpr std::string_view kWater = "water";                // kind of water of each cell (WaterKind)

// Values of the "water" layer (categories, in this order).
enum WaterKind : std::uint8_t { kDry = 0, kSea = 1, kLake = 2, kRiver = 3 };
inline constexpr std::string_view kWaterKindNames[] = {"dry", "sea", "lake", "river"};

enum CellFlags : std::uint8_t {
    kFixed = 1 << 0,  // boundary cell: height held at simulation.boundary_depth_m
    kInLagoon = 1 << 1, // water enclosed by an atoll ring (passes included)
};

} // namespace wg::layers
