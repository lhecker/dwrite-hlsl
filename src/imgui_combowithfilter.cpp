// Source: https://gist.github.com/idbrii/5ddb2135ca122a0ec240ce046d9e6030

// My modified ComboWithFilter with fts_fuzzy_match as include.
// Using dear imgui, v1.92.0 WIP
//
// Adds arrow/pgup/pgdn navigation, Enter to confirm, max_height_in_items, and
// fixed focus on open and avoids drawing past window edges.
// My contributions are CC0/public domain.

// Posted in issue: https://github.com/ocornut/imgui/issues/1658#issuecomment-1086193100
// Demo: https://github.com/idbrii/cpp-imgui/blob/demo-combo-fuzz/examples/example_win32_directx12/main.cpp#L272-L283

#include "imgui_combowithfilter.h"

#include <algorithm> // std::sort

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include "imgui.h"

// https://github.com/forrestthewoods/lib_fts/blob/632ca1ea82bdf65688241bb8788c77cb242fba4f/code/fts_fuzzy_match.h
#define FTS_FUZZY_MATCH_IMPLEMENTATION
#include "fts_fuzzy_match.h"

// bgfx packs FontAwesome (and kenney image font) with its imgui and this is a
// magnifying glass. Feel free to change to u8"\u1F50D" or empty string.
static const char* ICON_FA_SEARCH = "\ue935";

namespace ImGui {

static bool sortbysec_desc(const std::pair<int, int>& a, const std::pair<int, int>& b) {
    return (b.second < a.second);
}

static int index_of_key(std::vector<std::pair<int, int>> pair_list, int key) {
    for (int i = 0; i < pair_list.size(); ++i) {
        auto& p = pair_list[i];
        if (p.first == key) {
            return i;
        }
    }
    return -1;
}

// Copied from imgui_widgets.cpp
static float CalcMaxPopupHeightFromItemCount(int items_count) {
    ImGuiContext& g = *GImGui;
    if (items_count <= 0)
        return FLT_MAX;
    return (g.FontSize + g.Style.ItemSpacing.y) * items_count - g.Style.ItemSpacing.y + (g.Style.WindowPadding.y * 2);
}

bool ComboWithFilter(const char* label, int* current_item, const std::vector<std::string>& items, int popup_max_height_in_items /*= -1*/) {
    using namespace fts;
    const bool allow_repeat = ImGuiInputFlags_Repeat;

    ImGuiContext& g = *GImGui;

    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiStyle& style = g.Style;

    int items_count = static_cast<int>(items.size());

    // Could use imgui Items_ getters to support more input formats.
    const char* preview_value = NULL;
    if (*current_item >= 0 && *current_item < items_count)
        preview_value = items[*current_item].c_str();

    static int focus_idx = -1;
    static char pattern_buffer[256] = {0};

    bool value_changed = false;

    const ImGuiID id = window->GetID(label);
    const ImGuiID popup_id = ImHashStr("##ComboPopup", 0, id); // copied from BeginCombo
    const bool is_already_open = IsPopupOpen(popup_id, ImGuiPopupFlags_None);
    const bool is_filtering = is_already_open && pattern_buffer[0] != '\0';

    int show_count = items_count;

    std::vector<std::pair<int, int>> itemScoreVector;
    if (is_filtering) {
        // Filter before opening to ensure we show the correct size window.
        // We won't get in here unless the popup is open.
        for (int i = 0; i < items_count; i++) {
            int score = 0;
            bool matched = fuzzy_match(pattern_buffer, items[i].c_str(), score);
            if (matched)
                itemScoreVector.push_back(std::make_pair(i, score));
        }
        std::sort(itemScoreVector.begin(), itemScoreVector.end(), sortbysec_desc);
        int current_score_idx = index_of_key(itemScoreVector, focus_idx);
        if (current_score_idx < 0 && !itemScoreVector.empty()) {
            focus_idx = itemScoreVector[0].first;
        }
        show_count = static_cast<int>(itemScoreVector.size());
    }

    // Define the height to ensure our size calculation is valid.
    if (popup_max_height_in_items == -1) {
        popup_max_height_in_items = 5;
    }
    popup_max_height_in_items = ImMin(popup_max_height_in_items, show_count);

    if (!(g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint)) {
        int items = popup_max_height_in_items + 3; // extra for search bar
        SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, CalcMaxPopupHeightFromItemCount(items)));
    }

    if (!BeginCombo(label, preview_value, ImGuiComboFlags_None))
        return false;

    if (!is_already_open) {
        focus_idx = *current_item;
        memset(pattern_buffer, 0, IM_ARRAYSIZE(pattern_buffer));
    }

    ImGui::PushItemWidth(-FLT_MIN);
    // Filter input
    if (!is_already_open)
        ImGui::SetKeyboardFocusHere();
    InputTextWithHint("##ComboWithFilter_inputText", "Filter...", pattern_buffer, 256, ImGuiInputTextFlags_AutoSelectAll);

    const ImVec2 label_size = CalcTextSize(ICON_FA_SEARCH, NULL, true);
    const ImVec2 search_icon_pos(
        ImGui::GetItemRectMax().x - label_size.x - style.ItemInnerSpacing.x * 2,
        window->DC.CursorPos.y + style.FramePadding.y + g.FontSize * 0.3f
    );
    RenderText(search_icon_pos, ICON_FA_SEARCH);

    int move_delta = 0;
    // Use Shortcut to prevent NavEnableKeyboard from also responding to nav.
    if (Shortcut(ImGuiKey_UpArrow, allow_repeat)) {
        --move_delta;
    } else if (Shortcut(ImGuiKey_DownArrow, allow_repeat)) {
        ++move_delta;
    } else if (Shortcut(ImGuiKey_PageUp, ImGuiInputFlags_None)) {
        move_delta -= popup_max_height_in_items;
    } else if (Shortcut(ImGuiKey_PageDown, ImGuiInputFlags_None)) {
        move_delta += popup_max_height_in_items;
    }

    if (move_delta != 0) {
        if (is_filtering) {
            int current_score_idx = index_of_key(itemScoreVector, focus_idx);
            if (current_score_idx >= 0) {
                const int count = static_cast<int>(itemScoreVector.size());
                current_score_idx = ImClamp(current_score_idx + move_delta, 0, count - 1);
                focus_idx = itemScoreVector[current_score_idx].first;
            }
        } else {
            focus_idx = ImClamp(focus_idx + move_delta, 0, items_count - 1);
        }
    }

    // Copied from ListBoxHeader
    // If popup_max_height_in_items == -1, default height is maximum 7.
    float height_in_items_f = (popup_max_height_in_items < 0 ? ImMin(items_count, 7) : popup_max_height_in_items) + 0.25f;
    ImVec2 size;
    size.x = 0.0f;
    size.y = GetTextLineHeightWithSpacing() * height_in_items_f + g.Style.FramePadding.y * 2.0f;

    if (ImGui::BeginListBox("##ComboWithFilter_itemList", size)) {
        for (int i = 0; i < show_count; i++) {
            int idx = is_filtering ? itemScoreVector[i].first : i;
            PushID((void*)(intptr_t)idx);
            const bool item_selected = (idx == focus_idx);
            const char* item_text = items[idx].c_str();
            if (Selectable(item_text, item_selected)) {
                value_changed = true;
                *current_item = idx;
                CloseCurrentPopup();
            }

            if (item_selected) {
                SetItemDefaultFocus();
                // SetItemDefaultFocus doesn't work so also check IsWindowAppearing.
                if (move_delta != 0 || IsWindowAppearing()) {
                    SetScrollHereY();
                }
            }
            PopID();
        }
        ImGui::EndListBox();

        const bool repeat = false;
        if (IsKeyPressed(ImGuiKey_Enter, repeat)) {
            value_changed = true;
            *current_item = focus_idx;
            CloseCurrentPopup();
        }
        // Using Shortcut would make the first Esc clear input (even if empty) and the second close the popup.
        else if (IsKeyPressed(ImGuiKey_Escape, repeat)) {
            value_changed = false;
            CloseCurrentPopup();
        }
    }
    ImGui::PopItemWidth();
    ImGui::EndCombo();

    if (value_changed)
        MarkItemEdited(g.LastItemData.ID);

    return value_changed;
}

} // namespace ImGui
