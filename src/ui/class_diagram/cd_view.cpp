// src/ui/class_diagram/cd_view.cpp

#include "cd_view.h"
#include "cd_arrowhead.h"
#include "cd_layout.h"
#include "../ts_style.h"
#include <imgui.h>
#include <imnodes.h>
#include <cfloat>
#include <algorithm>
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

// Scales a colour's alpha rather than replacing it, so a fade composes with the
// dimming the selection already applied instead of overriding it.
ImU32 FadeU32(ImVec4 c, float a)
{
    c.w *= a;
    return ImGui::ColorConvertFloat4ToU32(c);
}

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

// DrawRowText, horizontally centred — the overview's file names sit in the
// middle of their box. Measuring is unavoidable here: the text is centred
// against the box rather than run from the cursor, so its width has to be known
// before its draw position is.
void DrawCenteredText(ImDrawList* dl, ImFont* font, float px,
                      ImVec2 tl, float w, float h, ImU32 col, const char* text)
{
    if (!font || !text || !*text) return;
    const float tw   = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
    const ImVec4 clip = { tl.x, tl.y, tl.x + w, tl.y + h };
    dl->AddText(font, px,
                { tl.x + std::max((w - tw) * 0.5f, 0.0f), tl.y + (h - px) * 0.5f },
                col, text, nullptr, 0.0f, &clip);
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

    // Faded out below the member band. The reservation above still happens: node
    // height is a pure function of the row count (CDNodeSize), and the layout is
    // computed from it up front, so collapsing the rows here would leave every
    // box in the wrong place until the next full relayout.
    if (((col >> IM_COL32_A_SHIFT) & 0xFF) == 0) return;

    for (size_t i = 0; i < rows.size(); ++i)
        DrawRowText(dl, font, px, {tl.x, tl.y + row_h * static_cast<float>(i)},
                    w, row_h, col, rows[i].display.c_str());
}

// Ellipsizes container labels against each container's own width, once.
void PrepareContainerLabels(CDGraph& graph)
{
    const int   lod   = TS::GetFontLOD(1.0f);
    const float base  = TS::FONT_SIZE_BASE  * TS::ui_scale;
    const float small = TS::FONT_SIZE_SMALL * TS::ui_scale;

    for (CDContainer& c : graph.containers) {
        const float pad = (c.is_file ? TS::CD_FILE_PAD : TS::CD_FOLDER_PAD) * TS::ui_scale;
        const float budget = std::max(c.size.x - 2.0f * pad, 1.0f);
        c.display_label = c.is_file
            ? Ellipsize(TS::FONT_SMALL_LOD[lod],  small, budget, c.label)
            : Ellipsize(TS::FONT_MEDIUM_LOD[lod], base,  budget, c.label);

        // Fitted once against the uniform box, at the logical size it will be
        // drawn at. Both the size and the box then scale by the same zoom every
        // frame, so the fit holds and the draw path never has to re-measure.
        const float ov_px = (c.is_file ? TS::CD_OVERVIEW_LABEL_PX
                                       : TS::CD_OVERVIEW_FOLDER_LABEL_PX) * TS::ui_scale;
        const float ov_budget =
            std::max(c.overview_size.x * TS::CD_OVERVIEW_LABEL_FILL, 1.0f);
        c.overview_label = Ellipsize(TS::FONT_MEDIUM_LOD[lod], ov_px, ov_budget, c.label);
    }
}

// Draws the file and folder boundaries behind the nodes.
//
// This must run after BeginNodeEditor() and BEFORE the first BeginNode().
// imnodes splits the canvas draw list into a channel pair per node, and channel
// 0 — the one the grid goes into — stays current until the first node is
// submitted, so anything drawn here ends up behind every node. Drawing after
// EndNodeEditor the way the arrowheads do would paint over the top instead,
// which is no use for a container. Keeping the call at the head of
// DrawClassDiagramContent is what enforces that ordering.
//
// `t` is the node fade: 0 = the overview placement, every file the same box with
// its name centred; 1 = the detail placement, every file wrapped around the
// classes it holds. In between, boxes interpolate and the two label treatments
// cross-fade.
void DrawContainers(const CDGraph& graph, float zoom, float t)
{
    if (graph.containers.empty()) return;

    // screen = canvas origin + panning + grid, and grid = logical * zoom.
    const ImVec2 canvas  = ImGui::GetCursorScreenPos();
    const ImVec2 panning = ImNodes::EditorContextGetPanning();

    const float z = zoom * TS::ui_scale;
    const float r = TS::CD_CONTAINER_ROUNDING * z;

    // Both boundaries stay in the warm paper family. LINE itself is only about
    // 1.2:1 against the canvas, so the folder border is a shaded LINE rather
    // than LINE at full alpha — no amount of opacity fixes a tone that close to
    // the background. Its fill stays light: it sits under every node.
    const ImU32 folder_fill = ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::LINE, 0.30f));
    const ImU32 folder_line = ImGui::ColorConvertFloat4ToU32(TS::Shade(TS::LINE, 0.72f));
    const ImU32 file_fill   = ImGui::ColorConvertFloat4ToU32(TS::WithAlpha(TS::PANEL, 0.55f));
    const ImU32 file_line   = TS::LINE_U32;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Folders first so the file boxes read as sitting inside them.
    for (int pass = 0; pass < 2; ++pass) {
        const bool files = (pass == 1);
        for (const CDContainer& c : graph.containers) {
            if (c.is_file != files) continue;

            // Interpolate the two placements. Both are absolute logical
            // coordinates, so this is a straight lerp — no basis change.
            const ImVec2 box_pos  = { c.overview_pos.x  + (c.pos.x  - c.overview_pos.x)  * t,
                                      c.overview_pos.y  + (c.pos.y  - c.overview_pos.y)  * t };
            const ImVec2 box_size = { c.overview_size.x + (c.size.x - c.overview_size.x) * t,
                                      c.overview_size.y + (c.size.y - c.overview_size.y) * t };

            const ImVec2 tl = { canvas.x + panning.x + box_pos.x * zoom,
                                canvas.y + panning.y + box_pos.y * zoom };
            const ImVec2 br = { tl.x + box_size.x * zoom, tl.y + box_size.y * zoom };
            const float  bw = box_size.x * zoom;
            const float  bh = box_size.y * zoom;

            dl->AddRectFilled(tl, br, files ? file_fill : folder_fill, r);
            // Floor at one pixel: scaled down, a 1.5px border all but vanishes
            // at ZOOM_MIN, which is exactly where the grouping matters most.
            dl->AddRect(tl, br, files ? file_line : folder_line, r, 0,
                        std::max((files ? TS::CD_FILE_BORDER : TS::CD_FOLDER_BORDER) * z, 1.0f));

            // ── Labels ────────────────────────────────────────────────────────
            // The two treatments cross-fade in place rather than one sliding into
            // the other: they differ in position, size and alignment all at once,
            // and interpolating that reads as the text wandering around the box.

            // Overview: large and centred, scaling with the box. No pixel floor
            // is needed — the box is uniform, so a label fitted to it once in
            // PrepareContainerLabels stays fitted at every zoom.
            if (t < 1.0f) {
                const float ov_px = (files ? TS::CD_OVERVIEW_LABEL_PX
                                           : TS::CD_OVERVIEW_FOLDER_LABEL_PX) * z;
                // A file's box holds nothing else, so its name takes the middle.
                // A folder's middle belongs to its files — its name stays up in
                // the header strip.
                const float strip = files ? bh : TS::CD_OVERVIEW_FOLDER_HEADER * z;
                const int   lod   = TS::GetFontLOD(ov_px / (TS::FONT_SIZE_BASE * TS::ui_scale));

                DrawCenteredText(dl, TS::FONT_MEDIUM_LOD[lod], ov_px, tl, bw, strip,
                                 FadeU32(files ? TS::INK_2 : TS::INK, 1.0f - t),
                                 c.overview_label.c_str());
            }

            // Detail: a thin strip at the top-left. This box is derived from its
            // children and can shrink to nothing, so here the pixel floor does
            // apply — and the strip has to grow with it, or at low zoom
            // CD_FILE_HEADER * z is three pixels and the clip rect below would
            // swallow the text whole.
            if (t > 0.0f) {
                const float pad     = (files ? TS::CD_FILE_PAD : TS::CD_FOLDER_PAD) * z;
                const float base_px = (files ? TS::FONT_SIZE_SMALL : TS::FONT_SIZE_BASE)
                                    * TS::ui_scale;
                const float px      = std::max(base_px * zoom,
                                               TS::CD_LABEL_MIN_PX * TS::ui_scale);
                const float header  = std::max(
                    (files ? TS::CD_FILE_HEADER : TS::CD_FOLDER_HEADER) * z, px);
                const float budget  = bw - 2.0f * pad;

                // A name clipped to its first three characters is noise, not a
                // label. Nothing is lost by dropping it — the overview label it
                // is cross-fading with is still on screen at that point.
                if (budget < px * 3.0f) continue;

                // Below the floor, px no longer tracks the zoom, so the atlas has
                // to be picked from the size actually being drawn —
                // GetFontLOD(0.12) would hand back the 0.5x atlas for an 11px
                // label.
                const int lod = TS::GetFontLOD(px / base_px);

                DrawRowText(dl, files ? TS::FONT_SMALL_LOD[lod] : TS::FONT_MEDIUM_LOD[lod],
                            px, { tl.x + pad, tl.y }, budget, header,
                            FadeU32(files ? TS::INK_3 : TS::INK_2, t),
                            c.display_label.c_str());
            }
        }
    }
}

// `node_alpha` fades the whole box in over CD_NODE_FADE_LO..HI; `member_alpha`
// fades the field and method rows in later, over CD_MEMBER_FADE_LO..HI. Both are
// multiplied into the colours — geometry is identical at every zoom.
void DrawNode(const CDNode& node, bool selected, bool dimmed, float zoom,
              float node_alpha, float member_alpha)
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

    ImVec4 title_v;
    ImVec4 bg_v;
    if (selected) {
        title_v = TS::ACCENT_SECONDARY;
        bg_v    = TS::PANEL;
    } else if (dimmed) {
        title_v = TS::WithAlpha(TS::ACCENT_PRIMARY_SUBTLE, 0.35f);
        bg_v    = TS::WithAlpha(TS::PANEL, 0.5f);
    } else {
        title_v = TS::ACCENT_PRIMARY_SUBTLE;
        bg_v    = TS::PANEL;
    }
    const ImU32 title_col = FadeU32(title_v, node_alpha);
    const ImU32 bg_col    = FadeU32(bg_v,    node_alpha);

    ImNodes::PushColorStyle(ImNodesCol_TitleBar,               title_col);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered,        title_col);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected,       title_col);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackground,         bg_col);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered,  bg_col);
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, bg_col);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinCircleRadius, 0.0f);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinHoverRadius,  0.0f);

    // The header travels with the box; the rows and the divider that separates
    // them are member detail and wait for the second band.
    const float detail_alpha = node_alpha * member_alpha;

    const ImU32 ink_u32  = FadeU32(dimmed ? TS::WithAlpha(TS::INK,   0.4f) : TS::INK,   node_alpha);
    const ImU32 ink3_u32 = FadeU32(dimmed ? TS::WithAlpha(TS::INK_3, 0.4f) : TS::INK_3, node_alpha);
    const ImU32 body_u32 = FadeU32(dimmed ? TS::WithAlpha(TS::INK_2, 0.4f) : TS::INK_2, detail_alpha);
    const ImU32 line_u32 = FadeU32(dimmed ? TS::WithAlpha(TS::LINE,  0.3f) : TS::LINE,  detail_alpha);

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

    const float s = TS::ui_scale;
    const CDHierarchyMetrics metrics = {
        { TS::NODE_WIDTH * s, TS::NODE_HEADER_H * s, TS::NODE_ROW_H * s,
          TS::NODE_DIVIDER_H * s, TS::NODE_PADDING_X * s, TS::NODE_PADDING_Y * s },
        TS::CD_CLASS_GAP_X  * s, TS::CD_CLASS_GAP_Y   * s,
        TS::CD_FILE_GAP_X   * s, TS::CD_FILE_GAP_Y    * s,
        TS::CD_FOLDER_GAP_X * s, TS::CD_FOLDER_GAP_Y  * s,
        TS::CD_FILE_PAD     * s, TS::CD_FILE_HEADER   * s,
        TS::CD_FOLDER_PAD   * s, TS::CD_FOLDER_HEADER * s,
        TS::CD_LAYOUT_ASPECT,
    };

    const CDOverviewMetrics ov_metrics = {
        { TS::CD_OVERVIEW_FILE_W * s, TS::CD_OVERVIEW_FILE_H * s },
        TS::CD_OVERVIEW_FILE_GAP_X   * s, TS::CD_OVERVIEW_FILE_GAP_Y   * s,
        TS::CD_OVERVIEW_FOLDER_GAP_X * s, TS::CD_OVERVIEW_FOLDER_GAP_Y * s,
        TS::CD_OVERVIEW_FOLDER_PAD   * s, TS::CD_OVERVIEW_FOLDER_HEADER * s,
        TS::CD_LAYOUT_ASPECT,
    };

    // Node sizes are a pure function of the row counts, so the whole hierarchy
    // can be laid out up front — no render-then-measure round trip needed.
    if (!graph.layout_valid) {
        std::vector<std::string> file_ids;
        file_ids.reserve(graph.nodes.size());
        for (const CDNode& n : graph.nodes) file_ids.push_back(n.file_id);

        CDBuildContainers(graph, CDChooseFolderDepth(file_ids,
                                                     TS::CD_FOLDER_GROUP_MIN,
                                                     TS::CD_FOLDER_GROUP_MAX));
        CDLayoutHierarchical(graph, metrics);
        // Both placements before the labels: PrepareContainerLabels fits each
        // label against the box it will be drawn in, and there are now two.
        CDLayoutOverview(graph, ov_metrics);
        PrepareContainerLabels(graph);
    }

    // Cheap, and it keeps the boundaries wrapped around dragged nodes.
    CDRefreshContainerBounds(graph, metrics);

    // ── Semantic zoom ─────────────────────────────────────────────────────────
    // Zoomed out far enough, the class boxes are gone and what remains is the
    // overview: one identically sized box per file, name centred, grouped by
    // folder. node_alpha doubles as the interpolation parameter between that
    // placement and the detail one, so the containers and the nodes inside them
    // always move together.
    const float node_alpha   = TS::CDFade(zoom, TS::CD_NODE_FADE_LO,   TS::CD_NODE_FADE_HI);
    const float member_alpha = TS::CDFade(zoom, TS::CD_MEMBER_FADE_LO, TS::CD_MEMBER_FADE_HI);

    DrawContainers(graph, zoom, node_alpha);

    // Nodes are skipped outright below the band rather than drawn transparent: a
    // submitted node still claims hover and box-select, so an invisible one would
    // answer the mouse.
    if (node_alpha <= 0.0f) return;

    // imnodes has no zoom of its own — node contents scale with `zoom` while
    // grid space does not — so positions must be rescaled every frame or the
    // boxes grow into each other as you zoom in.
    for (const auto& node : graph.nodes)
        ImNodes::SetNodeGridSpacePos(node.node_id, { node.pos.x * zoom, node.pos.y * zoom });

    // Mid-transition, a class collapses toward the centre of its file's overview
    // box. Without this the nodes would fade in already at their final spread
    // while the file boundary was still a small uniform box somewhere else, and
    // classes would appear outside the file they belong to.
    if (node_alpha < 1.0f) {
        for (const CDContainer& c : graph.containers) {
            if (!c.is_file) continue;
            const ImVec2 ctr = { c.overview_pos.x + c.overview_size.x * 0.5f,
                                 c.overview_pos.y + c.overview_size.y * 0.5f };
            for (int ni : c.child_nodes) {
                const CDNode& n = graph.nodes[static_cast<size_t>(ni)];
                ImNodes::SetNodeGridSpacePos(
                    n.node_id,
                    { (ctr.x + (n.pos.x - ctr.x) * node_alpha) * zoom,
                      (ctr.y + (n.pos.y - ctr.y) * node_alpha) * zoom });
            }
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
        DrawNode(node, sel, dimmed, zoom, node_alpha, member_alpha);
    }

    // ── Edges — nearest-pin routing ───────────────────────────────────────────
    for (const auto& edge : graph.edges) {
        const bool edge_dimmed = has_selection &&
            edge.src_node_id != graph.selected_node_id &&
            edge.dst_node_id != graph.selected_node_id;

        ImVec4 link_v;
        if (edge_dimmed) {
            link_v = TS::WithAlpha(TS::MUTED, 0.2f);
        } else if (edge.type == CDEdgeType::Dependency) {
            link_v = TS::MUTED;
        } else {
            link_v = TS::INK_2;
        }
        // Links arrive with the boxes they connect, not before them.
        const ImU32 link_col = FadeU32(link_v, node_alpha);

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
    // Must match the gate in DrawClassDiagramContent: below the node band no
    // node was submitted this frame, so GetNodeScreenSpacePos has nothing to
    // report and the arrowheads would be pinned to a stale position.
    const float node_alpha = TS::CDFade(zoom, TS::CD_NODE_FADE_LO, TS::CD_NODE_FADE_HI);
    if (node_alpha <= 0.0f) return;

    ImDrawList*       dl        = ImGui::GetWindowDrawList();
    const CDArrowDims arrow     = CDScaleArrowhead(zoom * TS::ui_scale);
    const float       outline_w = TS::LINK_THICKNESS * zoom * TS::ui_scale;
    const bool        has_sel   = (graph.selected_node_id != -1);

    for (const auto& edge : graph.edges) {
        const bool edge_dimmed = has_sel &&
            edge.src_node_id != graph.selected_node_id &&
            edge.dst_node_id != graph.selected_node_id;

        ImVec4 col_v;
        if (edge_dimmed) {
            col_v = TS::WithAlpha(TS::MUTED, 0.2f);
        } else if (edge.type == CDEdgeType::Dependency) {
            col_v = TS::MUTED;
        } else {
            col_v = TS::INK_2;
        }
        const ImU32 col = FadeU32(col_v, node_alpha);

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

    // Mid-transition the rendered positions are pulled toward the overview boxes,
    // not the layout's own. Reading them back there would bake that collapse into
    // CDNode::pos and permanently pile every class onto its file's centre, so only
    // a fully arrived diagram is safe to recover a drag from.
    if (TS::CDFade(zoom, TS::CD_NODE_FADE_LO, TS::CD_NODE_FADE_HI) < 1.0f) return;

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
