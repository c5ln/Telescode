// src/ui/class_diagram/cd_arrowhead.h
// Pure-math helpers for arrowhead rendering — no ImGui/imnodes context required.

#pragma once
#include <imgui.h>

namespace TS {

constexpr float k_arrow_half_base = 6.0f;
constexpr float k_arrow_length    = 10.0f;

struct CDArrowTri {
    ImVec2 tip;
    ImVec2 v1;
    ImVec2 v2;
};

struct CDArrowDims {
    float half_base;
    float length;
};

// dir_unit points from source toward the tip (not outward from it).
CDArrowTri CDArrowVertices(ImVec2 tip, ImVec2 dir_unit, float half_base, float length);

ImVec2 CDPinInputScreenPos(ImVec2 node_tl, ImVec2 node_size);
ImVec2 CDPinOutputScreenPos(ImVec2 node_tl, ImVec2 node_size);
CDArrowDims CDScaleArrowhead(float zoom);

} // namespace TS
