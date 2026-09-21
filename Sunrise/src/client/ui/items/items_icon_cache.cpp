#include "items_icon_cache.h"

#include <array>
#include <limits>

#include "../../hooks/graphics/textures/graphics_texture_upload.h"
#include "items_catalog.h"

namespace sunrise::client::ui::items::icons {
namespace {
using Uploaded = client::hooks::graphics::textures::Uploaded;
struct Slot {
    std::uint16_t index{package::kNoIcon};
    std::uint64_t request{};
    int used{-1};
    Uploaded gpu{};
};
// Worst case: 16 MiB GPU storage, plus at most 4 MiB completed CPU work in the worker.
std::array<Slot, 64> g_slots{};
ID3D11Device* g_device{}; // Borrowed only inside renderer-owned frames.
std::uint64_t g_request{};
int g_frame{-1};

void free(Slot& slot) noexcept {
    if (slot.gpu.view) slot.gpu.view->Release();
    if (slot.gpu.texture) slot.gpu.texture->Release();
    slot = {};
}
bool upload(const package::Icon& image, Uploaded& output) noexcept {
    if (!g_device || image.rgba.empty()) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{image.rgba.data(), image.width * 4U, 0};
    if (FAILED(g_device->CreateTexture2D(&desc, &initial, &output.texture))) return false;
    if (FAILED(g_device->CreateShaderResourceView(output.texture, nullptr, &output.view))) {
        output.texture->Release();
        output = {};
        return false;
    }
    return true;
}
} // namespace

void release() noexcept {
    for (auto& slot : g_slots) free(slot);
    g_device = nullptr;
    g_frame = -1;
}
void begin_frame(ID3D11Device* device) noexcept {
    if (g_device != device) { release(); g_device = device; }
    const int frame = ImGui::GetFrameCount();
    if (g_frame == frame) return;
    g_frame = frame;
    for (int count = 0; count < 2; ++count) {
        IconResult result{};
        if (!catalog::take_icon(result)) break;
        for (auto& slot : g_slots) {
            if (slot.request != result.request) continue;
            if (result.available) (void)upload(result.icon, slot.gpu);
            // Failed reads/uploads are cached too; scrolling cannot retry them every frame.
            break;
        }
    }
}
ImTextureID get(std::uint16_t index) noexcept {
    if (index == package::kNoIcon || !g_device) return ImTextureID_Invalid;
    Slot* oldest = nullptr;
    for (auto& slot : g_slots) {
        if (slot.index == index) {
            slot.used = g_frame;
            return slot.gpu.view ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(slot.gpu.view))
                                 : ImTextureID_Invalid;
        }
        // Never release a view already referenced by this frame's ImDrawData.
        if (slot.used != g_frame && (!oldest || slot.used < oldest->used)) oldest = &slot;
    }
    if (!oldest) return ImTextureID_Invalid;
    const auto request = ++g_request;
    if (!catalog::request_icon(request, index)) return ImTextureID_Invalid;
    free(*oldest);
    oldest->index = index;
    oldest->request = request;
    oldest->used = g_frame;
    return ImTextureID_Invalid;
}
} // namespace sunrise::client::ui::items::icons
