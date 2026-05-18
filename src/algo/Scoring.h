#pragma once

#include "AlgoConfig.h"
#include "Graph.h"
#include <vector>

class PageRank {
public:
    static std::vector<double> compute(const Graph& g, const AlgoConfig& cfg);
};

// Combines normalized PageRank and BC scores.
//
// score(v) = (alpha/(alpha+beta)) * PR_norm(v)
//          + (beta /(alpha+beta)) * BC_norm(v)
//
// min-max normalization applied to each input; if all values are equal the
// normalized value is 0.5 (avoids division by zero).
class ScoreCombiner {
public:
    static std::vector<double> combine(const std::vector<double>& pr,
                                       const std::vector<double>& bc,
                                       double alpha,
                                       double beta);
};
