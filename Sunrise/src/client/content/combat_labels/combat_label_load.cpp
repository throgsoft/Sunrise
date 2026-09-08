#include "combat_label_load.h"

#include <Windows.h>

#include <memory>
#include <mutex>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/combat_labels/catalog.h"
#include "../../../state/build_data/combat_labels/definition.h"
#include "../items/packages/internal.h"

namespace sunrise::client::content::combat_labels {
namespace {
namespace packages = items::packages;
namespace reader = middleware::content::packages::reader;
namespace domain = state::build_data::combat_labels;
std::mutex g_mutex;
struct Storage {
    reader::Scratch scratch{};
    std::vector<std::byte> blob{};
    domain::Catalog catalog{};
    ~Storage() {
        reader::close_files(scratch);
    }
};
} // namespace
bool ensure() noexcept {
    const std::lock_guard lock(g_mutex);
    if (domain::count() != 0) return true;
    const auto storage = std::make_unique<Storage>();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!packages::collect_keys(keys)) return false;
    if (!packages::package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        return false;
    }
    const reader::Source source{directory.chars.data(), &keys};
    std::uint32_t definitionClass{};
    // Installed-build 808094B4 catalog identity.
    // Resolve label positions and groups from its authored rows; never use capture-derived bits.
    const bool loaded =
        reader::read_tag(source, storage->scratch, 0x80C70CA1U, storage->blob, definitionClass)
        && definitionClass == domain::kRootClass && domain::parse(storage->blob, storage->catalog)
        && domain::replace(storage->catalog);
    SecureZeroMemory(&keys, sizeof keys);
    core::log::writef(core::log::Channel::client,
                      loaded ? core::log::Level::info : core::log::Level::warn,
                      "ev=combat_label_catalog result=%s labels=%zu class=%08X",
                      loaded ? "loaded" : "unavailable",
                      domain::count(),
                      definitionClass);
    return loaded;
}
} // namespace sunrise::client::content::combat_labels
