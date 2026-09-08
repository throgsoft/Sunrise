#include "console_overlay.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <imgui.h>
#include <string_view>

#include "../../core/ui/modules/registry/ui_module_registry.h"
#include "client_console_commands.h"
#include "console_line.h"
#include "console_registry.h"

namespace sunrise::client::console {
namespace {

constexpr std::size_t kScrollbackCapacity = 256;
constexpr std::size_t kOutputLineCapacity = 512;
constexpr std::size_t kHistoryCapacity = 64;
using Input = std::array<char, kLineCapacity>;
std::array<std::array<char, kOutputLineCapacity>, kScrollbackCapacity> g_lines{};
// Every retained line, newline and terminator fits; never undersize the flattened buffer.
std::array<char, kScrollbackCapacity * kOutputLineCapacity + 1> g_flat{};
std::size_t g_lineCount{};
bool g_dirty{};
std::array<Input, kHistoryCapacity> g_history{};
std::size_t g_historyCount{};
std::size_t g_historyCursor{};
Input g_input{};
Input g_draft{};
bool g_focusPending{true};
bool g_initialized{};
core::ui::modules::registry::PageRegistration g_page;

class Scrollback final : public Output {
public:
    void line(std::string_view text) noexcept override {
        if (g_lineCount == g_lines.size()) {
            std::move(g_lines.begin() + 1, g_lines.end(), g_lines.begin());
            --g_lineCount;
        }
        auto& target = g_lines[g_lineCount++];
        target = {};
        if (!text.empty()) {
            std::memcpy(target.data(), text.data(), (std::min)(text.size(), target.size() - 1));
        }
        g_dirty = true;
    }
};

void rebuild_scrollback() noexcept {
    if (!g_dirty) return;
    std::size_t at = 0;
    for (std::size_t index = 0; index < g_lineCount; ++index) {
        const std::size_t length = std::strlen(g_lines[index].data());
        std::memcpy(g_flat.data() + at, g_lines[index].data(), length);
        at += length;
        g_flat[at++] = '\n';
    }
    g_flat[at] = '\0';
    g_dirty = false;
}

bool help_command(std::span<const Value> arguments, Output& output) noexcept {
    if (!arguments.empty()) {
        Entry entry{};
        if (!find(arguments[0].text, entry)) {
            output.line("Unknown command; use help to list available commands.");
            return false;
        }
        output.format("%s: %s", entry.name, entry.help);
        for (const auto& parameter : entry.parameters) {
            if (parameter.name == nullptr) break;
            output.format("  %s%s: %s",
                          parameter.name,
                          parameter.optional ? " (optional)" : "",
                          parameter.help);
        }
        return true;
    }
    std::array<Entry, kEntryCapacity> entries{};
    std::size_t count = 0;
    if (!snapshot(entries, count)) {
        output.line("Command registry snapshot unavailable.");
        return false;
    }
    output.format("%zu commands; help <command> describes arguments:", count);
    for (std::size_t index = 0; index < count; ++index) {
        output.format("  %-24s %s", entries[index].name, entries[index].help);
    }
    return true;
}

const char* command_choice(std::size_t index) noexcept {
    static std::array<Entry, kEntryCapacity> entries{};
    static std::size_t count = 0;
    if (index == 0 && !snapshot(entries, count)) count = 0;
    return index < count ? entries[index].name : nullptr;
}

bool clear_command(std::span<const Value>, Output&) noexcept {
    g_lineCount = 0;
    g_dirty = true;
    return true;
}

bool copy_command(std::span<const Value>, Output& output) noexcept {
    rebuild_scrollback();
    ImGui::SetClipboardText(g_flat.data());
    output.line("Copy requested through the existing ImGui clipboard backend.");
    return true;
}

void remember() noexcept {
    if (g_historyCount == 0 || g_history[g_historyCount - 1] != g_input) {
        if (g_historyCount == g_history.size()) {
            std::move(g_history.begin() + 1, g_history.end(), g_history.begin());
            --g_historyCount;
        }
        g_history[g_historyCount++] = g_input;
    }
    g_historyCursor = g_historyCount;
    g_draft = {};
}

int input_event(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Complete only up to the caret and retain the suffix after it.
        const std::string_view before(data->Buf, static_cast<std::size_t>(data->CursorPos));
        std::array<std::string_view, kCandidateCapacity> candidates{};
        std::size_t count = 0;
        std::string_view prefix{};
        if (!complete(before, candidates, count, prefix)) return 0;
        const auto matches = std::span<const std::string_view>(candidates.data(), count);
        const auto shared = shared_prefix(matches);
        const int start = data->CursorPos - static_cast<int>(prefix.size());
        const std::size_t length =
            static_cast<std::size_t>(data->BufTextLen) - prefix.size() + shared.size();
        if (shared.size() > prefix.size() && length < static_cast<std::size_t>(data->BufSize)) {
            data->DeleteChars(start, static_cast<int>(prefix.size()));
            data->InsertChars(start, shared.data(), shared.data() + shared.size());
        }
        if (count > 1) {
            Scrollback output;
            for (const auto candidate : matches) {
                output.format("  %.*s", static_cast<int>(candidate.size()), candidate.data());
            }
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (g_historyCount == 0) return 0;
        if (g_historyCursor == g_historyCount) {
            g_draft = {};
            std::memcpy(g_draft.data(), data->Buf, static_cast<std::size_t>(data->BufTextLen));
        }
        if (data->EventKey == ImGuiKey_UpArrow && g_historyCursor > 0) --g_historyCursor;
        if (data->EventKey == ImGuiKey_DownArrow && g_historyCursor < g_historyCount) {
            ++g_historyCursor;
        }
        const auto& recalled =
            g_historyCursor < g_historyCount ? g_history[g_historyCursor] : g_draft;
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, recalled.data());
    }
    return 0;
}

void draw() noexcept {
    ImGui::TextUnformatted("Developer console");
    ImGui::TextDisabled("Tab completes; arrows recall. Select output and Ctrl+C to copy.");
    rebuild_scrollback();
    const float outputHeight =
        (std::max)(ImGui::GetTextLineHeight() * 3.0F,
                   ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.0F);
    ImGui::InputTextMultiline("##console-output",
                              g_flat.data(),
                              g_flat.size(),
                              {-1.0F, outputHeight},
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
    if (g_focusPending) {
        ImGui::SetKeyboardFocusHere();
        g_focusPending = false;
    }
    ImGui::SetNextItemWidth(-1.0F);
    constexpr auto flags = ImGuiInputTextFlags_EnterReturnsTrue
                           | ImGuiInputTextFlags_CallbackCompletion
                           | ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputText("##console-input", g_input.data(), g_input.size(), flags, &input_event)) {
        if (std::string_view(g_input.data()).find_first_not_of(" \t") != std::string_view::npos) {
            Scrollback output;
            output.format("> %s", g_input.data());
            remember();
            (void)invoke(g_input.data(), output);
        }
        g_input = {};
        g_focusPending = true;
    }
}

} // namespace

bool initialize() noexcept {
    if (g_initialized) return true;
    Entry help{"help", "Lists commands or describes one.", &help_command};
    help.parameters[0] = {
        "command", "Command name.", ValueType::text, &command_choice, 0.0, 0.0, true};
    if (!add(help) || !add({"clear", "Clears the retained output.", &clear_command})
        || !add({"copy",
                 "Copies retained output through the upstream ImGui clipboard backend.",
                 &copy_command})
        || !install_commands()
        || !g_page.acquire(core::ui::modules::Owner::client, "client.console", "Console", &draw)) {
        clear();
        return false;
    }
    g_initialized = true;
    return true;
}

void shutdown() noexcept {
    g_page.release();
    clear();
    g_lines = {};
    g_flat = {};
    g_lineCount = 0;
    g_dirty = false;
    g_history = {};
    g_historyCount = 0;
    g_historyCursor = 0;
    g_input = {};
    g_draft = {};
    g_focusPending = true;
    g_initialized = false;
}

} // namespace sunrise::client::console
