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

// Assigns graph.nodes[i].pos from measured sizes and sets graph.layout_valid.
// `sizes` must be index-aligned with graph.nodes; on a length mismatch nothing
// is written and layout_valid stays false so the caller retries.
void CDLayoutShelf(CDGraph& graph, const std::vector<CDBox>& sizes, float gap, float aspect);

} // namespace TS
