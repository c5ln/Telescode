#pragma once

#include "Graph.h"
#include <vector>

// Produces a reading order from a graph and per-node combined scores.
//
// Algorithm: SCC condensation (Tarjan) + Kahn's topological sort.
// When multiple candidates are available, picks the highest combined_score
// first (max-heap). For ties, uses cost_hint (ascending) then node name
// (alphabetical).
//
// cost_hint is the per-node understanding cost -- file complexity_score in [0,1]
// -- and lower is read first. It replaced a `loc` descending tie-break, which
// said "when two files matter equally, read the bigger one first"; nothing
// supported that, and it worked directly against the reading-cost objective.
// An empty vector disables the tie-break (all costs read as 0).
//
// Returns NodeIds in reading order (index 0 = read first, rank 1).
class ReadingSequencer {
public:
    static std::vector<NodeId> sequence(const Graph&              g,
                                         const std::vector<double>& combined_score,
                                         const std::vector<double>& cost_hint);
};
