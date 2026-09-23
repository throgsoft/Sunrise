#pragma once

#include "core/filesystem/path.h"
#include "middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::items::packages {
/** Copies installed block keys. The caller must clear them after its reads finish. */
[[nodiscard]] bool collect_keys(middleware::content::packages::reader::BlockKeys& keys) noexcept;
/** Resolves the installed package directory beside the game executable. */
[[nodiscard]] bool package_directory(core::path::Buffer& directory) noexcept;
} // namespace sunrise::client::content::items::packages
