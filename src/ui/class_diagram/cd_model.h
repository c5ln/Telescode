// src/ui/class_diagram/cd_model.h
// Class diagram data model: DB rows → renderable graph.
// No rendering code here — pure data only.

#pragma once

#include <string>
#include <vector>
#include <imgui.h>

namespace TS {

// One field row: e.g. "- _name"
struct CDField {
    char        access;     // '+' public  '-' private  '#' protected
    std::string name;
};

// One method row: e.g. "+ createOrder(req): Order"
struct CDMethod {
    char        access;     // '+' public  '-' private  '#' protected  '~' package
    std::string name;
    std::string params;     // already-joined param string, e.g. "req, qty"
    std::string ret_type;   // "void" if absent
};

// One class box. node_id is the imnodes integer ID.
// Each node exposes exactly two pins:
//   pin_in  = node_id * 2      (top,    receives incoming edges)
//   pin_out = node_id * 2 + 1  (bottom, emits outgoing edges)
struct CDNode {
    int         node_id;    // imnodes node ID (unique per graph)
    std::string class_id;   // FK → ClassEntity::class_id
    std::string class_name;
    std::string package;    // derived from file_id, shown below class_name
    std::vector<CDField>  fields;
    std::vector<CDMethod> methods;
    ImVec2      pos;        // initial grid position
};

inline int CDPinIn (int node_id) { return node_id * 2;     }
inline int CDPinOut(int node_id) { return node_id * 2 + 1; }

// Relationship between two class nodes.
enum class CDEdgeType {
    Dependency,   // dashed arrow  — INHERITS / IMPORTS
    Association,  // solid arrow   — CALLS / DECORATES
};

struct CDEdge {
    int        edge_id;
    int        src_node_id;
    int        dst_node_id;
    CDEdgeType type;
};

// Full diagram state, including interaction state.
struct CDGraph {
    std::vector<CDNode> nodes;
    std::vector<CDEdge> edges;

    int hovered_node_id  = -1;  // -1 = none
    int selected_node_id = -1;  // -1 = none
    // When selected_node_id != -1:
    //   directly connected nodes → normal brightness
    //   all others               → dimmed
};

} // namespace TS
