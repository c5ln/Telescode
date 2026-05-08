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

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  ImU32 variants — for ImDrawList call sites                                 │
// │  Zero-initialised here; precomputed once by ApplyStyle() at startup.        │
// │  Usage: ImDrawList::AddRectFilled(..., TS::PANEL_U32)                       │
// │  Never call ColorConvertFloat4ToU32 inline at draw time.                    │
// └─────────────────────────────────────────────────────────────────────────────┘

inline ImU32 BG_U32                      = 0;
inline ImU32 BG_SOFT_U32                 = 0;
inline ImU32 PANEL_U32                   = 0;
inline ImU32 PANEL_2_U32                 = 0;
inline ImU32 LINE_U32                    = 0;
inline ImU32 INK_U32                     = 0;
inline ImU32 INK_2_U32                   = 0;
inline ImU32 INK_3_U32                   = 0;
inline ImU32 MUTED_U32                   = 0;
inline ImU32 NIGHT_U32                   = 0;
inline ImU32 NIGHT_2_U32                 = 0;
inline ImU32 NIGHT_LINE_U32              = 0;
inline ImU32 ACCENT_PRIMARY_U32          = 0;
inline ImU32 ACCENT_PRIMARY_SUBTLE_U32   = 0;
inline ImU32 ACCENT_SECONDARY_U32        = 0;
inline ImU32 ACCENT_SECONDARY_SUBTLE_U32 = 0;
inline ImU32 ACCENT_FOCUS_U32            = 0;
inline ImU32 ACCENT_FOCUS_SUBTLE_U32     = 0;

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  DPI scale                                                                  │
// └─────────────────────────────────────────────────────────────────────────────┘

// Set once at startup from SDL_GetDisplayContentScale() or user preference.
// ALL sizing constants below are canonical 1× reference values.
// Every usage site must multiply: e.g.  NODE_WIDTH * TS::ui_scale
// ImGui is not a fluid-layout system — "responsive" means uniform DPI scale,
// not CSS-style reflow.
inline float ui_scale = 1.0f;

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  Sizing constants                                                           │
// └─────────────────────────────────────────────────────────────────────────────┘

// Node geometry
inline constexpr float NODE_WIDTH      = 220.0f;
inline constexpr float NODE_HEADER_H   =  52.0f;
inline constexpr float NODE_METHOD_ROW =  22.0f;
inline constexpr float NODE_ROUNDING   =  10.0f;
inline constexpr float NODE_PADDING_X  =  12.0f;

// Font sizes (px at 1×)
inline constexpr float FONT_SIZE_BASE  = 13.0f;  // Inter Regular  — body, sidebar
inline constexpr float FONT_SIZE_SMALL = 11.5f;  // Inter Regular  — subtitles, badges
inline constexpr float FONT_SIZE_MONO  = 11.5f;  // JetBrains Mono — method signatures

// Layout
inline constexpr float SIDEBAR_W  = 280.0f;
inline constexpr float HEADER_H   =  56.0f;
inline constexpr float SEQUENCE_H = 180.0f;

// Zoom
inline constexpr float ZOOM_MIN         = 0.40f;
inline constexpr float ZOOM_MAX         = 2.00f;
inline constexpr float ZOOM_STEP_SCROLL = 0.05f;
inline constexpr float ZOOM_STEP_BTN   = 0.10f;

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  Font pointers                                                              │
// │  All nullptr until LoadFonts() returns.                                     │
// │  SEQUENCE: LoadFonts() -> (font atlas built on first NewFrame) -> render   │
// │  Never call ImGui::PushFont(TS::FONT_*) before LoadFonts() has been called. │
// └─────────────────────────────────────────────────────────────────────────────┘

inline ImFont* FONT_BASE   = nullptr; // Inter Regular  13px  — body, sidebar
inline ImFont* FONT_MEDIUM = nullptr; // Inter Medium   13px  — node titles
inline ImFont* FONT_SMALL  = nullptr; // Inter Regular  11.5px — subtitles, badges
inline ImFont* FONT_MONO   = nullptr; // JetBrains Mono 11.5px — method signatures

// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  LoadFonts                                                                  │
// └─────────────────────────────────────────────────────────────────────────────┘

// Loads Inter Regular/Medium and JetBrains Mono from assets/fonts/ relative to
// the working directory. Falls back to ImGui's embedded default if a file is
// missing so the app always runs.
//
// MUST be called after ImGui::CreateContext() and BEFORE ImGui::NewFrame().
// The font atlas is built lazily on the first NewFrame() call.
inline void LoadFonts(ImGuiIO& io)
{
    io.Fonts->Clear();

    const float base_sz  = FONT_SIZE_BASE  * ui_scale;
    const float small_sz = FONT_SIZE_SMALL * ui_scale;
    const float mono_sz  = FONT_SIZE_MONO  * ui_scale;

    FONT_BASE   = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf",        base_sz);
    FONT_MEDIUM = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Medium.ttf",         base_sz);
    FONT_SMALL  = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf",        small_sz);
    FONT_MONO   = io.Fonts->AddFontFromFileTTF("assets/fonts/JetBrainsMono-Regular.ttf", mono_sz);

    // Graceful fallback: use ImGui's embedded ProggyClean if any file is absent.
    if (!FONT_BASE)   FONT_BASE   = io.Fonts->AddFontDefault();
    if (!FONT_MEDIUM) FONT_MEDIUM = io.Fonts->AddFontDefault();
    if (!FONT_SMALL)  FONT_SMALL  = io.Fonts->AddFontDefault();
    if (!FONT_MONO)   FONT_MONO   = io.Fonts->AddFontDefault();
}

} // namespace TS
