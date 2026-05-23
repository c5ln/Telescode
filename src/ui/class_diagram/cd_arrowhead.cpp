// src/ui/class_diagram/cd_arrowhead.cpp

#include "cd_arrowhead.h"

namespace TS {

CDArrowTri CDArrowVertices(ImVec2 tip, ImVec2 dir_unit, float half_base, float length)
{
    ImVec2 base = { tip.x - dir_unit.x * length, tip.y - dir_unit.y * length };
    ImVec2 perp = { -dir_unit.y, dir_unit.x };
    return {
        tip,
        { base.x + perp.x * half_base, base.y + perp.y * half_base },
        { base.x - perp.x * half_base, base.y - perp.y * half_base },
    };
}

ImVec2 CDPinInputScreenPos(ImVec2 node_tl, ImVec2 node_size)
{
    return { node_tl.x, node_tl.y + node_size.y * 0.5f };
}

ImVec2 CDPinOutputScreenPos(ImVec2 node_tl, ImVec2 node_size)
{
    return { node_tl.x + node_size.x, node_tl.y + node_size.y * 0.5f };
}

CDArrowDims CDScaleArrowhead(float zoom)
{
    return { k_arrow_half_base * zoom, k_arrow_length * zoom };
}

} // namespace TS
