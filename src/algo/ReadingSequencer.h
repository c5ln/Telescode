#pragma once

#include "Graph.h"
#include <vector>

// Produces a reading order from a graph and per-node combined scores.
//
// Algorithm: SCC condensation (Tarjan) + Kahn's topological sort.
// When multiple candidates are available, picks the highest combined_score
// first (max-heap). For ties, uses loc_hint (descending) then node name (alphabetical).
//
// Returns NodeIds in reading order (index 0 = read first, rank 1).
class ReadingSequencer {
public:
    static std::vector<NodeId> sequence(const Graph&              g,
                                         const std::vector<double>& combined_score,
                                         const std::vector<int>&    loc_hint);
};
