#pragma once

#include <cstddef>
#include <span>

#include "../../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::items::packages {
/** Loads objective metadata from the current package root, including when details came from cache.
 */
[[nodiscard]] bool build_objectives(const middleware::content::packages::reader::Source& source,
                                    middleware::content::packages::reader::Scratch& scratch,
                                    std::span<const std::byte> root) noexcept;
} // namespace sunrise::client::content::items::packages
