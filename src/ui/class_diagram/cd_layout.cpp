// src/ui/class_diagram/cd_layout.cpp

#include "cd_layout.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace TS {
namespace {

// ── Layered layout internals ─────────────────────────────────────────────────

// Returns a representative node per weakly connected component (union-find over
// the edges read as undirected). Representatives are node indices, not compacted.
std::vector<int> ComponentReps(int n, const std::vector<CDLayerEdge>& edges)
{
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);

    auto Find = [&parent](int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] =
                parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };

    for (const CDLayerEdge& e : edges) {
        if (e.src < 0 || e.src >= n || e.dst < 0 || e.dst >= n) continue;
        const int a = Find(e.src);
        const int b = Find(e.dst);
        if (a != b) parent[static_cast<size_t>(a)] = b;
    }

    std::vector<int> rep(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) rep[static_cast<size_t>(i)] = Find(i);
    return rep;
}

// Drops the edges that close a cycle, so layering has something acyclic to work
// with. Iterative DFS — call graphs get deep enough that recursion is a risk.
std::vector<CDLayerEdge> BreakCycles(int n, const std::vector<CDLayerEdge>& edges)
{
    std::vector<std::vector<int>> out(static_cast<size_t>(n));
    for (size_t i = 0; i < edges.size(); ++i)
        out[static_cast<size_t>(edges[i].src)].push_back(static_cast<int>(i));

    enum { UNVISITED = 0, ON_STACK, DONE };
    std::vector<char> state(static_cast<size_t>(n), UNVISITED);
    std::vector<char> keep(edges.size(), 1);
    std::vector<std::pair<int, size_t>> stack;  // node, next adjacency slot

    for (int root = 0; root < n; ++root) {
        if (state[static_cast<size_t>(root)] != UNVISITED) continue;
        state[static_cast<size_t>(root)] = ON_STACK;
        stack.push_back({root, 0});

        while (!stack.empty()) {
            const int    v  = stack.back().first;
            const size_t ai = stack.back().second;
            if (ai < out[static_cast<size_t>(v)].size()) {
                ++stack.back().second;
                const int ei = out[static_cast<size_t>(v)][ai];
                const int w  = edges[static_cast<size_t>(ei)].dst;
                if (state[static_cast<size_t>(w)] == ON_STACK) {
                    keep[static_cast<size_t>(ei)] = 0;  // back edge (self-loops included)
                } else if (state[static_cast<size_t>(w)] == UNVISITED) {
                    state[static_cast<size_t>(w)] = ON_STACK;
                    stack.push_back({w, 0});
                }
            } else {
                state[static_cast<size_t>(v)] = DONE;
                stack.pop_back();
            }
        }
    }

    std::vector<CDLayerEdge> acyclic;
    acyclic.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i)
        if (keep[i]) acyclic.push_back(edges[i]);
    return acyclic;
}

// Longest-path layering: a node sits one layer past its deepest predecessor.
std::vector<int> LongestPathLayers(int n, const std::vector<CDLayerEdge>& acyclic)
{
    std::vector<std::vector<int>> out(static_cast<size_t>(n));
    std::vector<int>              indeg(static_cast<size_t>(n), 0);
    for (const CDLayerEdge& e : acyclic) {
        out[static_cast<size_t>(e.src)].push_back(e.dst);
        ++indeg[static_cast<size_t>(e.dst)];
    }

    std::vector<int> layer(static_cast<size_t>(n), 0);
    std::vector<int> queue;
    for (int i = 0; i < n; ++i)
        if (indeg[static_cast<size_t>(i)] == 0) queue.push_back(i);

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int v = queue[qi];
        for (int w : out[static_cast<size_t>(v)]) {
            layer[static_cast<size_t>(w)] =
                std::max(layer[static_cast<size_t>(w)], layer[static_cast<size_t>(v)] + 1);
            if (--indeg[static_cast<size_t>(w)] == 0) queue.push_back(w);
        }
    }
    return layer;
}

// Barycenter heuristic: repeatedly reorder each layer by the average position of
// its neighbours in the layer before (or after) it, which pulls connected boxes
// into line and cuts crossings.
void OrderLayers(std::vector<std::vector<int>>& layers,
                 const std::vector<CDLayerEdge>& acyclic, int n)
{
    if (layers.size() < 2) return;

    std::vector<std::vector<int>> pred(static_cast<size_t>(n));
    std::vector<std::vector<int>> succ(static_cast<size_t>(n));
    for (const CDLayerEdge& e : acyclic) {
        succ[static_cast<size_t>(e.src)].push_back(e.dst);
        pred[static_cast<size_t>(e.dst)].push_back(e.src);
    }

    std::vector<int> rank(static_cast<size_t>(n), 0);
    auto Reindex = [&]() {
        for (const std::vector<int>& L : layers)
            for (size_t i = 0; i < L.size(); ++i) rank[static_cast<size_t>(L[i])] = static_cast<int>(i);
    };
    Reindex();

    // Alternating sweeps; four passes is where this heuristic stops paying off.
    for (int pass = 0; pass < 4; ++pass) {
        const bool downward = (pass % 2) == 0;
        for (size_t step = 1; step < layers.size(); ++step) {
            const size_t l = downward ? step : layers.size() - 1 - step;
            const std::vector<std::vector<int>>& neighbours = downward ? pred : succ;
            std::vector<int>& L = layers[l];

            std::vector<float> bary(L.size());
            for (size_t i = 0; i < L.size(); ++i) {
                const std::vector<int>& nb = neighbours[static_cast<size_t>(L[i])];
                if (nb.empty()) {
                    bary[i] = static_cast<float>(rank[static_cast<size_t>(L[i])]);
                    continue;
                }
                float sum = 0.0f;
                for (int u : nb) sum += static_cast<float>(rank[static_cast<size_t>(u)]);
                bary[i] = sum / static_cast<float>(nb.size());
            }

            // Sort an index permutation so equal barycenters keep their old order.
            std::vector<size_t> idx(L.size());
            std::iota(idx.begin(), idx.end(), size_t{0});
            std::stable_sort(idx.begin(), idx.end(),
                             [&bary](size_t a, size_t b) { return bary[a] < bary[b]; });

            std::vector<int> reordered(L.size());
            for (size_t i = 0; i < idx.size(); ++i) reordered[i] = L[idx[i]];
            L.swap(reordered);
        }
        Reindex();
    }
}

// Layers one connected component. Coordinates are local to the component.
std::vector<ImVec2> LayoutComponent(const std::vector<CDBox>& boxes,
                                    const std::vector<CDLayerEdge>& edges,
                                    float gap_x, float gap_y)
{
    const int n = static_cast<int>(boxes.size());
    std::vector<ImVec2> pos(boxes.size());
    if (n == 0) return pos;

    const std::vector<CDLayerEdge> acyclic = BreakCycles(n, edges);
    const std::vector<int>         layer   = LongestPathLayers(n, acyclic);

    int deepest = 0;
    for (int l : layer) deepest = std::max(deepest, l);

    std::vector<std::vector<int>> layers(static_cast<size_t>(deepest) + 1);
    for (int i = 0; i < n; ++i) layers[static_cast<size_t>(layer[static_cast<size_t>(i)])].push_back(i);

    OrderLayers(layers, acyclic, n);

    // x: a layer starts past the widest box of every layer before it.
    std::vector<float> layer_x(layers.size(), 0.0f);
    float x = 0.0f;
    for (size_t l = 0; l < layers.size(); ++l) {
        layer_x[l] = x;
        float widest = 0.0f;
        for (int v : layers[l]) widest = std::max(widest, boxes[static_cast<size_t>(v)].w);
        x += widest + gap_x;
    }

    // y: stack within a layer, then centre each layer on a shared midline so a
    // short layer sits beside the middle of a long one instead of its top.
    std::vector<float> layer_h(layers.size(), 0.0f);
    float tallest = 0.0f;
    for (size_t l = 0; l < layers.size(); ++l) {
        float h = 0.0f;
        for (size_t k = 0; k < layers[l].size(); ++k) {
            if (k) h += gap_y;
            h += boxes[static_cast<size_t>(layers[l][k])].h;
        }
        layer_h[l] = h;
        tallest    = std::max(tallest, h);
    }

    for (size_t l = 0; l < layers.size(); ++l) {
        float y = (tallest - layer_h[l]) * 0.5f;
        for (int v : layers[l]) {
            pos[static_cast<size_t>(v)] = { layer_x[l], y };
            y += boxes[static_cast<size_t>(v)].h + gap_y;
        }
    }
    return pos;
}

// ── Hierarchy internals ──────────────────────────────────────────────────────

// Edges with both endpoints in `to_local`, remapped to local indices and
// de-duplicated. Self-edges are dropped: they constrain nothing.
std::vector<CDLayerEdge> LocalEdges(const std::vector<CDEdge>& edges,
                                    const std::unordered_map<int, int>& node_id_to_local)
{
    std::vector<CDLayerEdge>       local;
    std::unordered_set<long long>  seen;

    for (const CDEdge& e : edges) {
        const auto s = node_id_to_local.find(e.src_node_id);
        if (s == node_id_to_local.end()) continue;
        const auto d = node_id_to_local.find(e.dst_node_id);
        if (d == node_id_to_local.end()) continue;
        if (s->second == d->second) continue;

        const long long key = static_cast<long long>(s->second) * (1LL << 32) + d->second;
        if (!seen.insert(key).second) continue;
        local.push_back({s->second, d->second});
    }
    return local;
}

} // anonymous namespace

// ── Node geometry ────────────────────────────────────────────────────────────

CDBox CDNodeSize(const CDNode& node, const CDNodeMetrics& m)
{
    const float rows = static_cast<float>(node.fields.size() + node.methods.size());
    return {
        m.content_w + 2.0f * m.pad_x,
        // pad_y lands four times, not twice: imnodes pads the title bar band on
        // both sides when it places the content below it (GetNodeContentOrigin),
        // then pads the finished node again in EndNode.
        m.header_h + rows * m.row_h + m.divider_h + 4.0f * m.pad_y
    };
}

std::vector<CDBox> CDGraphNodeSizes(const CDGraph& graph, const CDNodeMetrics& m)
{
    std::vector<CDBox> sizes;
    sizes.reserve(graph.nodes.size());
    for (const CDNode& n : graph.nodes) sizes.push_back(CDNodeSize(n, m));
    return sizes;
}

// ── Shelf packing ────────────────────────────────────────────────────────────

std::vector<ImVec2> CDShelfPack(const std::vector<CDBox>& boxes, float max_w, float gap)
{
    std::vector<ImVec2> pos(boxes.size());

    float cursor_x = 0.0f;   // left edge of the next box in the current row
    float shelf_y  = 0.0f;   // top edge of the current row
    float shelf_h  = 0.0f;   // tallest box placed in the current row so far

    for (size_t i = 0; i < boxes.size(); ++i) {
        // Wrap only when the row already holds something: a box wider than
        // max_w would otherwise wrap forever without ever being placed.
        if (cursor_x > 0.0f && cursor_x + boxes[i].w > max_w) {
            shelf_y += shelf_h + gap;
            cursor_x = 0.0f;
            shelf_h  = 0.0f;
        }
        pos[i]    = { cursor_x, shelf_y };
        cursor_x += boxes[i].w + gap;
        shelf_h   = std::max(shelf_h, boxes[i].h);
    }
    return pos;
}

float CDPreferredShelfWidth(const std::vector<CDBox>& boxes, float gap, float aspect)
{
    if (boxes.empty()) return 0.0f;

    // Charge each box for its own gap so the estimate survives many small boxes.
    float area   = 0.0f;
    float widest = 0.0f;
    for (const CDBox& b : boxes) {
        area   += (b.w + gap) * (b.h + gap);
        widest  = std::max(widest, b.w);
    }

    // area = w * h and w / h = aspect  =>  w = sqrt(area * aspect)
    return std::max(widest, std::sqrt(area * std::max(aspect, 0.01f)));
}

CDBox CDBoundingSize(const std::vector<CDBox>& boxes, const std::vector<ImVec2>& pos)
{
    if (boxes.size() != pos.size() || boxes.empty()) return {0.0f, 0.0f};

    float w = 0.0f;
    float h = 0.0f;
    for (size_t i = 0; i < boxes.size(); ++i) {
        w = std::max(w, pos[i].x + boxes[i].w);
        h = std::max(h, pos[i].y + boxes[i].h);
    }
    return {w, h};
}

// ── Layered layout ───────────────────────────────────────────────────────────

std::vector<ImVec2> CDLayeredLayout(const std::vector<CDBox>&        boxes,
                                    const std::vector<CDLayerEdge>&  edges,
                                    float gap_x, float gap_y, float aspect)
{
    const int n = static_cast<int>(boxes.size());
    std::vector<ImVec2> pos(boxes.size());
    if (n == 0) return pos;

    // Group nodes by component, keeping first-seen order so runs are repeatable.
    const std::vector<int>              rep = ComponentReps(n, edges);
    std::unordered_map<int, size_t>     rep_to_group;
    std::vector<std::vector<int>>       members;
    for (int i = 0; i < n; ++i) {
        const auto it = rep_to_group.find(rep[static_cast<size_t>(i)]);
        if (it == rep_to_group.end()) {
            rep_to_group.emplace(rep[static_cast<size_t>(i)], members.size());
            members.push_back({i});
        } else {
            members[it->second].push_back(i);
        }
    }

    std::vector<CDBox>               comp_box(members.size());
    std::vector<std::vector<ImVec2>> comp_pos(members.size());

    for (size_t g = 0; g < members.size(); ++g) {
        std::unordered_map<int, int> to_local;
        std::vector<CDBox>           sub;
        sub.reserve(members[g].size());
        for (int gi : members[g]) {
            to_local.emplace(gi, static_cast<int>(sub.size()));
            sub.push_back(boxes[static_cast<size_t>(gi)]);
        }

        std::vector<CDLayerEdge> sub_edges;
        for (const CDLayerEdge& e : edges) {
            const auto s = to_local.find(e.src);
            if (s == to_local.end()) continue;
            const auto d = to_local.find(e.dst);
            if (d == to_local.end()) continue;
            sub_edges.push_back({s->second, d->second});
        }

        comp_pos[g] = LayoutComponent(sub, sub_edges, gap_x, gap_y);
        comp_box[g] = CDBoundingSize(sub, comp_pos[g]);
    }

    const std::vector<ImVec2> origin =
        CDShelfPack(comp_box, CDPreferredShelfWidth(comp_box, gap_x, aspect), gap_x);

    for (size_t g = 0; g < members.size(); ++g)
        for (size_t k = 0; k < members[g].size(); ++k)
            pos[static_cast<size_t>(members[g][k])] = {
                origin[g].x + comp_pos[g][k].x,
                origin[g].y + comp_pos[g][k].y
            };
    return pos;
}

// ── Container tree ───────────────────────────────────────────────────────────

std::string CDFolderOf(const std::string& file_id)
{
    const size_t slash = file_id.rfind('/');
    return (slash == std::string::npos) ? std::string() : file_id.substr(0, slash);
}

std::string CDTruncatePath(const std::string& path, int depth)
{
    if (depth <= 0 || path.empty()) return std::string();

    size_t pos = 0;
    for (int i = 0; i < depth; ++i) {
        const size_t slash = path.find('/', pos);
        if (slash == std::string::npos) return path;  // fewer segments than asked for
        pos = slash + 1;
    }
    return path.substr(0, pos - 1);
}

int CDChooseFolderDepth(const std::vector<std::string>& file_ids,
                        int min_groups, int max_groups)
{
    if (file_ids.empty()) return 1;

    auto CountGroups = [&file_ids](int depth) {
        std::unordered_set<std::string> keys;
        for (const std::string& f : file_ids) keys.insert(CDTruncatePath(CDFolderOf(f), depth));
        return keys.size();
    };

    int    best = 1;
    size_t prev = 0;
    for (int depth = 1; depth <= 8; ++depth) {
        const size_t groups = CountGroups(depth);
        if (depth > 1 && groups == prev)                 break;  // deeper splits nothing further
        if (groups > static_cast<size_t>(max_groups))    break;  // too fragmented — keep `best`
        best = depth;
        prev = groups;
        if (groups >= static_cast<size_t>(min_groups))   break;  // deep enough to be useful
    }
    return best;
}

void CDBuildContainers(CDGraph& graph, int folder_depth)
{
    graph.containers.clear();

    std::unordered_map<std::string, int> folder_index;
    std::unordered_map<std::string, int> file_index;

    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const CDNode& node = graph.nodes[i];

        const std::string folder_key = CDTruncatePath(CDFolderOf(node.file_id), folder_depth);
        auto folder_it = folder_index.find(folder_key);
        if (folder_it == folder_index.end()) {
            CDContainer folder;
            folder.parent  = -1;
            folder.is_file = false;
            folder.label   = folder_key.empty() ? "(root)" : folder_key;
            folder_it = folder_index.emplace(folder_key,
                                             static_cast<int>(graph.containers.size())).first;
            graph.containers.push_back(std::move(folder));
        }

        auto file_it = file_index.find(node.file_id);
        if (file_it == file_index.end()) {
            const size_t slash = node.file_id.rfind('/');

            CDContainer file;
            file.parent  = folder_it->second;
            file.is_file = true;
            file.label   = (slash == std::string::npos) ? node.file_id
                                                        : node.file_id.substr(slash + 1);
            file_it = file_index.emplace(node.file_id,
                                         static_cast<int>(graph.containers.size())).first;
            graph.containers.push_back(std::move(file));
            graph.containers[static_cast<size_t>(folder_it->second)]
                .child_containers.push_back(file_it->second);
        }

        graph.containers[static_cast<size_t>(file_it->second)]
            .child_nodes.push_back(static_cast<int>(i));
    }
}

// ── Hierarchical layout ──────────────────────────────────────────────────────

void CDLayoutHierarchical(CDGraph& graph, const CDHierarchyMetrics& m)
{
    if (graph.containers.empty()) return;

    // node_id → container index, needed to lift class edges to file and folder level.
    std::unordered_map<int, int> node_id_to_file;
    for (size_t c = 0; c < graph.containers.size(); ++c)
        for (int ni : graph.containers[c].child_nodes)
            node_id_to_file.emplace(graph.nodes[static_cast<size_t>(ni)].node_id,
                                    static_cast<int>(c));

    // ── Pass 1: classes inside each file ─────────────────────────────────────
    for (CDContainer& file : graph.containers) {
        if (!file.is_file) continue;

        std::unordered_map<int, int> to_local;
        std::vector<CDBox>           boxes;
        boxes.reserve(file.child_nodes.size());
        for (int ni : file.child_nodes) {
            const CDNode& node = graph.nodes[static_cast<size_t>(ni)];
            to_local.emplace(node.node_id, static_cast<int>(boxes.size()));
            boxes.push_back(CDNodeSize(node, m.node));
        }

        const std::vector<ImVec2> local = CDLayeredLayout(
            boxes, LocalEdges(graph.edges, to_local), m.class_gap_x, m.class_gap_y, m.aspect);

        // Stash local coordinates; pass 3 turns them into absolute ones.
        for (size_t k = 0; k < file.child_nodes.size(); ++k)
            graph.nodes[static_cast<size_t>(file.child_nodes[k])].pos = local[k];

        const CDBox inner = CDBoundingSize(boxes, local);
        file.size = { inner.w + 2.0f * m.file_pad,
                      inner.h + m.file_header + 2.0f * m.file_pad };
    }

    // ── Pass 2: files inside each folder ─────────────────────────────────────
    for (CDContainer& folder : graph.containers) {
        if (folder.is_file) continue;

        std::unordered_map<int, int> file_to_local;  // container index → local index
        std::vector<CDBox>           boxes;
        boxes.reserve(folder.child_containers.size());
        for (int ci : folder.child_containers) {
            file_to_local.emplace(ci, static_cast<int>(boxes.size()));
            const ImVec2& s = graph.containers[static_cast<size_t>(ci)].size;
            boxes.push_back({s.x, s.y});
        }

        // Lift class edges to file level: an A→B between two files in this
        // folder becomes one edge between their boxes.
        std::unordered_map<int, int> node_to_local;
        for (const auto& kv : node_id_to_file) {
            const auto it = file_to_local.find(kv.second);
            if (it != file_to_local.end()) node_to_local.emplace(kv.first, it->second);
        }

        const std::vector<ImVec2> local = CDLayeredLayout(
            boxes, LocalEdges(graph.edges, node_to_local), m.file_gap_x, m.file_gap_y, m.aspect);

        for (size_t k = 0; k < folder.child_containers.size(); ++k)
            graph.containers[static_cast<size_t>(folder.child_containers[k])].pos = local[k];

        const CDBox inner = CDBoundingSize(boxes, local);
        folder.size = { inner.w + 2.0f * m.folder_pad,
                        inner.h + m.folder_header + 2.0f * m.folder_pad };
    }

    // ── Pass 3: folders across the canvas ────────────────────────────────────
    std::vector<int>             folders;
    std::unordered_map<int, int> folder_to_local;
    std::vector<CDBox>           boxes;
    for (size_t c = 0; c < graph.containers.size(); ++c) {
        if (graph.containers[c].is_file) continue;
        folder_to_local.emplace(static_cast<int>(c), static_cast<int>(boxes.size()));
        folders.push_back(static_cast<int>(c));
        boxes.push_back({graph.containers[c].size.x, graph.containers[c].size.y});
    }

    std::unordered_map<int, int> node_to_folder_local;
    for (const auto& kv : node_id_to_file) {
        const int  folder_idx = graph.containers[static_cast<size_t>(kv.second)].parent;
        const auto it         = folder_to_local.find(folder_idx);
        if (it != folder_to_local.end()) node_to_folder_local.emplace(kv.first, it->second);
    }

    const std::vector<ImVec2> folder_pos = CDLayeredLayout(
        boxes, LocalEdges(graph.edges, node_to_folder_local),
        m.folder_gap_x, m.folder_gap_y, m.aspect);

    // ── Resolve local coordinates into absolute ones, outermost first ────────
    for (size_t k = 0; k < folders.size(); ++k)
        graph.containers[static_cast<size_t>(folders[k])].pos = folder_pos[k];

    for (int fi : folders) {
        const CDContainer& folder = graph.containers[static_cast<size_t>(fi)];
        const ImVec2 origin = { folder.pos.x + m.folder_pad,
                                folder.pos.y + m.folder_header + m.folder_pad };

        for (int ci : folder.child_containers) {
            CDContainer& file = graph.containers[static_cast<size_t>(ci)];
            file.pos = { origin.x + file.pos.x, origin.y + file.pos.y };

            const ImVec2 inner = { file.pos.x + m.file_pad,
                                   file.pos.y + m.file_header + m.file_pad };
            for (int ni : file.child_nodes) {
                CDNode& node = graph.nodes[static_cast<size_t>(ni)];
                node.pos = { inner.x + node.pos.x, inner.y + node.pos.y };
            }
        }
    }

    graph.layout_valid = true;
}

void CDRefreshContainerBounds(CDGraph& graph, const CDHierarchyMetrics& m)
{
    // Files first — their bounds come from the class nodes.
    for (CDContainer& file : graph.containers) {
        if (!file.is_file || file.child_nodes.empty()) continue;

        float lo_x = FLT_MAX, lo_y = FLT_MAX, hi_x = -FLT_MAX, hi_y = -FLT_MAX;
        for (int ni : file.child_nodes) {
            const CDNode& node = graph.nodes[static_cast<size_t>(ni)];
            const CDBox   box  = CDNodeSize(node, m.node);
            lo_x = std::min(lo_x, node.pos.x);
            lo_y = std::min(lo_y, node.pos.y);
            hi_x = std::max(hi_x, node.pos.x + box.w);
            hi_y = std::max(hi_y, node.pos.y + box.h);
        }
        file.pos  = { lo_x - m.file_pad, lo_y - m.file_header - m.file_pad };
        file.size = { (hi_x - lo_x) + 2.0f * m.file_pad,
                      (hi_y - lo_y) + m.file_header + 2.0f * m.file_pad };
    }

    // Folders then wrap the files that were just refreshed.
    for (CDContainer& folder : graph.containers) {
        if (folder.is_file || folder.child_containers.empty()) continue;

        float lo_x = FLT_MAX, lo_y = FLT_MAX, hi_x = -FLT_MAX, hi_y = -FLT_MAX;
        for (int ci : folder.child_containers) {
            const CDContainer& file = graph.containers[static_cast<size_t>(ci)];
            lo_x = std::min(lo_x, file.pos.x);
            lo_y = std::min(lo_y, file.pos.y);
            hi_x = std::max(hi_x, file.pos.x + file.size.x);
            hi_y = std::max(hi_y, file.pos.y + file.size.y);
        }
        folder.pos  = { lo_x - m.folder_pad, lo_y - m.folder_header - m.folder_pad };
        folder.size = { (hi_x - lo_x) + 2.0f * m.folder_pad,
                        (hi_y - lo_y) + m.folder_header + 2.0f * m.folder_pad };
    }
}

} // namespace TS
