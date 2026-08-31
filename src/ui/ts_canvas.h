// src/ui/ts_canvas.h
// imnodes canvas: zoom, pan, coordinate transforms.
//
// DrawCanvas()     -- call once per frame inside ##shell Begin/End block.
// ShutdownCanvas() -- call at cleanup, before ImNodes::DestroyContext().
//
// Zoom constants (ZOOM_MIN, ZOOM_MAX, ZOOM_STEP_SCROLL) are defined in
// ts_style.h inside the TS:: namespace and re-exported from here via that include.

#pragma once
#include <imgui.h>
#include <string>
#include "ts_style.h"  // provides TS::ZOOM_MIN, TS::ZOOM_MAX, TS::ZOOM_STEP_SCROLL

// imnodes.h is needed only in the .cpp; forward-declare the opaque type here
// so the header stays includable from translation units that don't link imnodes.
struct ImNodesEditorContext;
struct sqlite3;

namespace TS {

    // Public API
    void   DrawCanvas(ImVec2 pos, ImVec2 size);
    void   ShutdownCanvas();

    // Call once after ImGui context is ready. Loads class diagram from DB.
    // No-op if db is nullptr (canvas stays empty).
    void   InitCanvasFromDB(sqlite3* db);

    // Centres the camera on `file_id`'s box in the class diagram, applied at the
    // start of the next DrawCanvas -- the rail is drawn after the canvas, so a
    // click there arrives too late to affect the frame it happened in.
    //
    // The jump also raises the zoom to at least CD_NODE_FADE_HI when it is below
    // it. A file with no classes has no box on the canvas at all; that request
    // is dropped and the camera stays put.
    void   RequestFocusFile(const std::string& file_id);

    float  GetCanvasZoom();

    // Test-only: directly sets internal s_zoom without side effects.
    void   SetCanvasZoom(float zoom);

    // Coordinate transforms.
    // WorldToGrid: world_pos * zoom
    // GridToWorld: grid_pos  / zoom
    ImVec2 WorldToGrid(ImVec2 world_pos);
    ImVec2 GridToWorld(ImVec2 grid_pos);

} // namespace TS
