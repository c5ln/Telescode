// src/ui/ts_canvas.cpp
// imnodes-backed canvas: zoom, pan, coordinate transforms, minimap.

#include "ts_canvas.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include "ts_style.h"

namespace {
    static float                 s_zoom    = 1.0f;
    static ImNodesEditorContext* s_context = nullptr;

    constexpr float k_zoom_label_pad_x = 8.0f;
    constexpr float k_zoom_label_pad_y = 20.0f;
}

namespace TS {

// ── Accessors ─────────────────────────────────────────────────────────────────

float GetCanvasZoom()        { return s_zoom; }
void  SetCanvasZoom(float z) { s_zoom = ImClamp(z, ZOOM_MIN, ZOOM_MAX); }

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

    // 3. Scroll zoom (only when canvas window is hovered)
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            const float old_zoom = s_zoom;
            const float new_zoom = ImClamp(old_zoom + wheel * ZOOM_STEP_SCROLL,
                                           ZOOM_MIN, ZOOM_MAX);
            if (new_zoom != old_zoom)
            {
                const ImVec2 mouse         = ImGui::GetIO().MousePos;
                const ImVec2 canvas_origin = ImGui::GetWindowPos();
                const ImVec2 old_pan       = ImNodes::EditorContextGetPanning();

                const ImVec2 cursor_in_grid = {
                    mouse.x - canvas_origin.x - old_pan.x,
                    mouse.y - canvas_origin.y - old_pan.y
                };
                const float ratio = 1.0f - new_zoom / old_zoom;
                const ImVec2 new_pan = {
                    old_pan.x + cursor_in_grid.x * ratio,
                    old_pan.y + cursor_in_grid.y * ratio
                };

                s_zoom = new_zoom;
                ImNodes::EditorContextResetPanning(new_pan);
                ImGui::GetIO().MouseWheel = 0.0f;  // prevent imnodes built-in scroll
            }
        }
    }

    // 4. Fit-to-view (F key while canvas is focused)
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F))
    {
        s_zoom = 1.0f;
        ImNodes::EditorContextResetPanning({ size.x * 0.5f, size.y * 0.5f });
    }

    // 5. PushStyleVar before BeginNodeEditor
    const float z = s_zoom * TS::ui_scale;
    ImNodes::PushStyleVar(ImNodesStyleVar_GridSpacing,        24.0f * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeCornerRounding, 10.0f * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeBorderThickness, 1.0f * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness,       2.0f * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinCircleRadius,     4.0f * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinHoverRadius,     10.0f * z);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinOffset,           0.0f);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(12.0f * z, 6.0f * z));
    // Total: 8 pushes

    // 6. Begin node editor
    ImNodes::BeginNodeEditor();

    // 7. Apply zoom to font scale inside child window
    ImGui::SetWindowFontScale(s_zoom);

    // 8. (Phase 3: no nodes yet)

    // 10. Restore font scale before ending editor
    ImGui::SetWindowFontScale(1.0f);

    // 11. End node editor
    ImNodes::EndNodeEditor();

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

// ── ShutdownCanvas ────────────────────────────────────────────────────────────

void ShutdownCanvas()
{
    if (s_context) {
        ImNodes::EditorContextFree(s_context);
        s_context = nullptr;
    }
}

} // namespace TS
