// src/core/core_vec2.h
// The 2D point type used by core geometry.
//
// The layout and model code below src/core/ used to spell this ImVec2, which
// dragged <imgui.h> -- and therefore the whole UI dependency -- into otherwise
// pure analysis code. It was only ever used as a pair of floats: brace
// initialisation and .x / .y member reads, never an ImGui operator or call. So
// the substitution is a rename, not a change of behaviour.
//
// Deliberately layout-compatible with ImVec2 (two floats, x then y) so the UI
// layer can keep brace-initialising one from the other at the boundary.

#pragma once

namespace TS {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

} // namespace TS
