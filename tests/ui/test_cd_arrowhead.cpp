// TDD: tests written before cd_arrowhead.cpp implementation.
// Only pure-math functions are tested here — no ImGui/imnodes context required.
//
// Suites:
//   ArrowVertices    — CDArrowVertices triangle geometry
//   PinScreenPos     — CDPinInputScreenPos / CDPinOutputScreenPos
//   ArrowheadSize    — CDScaleArrowhead zoom scaling

#include "ui/class_diagram/cd_arrowhead.h"
#include <imgui.h>
#include <gtest/gtest.h>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers — replicate the pure formulas so tests are self-documenting.
// ─────────────────────────────────────────────────────────────────────────────

static TS::CDArrowTri ref_arrow_vertices(ImVec2 tip, ImVec2 dir, float hb, float len)
{
    // base center: length units behind the tip along -dir
    ImVec2 base = { tip.x - dir.x * len, tip.y - dir.y * len };
    // perpendicular (90° CCW): {-dir.y, dir.x}
    ImVec2 perp = { -dir.y, dir.x };
    return {
        tip,
        { base.x + perp.x * hb, base.y + perp.y * hb },
        { base.x - perp.x * hb, base.y - perp.y * hb },
    };
}

static ImVec2 ref_input_pin(ImVec2 tl, ImVec2 sz)
{
    return { tl.x, tl.y + sz.y * 0.5f };
}

static ImVec2 ref_output_pin(ImVec2 tl, ImVec2 sz)
{
    return { tl.x + sz.x, tl.y + sz.y * 0.5f };
}

// ─────────────────────────────────────────────────────────────────────────────
// ArrowVertices
// ─────────────────────────────────────────────────────────────────────────────

TEST(ArrowVertices, tip_equals_given_position)
{
    ImVec2 tip = { 42.0f, 17.0f };
    auto r = TS::CDArrowVertices(tip, { 1.0f, 0.0f }, 5.0f, 10.0f);
    EXPECT_NEAR(r.tip.x, tip.x, 1e-4f);
    EXPECT_NEAR(r.tip.y, tip.y, 1e-4f);
}

TEST(ArrowVertices, rightward_base_behind_tip)
{
    // dir=(1,0), len=10, tip.x=10 → base.x == 0
    auto r = TS::CDArrowVertices({ 10.0f, 0.0f }, { 1.0f, 0.0f }, 5.0f, 10.0f);
    auto e = ref_arrow_vertices({ 10.0f, 0.0f }, { 1.0f, 0.0f }, 5.0f, 10.0f);
    EXPECT_NEAR(r.v1.x, e.v1.x, 1e-4f);
    EXPECT_NEAR(r.v2.x, e.v2.x, 1e-4f);
}

TEST(ArrowVertices, rightward_base_symmetric_about_axis)
{
    // dir=(1,0): perp=(0,1), v1.y=+hb, v2.y=-hb when tip.y=0
    auto r = TS::CDArrowVertices({ 10.0f, 0.0f }, { 1.0f, 0.0f }, 5.0f, 10.0f);
    EXPECT_NEAR(r.v1.y,  5.0f, 1e-4f);
    EXPECT_NEAR(r.v2.y, -5.0f, 1e-4f);
}

TEST(ArrowVertices, leftward_base_in_front_of_tip)
{
    // dir=(-1,0), len=10, tip.x=0 → base.x == 10
    auto r = TS::CDArrowVertices({ 0.0f, 0.0f }, { -1.0f, 0.0f }, 5.0f, 10.0f);
    auto e = ref_arrow_vertices({ 0.0f, 0.0f }, { -1.0f, 0.0f }, 5.0f, 10.0f);
    EXPECT_NEAR(r.v1.x, e.v1.x, 1e-4f);
    EXPECT_NEAR(r.v2.x, e.v2.x, 1e-4f);
}

TEST(ArrowVertices, upward_arrow_base_below_tip)
{
    // dir=(0,-1), len=10, tip.y=0 → base.y == 10
    auto r = TS::CDArrowVertices({ 0.0f, 0.0f }, { 0.0f, -1.0f }, 5.0f, 10.0f);
    auto e = ref_arrow_vertices({ 0.0f, 0.0f }, { 0.0f, -1.0f }, 5.0f, 10.0f);
    EXPECT_NEAR(r.v1.y, e.v1.y, 1e-4f);
    EXPECT_NEAR(r.v2.y, e.v2.y, 1e-4f);
}

TEST(ArrowVertices, half_base_zero_collapses_to_line)
{
    // half_base=0 → v1 == v2
    auto r = TS::CDArrowVertices({ 10.0f, 5.0f }, { 1.0f, 0.0f }, 0.0f, 10.0f);
    EXPECT_NEAR(r.v1.x, r.v2.x, 1e-4f);
    EXPECT_NEAR(r.v1.y, r.v2.y, 1e-4f);
}

TEST(ArrowVertices, double_half_base_doubles_base_width)
{
    ImVec2 tip = { 10.0f, 0.0f };
    ImVec2 dir = { 1.0f, 0.0f };
    auto r5  = TS::CDArrowVertices(tip, dir, 5.0f, 10.0f);
    auto r10 = TS::CDArrowVertices(tip, dir, 10.0f, 10.0f);
    float width5  = std::fabs(r5.v1.y  - r5.v2.y);
    float width10 = std::fabs(r10.v1.y - r10.v2.y);
    EXPECT_NEAR(width10, 2.0f * width5, 1e-4f);
}

TEST(ArrowVertices, double_length_doubles_depth)
{
    ImVec2 tip = { 10.0f, 0.0f };
    ImVec2 dir = { 1.0f, 0.0f };
    auto r5  = TS::CDArrowVertices(tip, dir, 5.0f, 5.0f);
    auto r10 = TS::CDArrowVertices(tip, dir, 5.0f, 10.0f);
    float depth5  = tip.x - r5.v1.x;
    float depth10 = tip.x - r10.v1.x;
    EXPECT_NEAR(depth10, 2.0f * depth5, 1e-4f);
}

TEST(ArrowVertices, v1_v2_equidistant_from_centerline)
{
    // For dir=(1,0): centerline is y=tip.y; perpendicular distance = |v.y - tip.y|
    ImVec2 tip = { 10.0f, 3.0f };
    auto r = TS::CDArrowVertices(tip, { 1.0f, 0.0f }, 7.0f, 10.0f);
    float d1 = std::fabs(r.v1.y - tip.y);
    float d2 = std::fabs(r.v2.y - tip.y);
    EXPECT_NEAR(d1, d2, 1e-4f);
    EXPECT_NEAR(d1, 7.0f, 1e-4f);
}

TEST(ArrowVertices, matches_reference_formula_diagonal)
{
    // 45° direction: dir=(√2/2, √2/2)
    const float s = 0.70710678f;
    ImVec2 tip = { 5.0f, 5.0f };
    ImVec2 dir = { s, s };
    auto r = TS::CDArrowVertices(tip, dir, 4.0f, 8.0f);
    auto e = ref_arrow_vertices(tip, dir, 4.0f, 8.0f);
    EXPECT_NEAR(r.tip.x, e.tip.x, 1e-3f);
    EXPECT_NEAR(r.tip.y, e.tip.y, 1e-3f);
    EXPECT_NEAR(r.v1.x,  e.v1.x,  1e-3f);
    EXPECT_NEAR(r.v1.y,  e.v1.y,  1e-3f);
    EXPECT_NEAR(r.v2.x,  e.v2.x,  1e-3f);
    EXPECT_NEAR(r.v2.y,  e.v2.y,  1e-3f);
}

// ─────────────────────────────────────────────────────────────────────────────
// PinScreenPos
// ─────────────────────────────────────────────────────────────────────────────

TEST(PinScreenPos, input_pin_x_at_left_edge)
{
    ImVec2 tl = { 100.0f, 200.0f };
    ImVec2 sz = { 160.0f, 80.0f };
    auto p = TS::CDPinInputScreenPos(tl, sz);
    EXPECT_NEAR(p.x, ref_input_pin(tl, sz).x, 1e-4f);
}

TEST(PinScreenPos, input_pin_y_at_vertical_center)
{
    ImVec2 tl = { 100.0f, 200.0f };
    ImVec2 sz = { 160.0f, 80.0f };
    auto p = TS::CDPinInputScreenPos(tl, sz);
    EXPECT_NEAR(p.y, 240.0f, 1e-4f);
}

TEST(PinScreenPos, input_pin_offset_node)
{
    ImVec2 tl = { 50.0f, 75.0f };
    ImVec2 sz = { 200.0f, 100.0f };
    auto p = TS::CDPinInputScreenPos(tl, sz);
    EXPECT_NEAR(p.x, 50.0f,  1e-4f);
    EXPECT_NEAR(p.y, 125.0f, 1e-4f);
}

TEST(PinScreenPos, output_pin_x_at_right_edge)
{
    ImVec2 tl = { 100.0f, 200.0f };
    ImVec2 sz = { 160.0f, 80.0f };
    auto p = TS::CDPinOutputScreenPos(tl, sz);
    EXPECT_NEAR(p.x, ref_output_pin(tl, sz).x, 1e-4f);
}

TEST(PinScreenPos, output_pin_y_at_vertical_center)
{
    ImVec2 tl = { 100.0f, 200.0f };
    ImVec2 sz = { 160.0f, 80.0f };
    auto p = TS::CDPinOutputScreenPos(tl, sz);
    EXPECT_NEAR(p.y, 240.0f, 1e-4f);
}

TEST(PinScreenPos, output_pin_offset_node)
{
    ImVec2 tl = { 50.0f, 75.0f };
    ImVec2 sz = { 200.0f, 100.0f };
    auto p = TS::CDPinOutputScreenPos(tl, sz);
    EXPECT_NEAR(p.x, 250.0f, 1e-4f);
    EXPECT_NEAR(p.y, 125.0f, 1e-4f);
}

TEST(PinScreenPos, input_and_output_same_y)
{
    ImVec2 tl = { 30.0f, 90.0f };
    ImVec2 sz = { 180.0f, 120.0f };
    auto in_p  = TS::CDPinInputScreenPos(tl, sz);
    auto out_p = TS::CDPinOutputScreenPos(tl, sz);
    EXPECT_NEAR(in_p.y, out_p.y, 1e-4f);
}

TEST(PinScreenPos, zero_size_node_does_not_crash)
{
    EXPECT_NO_THROW({
        TS::CDPinInputScreenPos({ 0.0f, 0.0f }, { 0.0f, 0.0f });
        TS::CDPinOutputScreenPos({ 0.0f, 0.0f }, { 0.0f, 0.0f });
    });
}

TEST(PinScreenPos, zero_size_input_pin_at_node_tl_y)
{
    ImVec2 tl = { 10.0f, 20.0f };
    auto p = TS::CDPinInputScreenPos(tl, { 0.0f, 0.0f });
    EXPECT_NEAR(p.y, tl.y, 1e-4f);
}

// ─────────────────────────────────────────────────────────────────────────────
// ArrowheadSize
// ─────────────────────────────────────────────────────────────────────────────

TEST(ArrowheadSize, zoom_one_returns_base_constants)
{
    auto d = TS::CDScaleArrowhead(1.0f);
    EXPECT_NEAR(d.half_base, TS::k_arrow_half_base, 1e-4f);
    EXPECT_NEAR(d.length,    TS::k_arrow_length,    1e-4f);
}

TEST(ArrowheadSize, zoom_two_scales_half_base)
{
    auto d = TS::CDScaleArrowhead(2.0f);
    EXPECT_NEAR(d.half_base, 2.0f * TS::k_arrow_half_base, 1e-4f);
}

TEST(ArrowheadSize, zoom_two_scales_length)
{
    auto d = TS::CDScaleArrowhead(2.0f);
    EXPECT_NEAR(d.length, 2.0f * TS::k_arrow_length, 1e-4f);
}

TEST(ArrowheadSize, zoom_half_halves_half_base)
{
    auto d = TS::CDScaleArrowhead(0.5f);
    EXPECT_NEAR(d.half_base, 0.5f * TS::k_arrow_half_base, 1e-4f);
}

TEST(ArrowheadSize, zoom_half_halves_length)
{
    auto d = TS::CDScaleArrowhead(0.5f);
    EXPECT_NEAR(d.length, 0.5f * TS::k_arrow_length, 1e-4f);
}

TEST(ArrowheadSize, zero_zoom_gives_zero_size)
{
    auto d = TS::CDScaleArrowhead(0.0f);
    EXPECT_NEAR(d.half_base, 0.0f, 1e-4f);
    EXPECT_NEAR(d.length,    0.0f, 1e-4f);
}

TEST(ArrowheadSize, proportional_to_zoom)
{
    auto d1 = TS::CDScaleArrowhead(1.0f);
    auto d3 = TS::CDScaleArrowhead(3.0f);
    EXPECT_NEAR(d3.half_base, 3.0f * d1.half_base, 1e-4f);
    EXPECT_NEAR(d3.length,    3.0f * d1.length,    1e-4f);
}
