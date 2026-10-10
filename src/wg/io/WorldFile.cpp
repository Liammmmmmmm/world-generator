#include "wg/io/WorldFile.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>

#include "wg/core/Config.hpp"

namespace wg {

static_assert(std::endian::native == std::endian::little, "WorldFile assumes a little-endian host");

namespace {

constexpr std::array<char, 4> kMagic{'W', 'G', 'W', '\0'};
constexpr std::uint32_t kVersion = 4;

template <typename T>
void writePod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
T readPod(std::ifstream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return value;
}

template <typename T>
void writeSpan(std::ofstream& out, std::span<const T> data) {
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size_bytes()));
}

template <typename T>
void readSpan(std::ifstream& in, std::span<T> data) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size_bytes()));
}

void writeString(std::ofstream& out, const std::string& text) {
    writePod(out, static_cast<std::uint32_t>(text.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readString(std::ifstream& in) {
    std::string text(readPod<std::uint32_t>(in), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

} // namespace

void writeWorld(const WorldState& world, const std::filesystem::path& path) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw WorldFileError("cannot write '" + path.string() + "'");

    const WorldInfo& info = world.info();
    out.write(kMagic.data(), kMagic.size());
    writePod(out, kVersion);
    writePod<std::int32_t>(out, info.width);
    writePod<std::int32_t>(out, info.length);
    writePod<std::int32_t>(out, info.minY);
    writePod<std::int32_t>(out, info.maxY);
    writePod<std::int32_t>(out, info.seaLevel);
    writePod<std::uint64_t>(out, info.seed);

    // Resolution is deterministic: storing the config is enough to restore the parameters.
    writeString(out, world.hasParams() ? world.params().config.dump() : std::string());

    const auto names = world.layerNames();
    writePod(out, static_cast<std::uint32_t>(names.size()));
    for (const auto& name : names) {
        const auto& grid = world.layer(name);
        const auto& meta = world.layerMeta(name);
        writeString(out, name);
        writePod<std::int32_t>(out, grid.width());
        writePod<std::int32_t>(out, grid.length());
        writePod(out, meta.style);
        writePod(out, static_cast<std::uint32_t>(meta.categories.size()));
        for (const auto& category : meta.categories)
            writeString(out, category);
        writeSpan(out, grid.data());
    }

    const auto byteNames = world.byteLayerNames();
    writePod(out, static_cast<std::uint32_t>(byteNames.size()));
    for (const auto& name : byteNames) {
        const auto& grid = world.byteLayer(name);
        writeString(out, name);
        writePod<std::int32_t>(out, grid.width());
        writePod<std::int32_t>(out, grid.length());
        writeSpan(out, grid.data());
    }

    writePod<std::uint8_t>(out, world.hasVoxels());
    if (world.hasVoxels()) {
        const VoxelVolume& voxels = world.voxels();
        for (int i = 0; i < voxels.sectionCount(); ++i) {
            const bool uniform = voxels.isSectionUniform(i);
            writePod<std::uint8_t>(out, uniform ? 0 : 1);
            if (uniform)
                writePod(out, voxels.sectionUniformBlock(i));
            else
                writeSpan(out, voxels.sectionBlocks(i));
        }
    }

    if (!out)
        throw WorldFileError("failed while writing '" + path.string() + "'");
}

WorldState readWorld(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw WorldFileError("cannot open '" + path.string() + "'");

    std::array<char, 4> magic{};
    in.read(magic.data(), magic.size());
    if (magic != kMagic)
        throw WorldFileError("'" + path.string() + "' is not a world file");
    if (const auto version = readPod<std::uint32_t>(in); version != kVersion)
        throw WorldFileError("unsupported world file version " + std::to_string(version));

    WorldInfo info;
    info.width = readPod<std::int32_t>(in);
    info.length = readPod<std::int32_t>(in);
    info.minY = readPod<std::int32_t>(in);
    info.maxY = readPod<std::int32_t>(in);
    info.seaLevel = readPod<std::int32_t>(in);
    info.seed = readPod<std::uint64_t>(in);
    if (!in || info.width <= 0 || info.length <= 0 || info.minY >= info.maxY)
        throw WorldFileError("corrupted header in '" + path.string() + "'");

    WorldState world(info);
    const std::string config = readString(in);
    if (!config.empty() && in) {
        try {
            world.setParams(WorldConfig::fromJson(nlohmann::json::parse(config)).params);
        } catch (const std::exception& e) {
            throw WorldFileError("invalid parameters in '" + path.string() + "': " + e.what());
        }
    }

    const auto checkSize = [&](std::int32_t width, std::int32_t length) {
        if (!in || width <= 0 || length <= 0 || width > (1 << 16) || length > (1 << 16))
            throw WorldFileError("corrupted layer in '" + path.string() + "'");
    };
    const auto layerCount = readPod<std::uint32_t>(in);
    for (std::uint32_t i = 0; i < layerCount && in; ++i) {
        const std::string name = readString(in);
        const auto width = readPod<std::int32_t>(in);
        const auto length = readPod<std::int32_t>(in);
        LayerMeta meta;
        meta.style = readPod<LayerStyle>(in);
        meta.categories.resize(readPod<std::uint32_t>(in));
        for (auto& category : meta.categories)
            category = readString(in);
        checkSize(width, length);
        readSpan(in, world.createLayer(name, width, length, 0.0f, std::move(meta)).data());
    }
    const auto byteLayerCount = readPod<std::uint32_t>(in);
    for (std::uint32_t i = 0; i < byteLayerCount && in; ++i) {
        const std::string name = readString(in);
        const auto width = readPod<std::int32_t>(in);
        const auto length = readPod<std::int32_t>(in);
        checkSize(width, length);
        readSpan(in, world.createByteLayer(name, width, length).data());
    }

    if (readPod<std::uint8_t>(in)) {
        VoxelVolume& voxels = world.createVoxels();
        for (int i = 0; i < voxels.sectionCount() && in; ++i) {
            if (readPod<std::uint8_t>(in) == 0)
                voxels.setSectionUniform(i, readPod<Block>(in));
            else
                readSpan(in, voxels.editSectionBlocks(i));
        }
    }

    if (!in)
        throw WorldFileError("truncated world file '" + path.string() + "'");
    return world;
}

} // namespace wg
