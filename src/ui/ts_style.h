// src/ui/ts_style.h
// Single source of truth for all Telescode visual tokens (Warm Cream palette).
//
// Usage sequence (enforced by comments throughout):
//   1. ImGui::CreateContext()
//   2. ImNodes::CreateContext()
//   3. TS::LoadFonts(io)          -- must precede NewFrame; builds font atlas
//   4. TS::ApplyStyle()           -- sets ImGuiStyle, ImNodesStyle, precomputes _U32
//   5. Enter render loop -> ImGui::NewFrame() ...
//
// Rules:
//   - Zero raw hex values, magic numbers, or hardcoded floats anywhere else.
//   - ImDrawList sites use TS::*_U32 -- no inline ColorConvertFloat4ToU32 at draw time.
//   - All sizing usage multiplies by TS::ui_scale.
//   - ImGui::PushFont(TS::FONT_MONO) works anywhere after LoadFonts() returns.

#pragma once

#include <imgui.h>
#include <imnodes.h>

namespace TS {

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  Derivation helpers                                                         │
// └─────────────────────────────────────────────────────────────────────────────┘

// Multiply RGB channels by (1 - amount), leave alpha unchanged.
// Usage: Darken(ACCENT_PRIMARY, 0.08f)  →  hovered variant
inline ImVec4 Darken(ImVec4 c, float amount)
{
    float f = 1.0f - amount;
    return { c.x * f, c.y * f, c.z * f, c.w };
}

// Replace alpha only; RGB is unchanged.
// Usage: WithAlpha(INK, 0.0f)  →  transparent overlay
inline ImVec4 WithAlpha(ImVec4 c, float a)
{
    return { c.x, c.y, c.z, a };
}

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  Color tokens — Warm Cream palette                                          │
// │  Source: telescode-design-system.html :root block                           │
// │  Layout: ImVec4{ r, g, b, a } normalised [0, 1]                            │
// └─────────────────────────────────────────────────────────────────────────────┘

// ── Background ────────────────────────────────────────────────────────────────
inline constexpr ImVec4 BG      = { 0.953f, 0.945f, 0.925f, 1.0f }; // #f3f1ec
inline constexpr ImVec4 BG_SOFT = { 0.922f, 0.910f, 0.878f, 1.0f }; // #ebe8e0
inline constexpr ImVec4 PANEL   = { 1.000f, 1.000f, 1.000f, 1.0f }; // #ffffff
inline constexpr ImVec4 PANEL_2 = { 0.973f, 0.965f, 0.945f, 1.0f }; // #f8f6f1
inline constexpr ImVec4 LINE    = { 0.851f, 0.831f, 0.784f, 1.0f }; // #d9d4c8

// ── Ink (text hierarchy) ──────────────────────────────────────────────────────
inline constexpr ImVec4 INK   = { 0.102f, 0.133f, 0.188f, 1.0f }; // #1a2230
inline constexpr ImVec4 INK_2 = { 0.290f, 0.329f, 0.400f, 1.0f }; // #4a5466
inline constexpr ImVec4 INK_3 = { 0.486f, 0.522f, 0.592f, 1.0f }; // #7c8597
inline constexpr ImVec4 MUTED = { 0.604f, 0.639f, 0.702f, 1.0f }; // #9aa3b3

// ── Header / canvas dark bar ──────────────────────────────────────────────────
inline constexpr ImVec4 NIGHT      = { 0.047f, 0.086f, 0.149f, 1.0f }; // #0c1626
inline constexpr ImVec4 NIGHT_2    = { 0.059f, 0.110f, 0.188f, 1.0f }; // #0f1c30
inline constexpr ImVec4 NIGHT_LINE = { 0.122f, 0.188f, 0.314f, 1.0f }; // #1f3050

// ── Accent: Primary — purple ──────────────────────────────────────────────────
// Semantic role: primary interactive colour, selection highlight.
// Caller/callee mapping lives in rendering code, not in token names.
inline constexpr ImVec4 ACCENT_PRIMARY        = { 0.655f, 0.545f, 0.980f, 1.0f }; // #a78bfa
inline constexpr ImVec4 ACCENT_PRIMARY_SUBTLE = { 0.937f, 0.918f, 1.000f, 1.0f }; // #efeaff

// ── Accent: Secondary — teal ──────────────────────────────────────────────────
inline constexpr ImVec4 ACCENT_SECONDARY        = { 0.176f, 0.831f, 0.749f, 1.0f }; // #2dd4bf
inline constexpr ImVec4 ACCENT_SECONDARY_SUBTLE = { 0.871f, 0.980f, 0.953f, 1.0f }; // #defaf3

// ── Accent: Focus — amber ─────────────────────────────────────────────────────
// Semantic role: selected / focused node in the graph.
inline constexpr ImVec4 ACCENT_FOCUS        = { 0.984f, 0.749f, 0.141f, 1.0f }; // #fbbf24
inline constexpr ImVec4 ACCENT_FOCUS_SUBTLE = { 0.996f, 0.953f, 0.780f, 1.0f }; // #fef3c7

} // namespace TS
