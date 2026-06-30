// src/ui/class_diagram/cd_view.cpp

#include "cd_view.h"
#include "cd_arrowhead.h"
#include "../ts_style.h"
#include <imgui.h>
#include <imnodes.h>
#include <unordered_set>
#include <vector>

namespace TS {
namespace {

static std::unordered_set<int> s_positioned;

void DrawNode(const CDNode& node, bool selected, bool dimmed, float zoom)
{
    const float node_w = TS::NODE_WIDTH * TS::ui_scale * zoom;

    const int   lod        = TS::GetFontLOD(zoom);
    const float correction = zoom / TS::FONT_LOD_SCALES[lod];

    ImU32 title_col;
    ImU32 bg_col;
    if (selected) {
        title_col = TS::ACCENT_SECONDARY_U32;
        bg_col    = TS::PANEL_U32;
    } else if (dimmed) {
        title_col = ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::ACCENT_PRIMARY_SUBTLE, 0.35f));
        bg_col    = ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::PANEL, 0.5f));
    } else {
        title_col = TS::ACCENT_PRIMARY_SUBTLE_U32;
        bg_col    = TS::PANEL_U32;
    }

    ImNodes::PushColorStyle(ImNodesCol_TitleBar,               title_col);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered,        title_col);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected,       title_col);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackground,         bg_col);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered,  bg_col);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, bg_col);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinCircleRadius, 0.0f);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinHoverRadius,  0.0f);

    ImNodes::BeginNode(node.node_id);

    // Prev-frame geometry for divider width
    const float  pad_x     = TS::NODE_PADDING_X * zoom * TS::ui_scale;
    const ImVec2 prev_sz   = ImNodes::GetNodeDimensions(node.node_id);
    const float  divider_w = (prev_sz.x > 1.0f) ? (prev_sz.x - 2.0f * pad_x) : node_w;

    // ── Title bar ─────────────────────────────────────────────────────────────
    ImNodes::BeginNodeTitleBar();

    const ImVec4 ink_col  = dimmed ? TS::WithAlpha(TS::INK,   0.4f) : TS::INK;
    const ImVec4 ink3_col = dimmed ? TS::WithAlpha(TS::INK_3, 0.4f) : TS::INK_3;

    ImGui::SetWindowFontScale(correction);
    ImGui::PushFont(TS::FONT_MEDIUM_LOD[lod]);
    ImGui::PushStyleColor(ImGuiCol_Text, ink_col);
    ImGui::TextUnformatted(node.class_name.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::PushFont(TS::FONT_SMALL_LOD[lod]);
    ImGui::PushStyleColor(ImGuiCol_Text, ink3_col);
    ImGui::TextUnformatted(node.package.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetWindowFontScale(zoom);

    ImNodes::EndNodeTitleBar();

    // ── Divider helper ────────────────────────────────────────────────────────
    auto DrawDivider = [&]() {
        const ImVec2 p   = ImGui::GetCursorScreenPos();
        const ImU32  col = dimmed
            ? ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::LINE, 0.3f))
            : TS::LINE_U32;
        ImGui::GetWindowDrawList()->AddLine(p, {p.x + divider_w, p.y}, col, 1.0f);
        ImGui::Dummy({node_w, 1.0f});
    };

    // ── Field rows ────────────────────────────────────────────────────────────
    ImGui::SetWindowFontScale(correction);
    ImGui::PushFont(TS::FONT_MONO_LOD[lod]);
    ImGui::PushStyleColor(ImGuiCol_Text,
        dimmed ? TS::WithAlpha(TS::INK_2, 0.4f) : TS::INK_2);
    for (const auto& f : node.fields) {
        ImGui::Text("%c %s", f.access, f.name.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetWindowFontScale(zoom);

    // ── Divider (fields / methods) ────────────────────────────────────────────
    DrawDivider();

    // ── Pins: left (input) and right (output) at divider position ────────────
    // ItemSpacing.y is zeroed so 0-height Dummy attributes don't push the cursor
    // down and create blank space before the method rows.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImNodes::BeginInputAttribute(CDPinLeft(node.node_id), ImNodesPinShape_Circle);
    ImGui::Dummy({node_w, 0.0f});
    ImNodes::EndInputAttribute();
    ImNodes::BeginOutputAttribute(CDPinRight(node.node_id), ImNodesPinShape_Circle);
    ImGui::Dummy({node_w, 0.0f});
    ImNodes::EndOutputAttribute();
    ImGui::PopStyleVar();

    // ── Method rows ───────────────────────────────────────────────────────────
    ImGui::SetWindowFontScale(correction);
    ImGui::PushFont(TS::FONT_MONO_LOD[lod]);
    ImGui::PushStyleColor(ImGuiCol_Text,
        dimmed ? TS::WithAlpha(TS::INK_2, 0.4f) : TS::INK_2);
    for (const auto& m : node.methods) {
        ImGui::Text("%c %s(%s): %s", m.access, m.name.c_str(), m.params.c_str(), m.ret_type.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetWindowFontScale(zoom);

    ImNodes::EndNode();

    ImNodes::PopStyleVar(2);
    ImNodes::PopColorStyle(); // NodeBackgroundSelected
    ImNodes::PopColorStyle(); // NodeBackgroundHovered
    ImNodes::PopColorStyle(); // NodeBackground
    ImNodes::PopColorStyle(); // TitleBarSelected
    ImNodes::PopColorStyle(); // TitleBarHovered
    ImNodes::PopColorStyle(); // TitleBar
}

} // anonymous namespace

void DrawClassDiagramContent(CDGraph& graph, float zoom)
{
    for (const auto& node : graph.nodes) {
        if (!s_positioned.count(node.node_id)) {
            ImNodes::SetNodeGridSpacePos(node.node_id, node.pos);
            s_positioned.insert(node.node_id);
        }
    }

    // ── Highlight/dim sets ────────────────────────────────────────────────────
    const bool has_selection = (graph.selected_node_id != -1);
    std::unordered_set<int> connected;
    if (has_selection) {
        for (const auto& e : graph.edges) {
            if (e.src_node_id == graph.selected_node_id) connected.insert(e.dst_node_id);
            if (e.dst_node_id == graph.selected_node_id) connected.insert(e.src_node_id);
        }
    }

    // ── Nodes ─────────────────────────────────────────────────────────────────
    for (const auto& node : graph.nodes) {
        const bool sel    = (node.node_id == graph.selected_node_id);
        const bool dimmed = has_selection && !sel && !connected.count(node.node_id);
        DrawNode(node, sel, dimmed, zoom);
    }

    // ── Edges — nearest-pin routing ───────────────────────────────────────────
    for (const auto& edge : graph.edges) {
        const bool edge_dimmed = has_selection &&
            edge.src_node_id != graph.selected_node_id &&
            edge.dst_node_id != graph.selected_node_id;

        ImU32 link_col;
        if (edge_dimmed) {
            link_col = ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::MUTED, 0.2f));
        } else if (edge.type == CDEdgeType::Dependency) {
            link_col = TS::MUTED_U32;
        } else {
            link_col = TS::INK_2_U32;
        }

        // Choose pins based on which node is geometrically to the left
        const ImVec2 src_scr  = ImNodes::GetNodeScreenSpacePos(edge.src_node_id);
        const ImVec2 dst_scr  = ImNodes::GetNodeScreenSpacePos(edge.dst_node_id);
        const bool   src_left = (src_scr.x <= dst_scr.x);
        const int from_pin = src_left ? CDPinRight(edge.src_node_id) : CDPinRight(edge.dst_node_id);
        const int to_pin   = src_left ? CDPinLeft(edge.dst_node_id)  : CDPinLeft(edge.src_node_id);

        ImNodes::PushColorStyle(ImNodesCol_Link,         link_col);
        ImNodes::PushColorStyle(ImNodesCol_LinkHovered,  TS::ACCENT_PRIMARY_U32);
        ImNodes::PushColorStyle(ImNodesCol_LinkSelected, TS::ACCENT_PRIMARY_U32);
        ImNodes::Link(edge.edge_id, from_pin, to_pin);
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }
}

void DrawClassDiagramArrowheads(CDGraph& graph, float zoom)
{
    ImDrawList*       dl        = ImGui::GetWindowDrawList();
    const CDArrowDims arrow     = CDScaleArrowhead(zoom * TS::ui_scale);
    const float       outline_w = TS::LINK_THICKNESS * zoom * TS::ui_scale;
    const bool        has_sel   = (graph.selected_node_id != -1);

    for (const auto& edge : graph.edges) {
        const bool edge_dimmed = has_sel &&
            edge.src_node_id != graph.selected_node_id &&
            edge.dst_node_id != graph.selected_node_id;

        ImU32 col;
        if (edge_dimmed) {
            col = ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::MUTED, 0.2f));
        } else if (edge.type == CDEdgeType::Dependency) {
            col = TS::MUTED_U32;
        } else {
            col = TS::INK_2_U32;
        }

        const ImVec2 src_pos   = ImNodes::GetNodeScreenSpacePos(edge.src_node_id);
        const ImVec2 dst_pos   = ImNodes::GetNodeScreenSpacePos(edge.dst_node_id);
        const ImVec2 dst_sz    = ImNodes::GetNodeDimensions(edge.dst_node_id);
        const float  dst_ctr_y = dst_pos.y + dst_sz.y * 0.5f;

        ImVec2 tip_pos, arrow_dir;
        if (src_pos.x <= dst_pos.x) {
            // src is left of dst → arrow arrives at dst's left edge
            tip_pos   = { dst_pos.x, dst_ctr_y };
            arrow_dir = { -1.0f, 0.0f };
        } else {
            // src is right of dst → arrow arrives at dst's right edge
            tip_pos   = { dst_pos.x + dst_sz.x, dst_ctr_y };
            arrow_dir = { +1.0f, 0.0f };
        }

        const CDArrowTri tri = CDArrowVertices(tip_pos, arrow_dir, arrow.half_base, arrow.length);
        if (edge.type == CDEdgeType::Dependency) {
            dl->AddTriangle(tri.tip, tri.v1, tri.v2, col, outline_w);
        } else {
            dl->AddTriangleFilled(tri.tip, tri.v1, tri.v2, col);
        }
    }
}

void UpdateClassDiagramInteraction(CDGraph& graph)
{
    int hovered;
    graph.hovered_node_id = ImNodes::IsNodeHovered(&hovered) ? hovered : -1;

    const int n = ImNodes::NumSelectedNodes();
    if (n > 0) {
        std::vector<int> ids(static_cast<size_t>(n));
        ImNodes::GetSelectedNodes(ids.data());
        graph.selected_node_id = ids[0];
    } else {
        graph.selected_node_id = -1;
    }
}

} // namespace TS
