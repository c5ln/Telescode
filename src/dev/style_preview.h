// src/dev/style_preview.h
// Warm Cream token preview — dev/debug only.
//
// Compiled in only when CMake option TELESCODE_STYLE_PREVIEW is ON.
// Call DrawStylePreview() once per frame inside the ImGui frame.
// Never include this file in release builds.

#pragma once

#include <imgui.h>
#include "ui/ts_style.h"

namespace TS {

inline void DrawStylePreview()
{
    bool show_demo = true;
    ImGui::ShowDemoWindow(&show_demo);

    ImGui::SetNextWindowSize(ImVec2(420.0f * ui_scale, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Warm Cream -- Token Preview");

    ImGui::PushFont(FONT_MONO);
    ImGui::TextDisabled("// ts_style.h  Warm Cream palette");
    ImGui::PopFont();
    ImGui::Separator();

    auto swatch = [](const char* name, ImVec4 col) {
        ImGui::ColorButton(name, col,
            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
            ImVec2(18, 18));
        ImGui::SameLine();
        ImGui::PushFont(FONT_MONO);
        ImGui::TextUnformatted(name);
        ImGui::PopFont();
    };

    if (ImGui::CollapsingHeader("Background", ImGuiTreeNodeFlags_DefaultOpen)) {
        swatch("BG",      BG);
        swatch("BG_SOFT", BG_SOFT);
        swatch("PANEL",   PANEL);
        swatch("PANEL_2", PANEL_2);
        swatch("LINE",    LINE);
    }
    if (ImGui::CollapsingHeader("Ink", ImGuiTreeNodeFlags_DefaultOpen)) {
        swatch("INK",   INK);
        swatch("INK_2", INK_2);
        swatch("INK_3", INK_3);
        swatch("MUTED", MUTED);
    }
    if (ImGui::CollapsingHeader("Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        swatch("NIGHT",      NIGHT);
        swatch("NIGHT_2",    NIGHT_2);
        swatch("NIGHT_LINE", NIGHT_LINE);
    }
    if (ImGui::CollapsingHeader("Accents", ImGuiTreeNodeFlags_DefaultOpen)) {
        swatch("ACCENT_PRIMARY",          ACCENT_PRIMARY);
        swatch("ACCENT_PRIMARY_SUBTLE",   ACCENT_PRIMARY_SUBTLE);
        swatch("ACCENT_SECONDARY",        ACCENT_SECONDARY);
        swatch("ACCENT_SECONDARY_SUBTLE", ACCENT_SECONDARY_SUBTLE);
        swatch("ACCENT_FOCUS",            ACCENT_FOCUS);
        swatch("ACCENT_FOCUS_SUBTLE",     ACCENT_FOCUS_SUBTLE);
    }
    if (ImGui::CollapsingHeader("Widgets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushFont(FONT_MEDIUM);
        ImGui::Text("Node Title  (FONT_MEDIUM)");
        ImGui::PopFont();
        ImGui::PushFont(FONT_BASE);
        ImGui::Text("Body text  (FONT_BASE)");
        ImGui::PopFont();
        ImGui::PushFont(FONT_SMALL);
        ImGui::TextDisabled("Subtitle / badge  (FONT_SMALL)");
        ImGui::PopFont();
        ImGui::PushFont(FONT_MONO);
        ImGui::Text("+ createOrder(req): Order   (FONT_MONO)");
        ImGui::PopFont();
        ImGui::Spacing();
        static float f = 0.5f;
        ImGui::SliderFloat("Slider", &f, 0.0f, 1.0f);
        static bool chk = true;
        ImGui::Checkbox("Checkbox", &chk);
        ImGui::Button("Button");
    }

    ImGui::End();
}

} // namespace TS
