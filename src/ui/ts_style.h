// src/ui/ts_style.h
// Single source of truth for all Telescode visual tokens (Warm Cream palette).
//
// Usage sequence (enforced by comments throughout):
//   1. ImGui::CreateContext()
//   2. ImNodes::CreateContext()
//   3. TS::LoadFonts(io)          -- must precede NewFrame; builds font atlas
//   4. TS::ApplyStyle()           -- sets ImGuiStyle, ImNodesStyle, syncs all _U32
//   5. Enter render loop -> ImGui::NewFrame() ...
//
// Rules:
//   - Zero raw hex values, magic numbers, or hardcoded floats anywhere else.
//   - All color writes go through ApplyToken() -- never write ImGuiStyle or
//     ImNodesStyle color slots directly outside of that function.
//   - ImDrawList sites use TS::*_U32 -- no inline ColorConvertFloat4ToU32 at draw time.
//   - All sizing usage multiplies by TS::ui_scale.
//   - ImGui::PushFont(TS::FONT_MONO) works anywhere after LoadFonts() returns.

#pragma once

#include <imgui.h>
#include <imnodes.h>

namespace TS {

// +-----------------------------------------------------------------------------+
// |  Derivation helpers                                                         |
// +-----------------------------------------------------------------------------+

// Multiply RGB channels by (1 - amount), leave alpha unchanged.
// Usage: Darken(ACCENT_PRIMARY, 0.08f)  ->  hovered variant
inline ImVec4 Darken(ImVec4 c, float amount)
{
    float f = 1.0f - amount;
    return { c.x * f, c.y * f, c.z * f, c.w };
}

// Replace alpha only; RGB is unchanged.
// Usage: WithAlpha(INK, 0.0f)  ->  transparent overlay
inline ImVec4 WithAlpha(ImVec4 c, float a)
{
    return { c.x, c.y, c.z, a };
}

// +-----------------------------------------------------------------------------+
// |  Color tokens -- Warm Cream palette                                         |
// |  Source: telescode-design-system.html :root block                           |
// |  Layout: ImVec4{ r, g, b, a } normalised [0, 1]                            |
// |                                                                             |
// |  Two layers per token:                                                      |
// |    *_DEFAULT  constexpr -- canonical palette value, never modified.         |
// |               Use as reset target: token = TOKEN_DEFAULT                    |
// |    (bare)     inline ImVec4 -- mutable runtime value, initialised from      |
// |               *_DEFAULT. Modified exclusively via ApplyToken().             |
// |               Draw code reads these; never read *_DEFAULT at draw time.     |
// +-----------------------------------------------------------------------------+

// -- Background ----------------------------------------------------------------
inline constexpr ImVec4 BG_DEFAULT      = { 0.953f, 0.945f, 0.925f, 1.0f }; // #f3f1ec
inline constexpr ImVec4 BG_SOFT_DEFAULT = { 0.922f, 0.910f, 0.878f, 1.0f }; // #ebe8e0
inline constexpr ImVec4 PANEL_DEFAULT   = { 1.000f, 1.000f, 1.000f, 1.0f }; // #ffffff
inline constexpr ImVec4 PANEL_2_DEFAULT = { 0.973f, 0.965f, 0.945f, 1.0f }; // #f8f6f1
inline constexpr ImVec4 LINE_DEFAULT    = { 0.851f, 0.831f, 0.784f, 1.0f }; // #d9d4c8

// -- Ink (text hierarchy) ------------------------------------------------------
inline constexpr ImVec4 INK_DEFAULT   = { 0.102f, 0.133f, 0.188f, 1.0f }; // #1a2230
inline constexpr ImVec4 INK_2_DEFAULT = { 0.290f, 0.329f, 0.400f, 1.0f }; // #4a5466
inline constexpr ImVec4 INK_3_DEFAULT = { 0.486f, 0.522f, 0.592f, 1.0f }; // #7c8597
inline constexpr ImVec4 MUTED_DEFAULT = { 0.604f, 0.639f, 0.702f, 1.0f }; // #9aa3b3

// -- Header / canvas dark bar --------------------------------------------------
inline constexpr ImVec4 HEADER_DEFAULT      = { 0.047f, 0.086f, 0.149f, 1.0f }; // #0c1626
inline constexpr ImVec4 HEADER_2_DEFAULT    = { 0.059f, 0.110f, 0.188f, 1.0f }; // #0f1c30
inline constexpr ImVec4 HEADER_LINE_DEFAULT = { 0.122f, 0.188f, 0.314f, 1.0f }; // #1f3050

// -- Accent: Primary -- purple -------------------------------------------------
// Semantic role: primary interactive colour, selection highlight.
// Caller/callee mapping lives in rendering code, not in token names.
inline constexpr ImVec4 ACCENT_PRIMARY_DEFAULT        = { 0.655f, 0.545f, 0.980f, 1.0f }; // #a78bfa
inline constexpr ImVec4 ACCENT_PRIMARY_SUBTLE_DEFAULT = { 0.937f, 0.918f, 1.000f, 1.0f }; // #efeaff

// -- Accent: Secondary -- teal -------------------------------------------------
inline constexpr ImVec4 ACCENT_SECONDARY_DEFAULT        = { 0.176f, 0.831f, 0.749f, 1.0f }; // #2dd4bf
inline constexpr ImVec4 ACCENT_SECONDARY_SUBTLE_DEFAULT = { 0.871f, 0.980f, 0.953f, 1.0f }; // #defaf3

// -- Accent: Focus -- amber ----------------------------------------------------
// Semantic role: selected / focused node in the graph.
inline constexpr ImVec4 ACCENT_FOCUS_DEFAULT        = { 0.984f, 0.749f, 0.141f, 1.0f }; // #fbbf24
inline constexpr ImVec4 ACCENT_FOCUS_SUBTLE_DEFAULT = { 0.996f, 0.953f, 0.780f, 1.0f }; // #fef3c7

// -- Mutable runtime values ----------------------------------------------------
// Initialised from *_DEFAULT. Modified exclusively through ApplyToken().
// Draw code reads these. Never read *_DEFAULT at draw time.
inline ImVec4 BG                      = BG_DEFAULT;
inline ImVec4 BG_SOFT                 = BG_SOFT_DEFAULT;
inline ImVec4 PANEL                   = PANEL_DEFAULT;
inline ImVec4 PANEL_2                 = PANEL_2_DEFAULT;
inline ImVec4 LINE                    = LINE_DEFAULT;
inline ImVec4 INK                     = INK_DEFAULT;
inline ImVec4 INK_2                   = INK_2_DEFAULT;
inline ImVec4 INK_3                   = INK_3_DEFAULT;
inline ImVec4 MUTED                   = MUTED_DEFAULT;
inline ImVec4 HEADER                  = HEADER_DEFAULT;
inline ImVec4 HEADER_2                = HEADER_2_DEFAULT;
inline ImVec4 HEADER_LINE             = HEADER_LINE_DEFAULT;
inline ImVec4 ACCENT_PRIMARY          = ACCENT_PRIMARY_DEFAULT;
inline ImVec4 ACCENT_PRIMARY_SUBTLE   = ACCENT_PRIMARY_SUBTLE_DEFAULT;
inline ImVec4 ACCENT_SECONDARY        = ACCENT_SECONDARY_DEFAULT;
inline ImVec4 ACCENT_SECONDARY_SUBTLE = ACCENT_SECONDARY_SUBTLE_DEFAULT;
inline ImVec4 ACCENT_FOCUS            = ACCENT_FOCUS_DEFAULT;
inline ImVec4 ACCENT_FOCUS_SUBTLE     = ACCENT_FOCUS_SUBTLE_DEFAULT;

// +-----------------------------------------------------------------------------+
// |  ImU32 variants -- for ImDrawList call sites                                |
// |  Zero-initialised here; kept in sync by ApplyToken().                      |
// |  Usage: ImDrawList::AddRectFilled(..., TS::PANEL_U32)                       |
// |  Never call ColorConvertFloat4ToU32 inline at draw time.                    |
// +-----------------------------------------------------------------------------+

inline ImU32 BG_U32                      = 0;
inline ImU32 BG_SOFT_U32                 = 0;
inline ImU32 PANEL_U32                   = 0;
inline ImU32 PANEL_2_U32                 = 0;
inline ImU32 LINE_U32                    = 0;
inline ImU32 INK_U32                     = 0;
inline ImU32 INK_2_U32                   = 0;
inline ImU32 INK_3_U32                   = 0;
inline ImU32 MUTED_U32                   = 0;
inline ImU32 HEADER_U32                  = 0;
inline ImU32 HEADER_2_U32                = 0;
inline ImU32 HEADER_LINE_U32             = 0;
inline ImU32 ACCENT_PRIMARY_U32          = 0;
inline ImU32 ACCENT_PRIMARY_SUBTLE_U32   = 0;
inline ImU32 ACCENT_SECONDARY_U32        = 0;
inline ImU32 ACCENT_SECONDARY_SUBTLE_U32 = 0;
inline ImU32 ACCENT_FOCUS_U32            = 0;
inline ImU32 ACCENT_FOCUS_SUBTLE_U32     = 0;

// +-----------------------------------------------------------------------------+
// |  DPI scale                                                                  |
// +-----------------------------------------------------------------------------+

// Set once at startup from SDL_GetDisplayContentScale() or user preference.
// ALL sizing constants below are canonical 1x reference values.
// Every usage site must multiply: e.g.  NODE_WIDTH * TS::ui_scale
// ImGui is not a fluid-layout system -- "responsive" means uniform DPI scale,
// not CSS-style reflow.
inline float ui_scale = 1.0f;

// +-----------------------------------------------------------------------------+
// |  Sizing constants                                                           |
// +-----------------------------------------------------------------------------+

// Node geometry
inline constexpr float NODE_WIDTH      = 220.0f;
inline constexpr float NODE_HEADER_H   =  52.0f;
inline constexpr float NODE_METHOD_ROW =  22.0f;
inline constexpr float NODE_ROUNDING   =  10.0f;
inline constexpr float NODE_PADDING_X  =  12.0f;

// Font sizes (px at 1x)
inline constexpr float FONT_SIZE_BASE  = 13.0f;  // Inter Regular  -- body, sidebar
inline constexpr float FONT_SIZE_SMALL = 11.5f;  // Inter Regular  -- subtitles, badges
inline constexpr float FONT_SIZE_MONO  = 11.5f;  // JetBrains Mono -- method signatures

// Layout
inline constexpr float SIDEBAR_W  = 280.0f;
inline constexpr float HEADER_H   =  56.0f;
inline constexpr float SEQUENCE_H = 180.0f;

// Zoom
inline constexpr float ZOOM_MIN         = 0.40f;
inline constexpr float ZOOM_MAX         = 2.00f;
inline constexpr float ZOOM_STEP_SCROLL = 0.05f;
inline constexpr float ZOOM_STEP_BTN    = 0.10f;

// +-----------------------------------------------------------------------------+
// |  Font pointers                                                              |
// |  All nullptr until LoadFonts() returns.                                     |
// |  SEQUENCE: LoadFonts() -> (font atlas built on first NewFrame) -> render    |
// |  Never call ImGui::PushFont(TS::FONT_*) before LoadFonts() has been called. |
// +-----------------------------------------------------------------------------+

inline ImFont* FONT_BASE   = nullptr; // Inter Regular  13px   -- body, sidebar
inline ImFont* FONT_MEDIUM = nullptr; // Inter Medium   13px   -- node titles
inline ImFont* FONT_SMALL  = nullptr; // Inter Regular  11.5px -- subtitles, badges
inline ImFont* FONT_MONO   = nullptr; // JetBrains Mono 11.5px -- method signatures

// +-----------------------------------------------------------------------------+
// |  LoadFonts                                                                  |
// +-----------------------------------------------------------------------------+

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

    FONT_BASE   = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf",         base_sz);
    FONT_MEDIUM = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Medium.ttf",          base_sz);
    FONT_SMALL  = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf",         small_sz);
    FONT_MONO   = io.Fonts->AddFontFromFileTTF("assets/fonts/JetBrainsMono-Regular.ttf", mono_sz);

    // Graceful fallback: use ImGui's embedded ProggyClean if any file is absent.
    if (!FONT_BASE)   FONT_BASE   = io.Fonts->AddFontDefault();
    if (!FONT_MEDIUM) FONT_MEDIUM = io.Fonts->AddFontDefault();
    if (!FONT_SMALL)  FONT_SMALL  = io.Fonts->AddFontDefault();
    if (!FONT_MONO)   FONT_MONO   = io.Fonts->AddFontDefault();
}

// +-----------------------------------------------------------------------------+
// |  ColorToken -- enum for ApplyToken()                                        |
// +-----------------------------------------------------------------------------+

enum class ColorToken {
    BG, BG_SOFT, PANEL, PANEL_2, LINE,
    INK, INK_2, INK_3, MUTED,
    HEADER, HEADER_2, HEADER_LINE,
    ACCENT_PRIMARY, ACCENT_PRIMARY_SUBTLE,
    ACCENT_SECONDARY, ACCENT_SECONDARY_SUBTLE,
    ACCENT_FOCUS, ACCENT_FOCUS_SUBTLE,
};

// +-----------------------------------------------------------------------------+
// |  ApplyToken                                                                 |
// +-----------------------------------------------------------------------------+

// The single write path for all color changes.
//
// Updates the runtime ImVec4, its paired _U32, and every dependent slot in
// both ImGuiStyle and ImNodesStyle. Darken/WithAlpha variants are re-derived
// automatically so interactive states always track the base token.
//
// Called by ApplyStyle() during initial setup.
// Called by the live tuning overlay (style_preview.h) on each edit.
// Never write ImGuiStyle or ImNodesStyle color slots directly.
inline void ApplyToken(ColorToken token, ImVec4 v)
{
    ImGuiStyle&    s  = ImGui::GetStyle();
    ImVec4*        c  = s.Colors;
    ImNodesStyle&  ns = ImNodes::GetStyle();
    unsigned int*  nc = ns.Colors;

    switch (token)
    {
    case ColorToken::BG:
        BG      = v;
        BG_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_WindowBg] = v;
        break;

    case ColorToken::BG_SOFT:
        BG_SOFT      = v;
        BG_SOFT_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_ChildBg]        = v;
        c[ImGuiCol_ScrollbarBg]    = v;
        c[ImGuiCol_TabDimmed]      = v;
        c[ImGuiCol_TableRowBgAlt]  = WithAlpha(v, 0.50f);
        nc[ImNodesCol_GridBackground] = BG_SOFT_U32;
        nc[ImNodesCol_MiniMapCanvas]  = BG_SOFT_U32;
        break;

    case ColorToken::PANEL:
        PANEL      = v;
        PANEL_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_PopupBg]    = v;
        c[ImGuiCol_TabSelected]= v;
        c[ImGuiCol_TableRowBg] = WithAlpha(v, 0.0f);
        nc[ImNodesCol_NodeBackground]            = PANEL_U32;
        nc[ImNodesCol_NodeBackgroundHovered]     = ImGui::ColorConvertFloat4ToU32(Darken(v, 0.03f));
        nc[ImNodesCol_MiniMapBackground]         = ImGui::ColorConvertFloat4ToU32(WithAlpha(v, 0.80f));
        nc[ImNodesCol_MiniMapBackgroundHovered]  = ImGui::ColorConvertFloat4ToU32(WithAlpha(v, 0.95f));
        break;

    case ColorToken::PANEL_2:
        PANEL_2      = v;
        PANEL_2_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_FrameBg]        = v;
        c[ImGuiCol_FrameBgHovered] = Darken(v, 0.04f);
        c[ImGuiCol_FrameBgActive]  = Darken(v, 0.08f);
        c[ImGuiCol_Button]         = v;
        c[ImGuiCol_TableHeaderBg]  = v;
        c[ImGuiCol_Tab]            = v;
        c[ImGuiCol_TabDimmedSelected] = v;
        nc[ImNodesCol_TitleBar]                     = PANEL_2_U32;
        nc[ImNodesCol_MiniMapNodeBackground]        = PANEL_2_U32;
        nc[ImNodesCol_MiniMapNodeBackgroundHovered] = ImGui::ColorConvertFloat4ToU32(Darken(v, 0.06f));
        break;

    case ColorToken::LINE:
        LINE      = v;
        LINE_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_Border]            = v;
        c[ImGuiCol_ScrollbarGrab]     = v;
        c[ImGuiCol_Separator]         = v;
        c[ImGuiCol_TableBorderStrong] = v;
        c[ImGuiCol_TableBorderLight]  = WithAlpha(v, 0.50f);
        nc[ImNodesCol_NodeOutline]          = LINE_U32;
        nc[ImNodesCol_GridLine]             = LINE_U32;
        nc[ImNodesCol_MiniMapOutline]       = LINE_U32;
        nc[ImNodesCol_MiniMapNodeOutline]   = LINE_U32;
        nc[ImNodesCol_MiniMapCanvasOutline] = LINE_U32;
        break;

    case ColorToken::INK:
        INK      = v;
        INK_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_Text]                = v;
        c[ImGuiCol_BorderShadow]        = WithAlpha(v, 0.0f);
        c[ImGuiCol_NavWindowingDimBg]   = WithAlpha(v, 0.20f);
        c[ImGuiCol_ModalWindowDimBg]    = WithAlpha(v, 0.35f);
        break;

    case ColorToken::INK_2:
        INK_2      = v;
        INK_2_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_ScrollbarGrabActive] = v;
        break;

    case ColorToken::INK_3:
        INK_3      = v;
        INK_3_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_ScrollbarGrabHovered]    = v;
        nc[ImNodesCol_MiniMapOutlineHovered]= INK_3_U32;
        break;

    case ColorToken::MUTED:
        MUTED      = v;
        MUTED_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_TextDisabled] = v;
        break;

    case ColorToken::HEADER:
        HEADER      = v;
        HEADER_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_TitleBg]          = v;
        c[ImGuiCol_TitleBgCollapsed] = WithAlpha(v, 0.75f);
        c[ImGuiCol_MenuBarBg]        = v;
        break;

    case ColorToken::HEADER_2:
        HEADER_2      = v;
        HEADER_2_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_TitleBgActive] = v;
        break;

    case ColorToken::HEADER_LINE:
        HEADER_LINE      = v;
        HEADER_LINE_U32  = ImGui::ColorConvertFloat4ToU32(v);
        // Used by rendering code for the header bar border; no ImGuiStyle slot.
        break;

    case ColorToken::ACCENT_PRIMARY:
        ACCENT_PRIMARY      = v;
        ACCENT_PRIMARY_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_CheckMark]                 = v;
        c[ImGuiCol_SliderGrab]                = v;
        c[ImGuiCol_SliderGrabActive]          = Darken(v, 0.10f);
        c[ImGuiCol_SeparatorHovered]          = v;
        c[ImGuiCol_SeparatorActive]           = Darken(v, 0.10f);
        c[ImGuiCol_ResizeGrip]                = WithAlpha(v, 0.20f);
        c[ImGuiCol_ResizeGripHovered]         = WithAlpha(v, 0.50f);
        c[ImGuiCol_ResizeGripActive]          = v;
        c[ImGuiCol_TabSelectedOverline]       = v;
        c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(v, 0.40f);
        c[ImGuiCol_PlotLines]                 = v;
        c[ImGuiCol_TextLink]                  = v;
        c[ImGuiCol_TextSelectedBg]            = WithAlpha(v, 0.25f);
        c[ImGuiCol_NavCursor]                 = v;
        c[ImGuiCol_NavWindowingHighlight]     = WithAlpha(v, 0.70f);
        nc[ImNodesCol_Link]                = ACCENT_PRIMARY_U32;
        nc[ImNodesCol_LinkHovered]         = ImGui::ColorConvertFloat4ToU32(Darken(v, 0.08f));
        nc[ImNodesCol_LinkSelected]        = ImGui::ColorConvertFloat4ToU32(Darken(v, 0.15f));
        nc[ImNodesCol_BoxSelector]         = ImGui::ColorConvertFloat4ToU32(WithAlpha(v, 0.10f));
        nc[ImNodesCol_BoxSelectorOutline]  = ACCENT_PRIMARY_U32;
        nc[ImNodesCol_MiniMapLink]         = ACCENT_PRIMARY_U32;
        nc[ImNodesCol_MiniMapLinkSelected] = ImGui::ColorConvertFloat4ToU32(Darken(v, 0.15f));
        break;

    case ColorToken::ACCENT_PRIMARY_SUBTLE:
        ACCENT_PRIMARY_SUBTLE      = v;
        ACCENT_PRIMARY_SUBTLE_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_TabHovered]     = v;
        c[ImGuiCol_ButtonHovered]  = v;
        c[ImGuiCol_ButtonActive]   = Darken(v, 0.08f);
        c[ImGuiCol_Header]         = v;
        c[ImGuiCol_HeaderHovered]  = Darken(v, 0.04f);
        c[ImGuiCol_HeaderActive]   = Darken(v, 0.10f);
        nc[ImNodesCol_NodeBackgroundSelected]       = ACCENT_PRIMARY_SUBTLE_U32;
        nc[ImNodesCol_TitleBarHovered]              = ACCENT_PRIMARY_SUBTLE_U32;
        nc[ImNodesCol_TitleBarSelected]             = ACCENT_PRIMARY_SUBTLE_U32;
        nc[ImNodesCol_MiniMapNodeBackgroundSelected]= ACCENT_PRIMARY_SUBTLE_U32;
        break;

    case ColorToken::ACCENT_SECONDARY:
        ACCENT_SECONDARY      = v;
        ACCENT_SECONDARY_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_PlotLinesHovered]  = v;
        nc[ImNodesCol_Pin]        = ACCENT_SECONDARY_U32;
        nc[ImNodesCol_PinHovered] = ImGui::ColorConvertFloat4ToU32(Darken(v, 0.08f));
        break;

    case ColorToken::ACCENT_SECONDARY_SUBTLE:
        ACCENT_SECONDARY_SUBTLE      = v;
        ACCENT_SECONDARY_SUBTLE_U32  = ImGui::ColorConvertFloat4ToU32(v);
        // Used by rendering code for callee node backgrounds; no ImGuiStyle slot.
        break;

    case ColorToken::ACCENT_FOCUS:
        ACCENT_FOCUS      = v;
        ACCENT_FOCUS_U32  = ImGui::ColorConvertFloat4ToU32(v);
        c[ImGuiCol_PlotHistogram]        = v;
        c[ImGuiCol_PlotHistogramHovered] = Darken(v, 0.10f);
        c[ImGuiCol_DragDropTarget]       = v;
        break;

    case ColorToken::ACCENT_FOCUS_SUBTLE:
        ACCENT_FOCUS_SUBTLE      = v;
        ACCENT_FOCUS_SUBTLE_U32  = ImGui::ColorConvertFloat4ToU32(v);
        // Used by rendering code for focused node backgrounds; no ImGuiStyle slot.
        break;
    }
}

// +-----------------------------------------------------------------------------+
// |  ApplyStyle                                                                 |
// +-----------------------------------------------------------------------------+

// Called ONCE at startup after ImGui::CreateContext() AND ImNodes::CreateContext().
// Sets ImGuiStyle geometry, ImNodesStyle geometry, then applies all color tokens
// through ApplyToken() so both style systems are always in sync.
inline void ApplyStyle()
{
    // -- ImGuiStyle -- geometry ------------------------------------------------
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 10.0f;
    s.ChildRounding     =  6.0f;
    s.FrameRounding     =  6.0f;
    s.PopupRounding     =  8.0f;
    s.ScrollbarRounding =  6.0f;
    s.GrabRounding      =  6.0f;
    s.TabRounding       =  6.0f;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;

    s.WindowPadding    = ImVec2(12.0f, 10.0f);
    s.FramePadding     = ImVec2( 8.0f,  4.0f);
    s.ItemSpacing      = ImVec2( 8.0f,  6.0f);
    s.ItemInnerSpacing = ImVec2( 6.0f,  4.0f);
    s.IndentSpacing    = 16.0f;
    s.ScrollbarSize    = 12.0f;
    s.GrabMinSize      =  8.0f;

    // -- ImNodesStyle -- geometry ----------------------------------------------
    ImNodesStyle& ns = ImNodes::GetStyle();

    ns.GridSpacing               = 24.0f;
    ns.NodeCornerRounding        = NODE_ROUNDING;
    ns.NodePadding               = ImVec2(NODE_PADDING_X, 6.0f);
    ns.NodeBorderThickness       = 1.0f;
    ns.LinkThickness             = 2.0f;
    ns.LinkLineSegmentsPerLength = 0.1f;
    ns.LinkHoverDistance         = 10.0f;
    ns.PinCircleRadius           = 4.0f;
    ns.PinQuadSideLength         = 7.0f;
    ns.PinTriangleSideLength     = 9.5f;
    ns.PinLineThickness          = 1.5f;
    ns.PinHoverRadius            = 10.0f;
    ns.PinOffset                 = 0.0f;
    ns.Flags = ImNodesStyleFlags_NodeOutline | ImNodesStyleFlags_GridLines;

    // -- Colors -- via ApplyToken (single write path) --------------------------
    ApplyToken(ColorToken::BG,                      BG);
    ApplyToken(ColorToken::BG_SOFT,                 BG_SOFT);
    ApplyToken(ColorToken::PANEL,                   PANEL);
    ApplyToken(ColorToken::PANEL_2,                 PANEL_2);
    ApplyToken(ColorToken::LINE,                    LINE);
    ApplyToken(ColorToken::INK,                     INK);
    ApplyToken(ColorToken::INK_2,                   INK_2);
    ApplyToken(ColorToken::INK_3,                   INK_3);
    ApplyToken(ColorToken::MUTED,                   MUTED);
    ApplyToken(ColorToken::HEADER,                  HEADER);
    ApplyToken(ColorToken::HEADER_2,                HEADER_2);
    ApplyToken(ColorToken::HEADER_LINE,             HEADER_LINE);
    ApplyToken(ColorToken::ACCENT_PRIMARY,          ACCENT_PRIMARY);
    ApplyToken(ColorToken::ACCENT_PRIMARY_SUBTLE,   ACCENT_PRIMARY_SUBTLE);
    ApplyToken(ColorToken::ACCENT_SECONDARY,        ACCENT_SECONDARY);
    ApplyToken(ColorToken::ACCENT_SECONDARY_SUBTLE, ACCENT_SECONDARY_SUBTLE);
    ApplyToken(ColorToken::ACCENT_FOCUS,            ACCENT_FOCUS);
    ApplyToken(ColorToken::ACCENT_FOCUS_SUBTLE,     ACCENT_FOCUS_SUBTLE);
}

} // namespace TS
