#include "wg/io/Nbt.hpp"

#include <bit>
#include <stdexcept>

namespace wg::nbt {

void Writer::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value >> 8));
    u8(static_cast<std::uint8_t>(value));
}

void Writer::u32(std::uint32_t value) {
    u16(static_cast<std::uint16_t>(value >> 16));
    u16(static_cast<std::uint16_t>(value));
}

void Writer::u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value >> 32));
    u32(static_cast<std::uint32_t>(value));
}

void Writer::header(Tag tag, std::string_view name) {
    u8(static_cast<std::uint8_t>(tag));
    stringPayload(name);
}

void Writer::stringPayload(std::string_view value) {
    // NBT strings are "modified UTF-8"; plain ASCII / UTF-8 without NUL is the same.
    if (value.size() > 0xFFFF)
        throw std::length_error("NBT string too long");
    u16(static_cast<std::uint16_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void Writer::beginCompound(std::string_view name) {
    header(Tag::Compound, name);
}

void Writer::endCompound() {
    u8(static_cast<std::uint8_t>(Tag::End));
}

void Writer::beginList(std::string_view name, Tag elements, std::int32_t count) {
    header(Tag::List, name);
    u8(static_cast<std::uint8_t>(count == 0 ? Tag::End : elements));
    u32(static_cast<std::uint32_t>(count));
}

void Writer::byteTag(std::string_view name, std::int8_t value) {
    header(Tag::Byte, name);
    u8(static_cast<std::uint8_t>(value));
}

void Writer::shortTag(std::string_view name, std::int16_t value) {
    header(Tag::Short, name);
    u16(static_cast<std::uint16_t>(value));
}

void Writer::intTag(std::string_view name, std::int32_t value) {
    header(Tag::Int, name);
    u32(static_cast<std::uint32_t>(value));
}

void Writer::longTag(std::string_view name, std::int64_t value) {
    header(Tag::Long, name);
    u64(static_cast<std::uint64_t>(value));
}

void Writer::floatTag(std::string_view name, float value) {
    header(Tag::Float, name);
    u32(std::bit_cast<std::uint32_t>(value));
}

void Writer::stringTag(std::string_view name, std::string_view value) {
    header(Tag::String, name);
    stringPayload(value);
}

void Writer::longArrayTag(std::string_view name, std::span<const std::int64_t> values) {
    header(Tag::LongArray, name);
    u32(static_cast<std::uint32_t>(values.size()));
    for (std::int64_t v : values)
        u64(static_cast<std::uint64_t>(v));
}

} // namespace wg::nbt
