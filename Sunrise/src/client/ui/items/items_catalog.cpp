#include "items_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "items_service_bridge.h"
#include "../../../state/account/inventory/dawning_oven_state.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/progression/season_pass_reward_catalog.h"
#include "../../../state/runtime/item_discard_policy.h"
#include "../../content/items/packages/internal.h"

namespace sunrise::client::ui::items::catalog {
namespace {
namespace data = state::build_data;
namespace content = client::content::items::packages;
struct Request { std::uint64_t id{}; std::uint16_t index{}; };
std::mutex g_mutex;
std::condition_variable_any g_wake;
std::shared_ptr<const Catalog> g_catalog;
std::deque<Request> g_requests;
std::deque<IconResult> g_results;
std::size_t g_outstanding{};
const char* g_status = "Waiting for installed item definitions.";
// Declared last: the worker joins before the data it borrows is destroyed.
std::jthread g_worker;

struct Source {
    middleware::content::packages::reader::BlockKeys keys{};
    core::path::Buffer directory{};
    ~Source() { SecureZeroMemory(&keys, sizeof keys); }
};

void number(std::string& search, std::uint32_t value) {
    char text[40]{};
    std::snprintf(text, sizeof text, " %u 0x%08X ", value, value);
    search += text;
}

Category category(const Entry& entry) noexcept {
    // The service distinguishes real residents from same-name preview/reward markers.
    // Its dummy result must win even when a marker has a plausible item type or bucket.
    if (entry.policy == GrantPolicy::dummy) return Category::dummies;
    namespace dawning = state::account::inventory::dawning;
    namespace pass = state::progression::season_pass;
    const auto hash = entry.identity.definitionHash;
    const auto ingredient = dawning::ingredient(hash);
    // Real Dawning pickups share character bucket 37 with other kinds of items. Their exact
    // pickup identities identify materials; the bucket alone identifies neither materials nor dummies.
    const bool knownMaterial = state::item_discard::find(hash)
        || pass::contains(pass::kDestinationResourceHashes, hash)
        || hash == state::runtime::detail::bounty_policy::kGlimmerHash
        || hash == dawning::kEssenceHash
        || (ingredient < dawning::kIngredientCount && hash == dawning::kIngredients[ingredient].pickupHash);
    if (knownMaterial) return Category::materials;
    if (entry.bounty) return Category::bounties;
    namespace buckets = data::inventory::buckets;
    buckets::Descriptor bucket{};
    if (!data::find_inventory_bucket_descriptor(entry.identity.bucketId, bucket)
        || bucket.bucketId != entry.identity.bucketId) return Category::other;
    if (bucket.arraySelector == buckets::ArraySelector::character) {
        // Native equipment slots, matching State's native-to-semantic equipment mapping.
        switch (bucket.equipmentSlot) {
        case 7: case 8: case 9: return Category::weapons;
        case 1: case 2: case 4: case 5: case 6: return Category::armor;
        case 10: case 11: case 12: case 13: case 14: case 15: case 17:
            return Category::cosmetics;
        default: break;
        }
        if (bucket.bucketId == 31) return Category::engrams;
    }
    if (bucket.arraySelector == buckets::ArraySelector::profile) {
        // Ornaments and shaders have their own structural profile buckets.
        if (bucket.bucketId == 13 || bucket.bucketId == 14) return Category::cosmetics;
        // Materials share profile storage with consumables. The installed type label refines
        // this otherwise ambiguous display subdivision; it never changes grant authority.
        if (entry.itemType.ends_with("Material")
            || entry.itemType.ends_with("Materials") || entry.itemType == "Currency")
            return Category::materials;
        if (bucket.bucketId == 28 || bucket.bucketId == 15) return Category::consumables;
    }
    return Category::other;
}

void resolve(package::PresentationReader& reader,
             std::span<const package::text::Reference> refs,
             std::span<std::string*> destinations) {
    package::text::Snapshot names;
    if (!reader.resolve(refs, names) || names.names.size() != destinations.size()) return;
    for (std::size_t i = 0; i < names.names.size(); ++i) {
        const auto& name = names.names[i];
        destinations[i]->assign(name.value.data(), name.length);
    }
}

std::shared_ptr<Catalog> build(package::PresentationReader& reader,
                              bool packagesReady, std::stop_token stop) {
    auto output = std::make_shared<Catalog>();
    output->packagesReady = packagesReady;
    std::vector<data::items::Definition> identities(data::items::kDefinitionCapacity);
    std::size_t count{};
    if (!data::items::snapshot(identities, count) || count == 0) return {};
    output->entries.resize(count);
    // Resolve each objective once. Literal English strings are capped by the existing resolver.
    std::vector<Objective> objectives(data::objectives::kDefinitionCapacity);
    for (std::size_t first = 0; first < data::objective_definition_count(); first += 32) {
        if (stop.stop_requested()) return {};
        std::array<package::text::Reference, 32> refs{};
        std::array<std::string*, 32> destinations{};
        const auto end = (std::min)({first + 32, data::objective_definition_count(), objectives.size()});
        for (auto i = first; i < end; ++i) {
            auto& target = objectives[i];
            data::objectives::Definition source{};
            target.index = static_cast<std::uint16_t>(i);
            target.resolved = data::find_objective_definition(target.index, source);
            target.hash = source.definitionHash;
            target.completion = source.completionValue;
            target.itemProgress = source.itemProgress;
            destinations[i - first] = &target.description;
            if (packagesReady && target.resolved)
                (void)reader.objective(target.index, target.hash, refs[i - first]);
        }
        if (packagesReady && end > first)
            resolve(reader, std::span(refs).first(end - first),
                    std::span(destinations).first(end - first));
        if (end == objectives.size()) break;
    }
    for (std::size_t first = 0; first < count; first += 8) {
        if (stop.stop_requested()) return {};
        std::array<package::text::Reference, 32> refs{};
        std::array<std::string*, 32> destinations{};
        const auto end = (std::min)(first + 8, count);
        for (auto i = first; i < end; ++i) {
            auto& entry = output->entries[i];
            entry.identity = identities[i];
            package::Display display{};
            if (packagesReady && reader.item(entry.identity.definitionIndex,
                                             entry.identity.definitionHash, display)) {
                entry.iconIndex = display.iconIndex;
                refs[(i - first) * 3] = display.name;
                refs[(i - first) * 3 + 1] = display.description;
                refs[(i - first) * 3 + 2] = display.itemType;
            }
            destinations[(i - first) * 3] = &entry.name;
            destinations[(i - first) * 3 + 1] = &entry.description;
            destinations[(i - first) * 3 + 2] = &entry.itemType;
            data::items::details::Definition detail{};
            entry.detailReady = data::find_configured_item_detail(entry.identity.definitionIndex, detail)
                                && detail.definitionHash == entry.identity.definitionHash
                                && detail.bucketId == entry.identity.bucketId;
            if (entry.detailReady) {
                entry.bounty = detail.bucketId == 40 && detail.lifetimeSeconds > 0
                               && detail.objectiveCount > 0 && detail.maxStackSize <= 1;
                entry.quantityLimit = detail.objectiveCount ? 1
                    : detail.instancedDefinitionState == data::items::details::InstancedDefinitionState::instanced
                        ? 64 : (std::max)(1, detail.maxStackSize);
                for (std::size_t lane = 0; lane < detail.objectiveCount
                     && lane < detail.objectiveIndices.size(); ++lane) {
                    const auto index = detail.objectiveIndices[lane];
                    if (index < objectives.size()) entry.objectives.push_back(objectives[index]);
                    else entry.objectives.push_back(Objective{index});
                }
            }
        }
        if (packagesReady)
            resolve(reader, std::span(refs).first((end - first) * 3),
                    std::span(destinations).first((end - first) * 3));
        for (auto i = first; i < end; ++i) {
            auto& entry = output->entries[i];
            entry.policy = service::classify(entry);
            if (const auto* variant = service::grant_variant(entry)) entry.grantVariant = variant;
            entry.category = category(entry);
            output->classified += entry.policy == GrantPolicy::legitimate;
            output->names += !entry.name.empty();
            output->descriptions += !entry.description.empty();
            entry.search = entry.name + " " + entry.description + " " + entry.itemType;
            number(entry.search, entry.identity.definitionIndex);
            number(entry.search, entry.identity.definitionHash);
            for (const auto& objective : entry.objectives) {
                entry.search += " " + objective.description;
                number(entry.search, objective.index);
                number(entry.search, objective.hash);
            }
            std::transform(entry.search.begin(), entry.search.end(), entry.search.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
    }
    return output;
}

void run(std::stop_token stop) noexcept {
    try {
        auto source = std::make_unique<Source>();
        const bool available = content::collect_keys(source->keys)
                               && content::package_directory(source->directory);
        auto reader = std::make_unique<package::PresentationReader>(
            middleware::content::packages::reader::Source{
                {source->directory.chars.data(), source->directory.length}, &source->keys});
        const bool readable = available && reader->initialize();
        bool needCatalog{};
        { const std::lock_guard lock(g_mutex); needCatalog = !g_catalog; }
        if (needCatalog) {
            auto pending = build(*reader, readable, stop);
            const std::lock_guard lock(g_mutex);
            if (stop.stop_requested()) return;
            g_catalog = std::move(pending);
            g_status = g_catalog ? (readable ? "Catalog ready." : "Package text and icons unavailable; Reload retries.")
                                 : "Item catalog unavailable; Reload retries.";
        }
        while (!stop.stop_requested()) {
            Request request{};
            {
                std::unique_lock lock(g_mutex);
                if (!g_wake.wait(lock, stop, [] { return !g_requests.empty(); })) break;
                request = g_requests.front();
                g_requests.pop_front();
            }
            IconResult result{};
            result.request = request.id;
            result.available = readable && reader->icon(request.index, result.icon);
            const std::lock_guard lock(g_mutex);
            if (!stop.stop_requested()) g_results.push_back(std::move(result));
        }
    } catch (...) {
        const std::lock_guard lock(g_mutex);
        g_status = "Package loading failed; Reload retries.";
        g_requests.clear();
        g_results.clear();
        g_outstanding = 0;
    }
}
} // namespace

void start() noexcept {
    if (g_worker.joinable() || !data::item_definitions_ready()) return;
    try {
        { const std::lock_guard lock(g_mutex); g_status = "Reading installed item presentation..."; }
        g_worker = std::jthread(&run);
    } catch (...) {
        const std::lock_guard lock(g_mutex);
        g_status = "Could not start package reader; Reload retries.";
    }
}
void stop() noexcept {
    if (g_worker.joinable()) {
        g_worker.request_stop();
        g_wake.notify_all();
        g_worker.join();
    }
    const std::lock_guard lock(g_mutex);
    g_requests.clear();
    g_results.clear();
    g_outstanding = 0;
}
void reload() noexcept {
    stop();
    { const std::lock_guard lock(g_mutex); g_catalog.reset(); }
    start();
}
std::shared_ptr<const Catalog> snapshot() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_catalog;
}
const char* status() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_status;
}
bool request_icon(std::uint64_t request, std::uint16_t index) noexcept {
    try {
        const std::lock_guard lock(g_mutex);
        if (!g_catalog || g_outstanding >= 16 || !g_worker.joinable()) return false;
        g_requests.push_back({request, index});
        ++g_outstanding;
        g_wake.notify_one();
        return true;
    } catch (...) { return false; }
}
bool take_icon(IconResult& output) noexcept {
    const std::lock_guard lock(g_mutex);
    if (g_results.empty()) return false;
    output = std::move(g_results.front());
    g_results.pop_front();
    --g_outstanding;
    return true;
}
} // namespace sunrise::client::ui::items::catalog
