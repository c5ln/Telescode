// src/ui/ts_canvas.cpp
// imnodes-backed canvas: zoom, pan, coordinate transforms, minimap.

#include "ts_canvas.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include "ts_style.h"
#include "class_diagram/cd_view.h"
#include "class_diagram/cd_builder.h"

namespace {
    static float                 s_zoom         = 1.0f;
    static float                 s_zoom_target  = 1.0f;
    static ImVec2                s_anchor_world  = { 0.0f, 0.0f };
    static ImVec2                s_anchor_screen = { 0.0f, 0.0f };
    static ImNodesEditorContext* s_context      = nullptr;

    constexpr float k_zoom_label_pad_x = 8.0f;
    constexpr float k_zoom_label_pad_y = 20.0f;

    static TS::CDGraph s_graph;
}

namespace TS {

// ── Accessors ─────────────────────────────────────────────────────────────────

float GetCanvasZoom()        { return s_zoom; }
void  SetCanvasZoom(float z) { s_zoom = s_zoom_target = ImClamp(z, ZOOM_MIN, ZOOM_MAX); }

// ── Coordinate transforms ─────────────────────────────────────────────────────

ImVec2 WorldToGrid(ImVec2 p) { return { p.x * s_zoom, p.y * s_zoom }; }
ImVec2 GridToWorld(ImVec2 p) { return { p.x / s_zoom, p.y / s_zoom }; }

// ── DrawCanvas ────────────────────────────────────────────────────────────────

void DrawCanvas(ImVec2 pos, ImVec2 size)
{
    if (size.x <= 0.0f || size.y <= 0.0f) return;

    // 1. EditorContext -- initialise once.
    if (!s_context)
        s_context = ImNodes::EditorContextCreate();
    ImNodes::EditorContextSet(s_context);

    // 2. BeginChild
    ImGui::PushStyleColor(ImGuiCol_ChildBg, TS::BG_SOFT);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowPos(pos);
    ImGui::BeginChild("##canvas", size,
        0,
        ImGuiWindowFlags_NoScrollbar         |
        ImGuiWindowFlags_NoScrollWithMouse   |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const ImVec2 canvas_origin = ImGui::GetWindowPos();

    // 3. Scroll: update target zoom and store cursor anchor
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const float new_target = ImClamp(s_zoom_target + wheel * ZOOM_STEP_SCROLL,
                                             ZOOM_MIN, ZOOM_MAX);
            if (new_target != s_zoom_target)
            {
                // Store world-space anchor under cursor at current displayed zoom.
                // This point will remain fixed on screen throughout the animation.
                const ImVec2 mouse   = ImGui::GetIO().MousePos;
                const ImVec2 old_pan = ImNodes::EditorContextGetPanning();
                s_anchor_screen = mouse;
                s_anchor_world  = {
                    (mouse.x - canvas_origin.x - old_pan.x) / s_zoom,
                    (mouse.y - canvas_origin.y - old_pan.y) / s_zoom
                };
                s_zoom_target = new_target;
            }
            ImGui::GetIO().MouseWheel = 0.0f;  // prevent imnodes built-in scroll
        }
    }

    // 4. Smooth zoom: lerp s_zoom toward s_zoom_target each frame
    {
        const float dt = ImGui::GetIO().DeltaTime;
        const float t  = 1.0f - expf(-dt * ZOOM_SMOOTH_SPEED);
        s_zoom = s_zoom + (s_zoom_target - s_zoom) * t;
        if (fabsf(s_zoom - s_zoom_target) < 0.0005f)
        {
            s_zoom = s_zoom_target;
            s_anchor_world  = { 0.0f, 0.0f };  // animation done — release pan lock
            s_anchor_screen = { 0.0f, 0.0f };
        }

        // Keep anchor world point fixed on screen while animating.
        if (s_anchor_world.x != 0.0f || s_anchor_world.y != 0.0f)
        {
            const ImVec2 new_pan = {
                s_anchor_screen.x - canvas_origin.x - s_anchor_world.x * s_zoom,
                s_anchor_screen.y - canvas_origin.y - s_anchor_world.y * s_zoom
            };
            ImNodes::EditorContextResetPanning(new_pan);
        }
    }

    // 5. Fit-to-view (F key while canvas is focused)
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F))
    {
        s_zoom = s_zoom_target = 1.0f;
        s_anchor_world  = { 0.0f, 0.0f };
        s_anchor_screen = { 0.0f, 0.0f };
        ImNodes::EditorContextResetPanning({ size.x * 0.5f, size.y * 0.5f });
    }

    // 6. PushStyleVar before BeginNodeEditor
    const float z = s_zoom * TS::ui_scale;
    ImNodes::PushStyleVar(ImNodesStyleVar_GridSpacing,         TS::NODE_GRID_SPACING     * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeCornerRounding,  TS::NODE_ROUNDING         * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeBorderThickness, TS::NODE_BORDER_THICKNESS * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness,       TS::LINK_THICKNESS        * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinCircleRadius,     TS::PIN_CIRCLE_RADIUS     * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinHoverRadius,      TS::PIN_HOVER_RADIUS      * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinOffset,           0.0f);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(TS::NODE_PADDING_X * z, TS::NODE_PADDING_Y * z));
    // Total: 8 pushes

    // 6. Begin node editor
    ImNodes::BeginNodeEditor();

    // 7. Camera transform: everything inside the node editor scales by s_zoom.
    //    Font sharpness is handled per-text-section inside DrawNode via LOD fonts.
    ImGui::SetWindowFontScale(s_zoom);

    // 8. Class diagram
    TS::DrawClassDiagramContent(s_graph, s_zoom);

    // 10. Restore font scale before ending editor
    ImGui::SetWindowFontScale(1.0f);

    // 11. End node editor
    ImNodes::EndNodeEditor();

    // 12. Arrowheads — drawn after EndNodeEditor so they sit above all imnodes channels.
    TS::DrawClassDiagramArrowheads(s_graph, s_zoom);

    TS::UpdateClassDiagramInteraction(s_graph);

    // 12. Pop imnodes style vars
    ImNodes::PopStyleVar(8);

    // 13. Zoom level label (bottom-left corner)
    ImGui::SetCursorScreenPos({ pos.x + k_zoom_label_pad_x * TS::ui_scale,
                                pos.y + size.y - k_zoom_label_pad_y * TS::ui_scale });
    ImGui::PushFont(TS::FONT_MONO);
    ImGui::PushStyleColor(ImGuiCol_Text, TS::MUTED);
    ImGui::Text("%.0f%%", s_zoom * 100.0f);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // 14. End child
    ImGui::EndChild();
}

// ── InitCanvasFromDB ──────────────────────────────────────────────────────────

void InitCanvasFromDB(sqlite3* db)
{
    s_graph = BuildCDGraph(db);
}

// ── ShutdownCanvas ────────────────────────────────────────────────────────────

void ShutdownCanvas()
{
    if (s_context) {
        ImNodes::EditorContextFree(s_context);
        s_context = nullptr;
    }
}

} // namespace TS
