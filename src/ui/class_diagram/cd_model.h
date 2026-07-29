// src/ui/class_diagram/cd_model.h
// Class diagram data model: DB rows → renderable graph.
// No rendering code here — pure data only.

#pragma once

#include <string>
#include <vector>
#include <imgui.h>

namespace TS {

// Rendered row text is ellipsized once to fit NODE_WIDTH and cached in the
// `display` members below. It is filled by cd_view on the first frame, not by
// the builder, because measuring text needs a live font atlas.

// One field row: e.g. "- _name"
struct CDField {
    char        access;     // '+' public  '-' private  '#' protected
    std::string name;
    std::string display;    // "- _name", ellipsized
};

// One method row: e.g. "+ createOrder(req): Order"
struct CDMethod {
    char        access;     // '+' public  '-' private  '#' protected  '~' package
    std::string name;
    std::string params;     // already-joined param string, e.g. "req, qty"
    std::string ret_type;   // "void" if absent
    std::string display;    // "+ createOrder(req): Order", ellipsized
};

// One class box. node_id is the imnodes integer ID.
// Each node exposes exactly two pins:
//   pin_in  = node_id * 2      (top,    receives incoming edges)
//   pin_out = node_id * 2 + 1  (bottom, emits outgoing edges)
struct CDNode {
    int         node_id;    // imnodes node ID (unique per graph)
    std::string class_id;   // FK → ClassEntity::class_id
    std::string file_id;    // FK → file::file_id, e.g. "sherlock_project/notify.py"
    std::string class_name;
    std::string package;    // derived from file_id, shown below class_name
    std::string display_name;      // class_name, ellipsized
    std::string display_package;   // package, ellipsized
    std::vector<CDField>  fields;
    std::vector<CDMethod> methods;
    ImVec2      pos;        // logical position — pixels at zoom 1.0, see cd_layout.h
};

inline int CDPinLeft (int node_id) { return node_id * 2;     }
inline int CDPinRight(int node_id) { return node_id * 2 + 1; }

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

// A file or the folder holding it. Files own class nodes; folders own files.
// Built from CDNode::file_id by cd_layout, and drawn as a boundary around its
// children. Two levels only for now — folder paths are truncated to a depth
// that keeps the group count readable.
struct CDContainer {
    int              parent;            // index into CDGraph::containers, -1 = folder
    bool             is_file;
    std::string      label;             // "notify.py" / "sherlock_project"
    std::string      display_label;     // label, ellipsized to the container width
    std::vector<int> child_nodes;       // indices into CDGraph::nodes; files only
    std::vector<int> child_containers;  // indices into CDGraph::containers
    ImVec2           pos;               // absolute logical top-left, as CDNode::pos
    ImVec2           size;
};

// Full diagram state, including interaction state.
struct CDGraph {
    std::vector<CDNode>      nodes;
    std::vector<CDEdge>      edges;
    std::vector<CDContainer> containers;

    // Both reset to false on rebuild, since BuildCDGraph returns a fresh graph.
    bool display_ready = false;  // until the `display` strings have been filled
    bool layout_valid  = false;  // until CDNode::pos has been assigned

    int hovered_node_id  = -1;  // -1 = none
    int selected_node_id = -1;  // -1 = none
    // When selected_node_id != -1:
    //   directly connected nodes → normal brightness
    //   all others               → dimmed
};

} // namespace TS
