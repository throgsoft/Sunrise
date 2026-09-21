#pragma once

#include "../internal.h"

namespace sunrise::server::bap::encrypted {
/** Publishes a credited batch or its later empty revision, respecting queue observation
 * grace and retained-row overlays. Frame construction and State mutation commit together. */
[[nodiscard]] bool consume_dawning_pickup_release(Session& session, Scratch& scratch,
    std::span<std::byte> response, std::size_t& written, bool& touchesScratch) noexcept;
}
