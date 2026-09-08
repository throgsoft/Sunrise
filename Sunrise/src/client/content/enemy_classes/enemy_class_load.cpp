#include "enemy_class_load.h"

#include <Windows.h>

#include <cstring>
#include <memory>
#include <mutex>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/enemy_classes/definition.h"
#include "../../../state/build_data/enemy_classes/enemy_class_catalog.h"
#include "../items/packages/internal.h"

namespace sunrise::client::content::enemy_classes {
namespace {
namespace packages = items::packages;
namespace reader = middleware::content::packages::reader;
namespace domain = state::build_data::enemy_classes;
std::mutex g_loadMutex;
struct Storage {
    reader::Scratch scratch{};
    std::vector<std::byte> container{}, child{}, root{}, classes{}, races{};
    domain::Catalog catalog{};
    ~Storage() {
        reader::close_files(scratch);
    }
};
bool read_ref(std::span<const std::byte> blob, std::size_t offset, std::uint32_t& tag) noexcept {
    if (offset > blob.size() || sizeof tag > blob.size() - offset) return false;
    std::memcpy(&tag, blob.data() + offset, sizeof tag);
    return tag >= 0x80800000U && tag < 0x82000000U;
}
} // namespace
bool ensure() noexcept {
    const std::lock_guard lock(g_loadMutex);
    if (domain::count() != 0) return true;
    auto storage = std::make_unique<Storage>();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!packages::collect_keys(keys)) return false;
    if (!packages::package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        return false;
    }
    const reader::Source source{directory.chars.data(), &keys};
    std::uint32_t classTag{}, definitionClass{}, raceTag{};
    const char* stage = "investment_root";
    const bool loaded = [&] {
        std::array<std::uint32_t, packages::kContainerCandidates> candidates{};
        std::size_t candidateCount{};
        if (!packages::investment_globals_tags(candidates, candidateCount)) return false;
        bool rootRead = false;
        for (std::size_t i = 0; i < candidateCount && !rootRead; ++i) {
            std::uint32_t rootTag{}, rootClass{};
            rootRead =
                reader::read_tag(source, storage->scratch, candidates[i], storage->container)
                && packages::tables::child_tag(
                    storage->container, packages::tables::kInvestmentRootChild, rootTag)
                && rootTag != 0
                && reader::read_tag(source, storage->scratch, rootTag, storage->root, rootClass)
                && rootClass == packages::tables::kInvestmentRootClass;
        }
        if (!rootRead) return false;
        // Native globals +170 belongs to the OUTER 80805BB1 container. The helper's
        // root is its 80807D84 child, which owns the separate race table at +1A8.
        stage = "class_reference";
        if (!read_ref(storage->container, 0x170, classTag)) return false;
        stage = "class_table";
        if (!reader::read_tag(source, storage->scratch, classTag, storage->classes, definitionClass)
            || definitionClass != domain::kClassRootClass)
            return false;
        stage = "taxonomy_reference";
        if (!read_ref(storage->root, 0x1A8, raceTag)) return false;
        stage = "taxonomy_table";
        if (!reader::read_tag(source, storage->scratch, raceTag, storage->races, definitionClass)
            || definitionClass != domain::kTaxonomyRootClass)
            return false;
        stage = "parse";
        if (!domain::parse(storage->classes, storage->races, storage->catalog)) return false;
        stage = "publish";
        return domain::replace(storage->catalog);
    }();
    SecureZeroMemory(&keys, sizeof keys);
    core::log::writef(
        core::log::Channel::client,
        loaded ? core::log::Level::info : core::log::Level::warn,
        "ev=enemy_class_catalog result=%s rows=%zu stage=%s class_tag=%08X taxonomy_tag=%08X",
        loaded ? "loaded" : "unavailable",
        domain::count(),
        stage,
        classTag,
        raceTag);
    return loaded;
}
} // namespace sunrise::client::content::enemy_classes
