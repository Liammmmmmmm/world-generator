#pragma once

#include <filesystem>

// Calibration mode: a fixed continental island (seed and shape imposed),
// lifted by a single dome at a constant rate U over the whole run, eroded by
// rivers with a constant K (one rock, erodibility 1), simulated until steady
// state (mean |dh/dt| on land below 1 % of U). Prints, for a grid of U and
// k_base, the maximum and mean altitude of the island and the time to reach
// equilibrium, to choose the ranges of k_base and of the uplift rates.
struct CalibrateOptions {
    std::filesystem::path configPath; // only world.* is taken from it
    int resolution = 512;             // simulation.coarse_resolution
};

// Returns 0 on success, prints errors and returns 1 otherwise.
int runCalibration(const CalibrateOptions& options);
