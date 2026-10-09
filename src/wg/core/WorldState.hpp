#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wg/core/Grid2D.hpp"
#include "wg/core/VoxelVolume.hpp"
#include "wg/core/WorldInfo.hpp"

namespace wg {

class WorldError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Everything the generation pipeline produces, at a given point in time:
// - named 2D float layers ("height", later "moisture", "flow", ...),
// - optionally the 3D block volume, once a stage has built it.
//
// Data is shared copy-on-write: copying a WorldState is cheap and the copy is
// unaffected by later edits. edit*() duplicates the data first if it is shared.
// This is what lets the pipeline keep a snapshot after every stage.
class WorldState {
public:
    WorldState() = default;
    explicit WorldState(const WorldInfo& info) : info_(info) {}

    const WorldInfo& info() const { return info_; }

    // --- 2D layers -----------------------------------------------------------
    std::vector<std::string> layerNames() const;
    bool hasLayer(std::string_view name) const;
    const Grid2D<float>& layer(std::string_view name) const;
    Grid2D<float>& editLayer(std::string_view name);
    // Creates (or replaces) a width x length layer.
    Grid2D<float>& createLayer(std::string_view name, float fill = 0.0f);
    void removeLayer(std::string_view name);

    // --- 3D blocks -----------------------------------------------------------
    bool hasVoxels() const { return voxels_ != nullptr; }
    const VoxelVolume& voxels() const;
    VoxelVolume& editVoxels();
    // Creates (or replaces) the volume, filled with `fill`.
    VoxelVolume& createVoxels(Block fill = Block::Air);

    // Address of the underlying data: changes whenever the data is modified
    // through this state. Used to detect what a stage changed. Null if absent.
    const void* layerIdentity(std::string_view name) const;
    const void* voxelsIdentity() const { return voxels_.get(); }

    // Same, but keeps the data alive (and so the address unique) while held.
    std::shared_ptr<const void> layerHandle(std::string_view name) const;
    std::shared_ptr<const void> voxelsHandle() const { return voxels_; }

    // Copy that shares no mutable data with this state, so it stays valid even
    // if a stage keeps writing through references it obtained from edit*().
    // Used for live previews of a running stage.
    WorldState deepCopy() const;

private:
    using LayerPtr = std::shared_ptr<Grid2D<float>>;

    const LayerPtr* findLayer(std::string_view name) const;

    WorldInfo info_;
    std::vector<std::pair<std::string, LayerPtr>> layers_; // creation order
    std::shared_ptr<VoxelVolume> voxels_;
};

} // namespace wg
