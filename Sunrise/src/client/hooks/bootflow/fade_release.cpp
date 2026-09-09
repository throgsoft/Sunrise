#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../teleport/runtime.h"
#include "bootflow_hook_lifecycle.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

using core::log::kLineCapacity;

/**
 * The narrow channel release: one channel, with a blend time, so the world fades in.
 * Anchored on the load of the channel key, then run on through the argument spills, because the
 * wildcarded frame size leaves the head too short to be unique.
 */
constexpr std::string_view kReleaseSignatureText =
    "48 83 EC ? 8B 02 0F 57 C0 F3 0F 10 0D ? ? ? ? 48 8D 54 24 ? F3 0F 11 44 24";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kReleaseSignature =
    signature<signature_length(kReleaseSignatureText)>(kReleaseSignatureText);

/**
 * The fade manager accessor: one load-effective-address of its static object, then a return.
 * Every displacement is wildcarded, so the match runs past the return to stay unique. The bytes
 * after it are the next function's prologue and its thread-block read in this build.
 */
constexpr std::string_view kAccessorSignatureText =
    "48 8D 05 ? ? ? ? C3 48 63 C0 E9 ? ? ? ? 48 83 EC ? 65 48 8B 04 25 58 00 00 00";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kAccessorSignature =
    signature<signature_length(kAccessorSignatureText)>(kAccessorSignatureText);

/** Byte offsets inside the accessor, used to decode the object address from its operand. */
struct AccessorLayout {
    /** The 4-byte displacement follows the 2-byte load opcode. */
    static constexpr std::size_t displacement = 3;
    /** The instruction after the load, which the displacement is relative to. */
    static constexpr std::size_t nextInstruction = 7;
};

/** The world-transition fade channel. Stage two of the transition arms it with opaque black. */
constexpr std::uint32_t kWorldTransitionChannel = 0x57572DAC;
/** The channel's colour is a static initialiser, so it needs no lookup. */
constexpr std::array<float, 4> kOpaqueBlack{0.0F, 0.0F, 0.0F, 1.0F};
/** Blend seconds, so the world fades in rather than popping. */
constexpr float kFadeInSeconds = 0.5F;

using ReleaseChannel = std::int64_t(__fastcall*)(void*, std::uint32_t*, float*, float) noexcept;

void* g_manager{nullptr};
std::atomic<ReleaseChannel> g_release{nullptr};
std::atomic_bool g_released{false};
std::atomic_bool g_waitReported{false};

} // namespace

/** Re-arms the release, so the next world load fades in once and logs its own line. */
void rearm_fade_release() noexcept {
    g_released.store(false, std::memory_order_release);
    g_waitReported.store(false, std::memory_order_relaxed);
}

/** Releases the transition only once the destination and the local player's biped are present. */
void release_world_fade() noexcept {
    const ReleaseChannel release = g_release.load(std::memory_order_acquire);
    if (release == nullptr || g_manager == nullptr
        || g_released.load(std::memory_order_relaxed) || !in_world()) {
        return;
    }
    const CurrentSliceSet slice = current_slice_set();
    std::uint32_t controlled = 0xFFFFFFFFU;
    const bool playerPresent = teleport::current_controlled_handle(controlled);
    // Step 38 can precede the local spawn. Keep polling until the native getter supplies a
    // biped, including when a slow load has already consumed its last spawn-gate callback.
    if (!slice.present || !playerPresent) {
        if (!g_waitReported.exchange(true, std::memory_order_relaxed)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             !slice.present
                                 ? "ev=bootflow stage=fade_release result=waiting reason=slice_set"
                                 : "ev=bootflow stage=fade_release result=waiting reason=player");
        }
        return;
    }
    // Fire once per arming. The camera poll calls this every frame after arrival, and
    // restarting the blend each frame re-slams the channel to black, which reads as flicker. The
    // off-destination step re-arms it through `rearm_fade_release`.
    if (g_released.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::uint32_t channel = kWorldTransitionChannel;
    // The native channel writer loads this argument with MOVAPS. std::array alone
    // guarantees only float alignment; optimized callers can otherwise pass rsp+8 mod 16.
    alignas(16) std::array<float, 4> colour = kOpaqueBlack;
    (void)release(g_manager, &channel, colour.data(), kFadeInSeconds);
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bootflow stage=fade_release result=issued channel=0x%X "
                                      "slice_set=%d controlled=0x%X",
                                      kWorldTransitionChannel,
                                      slice.index,
                                      controlled);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Finds the fade release and its manager object. */
bool install_fade_release() noexcept {
    std::byte* const release = scan_main_image_unique(kReleaseSignature, "fade_release_channel");
    std::byte* const accessor = scan_main_image_unique(kAccessorSignature, "fade_manager_accessor");
    if (release == nullptr || accessor == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=fade_release result=fail reason=target");
        return false;
    }
    // The manager is the static object the accessor returns. The address comes from that
    // instruction's own operand, not from a stored offset.
    g_manager = resolve_relative(accessor + AccessorLayout::displacement,
                                 accessor + AccessorLayout::nextInstruction);
    g_release.store(reinterpret_cast<ReleaseChannel>(release), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=fade_release result=ok");
    return true;
}

/** Clears the fade release it found. */
void uninstall_fade_release() noexcept {
    g_release.store(nullptr, std::memory_order_release);
    g_manager = nullptr;
    g_released.store(false, std::memory_order_release);
    g_waitReported.store(false, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::bootflow
