#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

using NodeId = uint32_t;

struct Graph {
    std::vector<std::vector<NodeId>> adj;
    std::vector<std::vector<NodeId>> radj;
    std::unordered_map<std::string, NodeId> id_to_node;
    std::vector<std::string>                node_to_id;

    NodeId get_or_add(const std::string& id);
    int size() const { return static_cast<int>(node_to_id.size()); }
};

class SCCFinder {
public:
    static std::vector<std::vector<NodeId>> find(const Graph& g);
};
