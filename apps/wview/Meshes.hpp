#pragma once

#include <functional>

#include <raylib.h>

#include "wg/core/Block.hpp"
#include "wg/core/VoxelVolume.hpp"

// Surface mesh of a 2D field (heightmap), vertex-colored and lit on the CPU.
// Downsampled so it has at most `maxCells` quads per side. `height` is in world
// units (blocks); grid cells are `cellSize` blocks wide.
using HeightFn = std::function<float(int x, int z)>;
using ColorFn = std::function<wg::Rgb(int x, int z)>;
Model buildHeightfieldModel(int width, int length, const HeightFn& height, const ColorFn& color, int maxCells = 512,
                            double cellSize = 1.0);

// Block mesh of a voxel volume: only faces touching air are emitted. Large
// volumes are rendered at a lower level of detail (one cube per step^3 blocks)
// so that there are at most `maxCells` cubes per horizontal side.
Model buildVoxelModel(const wg::VoxelVolume& voxels, int maxCells = 384);

// Blocks per rendered cube for a volume of this size.
int voxelLodStep(const wg::VoxelVolume& voxels, int maxCells = 384);
