#pragma once

#include <cstdint>
#include <d3d11.h>
#include <imgui.h>

namespace sunrise::client::ui::items::icons {
/** Called under the renderer lock before the module draws; at most two uploads per frame. */
void begin_frame(ID3D11Device* device) noexcept;
[[nodiscard]] ImTextureID get(std::uint16_t iconIndex) noexcept;
/** Called before renderer resources are released, and on catalog reload. */
void release() noexcept;
} // namespace sunrise::client::ui::items::icons
