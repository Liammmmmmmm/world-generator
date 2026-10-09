#include "wg/core/VoxelVolume.hpp"

#include <algorithm>

namespace wg {

namespace {

int ceilDiv(int a, int b) {
    return (a + b - 1) / b;
}

} // namespace

VoxelVolume::VoxelVolume(int width, int length, int minY, int maxY, Block fill)
    : width_(width), length_(length), minY_(minY), maxY_(maxY), sectionsX_(ceilDiv(width, kSectionSize)),
      sectionsY_(ceilDiv(maxY - minY, kSectionSize)), sectionsZ_(ceilDiv(length, kSectionSize)),
      slots_(static_cast<std::size_t>(sectionsX_) * sectionsY_ * sectionsZ_, Slot{nullptr, fill}) {}

VoxelVolume::Section& VoxelVolume::editSection(Slot& slot) {
    if (!slot.data) {
        slot.data = std::make_shared<Section>();
        slot.data->blocks.fill(slot.uniform);
    } else if (slot.data.use_count() > 1) {
        slot.data = std::make_shared<Section>(*slot.data); // copy-on-write
    }
    return *slot.data;
}

Block VoxelVolume::get(int x, int y, int z) const {
    if (!contains(x, y, z))
        return Block::Air;
    const int ly = y - minY_;
    const Slot& slot = slots_[slotIndex(x / kSectionSize, ly / kSectionSize, z / kSectionSize)];
    return slot.data ? slot.data->blocks[localIndex(x % kSectionSize, ly % kSectionSize, z % kSectionSize)]
                     : slot.uniform;
}

void VoxelVolume::set(int x, int y, int z, Block block) {
    if (!contains(x, y, z))
        return;
    const int ly = y - minY_;
    Slot& slot = slots_[slotIndex(x / kSectionSize, ly / kSectionSize, z / kSectionSize)];
    if (!slot.data && slot.uniform == block)
        return;
    editSection(slot).blocks[localIndex(x % kSectionSize, ly % kSectionSize, z % kSectionSize)] = block;
}

void VoxelVolume::fillColumn(int x, int z, int yBegin, int yEnd, Block block) {
    if (x < 0 || z < 0 || x >= width_ || z >= length_)
        return;
    const int begin = std::max(yBegin, minY_) - minY_;
    const int end = std::min(yEnd, maxY_) - minY_;
    const int lx = x % kSectionSize, lz = z % kSectionSize;

    for (int ly = begin; ly < end;) {
        const int sy = ly / kSectionSize;
        const int sectionEnd = std::min(end, (sy + 1) * kSectionSize);
        Slot& slot = slots_[slotIndex(x / kSectionSize, sy, z / kSectionSize)];
        if (slot.data || slot.uniform != block) {
            Section& section = editSection(slot);
            for (int y = ly; y < sectionEnd; ++y)
                section.blocks[localIndex(lx, y % kSectionSize, lz)] = block;
        }
        ly = sectionEnd;
    }
}

void VoxelVolume::fillBox(int x0, int y0, int z0, int x1, int y1, int z1, Block block) {
    x0 = std::max(x0, 0), z0 = std::max(z0, 0);
    x1 = std::min(x1, width_), z1 = std::min(z1, length_);
    y0 = std::max(y0, minY_) - minY_, y1 = std::min(y1, maxY_) - minY_;
    if (x0 >= x1 || y0 >= y1 || z0 >= z1)
        return;

    for (int sz = z0 / kSectionSize; sz <= (z1 - 1) / kSectionSize; ++sz) {
        for (int sx = x0 / kSectionSize; sx <= (x1 - 1) / kSectionSize; ++sx) {
            for (int sy = y0 / kSectionSize; sy <= (y1 - 1) / kSectionSize; ++sy) {
                // Section bounds, clipped to the volume.
                const int bx0 = sx * kSectionSize, bx1 = std::min(bx0 + kSectionSize, width_);
                const int bz0 = sz * kSectionSize, bz1 = std::min(bz0 + kSectionSize, length_);
                const int by0 = sy * kSectionSize, by1 = std::min(by0 + kSectionSize, maxY_ - minY_);

                Slot& slot = slots_[slotIndex(sx, sy, sz)];
                if (x0 <= bx0 && x1 >= bx1 && z0 <= bz0 && z1 >= bz1 && y0 <= by0 && y1 >= by1) {
                    slot = Slot{nullptr, block};
                    continue;
                }
                if (!slot.data && slot.uniform == block)
                    continue;
                Section& section = editSection(slot);
                for (int y = std::max(y0, by0); y < std::min(y1, by1); ++y)
                    for (int z = std::max(z0, bz0); z < std::min(z1, bz1); ++z)
                        for (int x = std::max(x0, bx0); x < std::min(x1, bx1); ++x)
                            section.blocks[localIndex(x - bx0, y - by0, z - bz0)] = block;
            }
        }
    }
}

int VoxelVolume::topNonAir(int x, int z) const {
    if (x < 0 || z < 0 || x >= width_ || z >= length_)
        return minY_ - 1;
    const int lx = x % kSectionSize, lz = z % kSectionSize;
    for (int sy = sectionsY_ - 1; sy >= 0; --sy) {
        const Slot& slot = slots_[slotIndex(x / kSectionSize, sy, z / kSectionSize)];
        const int topInSection = std::min(kSectionSize, maxY_ - minY_ - sy * kSectionSize) - 1;
        if (!slot.data) {
            if (slot.uniform != Block::Air)
                return minY_ + sy * kSectionSize + topInSection;
            continue;
        }
        for (int ly = topInSection; ly >= 0; --ly)
            if (slot.data->blocks[localIndex(lx, ly, lz)] != Block::Air)
                return minY_ + sy * kSectionSize + ly;
    }
    return minY_ - 1;
}

void VoxelVolume::compact() {
    for (Slot& slot : slots_) {
        if (!slot.data)
            continue;
        const auto& blocks = slot.data->blocks;
        if (std::all_of(blocks.begin(), blocks.end(), [&](Block b) { return b == blocks[0]; }))
            slot = Slot{nullptr, blocks[0]};
    }
}

std::span<const Block> VoxelVolume::sectionBlocks(int index) const {
    const Slot& slot = slots_[index];
    return slot.data ? std::span<const Block>(slot.data->blocks) : std::span<const Block>();
}

void VoxelVolume::setSectionUniform(int index, Block block) {
    slots_[index] = Slot{nullptr, block};
}

std::span<Block> VoxelVolume::editSectionBlocks(int index) {
    return editSection(slots_[index]).blocks;
}

} // namespace wg
