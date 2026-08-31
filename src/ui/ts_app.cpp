// src/ui/ts_app.cpp
// Static app shell — four layout zones, collapse toggles, no content.
//
// Zone summary:
//   Header        full width, 48px, always visible       (TS::HEADER)
//   Sidebar       left, 280px | 40px icon rail           (TS::PANEL)
//   Canvas        fills remaining space                   (TS::BG_SOFT)
//   Sequence rail full width, 120px | 32px strip         (TS::PANEL)
//
// The rail spans the whole window and the sidebar stops on top of it, so the
// rail's geometry is unaffected by the sidebar collapse. The reading-order list
// lives here, and shifting it sideways on an unrelated toggle would slide every
// item out from under the cursor.

#include "ts_app.h"
#include "ts_style.h"
#include "ts_canvas.h"
#include "ts_rail.h"
#include <imgui.h>

namespace TS {

namespace {
    // 1x reference values — multiply by TS::ui_scale at every use site.
    constexpr float k_header_h    = 48.0f;
    constexpr float k_sidebar_w   = 280.0f;
    constexpr float k_icon_rail_w = 40.0f;
    constexpr float k_rail_h      = 120.0f;
    constexpr float k_rail_strip  = 32.0f;
    constexpr float k_toggle_h    = 32.0f;  // sidebar collapse button height
    constexpr float k_chevron_w   = 48.0f;  // rail chevron button width  -- constant across open/collapsed
    constexpr float k_chevron_h   = 32.0f;  // rail chevron button height -- constant across open/collapsed
}

static bool s_sidebar_collapsed = false;
static bool s_rail_collapsed    = false;

void DrawAppShell()
{
    const float s  = TS::ui_scale;
    const float dw = ImGui::GetIO().DisplaySize.x;
    const float dh = ImGui::GetIO().DisplaySize.y;

    const float header_h  = k_header_h * s;
    const float sidebar_w = (s_sidebar_collapsed ? k_icon_rail_w : k_sidebar_w) * s;
    const float rail_h    = (s_rail_collapsed    ? k_rail_strip  : k_rail_h)    * s;

    const float body_y   = header_h;
    const float body_h   = dh - header_h;
    const float canvas_x = sidebar_w;
    const float canvas_w = (dw > sidebar_w) ? (dw - sidebar_w) : 0.0f;
    // Guard: canvas_h never goes negative if window is smaller than expected.
    const float canvas_h = (body_h > rail_h) ? (body_h - rail_h) : 0.0f;
    const float rail_y   = body_y + canvas_h;

    // Full-screen shell window — no decoration, not moveable, no saved layout.
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({dw, dh});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##shell", nullptr,
        ImGuiWindowFlags_NoDecoration         |
        ImGuiWindowFlags_NoMove               |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Header ───────────────────────────────────────────────────────────────
    dl->AddRectFilled({0.0f, 0.0f}, {dw, header_h}, TS::HEADER_U32);

    // ── Sidebar ──────────────────────────────────────────────────────────────
    // Stops at the rail's top edge -- the rail owns the full bottom strip.
    dl->AddRectFilled({0.0f, body_y}, {sidebar_w, rail_y}, TS::PANEL_U32);

    // Collapse toggle: fixed-size button at the upper-left of the sidebar.
    // Width is always k_icon_rail_w so the button doesn't resize on expand/collapse.
    const float tgl_h = k_toggle_h * s;
    ImGui::SetCursorScreenPos({0.0f, body_y});
    ImGui::PushStyleColor(ImGuiCol_Button,        TS::PANEL_2);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Darken(TS::PANEL_2, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Darken(TS::PANEL_2, 0.12f));
    if (ImGui::Button(s_sidebar_collapsed ? ">##sb" : "<##sb", {k_icon_rail_w * s, tgl_h}))
        s_sidebar_collapsed = !s_sidebar_collapsed;
    ImGui::PopStyleColor(3);

    // ── Canvas ────────────────────────────────────────────────────────────────
    TS::DrawCanvas({canvas_x, body_y}, {canvas_w, canvas_h});

    // ── Sequence Rail ─────────────────────────────────────────────────────────
    dl->AddRectFilled({0.0f, rail_y}, {dw, rail_y + rail_h}, TS::PANEL_U32);
    TS::DrawRail({0.0f, rail_y}, {dw, rail_h}, s_rail_collapsed);

    // Chevron toggle: upper-right corner of the rail, fixed height.
    const float chev_w = k_chevron_w * s;
    const float chev_h = k_chevron_h * s;
    const float chev_x = (dw > chev_w) ? (dw - chev_w) : 0.0f;
    ImGui::SetCursorScreenPos({chev_x, rail_y});
    ImGui::PushStyleColor(ImGuiCol_Button,        TS::PANEL_2);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Darken(TS::PANEL_2, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  Darken(TS::PANEL_2, 0.12f));
    if (ImGui::Button(s_rail_collapsed ? "^##rail" : "v##rail", {chev_w, chev_h}))
        s_rail_collapsed = !s_rail_collapsed;
    ImGui::PopStyleColor(3);

    ImGui::End();
}

} // namespace TS
