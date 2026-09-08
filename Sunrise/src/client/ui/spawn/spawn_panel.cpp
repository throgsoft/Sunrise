#include "spawn_panel.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/ui/components/picker/ui_picker_component.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../content/entity_names/entity_name_cache.h"
#include "../../content/items/packages/internal.h"
#include "../../hooks/spawn/spawn_runtime.h"

namespace sunrise::client::ui::spawn {
namespace {

namespace native = client::hooks::spawn;
namespace package_reader = middleware::content::packages::reader;
namespace picker = core::ui::components::picker;

constexpr std::uint32_t kEntityClass = 0x80809C0FU;
constexpr std::string_view kNameObject = "\"entities\"";
constexpr std::uint64_t kMaximumNameFile = 32ULL * 1024ULL * 1024ULL;

enum class ObjectType : std::uint8_t {
    Inherited = 0,
    StaticMesh = 1, // Interactable
    PropSimpleDeprecated = 2,
    PropExpensiveDeprecated = 3,
    PropCosmeticStatic = 4,  // World effects / decorations
    PropCosmeticMovable = 5, // Moving props
    PropCosmeticMovableGarbage = 6,
    PropNetworkedStatic = 7,  // Ad spawns
    PropNetworkedMovable = 8, // Explodable
    PropCinematic = 9,
    Speedtree = 10,
    Interactive = 11,
    Biped = 12, // Guardians, Enemies, NPCs
    Creature = 13,
    Weapon = 14,  // Weapon props
    Vehicle = 15, // Sparrows, Pikes, Ships
    Turret = 16,  // VehicleEntity
    Emitter = 17, // Effects, some interactive projectiles
    Projectile = 18,
    Item = 19,
    ItemAmmo = 20,
    ItemLoot = 21,
    Gear = 22,
    HopOn = 23,
    HopOnGearBiped = 24,
    HopOnGearWeapon = 25,
    HopOnGearShip = 26,
    HopOnGearSparrow = 27,
    System = 28,
    Invalid = 0xFF,
};

constexpr std::array<const char*, 29> kObjectTypeNames{
    "Inherited",
    "StaticMesh",
    "PropSimpleDeprecated",
    "PropExpensiveDeprecated",
    "PropCosmeticStatic",
    "PropCosmeticMovable",
    "PropCosmeticMovableGarbage",
    "PropNetworkedStatic",
    "PropNetworkedMovable",
    "PropCinematic",
    "Speedtree",
    "Interactive",
    "Biped",
    "Creature",
    "Weapon",
    "Vehicle",
    "Turret",
    "Emitter",
    "Projectile",
    "Item",
    "ItemAmmo",
    "ItemLoot",
    "Gear",
    "HopOn",
    "HopOnGearBiped",
    "HopOnGearWeapon",
    "HopOnGearShip",
    "HopOnGearSparrow",
    "System",
};

static_assert(kObjectTypeNames.size() == static_cast<std::uint8_t>(ObjectType::System) + 1);

struct Candidate {
    std::uint32_t tag{};
    ObjectType type{};
    bool named{};
    std::array<char, 224> label{};
};

struct EntityName {
    std::uint32_t tag{};
    std::array<char, 144> text{};
    std::uint32_t order{};
};

struct Column {
    std::vector<Candidate> candidates{};
    std::vector<picker::Item> items{};
    std::size_t selected{};
    native::Settings settings{};
};

Column g_main{};
std::vector<EntityName> g_names{};
bool g_scanned{};

template <std::size_t Capacity>
[[nodiscard]] bool parse_string(std::string_view document,
                                std::size_t& cursor,
                                std::array<char, Capacity>& output) noexcept {
    output = {};
    if (cursor >= document.size() || document[cursor++] != '"') {
        return false;
    }
    std::size_t written = 0;
    while (cursor < document.size()) {
        char value = document[cursor++];
        if (value == '"') {
            return true;
        }
        if (value == '\\') {
            if (cursor >= document.size()) {
                return false;
            }
            value = document[cursor++];
            if (value == 'u') {
                if (cursor + 4 > document.size()) {
                    return false;
                }
                cursor += 4;
                value = '?';
            } else if (value == 'n') {
                value = '\n';
            } else if (value == 'r') {
                value = '\r';
            } else if (value == 't') {
                value = '\t';
            }
        }
        if (written + 1 < output.size()) {
            output[written++] = value;
        }
    }
    return false;
}

void skip_space(std::string_view document, std::size_t& cursor) noexcept {
    while (cursor < document.size()) {
        const char value = document[cursor];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            return;
        }
        ++cursor;
    }
}

[[nodiscard]] bool load_names() noexcept {
    g_names.clear();
    HMODULE module = nullptr;
    constexpr DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&load_names), &module) == FALSE) {
        return false;
    }

    std::vector<char> bytes{};
    core::path::Buffer path{};
    if (core::path::artifact_directory(module, path)
        && core::path::append(path, L"\\EntityNames.json")) {
        const HANDLE file = CreateFileW(path.chars.data(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr);
        LARGE_INTEGER length{};
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &length) != FALSE
            && length.QuadPart > 0
            && static_cast<std::uint64_t>(length.QuadPart) <= kMaximumNameFile) {
            bytes.resize(static_cast<std::size_t>(length.QuadPart));
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
                    == FALSE
                || read != bytes.size()) {
                bytes.clear();
            }
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
    if (bytes.empty()) {
        return false;
    }

    const std::string_view document(bytes.data(), bytes.size());
    std::size_t cursor = document.find(kNameObject);
    cursor = cursor == std::string_view::npos ? cursor : document.find('{', cursor);
    if (cursor == std::string_view::npos) {
        return false;
    }
    ++cursor;
    while (cursor < document.size()) {
        skip_space(document, cursor);
        if (cursor < document.size() && document[cursor] == ',') {
            ++cursor;
            skip_space(document, cursor);
        }
        if (cursor >= document.size() || document[cursor] == '}') {
            break;
        }
        std::array<char, 16> tagText{};
        if (!parse_string(document, cursor, tagText)) {
            return false;
        }
        std::uint32_t tag = 0;
        const char* const end = tagText.data() + std::strlen(tagText.data());
        const auto parsed = std::from_chars(tagText.data(), end, tag, 16);
        skip_space(document, cursor);
        if (parsed.ec != std::errc{} || parsed.ptr != end || cursor >= document.size()
            || document[cursor++] != ':') {
            return false;
        }
        skip_space(document, cursor);
        if (cursor >= document.size() || document[cursor++] != '[') {
            return false;
        }
        std::uint32_t order = 0;
        for (;;) {
            skip_space(document, cursor);
            if (cursor < document.size() && document[cursor] == ',') {
                ++cursor;
                skip_space(document, cursor);
            }
            if (cursor >= document.size()) {
                return false;
            }
            if (document[cursor] == ']') {
                ++cursor;
                break;
            }
            EntityName name{};
            name.tag = tag;
            name.order = order++;
            if (!parse_string(document, cursor, name.text)) {
                return false;
            }
            if (name.text[0] != '\0') {
                g_names.push_back(name);
            }
        }
    }
    std::sort(
        g_names.begin(), g_names.end(), [](const EntityName& first, const EntityName& second) {
            return first.tag != second.tag ? first.tag < second.tag : first.order < second.order;
        });
    return !g_names.empty();
}

[[nodiscard]] const char* name_of(std::uint32_t tag) noexcept {
    const auto found = std::lower_bound(
        g_names.begin(), g_names.end(), tag, [](const EntityName& value, std::uint32_t wanted) {
            return value.tag < wanted;
        });
    return found != g_names.end() && found->tag == tag ? found->text.data() : nullptr;
}

void family_text(std::wstring_view family, std::array<char, 96>& output) noexcept {
    output = {};
    const std::size_t count = (std::min)(family.size(), output.size() - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const wchar_t value = family[index];
        output[index] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
    }
}

[[nodiscard]] constexpr const char* object_type_name(ObjectType type) noexcept {
    if (type == ObjectType::Invalid) {
        return "Invalid";
    }
    const std::size_t index = static_cast<std::uint8_t>(type);
    return index < kObjectTypeNames.size() ? kObjectTypeNames[index] : nullptr;
}

void add_candidate(Column& column, std::uint32_t tag, std::uint8_t type, std::wstring_view family) {
    std::array<char, 96> package{};
    family_text(family, package);
    Candidate value{};
    value.tag = tag;
    value.type = static_cast<ObjectType>(type);
    const char* const name = name_of(tag);
    const char* typeName = object_type_name(value.type);
    std::array<char, 32> unknownType{};
    if (typeName == nullptr) {
        (void)std::snprintf(unknownType.data(), unknownType.size(), "Unknown(%u)", type);
        typeName = unknownType.data();
    }
    value.named = name != nullptr;
    if (name != nullptr) {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "%s | %s | 0x%08X | %s",
                            name,
                            typeName,
                            tag,
                            package.data());
    } else {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "0x%08X | %s | %s",
                            tag,
                            typeName,
                            package.data());
    }
    column.candidates.push_back(value);
}

bool collect_entity(void*, const package_reader::ClassEntry& entry) noexcept {
    if (!native::is_tag_resident(entry.tag)) {
        return true;
    }
    std::uint8_t type = 0;
    if (!native::object_type(entry.tag, type)) {
        return true;
    }
    if (name_of(entry.tag) != nullptr) {
        add_candidate(g_main, entry.tag, type, entry.packageFamily);
    }
    return true;
}

void finish_column(Column& column) {
    std::sort(column.candidates.begin(),
              column.candidates.end(),
              [](const Candidate& first, const Candidate& second) {
                  if (first.named != second.named) {
                      return first.named;
                  }
                  return std::string_view(first.label.data())
                         < std::string_view(second.label.data());
              });
    column.candidates.erase(std::unique(column.candidates.begin(),
                                        column.candidates.end(),
                                        [](const Candidate& first, const Candidate& second) {
                                            return first.tag == second.tag;
                                        }),
                            column.candidates.end());
    column.items.clear();
    column.items.reserve(column.candidates.size());
    for (const Candidate& candidate : column.candidates) {
        column.items.push_back({candidate.label.data()});
    }
    column.selected = 0;
}

void refresh() noexcept {
    g_main.candidates.clear();
    core::path::Buffer directory{};
    const bool hasDirectory = client::content::items::packages::package_directory(directory);
    if (hasDirectory) {
        (void)client::content::entity_names::ensure(directory.chars.data());
    }
    (void)load_names();
    if (native::ready() && hasDirectory) {
        package_reader::ScanResult result{};
        (void)package_reader::scan_class_entries(
            directory.chars.data(), kEntityClass, &collect_entity, nullptr, result);
        package_reader::release_caches();
    }
    finish_column(g_main);
    g_scanned = true;
}

[[nodiscard]] const char* preview(const Column& column) noexcept {
    return column.selected < column.candidates.size()
               ? column.candidates[column.selected].label.data()
               : "[None]";
}

[[nodiscard]] const char* result_text(native::Result result) noexcept {
    switch (result) {
    case native::Result::idle:
        return "Select an entity and choose where to spawn it.";
    case native::Result::queued:
        return "Spawn queued; waiting for the player update.";
    case native::Result::placed:
        return "Entity placed.";
    case native::Result::raycastMiss:
        return "No surface hit. Aim at a surface or increase ray distance.";
    case native::Result::playerUnavailable:
        return "Player or camera unavailable. Enter a playable area and retry.";
    case native::Result::factoryFailed:
        return "Entity creation failed. Try another loaded entity.";
    case native::Result::timedOut:
        return "Spawn timed out waiting for a player update.";
    case native::Result::cancelled:
        return "Spawn cancelled.";
    case native::Result::rejected:
        return "Spawn not accepted. Check settings, refresh loaded entities, or wait for the "
               "pending spawn.";
    }
    return "Spawn status unavailable.";
}

void draw_settings() noexcept {
    ImGui::SetNextItemWidth(160.0F);
    ImGui::DragFloat("Vertical lift", &g_main.settings.lift, 0.1F, -100.0F, 100.0F, "%.1f");
    ImGui::SetNextItemWidth(160.0F);
    ImGui::DragFloat("Ray distance", &g_main.settings.rayDistance, 1.0F, 1.0F, 2000.0F, "%.0f");
    ImGui::SetNextItemWidth(160.0F);
    ImGui::DragFloat("Scale", &g_main.settings.scale, 0.01F, 0.01F, 100.0F, "%.2f");
    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Camera rotation", &g_main.settings.useCameraRotation);
        ImGui::Checkbox("Override rotation", &g_main.settings.overrideRotation);
        ImGui::InputFloat3("Position offset", g_main.settings.offset.data(), "%.2f");
        if (g_main.settings.overrideRotation) {
            ImGui::InputFloat4("Rotation quaternion", g_main.settings.rotation.data(), "%.3f");
        }
        ImGui::TreePop();
    }
}

} // namespace

void draw() noexcept {
    if (!native::ready()) {
        ImGui::TextWrapped(
            "Developer-only native entity spawner. Enter a destination before enabling.");
        if (ImGui::Button("Enable native spawner") && native::install()) {
            refresh();
        }
        return;
    }
    if (!g_scanned) {
        refresh();
    }
    if (ImGui::Button("Refresh loaded entities")) {
        refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu named entities loaded", g_main.candidates.size());
    const std::span<const picker::Item> rows(g_main.items.data(), g_main.items.size());
    (void)picker::control("entity", preview(g_main), rows, g_main.selected);
    const bool pending = native::busy();
    ImGui::BeginDisabled(pending || g_main.selected >= g_main.candidates.size());
    // request() publishes queued or rejected under the same lock as the eventual outcome.
    if (ImGui::Button("At crosshair")) {
        (void)native::request(
            g_main.candidates[g_main.selected].tag, native::Origin::crosshair, 1, g_main.settings);
    }
    ImGui::SameLine();
    if (ImGui::Button("At player")) {
        (void)native::request(
            g_main.candidates[g_main.selected].tag, native::Origin::player, 1, g_main.settings);
    }
    ImGui::EndDisabled();
    if (pending) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            native::cancel();
        }
    }
    if (g_main.candidates.empty()) {
        ImGui::TextDisabled("Enter a playable area, then refresh to find loaded named entities.");
    }
    ImGui::TextWrapped("%s", result_text(native::last_result()));
    draw_settings();
}

} // namespace sunrise::client::ui::spawn
