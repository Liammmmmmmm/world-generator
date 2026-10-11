#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "wg/core/Grid2D.hpp"
#include "wg/core/LayerMeta.hpp"
#include "wg/core/VoxelVolume.hpp"
#include "wg/core/WorldInfo.hpp"
#include "wg/params/IslandParams.hpp"

namespace wg {

class WorldError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Everything the generation pipeline produces, at a given point in time:
// - the resolved generation parameters, once the "parameters" stage ran,
// - named 2D layers: float fields ("h", "mask", ...) with a display style, and
//   byte layers for flags. A layer has its own size (e.g. the coarse
//   simulation grid) and always spans the whole map,
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

    // --- Parameters (immutable once resolved) ---------------------------------
    bool hasParams() const { return params_ != nullptr; }
    const ResolvedParams& params() const;
    std::shared_ptr<const ResolvedParams> paramsHandle() const { return params_; }
    void setParams(std::shared_ptr<const ResolvedParams> params) { params_ = std::move(params); }

    // --- Float layers --------------------------------------------------------
    std::vector<std::string> layerNames() const;
    bool hasLayer(std::string_view name) const;
    const Grid2D<float>& layer(std::string_view name) const;
    const LayerMeta& layerMeta(std::string_view name) const;
    Grid2D<float>& editLayer(std::string_view name);
    // Creates (or replaces) a layer of the given size.
    Grid2D<float>& createLayer(std::string_view name, int width, int length, float fill = 0.0f, LayerMeta meta = {});
    // Same, at the world's block resolution.
    Grid2D<float>& createLayer(std::string_view name, float fill = 0.0f, LayerMeta meta = {});
    void removeLayer(std::string_view name);

    // --- Byte layers (flags, ids) --------------------------------------------
    std::vector<std::string> byteLayerNames() const;
    bool hasByteLayer(std::string_view name) const;
    const Grid2D<std::uint8_t>& byteLayer(std::string_view name) const;
    Grid2D<std::uint8_t>& editByteLayer(std::string_view name);
    Grid2D<std::uint8_t>& createByteLayer(std::string_view name, int width, int length, std::uint8_t fill = 0);

    // --- Scalars (simulation time, phase progress...) ---------------------------
    bool hasScalar(std::string_view name) const;
    double scalar(std::string_view name, double fallback = 0.0) const;
    void setScalar(std::string_view name, double value);
    const std::map<std::string, double, std::less<>>& scalars() const { return scalars_; }

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
    template <typename T>
    struct Layer {
        std::string name;
        std::shared_ptr<Grid2D<T>> data;
        LayerMeta meta;
    };
    template <typename T>
    using Layers = std::vector<Layer<T>>; // creation order

    template <typename T>
    static const Layer<T>* find(const Layers<T>& layers, std::string_view name);
    template <typename T>
    static const Layer<T>& get(const Layers<T>& layers, std::string_view name);
    template <typename T>
    static Grid2D<T>& edit(Layers<T>& layers, std::string_view name);
    template <typename T>
    static Grid2D<T>& create(Layers<T>& layers, std::string_view name, int width, int length, T fill, LayerMeta meta);

    WorldInfo info_;
    std::shared_ptr<const ResolvedParams> params_;
    Layers<float> layers_;
    Layers<std::uint8_t> byteLayers_;
    std::map<std::string, double, std::less<>> scalars_;
    std::shared_ptr<VoxelVolume> voxels_;
};

} // namespace wg
