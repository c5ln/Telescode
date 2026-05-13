#pragma once

#include "Graph.h"
#include <string>
#include <unordered_map>
#include <utility>

struct sqlite3;

struct GraphBuilderResult {
    Graph file_graph;
    Graph func_graph;
    // entity_id -> file_id (for Pass 2 entities: functions and classes)
    std::unordered_map<std::string, std::string> entity_file_map;
    // entity_id -> "function" or "class"
    std::unordered_map<std::string, std::string> entity_type_map;
    // entity_id -> start_line (for local_rank tie-breaking)
    std::unordered_map<std::string, int>         entity_start_line;
    // file_id -> loc (for file-level pass loc_hint)
    std::unordered_map<std::string, int>         file_loc_map;
};

class GraphBuilder {
public:
    static GraphBuilderResult build(sqlite3* db);

private:
    static void build_file_graph(sqlite3* db, Graph& g);
    static void build_func_graph(sqlite3* db, Graph& g);
    static void build_entity_file_map(sqlite3* db,
                                      std::unordered_map<std::string, std::string>& m);
};
