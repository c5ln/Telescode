// src/ui/ts_rail.cpp
// Sequence rail: the reading order as a horizontal filmstrip, rank 1 leftmost.
//
// Zone layout, top to bottom:
//   strip   32px  section label + file count on the left; the collapse chevron
//                 (ts_app.cpp) sits at the right end. Also the collapsed height,
//                 so collapsing leaves exactly the strip.
//   items         one chip per file in file_rank order, scrolled horizontally,
//                 each followed by a chevron pointing at the file to read next.

#include "ts_rail.h"
#include "ts_style.h"

#include <sqlite3.h>
#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

namespace TS {
namespace {

// 1x reference values -- multiply by TS::ui_scale at every use site.
constexpr float k_strip_h    = 32.0f;   // must match k_rail_strip in ts_app.cpp
constexpr float k_pad_x      = 12.0f;
constexpr float k_chip_w     = 176.0f;
constexpr float k_chip_gap   = 18.0f;   // wide enough to seat the chevron
constexpr float k_arrow_w    =  4.0f;   // chevron half-width
constexpr float k_arrow_h    =  5.0f;   // chevron half-height
constexpr float k_arrow_th   =  1.5f;   // chevron stroke thickness
constexpr float k_chip_pad_x =  8.0f;   // text inset from the chip's left edge
constexpr float k_chip_pad_y =  6.0f;   // minimum inset above and below the rows
constexpr float k_chip_round =  8.0f;
constexpr float k_chip_mar_y =  4.0f;   // clearance above and below the chip row
constexpr float k_rank_h     = 14.0f;   // chip text rows, stacked from the top
constexpr float k_name_h     = 17.0f;
constexpr float k_dir_h      = 15.0f;
constexpr float k_wheel_step = 64.0f;   // horizontal scroll per wheel notch

struct RailEntry {
    std::string file_id;        // "src/ui/ts_app.cpp"
    std::string name;           // "ts_app.cpp"
    std::string dir;            // "src/ui"
    std::string display_name;   // name, ellipsized to the chip width
    std::string display_dir;    // dir,  ellipsized to the chip width
    int   rank = 0;
};

std::vector<RailEntry> s_entries;
bool s_display_ready = false;   // until the `display` strings have been filled
int  s_selected      = -1;      // index into s_entries, -1 = none

// ── Text preparation ─────────────────────────────────────────────────────────

// Trims `text` until it plus an ellipsis fits `max_w`, measured with `font` at
// `px`.
//
// NOTE: duplicated from the same helper in cd_view.cpp, which is file-local
// there. Worth promoting to one shared home rather than keeping two copies.
std::string Ellipsize(ImFont* font, float px, float max_w, std::string text)
{
    if (!font || text.empty()) return text;
    if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x <= max_w) return text;

    // U+2026 sits outside ImGui's default glyph range, so spell the ellipsis out.
    static const char* const kEllipsis = "...";
    const float ellipsis_w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, kEllipsis).x;

    while (!text.empty()) {
        // Drop one whole UTF-8 code point, not one byte.
        do { text.pop_back(); }
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);

        if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x + ellipsis_w <= max_w)
            break;
    }
    return text + kEllipsis;
}

// Needs a built font atlas, so it runs on the first frame rather than in
// InitRailFromDB -- same reason as PrepareDisplayText in cd_view.cpp.
void PrepareDisplayText()
{
    const int   lod     = TS::GetFontLOD(1.0f);
    const float budget  = (k_chip_w - k_chip_pad_x * 2.0f) * TS::ui_scale;
    const float px_name = TS::FONT_SIZE_BASE  * TS::ui_scale;
    const float px_dir  = TS::FONT_SIZE_SMALL * TS::ui_scale;

    for (RailEntry& e : s_entries) {
        e.display_name = Ellipsize(TS::FONT_MEDIUM_LOD[lod], px_name, budget, e.name);
        e.display_dir  = Ellipsize(TS::FONT_SMALL_LOD[lod],  px_dir,  budget, e.dir);
    }
    s_display_ready = true;
}

// ── Rendering ────────────────────────────────────────────────────────────────

// Draws one row of text inside the box already reserved for it, vertically
// centred and clipped. Going through the draw list rather than ImGui::Text
// keeps an over-long string from widening the chip.
void DrawRowText(ImDrawList* dl, ImFont* font, float px,
                 ImVec2 tl, float w, float h, ImU32 col, const char* text)
{
    if (!font || !text || !*text) return;
    const ImVec4 clip = { tl.x, tl.y, tl.x + w, tl.y + h };
    dl->AddText(font, px, { tl.x, tl.y + (h - px) * 0.5f }, col, text, nullptr, 0.0f, &clip);
}

} // anonymous namespace

// ── InitRailFromDB ───────────────────────────────────────────────────────────

void InitRailFromDB(sqlite3* db)
{
    s_entries.clear();
    s_display_ready = false;
    s_selected      = -1;
    if (!db) return;

    // file_rank is NOT NULL exactly when entity_type = 'file' -- enforced by the
    // CHECK constraint in db.cpp -- so this ordering is total.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT file_id, file_rank "
            "FROM reading_sequence WHERE entity_type = 'file' "
            "ORDER BY file_rank;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return;   // table absent (a DB from before the algo ran) -- empty state

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* fid = sqlite3_column_text(stmt, 0);
        if (!fid) continue;

        RailEntry e;
        e.file_id = reinterpret_cast<const char*>(fid);
        e.rank    = sqlite3_column_int(stmt, 1);

        const auto slash = e.file_id.rfind('/');
        e.name = (slash == std::string::npos) ? e.file_id : e.file_id.substr(slash + 1);
        e.dir  = (slash == std::string::npos) ? std::string() : e.file_id.substr(0, slash);

        s_entries.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
}

// ── DrawRail ─────────────────────────────────────────────────────────────────

void DrawRail(ImVec2 pos, ImVec2 size, bool collapsed)
{
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    if (!s_display_ready) PrepareDisplayText();

    const float s       = TS::ui_scale;
    const float strip_h = k_strip_h * s;
    const float pad_x   = k_pad_x   * s;

    const int   lod      = TS::GetFontLOD(1.0f);
    ImFont*     f_small  = TS::FONT_SMALL_LOD[lod];
    ImFont*     f_medium = TS::FONT_MEDIUM_LOD[lod];
    ImFont*     f_mono   = TS::FONT_MONO_LOD[lod];
    const float px_small = TS::FONT_SIZE_SMALL * s;
    const float px_name  = TS::FONT_SIZE_BASE  * s;
    const float px_mono  = TS::FONT_SIZE_MONO  * s;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Rail and canvas are both light surfaces; without this the seam vanishes.
    dl->AddLine(pos, { pos.x + size.x, pos.y }, TS::LINE_U32, s);

    // ── Strip ────────────────────────────────────────────────────────────────
    // Left-aligned on purpose: the right end belongs to ts_app.cpp's chevron.
    static const char* const kTitle = "READING SEQUENCE";
    DrawRowText(dl, f_small, px_small, { pos.x + pad_x, pos.y },
                size.x, strip_h, TS::INK_2_U32, kTitle);

    if (f_small) {
        char count[32];
        std::snprintf(count, sizeof(count), "%zu files", s_entries.size());
        const float title_w = f_small->CalcTextSizeA(px_small, FLT_MAX, 0.0f, kTitle).x;
        DrawRowText(dl, f_small, px_small, { pos.x + pad_x + title_w + pad_x, pos.y },
                    size.x, strip_h, TS::MUTED_U32, count);
    }

    if (collapsed) return;

    dl->AddLine({ pos.x, pos.y + strip_h }, { pos.x + size.x, pos.y + strip_h },
                TS::LINE_U32, s);

    // The separator sits on the shell's draw list, which renders beneath child
    // windows -- start the item strip below it or the child would paint over it.
    const float content_y = pos.y + strip_h + s;
    const float content_h = size.y - strip_h - s;
    if (content_h <= 0.0f) return;

    // The viewer recomputes the sequence on every launch, so an empty rail does
    // not mean "the algorithm has not run" -- it means the run produced no
    // file-level rows. The usual cause is a database whose `file` table predates
    // the current schema: AlgoRunner::merge keeps only files present in
    // file_loc_map, which the old columns cannot fill. Such a database cannot be
    // migrated in place, hence the advice to rescan into a new file.
    if (s_entries.empty()) {
        DrawRowText(dl, f_small, px_small, { pos.x + pad_x, content_y },
                    size.x, content_h, TS::MUTED_U32,
                    "No file-level reading sequence in this database. If it was built by an "
                    "older version, rescan into a new file: Telescode scan <repo> <new.db>");
        return;
    }

    // ── Items ────────────────────────────────────────────────────────────────
    ImGui::SetNextWindowPos({ pos.x, content_y });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, TS::PANEL);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##rail_items", { size.x, content_h }, 0,
        ImGuiWindowFlags_HorizontalScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse   |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // The strip has no vertical overflow, so a vertical wheel would do nothing
    // over it. Spend it sideways instead.
    if (ImGui::IsWindowHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            ImGui::SetScrollX(ImGui::GetScrollX() - wheel * k_wheel_step * s);
    }

    // Measured rather than derived from content_h: a horizontal scrollbar eats
    // into the child's client area, and sizing the chips against the outer
    // height would run them under it.
    const float inner_h = ImGui::GetContentRegionAvail().y;

    const float chip_w = k_chip_w * s;
    const float chip_h = inner_h - k_chip_mar_y * s * 2.0f;
    const float gap    = k_chip_gap * s;
    const float pad_l  = k_chip_pad_x * s;
    const float pad_v  = k_chip_pad_y * s;
    const float text_w = chip_w - pad_l * 2.0f;

    // Derived fills, resolved once per frame rather than per chip: neither has a
    // precomputed _U32, and the rule against inline conversions is about paying
    // for them at every draw call.
    const ImU32 chip_hover_u32 = ImGui::ColorConvertFloat4ToU32(Darken(TS::BG_SOFT, 0.04f));

    if (chip_h > 0.0f) {
        const float scroll_x = ImGui::GetScrollX();

        for (size_t i = 0; i < s_entries.size(); ++i) {
            const float x = pad_x + static_cast<float>(i) * (chip_w + gap);

            // Cull off-screen chips -- a large repo puts thousands of files here.
            // The left bound carries one gap of slack: a chip just past the edge
            // still owns the chevron after it, which reaches back into view. The
            // chip itself is drawn and then clipped by the child, which is
            // cheaper than tracking the two cases apart.
            if (x + chip_w + gap < scroll_x || x > scroll_x + size.x) continue;

            const RailEntry& e = s_entries[i];

            ImGui::SetCursorPos({ x, k_chip_mar_y * s });
            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##chip", { chip_w, chip_h });
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemActivated()) s_selected = static_cast<int>(i);
            ImGui::PopID();

            const ImVec2 tl  = ImGui::GetItemRectMin();
            const ImVec2 br  = ImGui::GetItemRectMax();
            const bool   sel = (static_cast<int>(i) == s_selected);

            const ImU32 fill   = sel     ? TS::ACCENT_PRIMARY_SUBTLE_U32
                               : hovered ? chip_hover_u32
                                         : TS::BG_SOFT_U32;
            const ImU32 border = sel ? TS::ACCENT_PRIMARY_U32 : TS::LINE_U32;

            dl->AddRectFilled(tl, br, fill,   k_chip_round * s);
            dl->AddRect      (tl, br, border, k_chip_round * s, 0, s);

            char rank[16];
            std::snprintf(rank, sizeof(rank), "%02d", e.rank);

            // Centred as a block: with the score bar gone the rows no longer fill
            // the chip, and pinning them to the top would leave the slack pooled
            // under the last line. pad_v stays as a floor so a chip shorter than
            // its own text starts inside the border rather than above it.
            const float rows_h = (k_rank_h + k_name_h + k_dir_h) * s;
            const float slack  = (chip_h - rows_h) * 0.5f;
            float y = tl.y + (slack > pad_v ? slack : pad_v);
            DrawRowText(dl, f_mono, px_mono, { tl.x + pad_l, y }, text_w, k_rank_h * s,
                        TS::MUTED_U32, rank);
            y += k_rank_h * s;
            DrawRowText(dl, f_medium, px_name, { tl.x + pad_l, y }, text_w, k_name_h * s,
                        TS::INK_U32, e.display_name.c_str());
            y += k_name_h * s;
            DrawRowText(dl, f_small, px_small, { tl.x + pad_l, y }, text_w, k_dir_h * s,
                        TS::INK_3_U32, e.display_dir.c_str());

            // Sequence chevron, seated in the gap to the right: it points at the
            // file to read next, so the last chip has nothing to point at. Two
            // stroked segments rather than a glyph -- the ellipsis in Ellipsize()
            // is spelled out for the same reason, and a drawn chevron stays crisp
            // at every ui_scale instead of depending on the atlas.
            if (i + 1 < s_entries.size()) {
                const float  cx = br.x + gap * 0.5f;
                const float  cy = (tl.y + br.y) * 0.5f;
                const float  aw = k_arrow_w * s;
                const float  ah = k_arrow_h * s;
                const ImVec2 pts[3] = {
                    { cx - aw, cy - ah },
                    { cx + aw, cy      },
                    { cx - aw, cy + ah },
                };
                dl->AddPolyline(pts, 3, TS::MUTED_U32, 0, k_arrow_th * s);
            }

            // The chip shows a basename; the tooltip carries the full path.
            if (hovered) ImGui::SetTooltip("%s", e.file_id.c_str());
        }
    }

    // Trailing padding so the last chip can scroll clear of the right edge.
    ImGui::SetCursorPos({ pad_x + static_cast<float>(s_entries.size()) * (chip_w + gap),
                          k_chip_mar_y * s });
    ImGui::Dummy({ pad_x, 1.0f });

    ImGui::EndChild();
}

} // namespace TS
