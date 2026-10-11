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

template <typename L>
std::vector<std::string> namesOf(const L& layers) {
    std::vector<std::string> names;
    for (const auto& layer : layers)
        names.push_back(layer.name);
    return names;
}

} // namespace

// --- Generic layer storage ---------------------------------------------------------

template <typename T>
const WorldState::Layer<T>* WorldState::find(const Layers<T>& layers, std::string_view name) {
    const auto it = std::find_if(layers.begin(), layers.end(), [&](const auto& layer) { return layer.name == name; });
    return it != layers.end() ? &*it : nullptr;
}

template <typename T>
const WorldState::Layer<T>& WorldState::get(const Layers<T>& layers, std::string_view name) {
    if (const auto* layer = find(layers, name))
        return *layer;
    throw WorldError("layer '" + std::string(name) + "' does not exist (it must be created by an earlier stage)");
}

template <typename T>
Grid2D<T>& WorldState::edit(Layers<T>& layers, std::string_view name) {
    return makeUnique(const_cast<Layer<T>&>(get(layers, name)).data);
}

template <typename T>
Grid2D<T>& WorldState::create(Layers<T>& layers, std::string_view name, int width, int length, T fill,
                              LayerMeta meta) {
    if (width <= 0 || length <= 0)
        throw WorldError("layer '" + std::string(name) + "' must have a positive size");
    auto grid = std::make_shared<Grid2D<T>>(width, length, fill);
    if (auto* existing = const_cast<Layer<T>*>(find(layers, name)))
        *existing = Layer<T>{existing->name, std::move(grid), std::move(meta)};
    else
        layers.push_back(Layer<T>{std::string(name), std::move(grid), std::move(meta)});
    return *find(layers, name)->data;
}

// --- Parameters ----------------------------------------------------------------------

const ResolvedParams& WorldState::params() const {
    if (!params_)
        throw WorldError("the parameters are not resolved yet (the 'parameters' stage must run first)");
    return *params_;
}

// --- Float layers --------------------------------------------------------------------

std::vector<std::string> WorldState::layerNames() const {
    return namesOf(layers_);
}

bool WorldState::hasLayer(std::string_view name) const {
    return find(layers_, name) != nullptr;
}

const Grid2D<float>& WorldState::layer(std::string_view name) const {
    return *get(layers_, name).data;
}

const LayerMeta& WorldState::layerMeta(std::string_view name) const {
    return get(layers_, name).meta;
}

Grid2D<float>& WorldState::editLayer(std::string_view name) {
    return edit(layers_, name);
}

Grid2D<float>& WorldState::createLayer(std::string_view name, int width, int length, float fill, LayerMeta meta) {
    return create(layers_, name, width, length, fill, std::move(meta));
}

Grid2D<float>& WorldState::createLayer(std::string_view name, float fill, LayerMeta meta) {
    return createLayer(name, info_.width, info_.length, fill, std::move(meta));
}

void WorldState::removeLayer(std::string_view name) {
    std::erase_if(layers_, [&](const auto& layer) { return layer.name == name; });
}

// --- Byte layers ---------------------------------------------------------------------

std::vector<std::string> WorldState::byteLayerNames() const {
    return namesOf(byteLayers_);
}

bool WorldState::hasByteLayer(std::string_view name) const {
    return find(byteLayers_, name) != nullptr;
}

const Grid2D<std::uint8_t>& WorldState::byteLayer(std::string_view name) const {
    return *get(byteLayers_, name).data;
}

Grid2D<std::uint8_t>& WorldState::editByteLayer(std::string_view name) {
    return edit(byteLayers_, name);
}

Grid2D<std::uint8_t>& WorldState::createByteLayer(std::string_view name, int width, int length, std::uint8_t fill) {
    return create(byteLayers_, name, width, length, fill, {});
}

// --- Scalars -------------------------------------------------------------------------

bool WorldState::hasScalar(std::string_view name) const {
    return scalars_.find(name) != scalars_.end();
}

double WorldState::scalar(std::string_view name, double fallback) const {
    const auto it = scalars_.find(name);
    return it != scalars_.end() ? it->second : fallback;
}

void WorldState::setScalar(std::string_view name, double value) {
    scalars_[std::string(name)] = value;
}

// --- Voxels --------------------------------------------------------------------------

const VoxelVolume& WorldState::voxels() const {
    if (!voxels_)
        throw WorldError("the world has no voxels yet (an earlier stage must build them)");
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

// --- Identity & copies ---------------------------------------------------------------

const void* WorldState::layerIdentity(std::string_view name) const {
    if (const auto* layer = find(layers_, name))
        return layer->data.get();
    const auto* layer = find(byteLayers_, name);
    return layer ? layer->data.get() : nullptr;
}

std::shared_ptr<const void> WorldState::layerHandle(std::string_view name) const {
    if (const auto* layer = find(layers_, name))
        return layer->data;
    const auto* layer = find(byteLayers_, name);
    return layer ? std::shared_ptr<const void>(layer->data) : nullptr;
}

WorldState WorldState::deepCopy() const {
    WorldState copy(info_);
    copy.params_ = params_; // immutable, safe to share
    copy.scalars_ = scalars_;
    for (const auto& layer : layers_)
        copy.layers_.push_back({layer.name, std::make_shared<Grid2D<float>>(*layer.data), layer.meta});
    for (const auto& layer : byteLayers_)
        copy.byteLayers_.push_back({layer.name, std::make_shared<Grid2D<std::uint8_t>>(*layer.data), layer.meta});
    // Copying the volume object is enough: its sections are copy-on-write and
    // only ever modified through VoxelVolume methods.
    if (voxels_)
        copy.voxels_ = std::make_shared<VoxelVolume>(*voxels_);
    return copy;
}

} // namespace wg
