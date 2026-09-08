#include "console_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

#include "../../core/ui/fonts/runtime/ui_runtime_font_lifecycle.h"
#include "../../core/ui/runtime/ui_visibility_runtime.h"
#include "client_console_commands.h"
#include "console_line.h"
#include "console_registry.h"

namespace sunrise::client::console {
namespace {

constexpr std::size_t kScrollbackCapacity = 512;
constexpr std::size_t kOutputLineCapacity = 512;
constexpr std::size_t kHistoryCapacity = 64;
constexpr float kHeightShare = 0.45F;
constexpr float kWidthShare = 0.66F;
/** The console prints denser text than the main UI. */
constexpr float kFontScale = 0.80F;
/** The echoed line and the completion hint both print at this weight. */
constexpr ImVec4 kEchoColor{0.60F, 0.63F, 0.70F, 1.0F};
constexpr ImVec4 kHintColor{0.55F, 0.58F, 0.66F, 1.0F};
/** The single fill the whole pane paints with. Readable over a bright skybox, still see-through. */
constexpr ImVec4 kPaneColor{0.05F, 0.06F, 0.08F, 0.82F};
constexpr ImVec4 kTransparent{0.0F, 0.0F, 0.0F, 0.0F};
/** The drag handle. Opaque, so it reads as a handle and not as more of the body. */
constexpr ImVec4 kTitleColor{0.13F, 0.15F, 0.19F, 1.0F};

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
std::atomic_bool g_focusPending{true};
bool g_initialized{};
std::atomic_bool g_open{};
bool g_scrollPending{};

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
        g_scrollPending = true;
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

void draw_completion_hint(const ImVec2& inputPosition) noexcept {
    const std::string_view line(g_input.data());
    if (line.empty()) {
        return;
    }
    std::array<std::string_view, kCandidateCapacity> candidates{};
    std::size_t count = 0;
    std::string_view prefix{};
    if (!complete(line, candidates, count, prefix)) {
        return;
    }
    const std::string_view shared =
        shared_prefix(std::span<const std::string_view>(candidates.data(), count));
    if (shared.size() <= prefix.size()) {
        return;
    }
    const std::string_view remainder = shared.substr(prefix.size());
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    if (drawList == nullptr) {
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float typedWidth = ImGui::CalcTextSize(line.data(), line.data() + line.size()).x;
    const ImVec2 at{inputPosition.x + style.FramePadding.x + typedWidth,
                    inputPosition.y + style.FramePadding.y};
    drawList->AddText(
        at, ImGui::GetColorU32(kHintColor), remainder.data(), remainder.data() + remainder.size());
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
        || !install_commands()) {
        clear();
        return false;
    }
    g_initialized = true;
    return true;
}

void toggle() noexcept {
    g_open.store(!g_open.load());
    g_focusPending.store(true);
}

bool open() noexcept {
    return g_open.load();
}

bool captures_input() noexcept {
    return open() || core::ui::runtime::snapshot().visible;
}

bool draw() noexcept {
    if (!g_open.load()) {
        return false;
    }
    rebuild_scrollback();
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return false;
    }
    // Centred, and placed once. After that the window keeps wherever it was dragged to.
    const ImVec2 size{viewport->WorkSize.x * kWidthShare, viewport->WorkSize.y * kHeightShare};
    ImGui::SetNextWindowPos({viewport->WorkPos.x + ((viewport->WorkSize.x - size.x) * 0.5F),
                             viewport->WorkPos.y + ((viewport->WorkSize.y - size.y) * 0.5F)},
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    if (g_focusPending) {
        ImGui::SetNextWindowFocus();
    }
    // Square: a terminal has corners.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    // One pane: the scrollback, the prompt and the title all sit on the window's own fill, so
    // nothing inside paints a second surface over it.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPaneColor);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kTransparent);
    // The title strip is what the window is dragged by, so it is opaque: a translucent one reads
    // as more body rather than as a handle.
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kTitleColor);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kTitleColor);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, kTitleColor);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin("Sunrise console", nullptr, kFlags);
    ImFont* const monospace = core::ui::fonts::runtime::monospace();
    if (monospace != nullptr) {
        ImGui::PushFont(monospace, ImGui::GetStyle().FontSizeBase * kFontScale);
    }
    if (!visible) {
        if (monospace != nullptr) {
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor(8);
        ImGui::PopStyleVar(8);
        return true;
    }

    const float promptHeight = ImGui::GetFrameHeightWithSpacing();
    // A field, not a column of labels: Dear ImGui can only highlight and copy text inside one, and
    // reading a command's output usually ends in wanting to paste it somewhere. Read-only rather
    // than disabled, because a disabled field cannot be selected either.
    ImGui::InputTextMultiline("##scrollback",
                              g_flat.data(),
                              g_flat.size(),
                              {-1.0F, -promptHeight},
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
    if (g_scrollPending) {
        // The field scrolls inside its own child window, which is where the tail has to be set.
        if (ImGuiWindow* const view = ImGui::FindWindowByID(ImGui::GetID("##scrollback"))) {
            view->Scroll.y = view->ScrollMax.y;
        }
        g_scrollPending = false;
    }

    ImGui::Separator();
    ImGui::TextUnformatted(">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0F);
    constexpr ImGuiInputTextFlags kInputFlags = ImGuiInputTextFlags_EnterReturnsTrue
                                                | ImGuiInputTextFlags_CallbackCompletion
                                                | ImGuiInputTextFlags_CallbackHistory;
    if (g_focusPending.exchange(false)) {
        ImGui::SetKeyboardFocusHere();
    }
    const ImVec2 inputPosition = ImGui::GetCursorScreenPos();
    const bool entered =
        ImGui::InputText("##line", g_input.data(), g_input.size(), kInputFlags, &input_event);
    draw_completion_hint(inputPosition);
    if (entered) {
        if (std::string_view(g_input.data()).find_first_not_of(" \t") != std::string_view::npos) {
            Scrollback output;
            output.format("> %s", g_input.data());
            remember();
            (void)invoke(g_input.data(), output);
        }
        g_input = {};
        g_focusPending = true;
    }
    if (monospace != nullptr) {
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(8);
    return true;
}

void shutdown() noexcept {
    g_open.store(false);
    g_scrollPending = false;
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
