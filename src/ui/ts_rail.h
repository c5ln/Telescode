// src/ui/ts_rail.h
// Sequence rail content: the reading order, along the bottom of the shell.
//
// InitRailFromDB() -- call once after the ImGui context exists and before the
//                     DB handle is closed. Mirrors InitCanvasFromDB().
// DrawRail()       -- call once per frame from DrawAppShell(), inside the
//                     ##shell Begin/End block, after the rail's background rect
//                     and before the collapse chevron. The chevron stays with
//                     ts_app.cpp, which owns the collapse flag; the rail leaves
//                     the strip's right end clear for it.

#pragma once
#include <imgui.h>

struct sqlite3;

namespace TS {

// Reads reading_sequence into memory. No-op if db is nullptr or the table is
// absent, in which case the rail draws its empty state.
void InitRailFromDB(sqlite3* db);

// `pos` / `size` are the rail zone in screen space, the chevron area included.
void DrawRail(ImVec2 pos, ImVec2 size, bool collapsed);

} // namespace TS
