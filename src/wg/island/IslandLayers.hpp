#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Names of the island state layers shared between stages, all on the coarse
// simulation grid (see IslandGrid). Heights are absolute altitudes in meters
// (sea level at WorldInfo::seaLevelM()).
namespace wg::layers {

inline constexpr std::string_view kHeight = "h";                 // altitude (m)
inline constexpr std::string_view kHeightInitial = "h_initial";  // h at the end of the initialization
inline constexpr std::string_view kMask = "mask";                // land mask, [0, 1]
inline constexpr std::string_view kCoastDist = "coast_dist";     // signed distance to the coast (m), > 0 on land
inline constexpr std::string_view kLagoon = "lagoon";            // 1 inside an atoll lagoon, else 0
inline constexpr std::string_view kCumUplift = "cum_uplift";     // cumulated tectonic uplift (m)
inline constexpr std::string_view kSediment = "sediment";        // sediment thickness (m)
inline constexpr std::string_view kIce = "ice";                  // ice thickness (m)
inline constexpr std::string_view kSurfaceMaterial = "surface_material"; // material id at the surface
inline constexpr std::string_view kFoldOffset = "fold_offset"; // vertical displacement of the strata by folds (m)
inline constexpr std::string_view kFaultTraces = "fault_traces"; // 1 on the surface trace of a fault
inline constexpr std::string_view kFlags = "flags";              // byte layer of CellFlags

// Step 3 (simulation).
inline constexpr std::string_view kBedrock = "bedrock";           // top of the rock b (m); h = b + sediment
inline constexpr std::string_view kDischarge = "discharge";       // water discharge Q (m3/yr)
inline constexpr std::string_view kLakeDepth = "lake_depth";      // depth of the filled depressions (m)
inline constexpr std::string_view kErosionRate = "erosion_rate";  // fluvial erosion of the last step (m/yr)
inline constexpr std::string_view kStrataUplift = "strata_uplift"; // uplift moving the strata (cum_uplift minus the faulted phases)
// Composition of the sediment cover: fraction of each kind, and the dominant one
// (categories: none, gravel, sand, clay).
inline constexpr std::string_view kSedimentGravel = "sediment_gravel";
inline constexpr std::string_view kSedimentSand = "sediment_sand";
inline constexpr std::string_view kSedimentClay = "sediment_clay";
inline constexpr std::string_view kSedimentDominant = "sediment_dominant";
inline constexpr std::string_view kSedimentFlux = "sediment_flux";      // fluvial sediment flux Qs leaving the cell (m3/yr)
inline constexpr std::string_view kDepositionRate = "deposition_rate";  // fluvial deposition of the last step (m/yr)
inline constexpr std::string_view kTemperature = "temperature";       // °C at the surface (h + ice)
inline constexpr std::string_view kPrecipitation = "precipitation";   // orographic precipitation (m/yr)
inline constexpr std::string_view kCoastExposure = "coast_exposure";  // wave exposure of the coast cells, [0, 1]
inline constexpr std::string_view kCoastAccumulator = "coast_wear";   // accumulated wave wear / volume of the cell, [0, 1]
inline constexpr std::string_view kSlidingVelocity = "ice_sliding";      // glacier sliding velocity u_s (m/yr)
inline constexpr std::string_view kMassBalance = "ice_mass_balance";    // glacier mass balance M (m/yr)
inline constexpr std::string_view kGlacialErosion = "glacial_erosion";  // glacial erosion of the last step (m/yr)

// Spatial pattern of tectonic phase k, in [-1, 1] (rate and time are applied later).
inline std::string upliftPattern(int k) {
    return "uplift_pattern_" + std::to_string(k);
}

enum CellFlags : std::uint8_t {
    kFixed = 1 << 0,  // boundary cell: height held at simulation.boundary_depth_m
    kInLagoon = 1 << 1, // water enclosed by an atoll ring (passes included)
};

} // namespace wg::layers
