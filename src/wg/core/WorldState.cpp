#include "wg/core/WorldState.hpp"

#include <algorithm>

namespace wg {

namespace {

template <typename T>
T& makeUnique(std::shared_ptr<T>& ptr) {
    if (ptr.use_count() > 1)
        ptr = std::make_shared<T>(*ptr);
    return *ptr;
}

} // namespace

const WorldState::LayerPtr* WorldState::findLayer(std::string_view name) const {
    const auto it = std::find_if(layers_.begin(), layers_.end(), [&](const auto& entry) { return entry.first == name; });
    return it != layers_.end() ? &it->second : nullptr;
}

std::vector<std::string> WorldState::layerNames() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : layers_)
        names.push_back(name);
    return names;
}

bool WorldState::hasLayer(std::string_view name) const {
    return findLayer(name) != nullptr;
}

const Grid2D<float>& WorldState::layer(std::string_view name) const {
    if (const auto* ptr = findLayer(name))
        return **ptr;
    throw WorldError("layer '" + std::string(name) + "' does not exist (it must be created by an earlier stage)");
}

Grid2D<float>& WorldState::editLayer(std::string_view name) {
    layer(name); // throws if missing
    return makeUnique(*const_cast<LayerPtr*>(findLayer(name)));
}

Grid2D<float>& WorldState::createLayer(std::string_view name, float fill) {
    auto grid = std::make_shared<Grid2D<float>>(info_.width, info_.length, fill);
    if (auto* existing = const_cast<LayerPtr*>(findLayer(name)))
        *existing = std::move(grid);
    else
        layers_.emplace_back(std::string(name), std::move(grid));
    return *findLayer(name)->get();
}

void WorldState::removeLayer(std::string_view name) {
    std::erase_if(layers_, [&](const auto& entry) { return entry.first == name; });
}

const VoxelVolume& WorldState::voxels() const {
    if (!voxels_)
        throw WorldError("the world has no voxels yet (a stage like 'voxelize' must build them first)");
    return *voxels_;
}

VoxelVolume& WorldState::editVoxels() {
    voxels(); // throws if missing
    return makeUnique(voxels_);
}

VoxelVolume& WorldState::createVoxels(Block fill) {
    voxels_ = std::make_shared<VoxelVolume>(info_.width, info_.length, info_.minY, info_.maxY, fill);
    return *voxels_;
}

const void* WorldState::layerIdentity(std::string_view name) const {
    const auto* ptr = findLayer(name);
    return ptr ? ptr->get() : nullptr;
}

} // namespace wg
