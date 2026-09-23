#include "items_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <mutex>

#include "../../../state/build_data/runtime.h"
#include "../../content/items/packages/source.h"

namespace sunrise::client::ui::items::catalog {
namespace {
namespace data = state::build_data;
namespace content = client::content::items::packages;
struct Request {
    std::uint64_t id{};
    std::uint16_t index{};
};
std::mutex g_mutex;
HANDLE g_wake{}, g_worker{};
std::atomic<bool> g_stopping{true};
std::shared_ptr<const Catalog> g_catalog;
std::deque<Request> g_requests;
std::deque<IconResult> g_results;
std::size_t g_outstanding{};
// Bound decoded CPU images while the renderer drains completed requests.
constexpr std::size_t kIconRequestCapacity = 16;
// Resolve name, description and type in small batches so shutdown can cancel catalog loading.
constexpr std::size_t kItemsPerBatch = 8, kTextFieldsPerItem = 3;
const char* g_status = "Waiting for installed item definitions.";

struct Source {
    middleware::content::packages::reader::BlockKeys keys{};
    core::path::Buffer directory{};
    ~Source() {
        SecureZeroMemory(&keys, sizeof keys);
    }
};

Category category(const Entry& entry) noexcept {
    if (entry.identity.bucketId == data::bounties::kBountyBucketId
        && entry.identity.tier == data::bounties::kBountyTier) {
        return Category::bounties;
    }
    if (entry.identity.questInitialization.scope != data::items::QuestInitialization::Scope::none) {
        return Category::quests;
    }
    namespace buckets = data::inventory::buckets;
    buckets::Descriptor bucket{};
    if (!data::find_inventory_bucket_descriptor(entry.identity.bucketId, bucket)
        || bucket.bucketId != entry.identity.bucketId) {
        return Category::other;
    }
    if (bucket.arraySelector == buckets::ArraySelector::character) {
        // These are native bucket slots, not the ordinal values of State::EquipmentSlot.
        switch (bucket.equipmentSlot) {
        case 7: // Kinetic.
        case 8: // Energy.
        case 9: // Power.
            return Category::weapons;
        case 1: // Helmet.
        case 2: // Gauntlets.
        case 4: // Chest.
        case 5: // Legs.
        case 6: // Class item.
            return Category::armor;
        case 10: // Ship.
        case 11: // Vehicle.
        case 12: // Ghost.
        case 13: // Emblem.
        case 14: // Emotes.
        case 15: // Clan banner.
        case 17: // Finisher.
            return Category::cosmetics;
        default:
            break;
        }
        if (bucket.bucketId == buckets::kEngramBucketId) { // Engrams.
            return Category::engrams;
        }
    }
    if (bucket.arraySelector == buckets::ArraySelector::profile) {
        // Ornaments and shaders have their own structural profile buckets.
        if (bucket.bucketId == 13 || bucket.bucketId == 14) {
            return Category::cosmetics;
        }
        return Category::materialsAndConsumables;
    }
    return Category::other;
}

void resolve(package::PresentationReader& reader,
             std::span<const package::text::Reference> refs,
             std::span<std::string*> destinations) {
    package::text::Snapshot names;
    if (!reader.resolve(refs, names) || names.names.size() != destinations.size()) {
        return;
    }
    for (std::size_t i = 0; i < names.names.size(); ++i) {
        const auto& name = names.names[i];
        destinations[i]->assign(name.value.data(), name.length);
    }
}

std::shared_ptr<Catalog> build(package::PresentationReader& reader, bool packagesReady) {
    auto output = std::make_shared<Catalog>();
    std::vector<data::items::Definition> identities(data::items::kDefinitionCapacity);
    std::size_t count{};
    if (!data::items::snapshot(identities, count) || count == 0) {
        return {};
    }
    output->entries.resize(count);
    for (std::size_t first = 0; first < count; first += kItemsPerBatch) {
        if (g_stopping.load(std::memory_order_relaxed)) {
            return {};
        }
        std::array<package::text::Reference, kItemsPerBatch * kTextFieldsPerItem> refs{};
        std::array<std::string*, kItemsPerBatch * kTextFieldsPerItem> destinations{};
        const auto end = (std::min)(first + kItemsPerBatch, count);
        for (auto i = first; i < end; ++i) {
            auto& entry = output->entries[i];
            entry.identity = identities[i];
            package::Display display{};
            if (packagesReady
                && reader.item(
                    entry.identity.definitionIndex, entry.identity.definitionHash, display)) {
                entry.iconIndex = display.iconIndex;
                refs[(i - first) * kTextFieldsPerItem] = display.name;
                refs[(i - first) * kTextFieldsPerItem + 1] = display.description;
                refs[(i - first) * kTextFieldsPerItem + 2] = display.itemType;
            }
            destinations[(i - first) * kTextFieldsPerItem] = &entry.name;
            destinations[(i - first) * kTextFieldsPerItem + 1] = &entry.description;
            destinations[(i - first) * kTextFieldsPerItem + 2] = &entry.itemType;
        }
        if (packagesReady) {
            resolve(reader,
                    std::span(refs).first((end - first) * kTextFieldsPerItem),
                    std::span(destinations).first((end - first) * kTextFieldsPerItem));
        }
        for (auto i = first; i < end; ++i) {
            auto& entry = output->entries[i];
            entry.category = category(entry);
            output->names += !entry.name.empty();
            entry.search = entry.name + " " + entry.description + " " + entry.itemType;
            append_identity(
                entry.search, entry.identity.definitionIndex, entry.identity.definitionHash);
            entry.search = search_text(std::move(entry.search));
        }
    }
    return output;
}

DWORD WINAPI run(void*) noexcept {
    try {
        auto source = std::make_unique<Source>();
        const bool available =
            content::collect_keys(source->keys) && content::package_directory(source->directory);
        auto reader = std::make_unique<package::PresentationReader>(
            middleware::content::packages::reader::Source{
                {source->directory.chars.data(), source->directory.length}, &source->keys});
        const bool readable = available && reader->initialize();
        bool needCatalog{};
        {
            const std::lock_guard lock(g_mutex);
            needCatalog = !g_catalog;
        }
        if (needCatalog) {
            auto pending = build(*reader, readable);
            const std::lock_guard lock(g_mutex);
            if (g_stopping.load(std::memory_order_relaxed)) {
                return 0;
            }
            g_catalog = std::move(pending);
            g_status = g_catalog
                           ? (readable ? "Catalog ready." : "Package text and icons unavailable.")
                           : "Item catalog unavailable.";
        }
        while (!g_stopping.load(std::memory_order_relaxed)) {
            Request request{};
            {
                std::unique_lock lock(g_mutex);
                if (g_requests.empty()) {
                    lock.unlock();
                    WaitForSingleObject(g_wake, INFINITE);
                    continue;
                }
                request = g_requests.front();
                g_requests.pop_front();
            }
            IconResult result{};
            result.request = request.id;
            result.available = readable && reader->icon(request.index, result.icon);
            const std::lock_guard lock(g_mutex);
            if (!g_stopping.load(std::memory_order_relaxed)) {
                g_results.push_back(std::move(result));
            }
        }
    } catch (...) {
        const std::lock_guard lock(g_mutex);
        g_stopping.store(true, std::memory_order_relaxed);
        g_status = "Package loading failed.";
        g_requests.clear();
        g_results.clear();
        g_outstanding = 0;
    }
    return 0;
}
} // namespace

std::string search_text(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool matches(std::string_view text, std::string_view query) noexcept {
    while (!query.empty()) {
        const auto first = query.find_first_not_of(" \t");
        if (first == std::string_view::npos) {
            return true;
        }
        query.remove_prefix(first);
        const auto end = query.find_first_of(" \t");
        if (text.find(query.substr(0, end)) == std::string_view::npos) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        query.remove_prefix(end);
    }
    return true;
}

void append_identity(std::string& text, std::uint32_t index, std::uint32_t hash) {
    char identity[64]{};
    std::snprintf(identity, sizeof identity, " %u 0x%08x %u 0x%08x ", index, index, hash, hash);
    text += identity;
}

void start() noexcept {
    const std::lock_guard lock(g_mutex);
    if (g_worker || !data::item_definitions_ready()) {
        return;
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_wake) {
        g_stopping.store(false, std::memory_order_relaxed);
        g_worker = CreateThread(nullptr, 0, &run, nullptr, 0, nullptr);
    }
    if (!g_worker) {
        g_stopping.store(true, std::memory_order_relaxed);
        if (g_wake) {
            CloseHandle(g_wake);
            g_wake = nullptr;
        }
        g_status = "Could not start package reader.";
        return;
    }
    g_status = "Reading installed item presentation...";
}
void stop() noexcept {
    HANDLE worker = nullptr;
    {
        const std::lock_guard lock(g_mutex);
        g_stopping.store(true, std::memory_order_relaxed);
        worker = g_worker;
        if (g_wake) {
            SetEvent(g_wake);
        }
    }
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
    }
    const std::lock_guard lock(g_mutex);
    if (g_worker) {
        CloseHandle(g_worker);
        CloseHandle(g_wake);
        g_worker = g_wake = nullptr;
    }
    g_requests.clear();
    g_results.clear();
    g_outstanding = 0;
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
        if (!g_catalog || g_outstanding >= kIconRequestCapacity
            || g_stopping.load(std::memory_order_relaxed)) {
            return false;
        }
        g_requests.push_back({request, index});
        ++g_outstanding;
        SetEvent(g_wake);
        return true;
    } catch (...) {
        return false;
    }
}
bool take_icon(IconResult& output) noexcept {
    const std::lock_guard lock(g_mutex);
    if (g_results.empty()) {
        return false;
    }
    output = std::move(g_results.front());
    g_results.pop_front();
    --g_outstanding;
    return true;
}
} // namespace sunrise::client::ui::items::catalog
