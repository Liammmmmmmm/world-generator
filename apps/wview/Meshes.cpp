#include "Meshes.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <raymath.h>

namespace {

struct MeshBuilder {
    std::vector<float> vertices;
    std::vector<unsigned char> colors;

    void vertex(float x, float y, float z, wg::Rgb c, float light) {
        vertices.insert(vertices.end(), {x, y, z});
        const auto shade = [light](std::uint8_t ch) {
            return static_cast<unsigned char>(std::clamp(ch * light, 0.0f, 255.0f));
        };
        colors.insert(colors.end(), {shade(c.r), shade(c.g), shade(c.b), 255});
    }

    bool empty() const { return vertices.empty(); }

    // Uploads to the GPU; raylib takes ownership of the CPU copies.
    Mesh upload() const {
        Mesh mesh{};
        mesh.vertexCount = static_cast<int>(vertices.size() / 3);
        mesh.triangleCount = mesh.vertexCount / 3;
        mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned>(vertices.size() * sizeof(float))));
        mesh.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned>(colors.size())));
        std::memcpy(mesh.vertices, vertices.data(), vertices.size() * sizeof(float));
        std::memcpy(mesh.colors, colors.data(), colors.size());
        UploadMesh(&mesh, false);
        return mesh;
    }
};

// Same as LoadModelFromMesh, for several meshes sharing the default material.
Model modelFromMeshes(const std::vector<Mesh>& meshes) {
    Model model{};
    model.transform = MatrixIdentity();
    model.meshCount = static_cast<int>(meshes.size());
    model.meshes = static_cast<Mesh*>(MemAlloc(static_cast<unsigned>(sizeof(Mesh) * std::max<std::size_t>(1, meshes.size()))));
    std::copy(meshes.begin(), meshes.end(), model.meshes);
    model.materialCount = 1;
    model.materials = static_cast<Material*>(MemAlloc(sizeof(Material)));
    model.materials[0] = LoadMaterialDefault();
    model.meshMaterial = static_cast<int*>(MemAlloc(static_cast<unsigned>(sizeof(int) * std::max<std::size_t>(1, meshes.size()))));
    return model;
}

} // namespace

Model buildHeightfieldModel(int width, int length, const HeightFn& height, const ColorFn& color, int maxCells) {
    const int step = std::max(1, (std::max(width, length) + maxCells - 1) / maxCells);
    const int cellsX = std::max(1, (width - 1 + step - 1) / step);
    const int cellsZ = std::max(1, (length - 1 + step - 1) / step);
    const Vector3 sun = Vector3Normalize({-0.5f, 0.8f, -0.3f});

    const auto heightAt = [&](int x, int z) { return height(std::clamp(x, 0, width - 1), std::clamp(z, 0, length - 1)); };

    MeshBuilder builder;
    builder.vertices.reserve(static_cast<std::size_t>(cellsX) * cellsZ * 18);
    builder.colors.reserve(static_cast<std::size_t>(cellsX) * cellsZ * 24);
    const auto emit = [&](int x, int z) {
        const float dx = (heightAt(x + step, z) - heightAt(x - step, z)) / (2.0f * step);
        const float dz = (heightAt(x, z + step) - heightAt(x, z - step)) / (2.0f * step);
        const Vector3 normal = Vector3Normalize({-dx, 1.0f, -dz});
        const float light = 0.45f + 0.55f * std::max(0.0f, Vector3DotProduct(normal, sun)) / sun.y;
        builder.vertex(static_cast<float>(x), heightAt(x, z), static_cast<float>(z), color(x, z), light);
    };

    for (int j = 0; j < cellsZ; ++j) {
        for (int i = 0; i < cellsX; ++i) {
            const int x0 = std::min(i * step, width - 1), x1 = std::min((i + 1) * step, width - 1);
            const int z0 = std::min(j * step, length - 1), z1 = std::min((j + 1) * step, length - 1);
            // Counter-clockwise seen from above (raylib front faces).
            emit(x0, z0), emit(x0, z1), emit(x1, z0);
            emit(x1, z0), emit(x0, z1), emit(x1, z1);
        }
    }
    return modelFromMeshes({builder.upload()});
}

int voxelLodStep(const wg::VoxelVolume& voxels, int maxCells) {
    return std::max(1, (std::max(voxels.width(), voxels.length()) + maxCells - 1) / maxCells);
}

Model buildVoxelModel(const wg::VoxelVolume& voxels, int maxCells) {
    const int step = voxelLodStep(voxels, maxCells);
    const int nx = (voxels.width() + step - 1) / step;
    const int ny = (voxels.maxY() - voxels.minY() + step - 1) / step;
    const int nz = (voxels.length() + step - 1) / step;

    // Downsampled copy: each cell takes the highest non-air block of the
    // sampled column within its height, so thin surface layers stay visible.
    std::vector<wg::Block> cells(static_cast<std::size_t>(nx) * ny * nz);
    const auto index = [&](int i, int j, int k) { return (static_cast<std::size_t>(k) * nx + i) * ny + j; };
    for (int k = 0; k < nz; ++k) {
        for (int i = 0; i < nx; ++i) {
            for (int j = 0; j < ny; ++j) {
                wg::Block block = wg::Block::Air;
                for (int dy = step - 1; dy >= 0 && block == wg::Block::Air; --dy)
                    block = voxels.get(i * step, voxels.minY() + j * step + dy, k * step);
                cells[index(i, j, k)] = block;
            }
        }
    }

    const auto cellAt = [&](int i, int j, int k) {
        if (j < 0)
            return wg::Block::Bedrock; // never show the bottom of the world
        if (i < 0 || k < 0 || i >= nx || k >= nz || j >= ny)
            return wg::Block::Air; // world borders are visible as cut-away sides
        return cells[index(i, j, k)];
    };

    struct Face {
        int dx, dy, dz;
        float light;
        int corners[4][3]; // CCW seen from outside, as (x, y, z) offsets in {0, 1}
    };
    static constexpr Face kFaces[6] = {
        {0, 1, 0, 1.00f, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},
        {0, -1, 0, 0.50f, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
        {1, 0, 0, 0.80f, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}},
        {-1, 0, 0, 0.70f, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}},
        {0, 0, 1, 0.65f, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},
        {0, 0, -1, 0.85f, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}},
    };

    // One mesh per chunk of columns keeps buffers small and allows partial
    // rebuilds later.
    constexpr int kChunk = 64;
    std::vector<Mesh> meshes;
    for (int ck = 0; ck < nz; ck += kChunk) {
        for (int ci = 0; ci < nx; ci += kChunk) {
            MeshBuilder builder;
            for (int k = ck; k < std::min(ck + kChunk, nz); ++k) {
                for (int i = ci; i < std::min(ci + kChunk, nx); ++i) {
                    for (int j = 0; j < ny; ++j) {
                        const wg::Block block = cells[index(i, j, k)];
                        if (block == wg::Block::Air)
                            continue;
                        const wg::Rgb color = wg::blockInfo(block).color;
                        for (const Face& face : kFaces) {
                            if (cellAt(i + face.dx, j + face.dy, k + face.dz) != wg::Block::Air)
                                continue;
                            float corner[4][3];
                            for (int c = 0; c < 4; ++c) {
                                corner[c][0] = static_cast<float>((i + face.corners[c][0]) * step);
                                corner[c][1] = static_cast<float>(voxels.minY() + (j + face.corners[c][1]) * step);
                                corner[c][2] = static_cast<float>((k + face.corners[c][2]) * step);
                            }
                            for (int c : {0, 1, 2, 0, 2, 3})
                                builder.vertex(corner[c][0], corner[c][1], corner[c][2], color, face.light);
                        }
                    }
                }
            }
            if (!builder.empty())
                meshes.push_back(builder.upload());
        }
    }
    return modelFromMeshes(meshes);
}
