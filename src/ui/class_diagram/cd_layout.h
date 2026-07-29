// src/ui/class_diagram/cd_layout.h
// Node placement — pure geometry, no ImGui/imnodes context required.
//
// Everything here is in LOGICAL space: pixels as they would appear at zoom 1.0.
// imnodes has no zoom of its own — the canvas fakes it by scaling fonts and
// style vars — so node spacing does not follow the content unless the caller
// rescales it. Keeping the layout in logical space and multiplying by the
// current zoom on the way into imnodes is what keeps boxes apart at every zoom
// level, and what keeps the arrangement stable when the zoom changes.

#pragma once
#include "cd_model.h"
#include <vector>

namespace TS {

struct CDBox {
    float w;
    float h;
};

// Node geometry in logical pixels. cd_view reserves exactly these amounts, so a
// node's size never depends on how long its text happens to be — which is what
// lets the layout be computed up front instead of measured after a render pass.
struct CDNodeMetrics {
    float content_w;   // row width, inside the padding
    float header_h;    // title bar: class name + package
    float row_h;       // one field or method row
    float divider_h;   // fields / methods separator band
    float pad_x;       // imnodes NodePadding, added on both sides
    float pad_y;
};

// Height follows the row count; width is the same for every node.
CDBox CDNodeSize(const CDNode& node, const CDNodeMetrics& m);

// CDNodeSize over the whole graph, index-aligned with graph.nodes.
std::vector<CDBox> CDGraphNodeSizes(const CDGraph& graph, const CDNodeMetrics& m);

// Shelf packing: fills a row left-to-right, then wraps to a new row below the
// tallest box of the row it just closed.
//
// Input order is preserved, so boxes that belong together stay adjacent.
// Pre-sorting by descending height packs tighter (next-fit decreasing height)
// but scatters that grouping — that trade-off is the caller's to make.
//
// Returns each box's top-left corner, index-aligned with `boxes`.
std::vector<ImVec2> CDShelfPack(const std::vector<CDBox>& boxes, float max_w, float gap);

// Row width that makes the packed result roughly `aspect` times wider than
// tall. Never narrower than the widest box.
float CDPreferredShelfWidth(const std::vector<CDBox>& boxes, float gap, float aspect);

// Size of the region a packing occupies. `pos` must be index-aligned with `boxes`.
CDBox CDBoundingSize(const std::vector<CDBox>& boxes, const std::vector<ImVec2>& pos);

// ── Layered layout ───────────────────────────────────────────────────────────

struct CDLayerEdge {
    int src;
    int dst;
};

// Sugiyama-style layered layout, left to right: an edge pushes its target into
// a later layer, so the layer index becomes x and the order within a layer
// becomes y. Left-to-right matches the node pins, which sit on the left and
// right edges.
//
// Disconnected input is the normal case, not a corner case — most classes in a
// real repo have no relationships at all. Each weakly connected component is
// laid out on its own and the results are shelf-packed; without that, every
// unconnected node would pile into layer 0 as one endless column.
//
// Edges that close a cycle are dropped for layering purposes (call graphs are
// full of them). They still exist in the model and are still drawn — they just
// stop constraining the arrangement.
//
// Returns each box's top-left corner, index-aligned with `boxes`.
std::vector<ImVec2> CDLayeredLayout(const std::vector<CDBox>&      boxes,
                                    const std::vector<CDLayerEdge>& edges,
                                    float gap_x, float gap_y, float aspect);

// ── Container tree ───────────────────────────────────────────────────────────

// "sherlock_project/notify.py" → "sherlock_project"; a bare filename → "".
std::string CDFolderOf(const std::string& file_id);

// Keeps the first `depth` slash-separated segments: ("a/b/c", 2) → "a/b".
std::string CDTruncatePath(const std::string& path, int depth);

// Folder depth that lands the group count inside [min_groups, max_groups] where
// possible. A fixed depth does not work across repos: depth 1 puts everything
// under "src" in a big tree, while the full path fragments a deep one back into
// one group per directory.
int CDChooseFolderDepth(const std::vector<std::string>& file_ids,
                        int min_groups, int max_groups);

// Fills graph.containers from CDNode::file_id, preserving first-seen order so
// the result is stable across runs.
void CDBuildContainers(CDGraph& graph, int folder_depth);

// ── Hierarchical layout ──────────────────────────────────────────────────────

struct CDHierarchyMetrics {
    CDNodeMetrics node;
    float class_gap_x,  class_gap_y;   // between classes inside a file
    float file_gap_x,   file_gap_y;    // between files inside a folder
    float folder_gap_x, folder_gap_y;  // between folders on the canvas
    float file_pad,     file_header;   // border inset / label strip of a file box
    float folder_pad,   folder_header;
    float aspect;
};

// Lays the diagram out in three passes — classes within a file, files within a
// folder, folders across the canvas — each using CDLayeredLayout, then writes
// absolute positions into CDNode::pos and CDContainer::pos and sets
// graph.layout_valid.
//
// Call CDBuildContainers first; if graph.containers is empty this does nothing
// and leaves layout_valid false so the caller retries.
void CDLayoutHierarchical(CDGraph& graph, const CDHierarchyMetrics& m);

// Re-fits every container box around where its children actually are. Dragging a
// node moves it after the layout ran, and without this the file boundary would
// stay put while the class walked out of it. Idempotent, and reproduces exactly
// what CDLayoutHierarchical produced when nothing has moved.
void CDRefreshContainerBounds(CDGraph& graph, const CDHierarchyMetrics& m);

} // namespace TS
