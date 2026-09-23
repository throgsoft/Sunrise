#pragma once

#include <cstdint>

#include "field_reader.h"

namespace sunrise::middleware::content::packages::tables {
/** Serialized nonstreamed texture header layout and RGBA8 channel width. */
inline constexpr std::size_t kTextureHeaderSize = 40;
inline constexpr std::size_t kRgba8BytesPerPixel = 4;
inline constexpr std::size_t kTextureFormatOffset = 4, kTextureMarkerOffset = 12;
inline constexpr std::size_t kTextureWidthOffset = 14, kTextureHeightOffset = 16;
inline constexpr std::size_t kTextureDepthOffset = 18, kTextureLayerOffset = 20;
inline constexpr std::size_t kTextureLargeBufferOffset = 36;
inline constexpr std::uint32_t kRgba8UnormFormat = 28;
inline constexpr std::uint16_t kTextureMarker = 0xCAFE;
inline constexpr std::uint32_t kAbsentLargeTexture = 0xFFFFFFFFU;

/** Single-layer RGBA8 package texture. The entry reference names its pixel resource. */
struct Rgba8Texture {
    std::uint32_t byteCount{};
    std::uint16_t width{};
    std::uint16_t height{};
};

/** Reads a nonstreamed 2D texture header; pixel data is validated by the package reader. */
[[nodiscard]] inline bool rgba8_texture(std::span<const std::byte> header,
                                        Rgba8Texture& output) noexcept {
    Rgba8Texture value{};
    std::uint32_t format{}, large{};
    std::uint16_t marker{}, depth{}, layers{};
    // DXGI format 28 is RGBA8 UNORM; CAFE identifies the package texture header.
    if (header.size() != kTextureHeaderSize || !read(header, 0, value.byteCount)
        || !read(header, kTextureFormatOffset, format) || format != kRgba8UnormFormat
        || !read(header, kTextureMarkerOffset, marker) || marker != kTextureMarker
        || !read(header, kTextureWidthOffset, value.width)
        || !read(header, kTextureHeightOffset, value.height)
        || !read(header, kTextureDepthOffset, depth) || depth != 1
        || !read(header, kTextureLayerOffset, layers) || layers != 1
        || !read(header, kTextureLargeBufferOffset, large) || large != kAbsentLargeTexture
        || value.width == 0 || value.height == 0
        || static_cast<std::uint64_t>(value.width) * value.height * kRgba8BytesPerPixel
               > value.byteCount) {
        output = {};
        return false;
    }
    output = value;
    return true;
}
} // namespace sunrise::middleware::content::packages::tables
