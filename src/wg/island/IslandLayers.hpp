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

// Produced by the hydrology.
inline constexpr std::string_view kWaterSurface = "water_surface"; // h with its depressions filled: lake levels (m)
inline constexpr std::string_view kLakeDepth = "lake_depth";       // water depth of the lakes (m), 0 elsewhere
inline constexpr std::string_view kDrainage = "drainage";          // log10 of the drained area (m²)
inline constexpr std::string_view kRiverWidth = "river_width";     // river width (m), 0 where there is none
inline constexpr std::string_view kRiverDepth = "river_depth";     // river depth (m)
inline constexpr std::string_view kFlowDirection = "flow_direction"; // byte layer: D8 receiver (0..7), kNoFlow at outlets
inline constexpr std::uint8_t kNoFlow = 255;
// Cell offsets of the flow directions 0..7 (the receiver of (i, j) is
// (i + kFlowDi[d], j + kFlowDj[d])).
inline constexpr int kFlowDi[8] = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr int kFlowDj[8] = {0, 1, 1, 1, 0, -1, -1, -1};

enum CellFlags : std::uint8_t {
    kFixed = 1 << 0,  // boundary cell: height held at simulation.boundary_depth_m
    kInLagoon = 1 << 1, // water enclosed by an atoll ring (passes included)
};

} // namespace wg::layers
