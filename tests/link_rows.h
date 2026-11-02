// tests/link_rows.h
// Putting `link` rows into a test database. `link` is a view (src/db/db.cpp):
// a CALLS / INHERITS / DECORATES row is a resolved ref the resolver marked
// emits_link, an IMPORTS row a resolved import_resolution row. linkRowsSql
// writes those, so a test can still say "these links" without running the
// resolver. Every file the rows name must already be in `file` (foreign keys).

#pragma once

#include <sqlite3.h>

#include <string>
#include <tuple>
#include <vector>

namespace test_links {

using Link = std::tuple<std::string, std::string, std::string>;   // source, target, type

inline std::string fileOf(const std::string& id)
{
    return id.substr(0, id.find("::"));
}

inline std::string quoted(const std::string& s)
{
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("''") : std::string(1, c);
    return q + "'";
}

// For the view. An IMPORTS target is a file id. A CALLS / INHERITS target, and
// a DECORATES source (the decorator), gets a symbol row of a fitting kind if it
// has none; refs are numbered by `line` so that none collide.
inline std::string linkRowsSql(const std::vector<Link>& links)
{
    std::string sql;
    int line = 0;
    for (const auto& [source, target, type] : links) {
        if (type == "IMPORTS") {
            sql += "INSERT INTO import_resolution(file_id, module_spec, resolved_file_id, status)"
                   " VALUES(" + quoted(source) + ", " + quoted(target) + ", " + quoted(target) +
                   ", 'resolved');";
            continue;
        }
        // The ref runs from the symbol it is in to the symbol it names.
        const bool decorates = type == "DECORATES";
        const std::string& from = decorates ? target : source;
        const std::string& to   = decorates ? source : target;
        const char* kind = type == "INHERITS" ? "class" : "function";
        sql += "INSERT OR IGNORE INTO symbol(symbol_id, file_id, kind, name, start_line, end_line)"
               " VALUES(" + quoted(to) + ", " + quoted(fileOf(to)) + ", '" + kind + "', " +
               quoted(to.substr(to.rfind(':') + 1)) + ", 0, 0);";
        sql += "INSERT INTO ref(file_id, source_id, scope_id, kind, name_path, raw_text, line,"
               "                resolved_id, status, emits_link)"
               " VALUES(" + quoted(fileOf(from)) + ", " + quoted(from) + ", " + quoted(from) + ", " +
               quoted(type) + ", 'x', 'x', " + std::to_string(line++) + ", " + quoted(to) +
               ", 'resolved', 1);";
    }
    return sql;
}

}  // namespace test_links
