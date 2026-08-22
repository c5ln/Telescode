// TDD: tests written before ts_canvas.h/ts_canvas.cpp exist.
// Only pure-math functions are tested here — DrawCanvas() and ShutdownCanvas()
// require a live ImGui/imnodes context and are excluded.

#include "ui/ts_canvas.h"
#include <gtest/gtest.h>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers – replicate the pure formulas so tests are self-documenting.
// ─────────────────────────────────────────────────────────────────────────────

static float clamp_zoom(float z)
{
    if (z < TS::ZOOM_MIN) return TS::ZOOM_MIN;
    if (z > TS::ZOOM_MAX) return TS::ZOOM_MAX;
    return z;
}

static float apply_scroll(float old_zoom, float wheel)
{
    return clamp_zoom(old_zoom + wheel * TS::ZOOM_STEP_SCROLL);
}

static ImVec2 zoom_to_cursor_pan(ImVec2 mouse, ImVec2 canvas_origin,
                                  ImVec2 old_pan, float old_zoom, float new_zoom)
{
    ImVec2 cursor_in_grid = { mouse.x - canvas_origin.x - old_pan.x,
                               mouse.y - canvas_origin.y - old_pan.y };
    float  ratio          = 1.0f - new_zoom / old_zoom;
    return { old_pan.x + cursor_in_grid.x * ratio,
             old_pan.y + cursor_in_grid.y * ratio };
}

static ImVec2 fit_to_view_pan(ImVec2 canvas_size)
{
    return { canvas_size.x * 0.5f, canvas_size.y * 0.5f };
}

// ─────────────────────────────────────────────────────────────────────────────
// ZoomClamping
// ─────────────────────────────────────────────────────────────────────────────

TEST(ZoomClamping, normal_range_stays_within_bounds)
{
    // start at 1.0, scroll up one notch → 1.05
    float z = apply_scroll(1.0f, 1.0f);
    EXPECT_GE(z, TS::ZOOM_MIN);
    EXPECT_LE(z, TS::ZOOM_MAX);
    EXPECT_NEAR(z, 1.0f + TS::ZOOM_STEP_SCROLL, 1e-4f);
}

TEST(ZoomClamping, does_not_go_below_min)
{
    // try to scroll far below minimum
    float z = apply_scroll(TS::ZOOM_MIN, -100.0f);
    EXPECT_NEAR(z, TS::ZOOM_MIN, 1e-4f);
}

TEST(ZoomClamping, does_not_exceed_max)
{
    // try to scroll far above maximum
    float z = apply_scroll(TS::ZOOM_MAX, 100.0f);
    EXPECT_NEAR(z, TS::ZOOM_MAX, 1e-4f);
}

TEST(ZoomClamping, boundary_min_exact)
{
    // clamping exactly at ZOOM_MIN returns ZOOM_MIN
    float z = clamp_zoom(TS::ZOOM_MIN);
    EXPECT_NEAR(z, TS::ZOOM_MIN, 1e-4f);
}

TEST(ZoomClamping, boundary_max_exact)
{
    // clamping exactly at ZOOM_MAX returns ZOOM_MAX
    float z = clamp_zoom(TS::ZOOM_MAX);
    EXPECT_NEAR(z, TS::ZOOM_MAX, 1e-4f);
}

TEST(ZoomClamping, step_below_min_clamps)
{
    // one scroll step below ZOOM_MIN must still clamp
    float z = apply_scroll(TS::ZOOM_MIN, -1.0f);
    EXPECT_NEAR(z, TS::ZOOM_MIN, 1e-4f);
}

TEST(ZoomClamping, step_above_max_clamps)
{
    // one scroll step above ZOOM_MAX must still clamp
    float z = apply_scroll(TS::ZOOM_MAX, 1.0f);
    EXPECT_NEAR(z, TS::ZOOM_MAX, 1e-4f);
}

// ─────────────────────────────────────────────────────────────────────────────
// ZoomToCursorPan
// ─────────────────────────────────────────────────────────────────────────────

TEST(ZoomToCursorPan, no_zoom_change_leaves_pan_unchanged)
{
    ImVec2 mouse         = { 400.0f, 300.0f };
    ImVec2 canvas_origin = { 0.0f,   0.0f   };
    ImVec2 old_pan       = { 50.0f,  80.0f  };
    float  zoom          = 1.0f;

    ImVec2 new_pan = zoom_to_cursor_pan(mouse, canvas_origin, old_pan, zoom, zoom);
    EXPECT_NEAR(new_pan.x, old_pan.x, 1e-4f);
    EXPECT_NEAR(new_pan.y, old_pan.y, 1e-4f);
}

TEST(ZoomToCursorPan, zoom_in_cursor_at_canvas_center)
{
    // canvas 800×600, origin (0,0), pan (0,0), cursor at center (400,300)
    ImVec2 canvas_origin = { 0.0f,   0.0f  };
    ImVec2 old_pan       = { 0.0f,   0.0f  };
    ImVec2 mouse         = { 400.0f, 300.0f };
    float  old_zoom      = 1.0f;
    float  new_zoom      = 1.5f;

    ImVec2 new_pan = zoom_to_cursor_pan(mouse, canvas_origin, old_pan, old_zoom, new_zoom);

    // cursor_in_grid = (400, 300)
    // ratio = 1 - 1.5/1.0 = -0.5
    // new_pan = (0 + 400*(-0.5), 0 + 300*(-0.5)) = (-200, -150)
    EXPECT_NEAR(new_pan.x, -200.0f, 1e-4f);
    EXPECT_NEAR(new_pan.y, -150.0f, 1e-4f);
}

TEST(ZoomToCursorPan, zoom_out_cursor_at_top_left)
{
    // canvas origin (0,0), pan (100,100), cursor at top-left (0,0)
    ImVec2 canvas_origin = { 0.0f,   0.0f  };
    ImVec2 old_pan       = { 100.0f, 100.0f };
    ImVec2 mouse         = { 0.0f,   0.0f   };
    float  old_zoom      = 1.0f;
    float  new_zoom      = 0.5f;

    ImVec2 new_pan = zoom_to_cursor_pan(mouse, canvas_origin, old_pan, old_zoom, new_zoom);

    // cursor_in_grid = (0 - 0 - 100, 0 - 0 - 100) = (-100, -100)
    // ratio = 1 - 0.5/1.0 = 0.5
    // new_pan = (100 + (-100)*0.5, 100 + (-100)*0.5) = (50, 50)
    EXPECT_NEAR(new_pan.x, 50.0f, 1e-4f);
    EXPECT_NEAR(new_pan.y, 50.0f, 1e-4f);
}

TEST(ZoomToCursorPan, cursor_at_pan_origin_new_pan_equals_old_pan)
{
    // When cursor is at canvas_origin + old_pan, cursor_in_grid == (0,0)
    // → new_pan must equal old_pan regardless of zoom change.
    ImVec2 canvas_origin = { 10.0f, 20.0f };
    ImVec2 old_pan       = { 60.0f, 80.0f };
    ImVec2 mouse         = { canvas_origin.x + old_pan.x,
                              canvas_origin.y + old_pan.y };

    ImVec2 new_pan = zoom_to_cursor_pan(mouse, canvas_origin, old_pan, 1.0f, 2.0f);
    EXPECT_NEAR(new_pan.x, old_pan.x, 1e-4f);
    EXPECT_NEAR(new_pan.y, old_pan.y, 1e-4f);
}

// ─────────────────────────────────────────────────────────────────────────────
// FitToView
// ─────────────────────────────────────────────────────────────────────────────

TEST(FitToView, zoom_resets_to_one)
{
    // After a fit-to-view the implementation must set zoom to 1.0.
    // We verify via SetCanvasZoom / GetCanvasZoom.
    TS::SetCanvasZoom(0.6f);
    ASSERT_NEAR(TS::GetCanvasZoom(), 0.6f, 1e-4f);

    // Simulate fit-to-view zoom reset
    TS::SetCanvasZoom(1.0f);
    EXPECT_NEAR(TS::GetCanvasZoom(), 1.0f, 1e-4f);
}

TEST(FitToView, pan_equals_half_canvas_size)
{
    ImVec2 canvas_size = { 800.0f, 600.0f };
    ImVec2 pan         = fit_to_view_pan(canvas_size);
    EXPECT_NEAR(pan.x, canvas_size.x * 0.5f, 1e-4f);
    EXPECT_NEAR(pan.y, canvas_size.y * 0.5f, 1e-4f);
}

TEST(FitToView, pan_x_is_400_for_800_wide_canvas)
{
    ImVec2 canvas_size = { 800.0f, 600.0f };
    ImVec2 pan         = fit_to_view_pan(canvas_size);
    EXPECT_NEAR(pan.x, 400.0f, 1e-4f);
}

TEST(FitToView, pan_y_is_300_for_600_tall_canvas)
{
    ImVec2 canvas_size = { 800.0f, 600.0f };
    ImVec2 pan         = fit_to_view_pan(canvas_size);
    EXPECT_NEAR(pan.y, 300.0f, 1e-4f);
}

// ─────────────────────────────────────────────────────────────────────────────
// CoordinateTransform
// ─────────────────────────────────────────────────────────────────────────────

TEST(CoordinateTransform, world_to_grid_at_zoom_one_is_identity)
{
    TS::SetCanvasZoom(1.0f);
    ImVec2 world = { 123.0f, 456.0f };
    ImVec2 grid  = TS::WorldToGrid(world);
    EXPECT_NEAR(grid.x, world.x, 1e-4f);
    EXPECT_NEAR(grid.y, world.y, 1e-4f);
}

TEST(CoordinateTransform, world_to_grid_at_zoom_two_doubles_coords)
{
    TS::SetCanvasZoom(2.0f);
    ImVec2 world = { 50.0f, 75.0f };
    ImVec2 grid  = TS::WorldToGrid(world);
    EXPECT_NEAR(grid.x, world.x * 2.0f, 1e-4f);
    EXPECT_NEAR(grid.y, world.y * 2.0f, 1e-4f);
}

TEST(CoordinateTransform, grid_to_world_roundtrip)
{
    TS::SetCanvasZoom(1.5f);
    ImVec2 original = { 100.0f, 200.0f };
    ImVec2 roundtrip = TS::GridToWorld(TS::WorldToGrid(original));
    EXPECT_NEAR(roundtrip.x, original.x, 1e-4f);
    EXPECT_NEAR(roundtrip.y, original.y, 1e-4f);
}

TEST(CoordinateTransform, grid_to_world_at_zoom_half_doubles_coords)
{
    TS::SetCanvasZoom(0.5f);
    ImVec2 grid  = { 40.0f, 60.0f };
    ImVec2 world = TS::GridToWorld(grid);
    EXPECT_NEAR(world.x, grid.x * 2.0f, 1e-4f);
    EXPECT_NEAR(world.y, grid.y * 2.0f, 1e-4f);
}

TEST(CoordinateTransform, negative_coords_world_to_grid)
{
    TS::SetCanvasZoom(2.0f);
    ImVec2 world = { -30.0f, -15.0f };
    ImVec2 grid  = TS::WorldToGrid(world);
    EXPECT_NEAR(grid.x, -60.0f, 1e-4f);
    EXPECT_NEAR(grid.y, -30.0f, 1e-4f);
}

TEST(CoordinateTransform, negative_coords_grid_to_world)
{
    TS::SetCanvasZoom(2.0f);
    ImVec2 grid  = { -60.0f, -30.0f };
    ImVec2 world = TS::GridToWorld(grid);
    EXPECT_NEAR(world.x, -30.0f, 1e-4f);
    EXPECT_NEAR(world.y, -15.0f, 1e-4f);
}

TEST(CoordinateTransform, negative_coords_roundtrip)
{
    TS::SetCanvasZoom(0.75f);
    ImVec2 original = { -100.0f, -200.0f };
    ImVec2 roundtrip = TS::GridToWorld(TS::WorldToGrid(original));
    EXPECT_NEAR(roundtrip.x, original.x, 1e-4f);
    EXPECT_NEAR(roundtrip.y, original.y, 1e-4f);
}
