// src/ui/class_diagram/cd_layout.cpp

#include "cd_layout.h"
#include <algorithm>
#include <cmath>

namespace TS {

std::vector<ImVec2> CDShelfPack(const std::vector<CDBox>& boxes, float max_w, float gap)
{
    std::vector<ImVec2> pos(boxes.size());

    float cursor_x = 0.0f;   // left edge of the next box in the current row
    float shelf_y  = 0.0f;   // top edge of the current row
    float shelf_h  = 0.0f;   // tallest box placed in the current row so far

    for (size_t i = 0; i < boxes.size(); ++i) {
        // Wrap only when the row already holds something: a box wider than
        // max_w would otherwise wrap forever without ever being placed.
        if (cursor_x > 0.0f && cursor_x + boxes[i].w > max_w) {
            shelf_y += shelf_h + gap;
            cursor_x = 0.0f;
            shelf_h  = 0.0f;
        }
        pos[i]    = { cursor_x, shelf_y };
        cursor_x += boxes[i].w + gap;
        shelf_h   = std::max(shelf_h, boxes[i].h);
    }
    return pos;
}

float CDPreferredShelfWidth(const std::vector<CDBox>& boxes, float gap, float aspect)
{
    if (boxes.empty()) return 0.0f;

    // Charge each box for its own gap so the estimate survives many small boxes.
    float area   = 0.0f;
    float widest = 0.0f;
    for (const CDBox& b : boxes) {
        area   += (b.w + gap) * (b.h + gap);
        widest  = std::max(widest, b.w);
    }

    // area = w * h and w / h = aspect  =>  w = sqrt(area * aspect)
    return std::max(widest, std::sqrt(area * std::max(aspect, 0.01f)));
}

void CDLayoutShelf(CDGraph& graph, const std::vector<CDBox>& sizes, float gap, float aspect)
{
    if (graph.nodes.size() != sizes.size()) return;

    const std::vector<ImVec2> pos =
        CDShelfPack(sizes, CDPreferredShelfWidth(sizes, gap, aspect), gap);

    for (size_t i = 0; i < graph.nodes.size(); ++i)
        graph.nodes[i].pos = pos[i];

    graph.layout_valid = true;
}

} // namespace TS
