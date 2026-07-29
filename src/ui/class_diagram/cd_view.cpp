// src/ui/class_diagram/cd_view.cpp

#include "cd_view.h"
#include "cd_arrowhead.h"
#include "cd_layout.h"
#include "../ts_style.h"
#include <imgui.h>
#include <imnodes.h>
#include <cfloat>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace TS {
namespace {

// ── Text preparation ─────────────────────────────────────────────────────────

// Trims `text` until it plus an ellipsis fits `max_w`, measured with `font` at
// `px`. Callers pass logical (zoom 1.0) metrics: measuring at the live zoom
// instead would move the cut point every time the font LOD changed, so a
// signature would flicker between "create_or..." and "create_ord..." mid-zoom.
std::string Ellipsize(ImFont* font, float px, float max_w, std::string text)
{
    if (!font || text.empty()) return text;
    if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x <= max_w) return text;

    // U+2026 sits outside ImGui's default glyph range, so spell the ellipsis out.
    static const char* const kEllipsis = "...";
    const float ellipsis_w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, kEllipsis).x;

    while (!text.empty()) {
        // Drop one whole UTF-8 code point, not one byte.
        do { text.pop_back(); }
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);

        if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x + ellipsis_w <= max_w)
            break;
    }
    return text + kEllipsis;
}

std::string FieldLine(const CDField& f)
{
    return std::string(1, f.access) + ' ' + f.name;
}

std::string MethodLine(const CDMethod& m)
{
    return std::string(1, m.access) + ' ' + m.name + '(' + m.params + "): " + m.ret_type;
}

// Fills every cached display string. Needs a built font atlas, so it runs on the
// first frame rather than in the builder.
void PrepareDisplayText(CDGraph& graph)
{
    const int   lod     = TS::GetFontLOD(1.0f);   // the atlas baked at 1x
    const float budget  = TS::NODE_WIDTH     * TS::ui_scale;
    const float px_name = TS::FONT_SIZE_BASE  * TS::ui_scale;
    const float px_pkg  = TS::FONT_SIZE_SMALL * TS::ui_scale;
    const float px_mono = TS::FONT_SIZE_MONO  * TS::ui_scale;

    for (CDNode& n : graph.nodes) {
        n.display_name    = Ellipsize(TS::FONT_MEDIUM_LOD[lod], px_name, budget, n.class_name);
        n.display_package = Ellipsize(TS::FONT_SMALL_LOD[lod],  px_pkg,  budget, n.package);
        for (CDField& f : n.fields)
            f.display = Ellipsize(TS::FONT_MONO_LOD[lod], px_mono, budget, FieldLine(f));
        for (CDMethod& m : n.methods)
            m.display = Ellipsize(TS::FONT_MONO_LOD[lod], px_mono, budget, MethodLine(m));
    }
    graph.display_ready = true;
}

// ── Rendering ────────────────────────────────────────────────────────────────

// Draws one row of text inside the box already reserved for it. Going through
// the draw list is deliberate: a laid-out ImGui::Text would let an over-long
// string widen the node, which is exactly what this change removes.
void DrawRowText(ImDrawList* dl, ImFont* font, float px,
                 ImVec2 tl, float w, float h, ImU32 col, const char* text)
{
    if (!font || !text || !*text) return;
    const ImVec4 clip = { tl.x, tl.y, tl.x + w, tl.y + h };
    dl->AddText(font, px, { tl.x, tl.y + (h - px) * 0.5f }, col, text, nullptr, 0.0f, &clip);
}

// Reserves one item covering every row in `rows`, then draws the rows into it.
// Works for CDField and CDMethod alike — both carry a `display` string.
template <typename Row>
void DrawRowBlock(ImDrawList* dl, ImFont* font, float px, float w, float row_h,
                  ImU32 col, const std::vector<Row>& rows)
{
    if (rows.empty()) return;

    const ImVec2 tl = ImGui::GetCursorScreenPos();
    ImGui::Dummy({w, row_h * static_cast<float>(rows.size())});

    for (size_t i = 0; i < rows.size(); ++i)
        DrawRowText(dl, font, px, {tl.x, tl.y + row_h * static_cast<float>(i)},
                    w, row_h, col, rows[i].display.c_str());
}

void DrawNode(const CDNode& node, bool selected, bool dimmed, float zoom)
{
    const float z         = zoom * TS::ui_scale;
    const float content_w = TS::NODE_WIDTH     * z;
    const float header_h  = TS::NODE_HEADER_H  * z;
    const float row_h     = TS::NODE_ROW_H     * z;
    const float divider_h = TS::NODE_DIVIDER_H * z;

    // Nearest pre-baked atlas, then ask AddText for the exact pixel size wanted.
    // AddText scales the remainder itself, so glyphs stay crisp and no
    // SetWindowFontScale is needed — that scaled ImGui's layout too, which is
    // part of why node sizes used to drift.
    const int   lod     = TS::GetFontLOD(zoom);
    const float px_name = TS::FONT_SIZE_BASE  * z;
    const float px_pkg  = TS::FONT_SIZE_SMALL * z;
    const float px_mono = TS::FONT_SIZE_MONO  * z;

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

    const ImU32 ink_u32  = ImGui::ColorConvertFloat4ToU32(
        dimmed ? TS::WithAlpha(TS::INK,   0.4f) : TS::INK);
    const ImU32 ink3_u32 = ImGui::ColorConvertFloat4ToU32(
        dimmed ? TS::WithAlpha(TS::INK_3, 0.4f) : TS::INK_3);
    const ImU32 body_u32 = ImGui::ColorConvertFloat4ToU32(
        dimmed ? TS::WithAlpha(TS::INK_2, 0.4f) : TS::INK_2);
    const ImU32 line_u32 = dimmed
        ? ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::LINE, 0.3f))
        : TS::LINE_U32;

    ImNodes::BeginNode(node.node_id);

    // Every block below reserves its own exact height, so drop the spacing ImGui
    // would insert between items: it is a fixed pixel count that does not scale
    // with zoom, which would make node height a non-linear function of it.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Title bar ─────────────────────────────────────────────────────────────
    ImNodes::BeginNodeTitleBar();
    const ImVec2 head_tl = ImGui::GetCursorScreenPos();
    ImGui::Dummy({content_w, header_h});
    ImNodes::EndNodeTitleBar();

    const float name_h = header_h * 0.55f;
    DrawRowText(dl, TS::FONT_MEDIUM_LOD[lod], px_name,
                head_tl, content_w, name_h, ink_u32, node.display_name.c_str());
    DrawRowText(dl, TS::FONT_SMALL_LOD[lod], px_pkg,
                {head_tl.x, head_tl.y + name_h}, content_w, header_h - name_h,
                ink3_u32, node.display_package.c_str());

    // ── Field rows ────────────────────────────────────────────────────────────
    // Reserved as one block, not one item per row. ImGui truncates the layout
    // cursor to whole pixels after every item, so per-row items would shed a
    // fraction of a pixel each and leave tall nodes measurably shorter than the
    // slot the layout reserved for them — gaps that drift with the row count.
    DrawRowBlock(dl, TS::FONT_MONO_LOD[lod], px_mono, content_w, row_h, body_u32, node.fields);

    // ── Divider (fields / methods) ────────────────────────────────────────────
    {
        const ImVec2 tl = ImGui::GetCursorScreenPos();
        ImGui::Dummy({content_w, divider_h});
        const float y = tl.y + divider_h * 0.5f;
        dl->AddLine({tl.x, y}, {tl.x + content_w, y}, line_u32,
                    TS::NODE_BORDER_THICKNESS * z);
    }

    // ── Pins: left (input) and right (output) at divider position ────────────
    ImNodes::BeginInputAttribute(CDPinLeft(node.node_id), ImNodesPinShape_Circle);
    ImGui::Dummy({content_w, 0.0f});
    ImNodes::EndInputAttribute();
    ImNodes::BeginOutputAttribute(CDPinRight(node.node_id), ImNodesPinShape_Circle);
    ImGui::Dummy({content_w, 0.0f});
    ImNodes::EndOutputAttribute();

    // ── Method rows ───────────────────────────────────────────────────────────
    DrawRowBlock(dl, TS::FONT_MONO_LOD[lod], px_mono, content_w, row_h, body_u32, node.methods);

    ImGui::PopStyleVar();  // ItemSpacing
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
    if (!graph.display_ready) PrepareDisplayText(graph);

    // Node sizes are now a pure function of the row counts, so the layout can be
    // computed up front — no render-then-measure round trip needed.
    if (!graph.layout_valid) {
        const CDNodeMetrics metrics = {
            TS::NODE_WIDTH     * TS::ui_scale,
            TS::NODE_HEADER_H  * TS::ui_scale,
            TS::NODE_ROW_H     * TS::ui_scale,
            TS::NODE_DIVIDER_H * TS::ui_scale,
            TS::NODE_PADDING_X * TS::ui_scale,
            TS::NODE_PADDING_Y * TS::ui_scale,
        };
        CDLayoutShelf(graph, CDGraphNodeSizes(graph, metrics),
                      TS::CD_LAYOUT_GAP * TS::ui_scale, TS::CD_LAYOUT_ASPECT);
    }

    // imnodes has no zoom of its own — node contents scale with `zoom` while
    // grid space does not — so positions must be rescaled every frame or the
    // boxes grow into each other as you zoom in.
    for (const auto& node : graph.nodes)
        ImNodes::SetNodeGridSpacePos(node.node_id, { node.pos.x * zoom, node.pos.y * zoom });

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

void SyncClassDiagramPositions(CDGraph& graph, float zoom)
{
    if (zoom <= 0.0f) return;

    // imnodes applies node dragging inside EndNodeEditor, so a drag only shows
    // up in grid space once the editor has closed. Convert it back to logical
    // space, otherwise the next frame's PushPositions would undo the drag.
    //
    // Only selected nodes are draggable, and skipping the rest avoids
    // round-tripping pos * zoom / zoom every frame for nodes that never moved.
    for (auto& node : graph.nodes) {
        if (!ImNodes::IsNodeSelected(node.node_id)) continue;
        const ImVec2 g = ImNodes::GetNodeGridSpacePos(node.node_id);
        node.pos = { g.x / zoom, g.y / zoom };
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
