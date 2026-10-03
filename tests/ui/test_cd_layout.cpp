// Pure-geometry tests for cd_layout — no ImGui/imnodes context required.
//
// Suites:
//   NodeSize            — CDNodeSize / CDGraphNodeSizes deterministic geometry
//   ShelfPack           — CDShelfPack row filling, wrapping, overlap freedom
//   PreferredShelfWidth — CDPreferredShelfWidth sizing
//   LayeredLayout       — CDLayeredLayout layering, components, cycles
//   ContainerTree       — path helpers, depth selection, CDBuildContainers
//   Hierarchical        — CDLayoutHierarchical end to end

#include "ui/class_diagram/cd_layout.h"
#include "ui/ts_style.h"
#include <gtest/gtest.h>
#include <imgui.h>
#include <vector>

namespace {

// Two boxes overlap when they intersect on both axes. Touching edges are fine.
bool Overlaps(ImVec2 a_tl, TS::CDBox a, ImVec2 b_tl, TS::CDBox b)
{
    const bool x_apart = (a_tl.x + a.w <= b_tl.x) || (b_tl.x + b.w <= a_tl.x);
    const bool y_apart = (a_tl.y + a.h <= b_tl.y) || (b_tl.y + b.h <= a_tl.y);
    return !(x_apart || y_apart);
}

void ExpectNoOverlap(const std::vector<TS::CDBox>& boxes, const std::vector<ImVec2>& pos)
{
    ASSERT_EQ(boxes.size(), pos.size());
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j)
            EXPECT_FALSE(Overlaps(pos[i], boxes[i], pos[j], boxes[j]))
                << "box " << i << " overlaps box " << j;
}

// Deliberately ragged: the fixed-step grid this replaces broke on exactly this
// shape — tall boxes running past the row step, wide ones past the column step.
std::vector<TS::CDBox> RaggedBoxes()
{
    return {
        {220.0f,  90.0f}, {380.0f, 140.0f}, {220.0f, 420.0f}, {260.0f,  70.0f},
        {520.0f, 110.0f}, {220.0f, 260.0f}, {300.0f, 180.0f}, {240.0f,  95.0f},
        {200.0f, 610.0f}, {440.0f, 130.0f}, {220.0f,  85.0f}, {350.0f, 240.0f},
    };
}

TS::CDGraph GraphWithNodes(int n)
{
    TS::CDGraph g;
    for (int i = 0; i < n; ++i) {
        TS::CDNode node;
        node.node_id    = i;
        node.class_id   = "c" + std::to_string(i);
        node.class_name = "C" + std::to_string(i);
        node.pos        = {-1.0f, -1.0f};  // poison: must be overwritten
        g.nodes.push_back(node);
    }
    return g;
}

// Stand-in for the ts_style constants; the values only need to be distinct so a
// term dropped from the height formula shows up as a wrong total.
const TS::CDNodeMetrics kMetrics = {
    /*content_w*/ 220.0f, /*header_h*/ 36.0f, /*row_h*/ 18.0f,
    /*divider_h*/  12.0f, /*pad_x*/    12.0f, /*pad_y*/  6.0f,
};

TS::CDNode NodeWith(int fields, int methods)
{
    TS::CDNode n;
    n.node_id = 0;
    n.fields.resize(static_cast<size_t>(fields));
    n.methods.resize(static_cast<size_t>(methods));
    return n;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// NodeSize
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeSize, WidthIsConstantRegardlessOfContent)
{
    // The whole point of ellipsizing: text length can never change node width.
    const float expected = kMetrics.content_w + 2.0f * kMetrics.pad_x;
    EXPECT_FLOAT_EQ(expected, TS::CDNodeSize(NodeWith(0, 0),   kMetrics).w);
    EXPECT_FLOAT_EQ(expected, TS::CDNodeSize(NodeWith(3, 7),   kMetrics).w);
    EXPECT_FLOAT_EQ(expected, TS::CDNodeSize(NodeWith(40, 90), kMetrics).w);
}

TEST(NodeSize, HeightIsHeaderPlusRowsPlusDividerPlusPadding)
{
    // pad_y counts four times — imnodes pads the title bar band and then the
    // whole node. Verified against ImNodes::GetNodeDimensions at runtime.
    // 36 + (2 + 5) * 18 + 12 + 4 * 6 = 198
    EXPECT_FLOAT_EQ(198.0f, TS::CDNodeSize(NodeWith(2, 5), kMetrics).h);
}

TEST(NodeSize, EmptyNodeStillHasHeaderDividerAndPadding)
{
    EXPECT_FLOAT_EQ(36.0f + 12.0f + 24.0f, TS::CDNodeSize(NodeWith(0, 0), kMetrics).h);
}

TEST(NodeSize, FieldsAndMethodsContributeEqually)
{
    EXPECT_FLOAT_EQ(TS::CDNodeSize(NodeWith(6, 0), kMetrics).h,
                    TS::CDNodeSize(NodeWith(0, 6), kMetrics).h);
}

TEST(NodeSize, ScalesLinearlyWithZoom)
{
    // The reason for computing rather than measuring: at any zoom the node must
    // be exactly `zoom` times its logical size, or the gaps breathe as you zoom.
    const TS::CDNode node = NodeWith(3, 4);
    const TS::CDBox  base = TS::CDNodeSize(node, kMetrics);

    for (float zoom : {0.4f, 0.75f, 1.6f, 2.0f}) {
        const TS::CDNodeMetrics m = {
            kMetrics.content_w * zoom, kMetrics.header_h * zoom, kMetrics.row_h    * zoom,
            kMetrics.divider_h * zoom, kMetrics.pad_x    * zoom, kMetrics.pad_y    * zoom,
        };
        const TS::CDBox scaled = TS::CDNodeSize(node, m);
        EXPECT_FLOAT_EQ(base.w * zoom, scaled.w);
        EXPECT_FLOAT_EQ(base.h * zoom, scaled.h);
    }
}

TEST(NodeSize, GraphSizesAreIndexAligned)
{
    TS::CDGraph graph;
    graph.nodes.push_back(NodeWith(1, 1));
    graph.nodes.push_back(NodeWith(9, 9));

    const std::vector<TS::CDBox> sizes = TS::CDGraphNodeSizes(graph, kMetrics);
    ASSERT_EQ(2u, sizes.size());
    EXPECT_FLOAT_EQ(TS::CDNodeSize(graph.nodes[0], kMetrics).h, sizes[0].h);
    EXPECT_FLOAT_EQ(TS::CDNodeSize(graph.nodes[1], kMetrics).h, sizes[1].h);
    EXPECT_LT(sizes[0].h, sizes[1].h);
}

// ─────────────────────────────────────────────────────────────────────────────
// ShelfPack
// ─────────────────────────────────────────────────────────────────────────────

TEST(ShelfPack, EmptyInputProducesEmptyOutput)
{
    EXPECT_TRUE(TS::CDShelfPack({}, 800.0f, 40.0f).empty());
}

TEST(ShelfPack, SingleRowWhenEverythingFits)
{
    const std::vector<TS::CDBox> boxes = {{100.0f, 50.0f}, {100.0f, 80.0f}, {100.0f, 30.0f}};
    const std::vector<ImVec2>    pos   = TS::CDShelfPack(boxes, 1000.0f, 10.0f);

    for (const ImVec2& p : pos) EXPECT_FLOAT_EQ(0.0f, p.y);
    EXPECT_FLOAT_EQ(  0.0f, pos[0].x);
    EXPECT_FLOAT_EQ(110.0f, pos[1].x);
    EXPECT_FLOAT_EQ(220.0f, pos[2].x);
}

TEST(ShelfPack, WrapsToNextRowBelowTallestBoxOfClosedRow)
{
    // Row 0 takes the first two (100 + 10 + 100 = 210); the third would reach
    // 320 > 250, so it wraps below the tallest of row 0 (80) plus the gap.
    const std::vector<TS::CDBox> boxes = {{100.0f, 50.0f}, {100.0f, 80.0f}, {100.0f, 30.0f}};
    const std::vector<ImVec2>    pos   = TS::CDShelfPack(boxes, 250.0f, 10.0f);

    EXPECT_FLOAT_EQ( 0.0f, pos[1].y);
    EXPECT_FLOAT_EQ( 0.0f, pos[2].x);
    EXPECT_FLOAT_EQ(90.0f, pos[2].y);
}

TEST(ShelfPack, BoxWiderThanRowGetsItsOwnRow)
{
    // The oversized box must still be placed — an unconditional wrap would spin.
    const std::vector<TS::CDBox> boxes = {{50.0f, 20.0f}, {900.0f, 40.0f}, {50.0f, 20.0f}};
    const std::vector<ImVec2>    pos   = TS::CDShelfPack(boxes, 200.0f, 10.0f);

    EXPECT_FLOAT_EQ(0.0f, pos[1].x);
    EXPECT_LT(pos[0].y, pos[1].y);
    EXPECT_LT(pos[1].y, pos[2].y);
    ExpectNoOverlap(boxes, pos);
}

TEST(ShelfPack, RaggedSizesNeverOverlap)
{
    const std::vector<TS::CDBox> boxes = RaggedBoxes();
    for (float max_w : {300.0f, 700.0f, 1200.0f, 5000.0f})
        ExpectNoOverlap(boxes, TS::CDShelfPack(boxes, max_w, 48.0f));
}

TEST(ShelfPack, ZeroGapStillNeverOverlaps)
{
    const std::vector<TS::CDBox> boxes = RaggedBoxes();
    ExpectNoOverlap(boxes, TS::CDShelfPack(boxes, 900.0f, 0.0f));
}

TEST(ShelfPack, PreservesInputOrder)
{
    // Reading order: never move left without also moving down a row.
    const std::vector<TS::CDBox> boxes = RaggedBoxes();
    const std::vector<ImVec2>    pos   = TS::CDShelfPack(boxes, 900.0f, 48.0f);

    for (size_t i = 1; i < pos.size(); ++i)
        EXPECT_TRUE(pos[i].x > pos[i - 1].x || pos[i].y > pos[i - 1].y)
            << "box " << i << " went backwards";
}

TEST(ShelfPack, IsDeterministic)
{
    const std::vector<TS::CDBox> boxes = RaggedBoxes();
    const std::vector<ImVec2>    a     = TS::CDShelfPack(boxes, 900.0f, 48.0f);
    const std::vector<ImVec2>    b     = TS::CDShelfPack(boxes, 900.0f, 48.0f);

    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i].x, b[i].x);
        EXPECT_FLOAT_EQ(a[i].y, b[i].y);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PreferredShelfWidth
// ─────────────────────────────────────────────────────────────────────────────

TEST(PreferredShelfWidth, EmptyInputIsZero)
{
    EXPECT_FLOAT_EQ(0.0f, TS::CDPreferredShelfWidth({}, 48.0f, 1.6f));
}

TEST(PreferredShelfWidth, NeverNarrowerThanWidestBox)
{
    const std::vector<TS::CDBox> boxes = {{40.0f, 10.0f}, {900.0f, 10.0f}};
    EXPECT_GE(TS::CDPreferredShelfWidth(boxes, 0.0f, 1.6f), 900.0f);
}

TEST(PreferredShelfWidth, LargerAspectGivesWiderRow)
{
    const std::vector<TS::CDBox> boxes = RaggedBoxes();
    EXPECT_LT(TS::CDPreferredShelfWidth(boxes, 48.0f, 1.0f),
              TS::CDPreferredShelfWidth(boxes, 48.0f, 2.5f));
}

TEST(PreferredShelfWidth, ProducesRoughlyTheRequestedAspect)
{
    // Uniform boxes so packing waste stays small enough to assert on.
    std::vector<TS::CDBox> boxes(64, {200.0f, 120.0f});
    const float gap    = 40.0f;
    const float aspect = 1.6f;
    const std::vector<ImVec2> pos =
        TS::CDShelfPack(boxes, TS::CDPreferredShelfWidth(boxes, gap, aspect), gap);

    float w = 0.0f, h = 0.0f;
    for (size_t i = 0; i < boxes.size(); ++i) {
        w = std::max(w, pos[i].x + boxes[i].w);
        h = std::max(h, pos[i].y + boxes[i].h);
    }
    EXPECT_GT(w / h, aspect * 0.5f);
    EXPECT_LT(w / h, aspect * 2.0f);
}


// ─────────────────────────────────────────────────────────────────────────────
// LayeredLayout
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::vector<TS::CDBox> UniformBoxes(int n, float w = 100.0f, float h = 40.0f)
{
    return std::vector<TS::CDBox>(static_cast<size_t>(n), TS::CDBox{w, h});
}

} // namespace

TEST(LayeredLayout, EmptyInputProducesEmptyOutput)
{
    EXPECT_TRUE(TS::CDLayeredLayout({}, {}, 40.0f, 20.0f, 1.6f).empty());
}

TEST(LayeredLayout, EdgeTargetSitsInALaterLayer)
{
    // A → B → C must read left to right, matching the left/right node pins.
    const std::vector<TS::CDBox> boxes = UniformBoxes(3);
    const std::vector<ImVec2>    pos   =
        TS::CDLayeredLayout(boxes, {{0, 1}, {1, 2}}, 40.0f, 20.0f, 1.6f);

    EXPECT_LT(pos[0].x, pos[1].x);
    EXPECT_LT(pos[1].x, pos[2].x);
}

TEST(LayeredLayout, LongestPathDecidesTheLayer)
{
    // 0→1, 1→2, 0→2: node 2 follows the longer path, so it lands past node 1
    // rather than sharing a layer with it.
    const std::vector<TS::CDBox> boxes = UniformBoxes(3);
    const std::vector<ImVec2>    pos   =
        TS::CDLayeredLayout(boxes, {{0, 1}, {1, 2}, {0, 2}}, 40.0f, 20.0f, 1.6f);

    EXPECT_LT(pos[1].x, pos[2].x);
}

TEST(LayeredLayout, SiblingsShareALayerAndStackVertically)
{
    // 0→1, 0→2: both targets are one step away, so they share an x and differ in y.
    const std::vector<TS::CDBox> boxes = UniformBoxes(3);
    const std::vector<ImVec2>    pos   =
        TS::CDLayeredLayout(boxes, {{0, 1}, {0, 2}}, 40.0f, 20.0f, 1.6f);

    EXPECT_FLOAT_EQ(pos[1].x, pos[2].x);
    EXPECT_NE(pos[1].y, pos[2].y);
}

TEST(LayeredLayout, CyclesDoNotHangOrOverlap)
{
    // Call graphs are full of cycles; the layering must still terminate.
    const std::vector<TS::CDBox> boxes = UniformBoxes(4);
    const std::vector<ImVec2>    pos   =
        TS::CDLayeredLayout(boxes, {{0, 1}, {1, 2}, {2, 3}, {3, 0}}, 40.0f, 20.0f, 1.6f);

    ExpectNoOverlap(boxes, pos);
}

TEST(LayeredLayout, SelfEdgeIsHarmless)
{
    const std::vector<TS::CDBox> boxes = UniformBoxes(2);
    const std::vector<ImVec2>    pos   =
        TS::CDLayeredLayout(boxes, {{0, 0}, {0, 1}}, 40.0f, 20.0f, 1.6f);

    EXPECT_LT(pos[0].x, pos[1].x);
    ExpectNoOverlap(boxes, pos);
}

TEST(LayeredLayout, IsolatedNodesDoNotPileIntoOneColumn)
{
    // The regression this guards: with every node in layer 0, 40 unconnected
    // classes would form a single endless column instead of a packed block.
    const std::vector<TS::CDBox> boxes = UniformBoxes(40);
    const std::vector<ImVec2>    pos   = TS::CDLayeredLayout(boxes, {}, 40.0f, 20.0f, 1.6f);

    const TS::CDBox bounds = TS::CDBoundingSize(boxes, pos);
    EXPECT_GT(bounds.w, bounds.h * 0.5f) << "isolated nodes collapsed into a column";
    ExpectNoOverlap(boxes, pos);
}

TEST(LayeredLayout, DisconnectedComponentsDoNotOverlap)
{
    const std::vector<TS::CDBox> boxes = UniformBoxes(6);
    const std::vector<ImVec2>    pos   =
        TS::CDLayeredLayout(boxes, {{0, 1}, {1, 2}, {3, 4}}, 40.0f, 20.0f, 1.6f);

    ExpectNoOverlap(boxes, pos);
}

TEST(LayeredLayout, RaggedBoxesNeverOverlap)
{
    const std::vector<TS::CDBox> boxes = RaggedBoxes();
    const std::vector<ImVec2>    pos   = TS::CDLayeredLayout(
        boxes, {{0, 1}, {1, 2}, {2, 3}, {4, 5}, {6, 7}, {7, 8}, {9, 10}}, 48.0f, 32.0f, 1.6f);

    ExpectNoOverlap(boxes, pos);
}

TEST(LayeredLayout, IsDeterministic)
{
    const std::vector<TS::CDBox>      boxes = RaggedBoxes();
    const std::vector<TS::CDLayerEdge> e    = {{0, 1}, {1, 2}, {3, 4}, {5, 6}, {6, 2}};

    const std::vector<ImVec2> a = TS::CDLayeredLayout(boxes, e, 48.0f, 32.0f, 1.6f);
    const std::vector<ImVec2> b = TS::CDLayeredLayout(boxes, e, 48.0f, 32.0f, 1.6f);

    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i].x, b[i].x);
        EXPECT_FLOAT_EQ(a[i].y, b[i].y);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ContainerTree
// ─────────────────────────────────────────────────────────────────────────────

TEST(ContainerTree, FolderOfStripsTheFilename)
{
    EXPECT_EQ("sherlock_project", TS::CDFolderOf("sherlock_project/notify.py"));
    EXPECT_EQ("src/api/v2",       TS::CDFolderOf("src/api/v2/order.py"));
    EXPECT_EQ("",                 TS::CDFolderOf("setup.py"));
}

TEST(ContainerTree, TruncatePathKeepsLeadingSegments)
{
    EXPECT_EQ("a",     TS::CDTruncatePath("a/b/c", 1));
    EXPECT_EQ("a/b",   TS::CDTruncatePath("a/b/c", 2));
    EXPECT_EQ("a/b/c", TS::CDTruncatePath("a/b/c", 3));
    EXPECT_EQ("a/b/c", TS::CDTruncatePath("a/b/c", 9));  // fewer segments than asked
    EXPECT_EQ("",      TS::CDTruncatePath("a/b/c", 0));
    EXPECT_EQ("",      TS::CDTruncatePath("", 2));
}

TEST(ContainerTree, ShallowRepoStopsAtDepthOne)
{
    // The real test DB: two top-level folders. Going deeper cannot split them
    // further, so the search must stop rather than run to the depth cap.
    const std::vector<std::string> files = {
        "sherlock_project/notify.py", "sherlock_project/result.py",
        "sherlock_project/sites.py",  "tests/test_probes.py",
    };
    EXPECT_EQ(1, TS::CDChooseFolderDepth(files, 5, 15));
}

TEST(ContainerTree, DeepRepoDescendsUntilGroupsAppear)
{
    // Everything under "src": depth 1 yields one useless group, so go deeper.
    std::vector<std::string> files;
    for (const char* mod : {"a", "b", "c", "d", "e", "f"})
        files.push_back(std::string("src/") + mod + "/impl.py");

    EXPECT_EQ(2, TS::CDChooseFolderDepth(files, 5, 15));
}

TEST(ContainerTree, StopsBeforeFragmentingPastTheCap)
{
    std::vector<std::string> files;
    for (int i = 0; i < 40; ++i)
        files.push_back("src/mod" + std::to_string(i) + "/impl.py");

    // Depth 2 would make 40 groups — over the cap, so depth 1 is kept.
    EXPECT_EQ(1, TS::CDChooseFolderDepth(files, 5, 15));
}

TEST(ContainerTree, EmptyInputIsSafe)
{
    EXPECT_EQ(1, TS::CDChooseFolderDepth({}, 5, 15));
}

namespace {

TS::CDGraph GraphFromFileIds(const std::vector<std::string>& file_ids)
{
    TS::CDGraph graph;
    for (size_t i = 0; i < file_ids.size(); ++i) {
        TS::CDNode n;
        n.node_id    = static_cast<int>(i);
        n.file_id    = file_ids[i];
        n.class_name = "C" + std::to_string(i);
        n.methods.resize(2);
        graph.nodes.push_back(n);
    }
    return graph;
}

const TS::CDHierarchyMetrics kHierarchy = {
    kMetrics,
    /*class */ 56.0f, 32.0f,
    /*file  */ 72.0f, 56.0f,
    /*folder*/ 96.0f, 80.0f,
    /*file  pad/header*/ 16.0f, 24.0f,
    /*folder pad/header*/ 20.0f, 30.0f,
    /*aspect*/ 1.6f,
};

} // namespace

TEST(ContainerTree, GroupsNodesByFileAndFolder)
{
    TS::CDGraph graph = GraphFromFileIds({
        "pkg/a.py", "pkg/a.py", "pkg/b.py", "tests/t.py",
    });
    TS::CDBuildContainers(graph, 1);

    int files = 0, folders = 0;
    for (const TS::CDContainer& c : graph.containers) (c.is_file ? files : folders)++;
    EXPECT_EQ(3, files);    // a.py, b.py, t.py
    EXPECT_EQ(2, folders);  // pkg, tests

    for (const TS::CDContainer& c : graph.containers) {
        if (!c.is_file) continue;
        if (c.label == "a.py") EXPECT_EQ(2u, c.child_nodes.size());
        else                   EXPECT_EQ(1u, c.child_nodes.size());
        EXPECT_NE(-1, c.parent);
    }
}

TEST(ContainerTree, EveryNodeLandsInExactlyOneFile)
{
    TS::CDGraph graph = GraphFromFileIds({"pkg/a.py", "pkg/b.py", "root.py", "pkg/a.py"});
    TS::CDBuildContainers(graph, 1);

    std::vector<int> seen(graph.nodes.size(), 0);
    for (const TS::CDContainer& c : graph.containers)
        for (int ni : c.child_nodes) ++seen[static_cast<size_t>(ni)];

    for (size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(1, seen[i]) << "node " << i;
}

TEST(ContainerTree, FileWithoutFolderGetsARootLabel)
{
    TS::CDGraph graph = GraphFromFileIds({"setup.py"});
    TS::CDBuildContainers(graph, 1);

    bool found = false;
    for (const TS::CDContainer& c : graph.containers)
        if (!c.is_file && c.label == "(root)") found = true;
    EXPECT_TRUE(found);
}

TEST(ContainerTree, RebuildingReplacesRatherThanAppends)
{
    TS::CDGraph graph = GraphFromFileIds({"pkg/a.py", "pkg/b.py"});
    TS::CDBuildContainers(graph, 1);
    const size_t first = graph.containers.size();
    TS::CDBuildContainers(graph, 1);
    EXPECT_EQ(first, graph.containers.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// Hierarchical
// ─────────────────────────────────────────────────────────────────────────────

TEST(Hierarchical, EmptyContainersLeaveLayoutInvalid)
{
    // The caller retries next frame rather than laying out against nothing.
    TS::CDGraph graph = GraphFromFileIds({"pkg/a.py"});
    TS::CDLayoutHierarchical(graph, kHierarchy);
    EXPECT_FALSE(graph.layout_valid);
}

TEST(Hierarchical, NodesNeverOverlapAcrossFilesAndFolders)
{
    TS::CDGraph graph = GraphFromFileIds({
        "pkg/a.py", "pkg/a.py", "pkg/a.py", "pkg/b.py", "pkg/b.py",
        "tests/t.py", "tests/u.py", "root.py",
    });
    graph.edges = {
        {0, 0, 1, TS::CDEdgeType::Association},   // within a.py
        {1, 3, 5, TS::CDEdgeType::Association},   // pkg/b.py → tests/t.py
    };
    TS::CDBuildContainers(graph, 1);
    TS::CDLayoutHierarchical(graph, kHierarchy);

    ASSERT_TRUE(graph.layout_valid);

    std::vector<TS::CDBox> boxes;
    std::vector<ImVec2>    pos;
    for (const TS::CDNode& n : graph.nodes) {
        boxes.push_back(TS::CDNodeSize(n, kMetrics));
        pos.push_back(n.pos);
    }
    ExpectNoOverlap(boxes, pos);
}

TEST(Hierarchical, ClassesOfOneFileStayInsideItsContainer)
{
    TS::CDGraph graph = GraphFromFileIds({"pkg/a.py", "pkg/a.py", "pkg/b.py", "tests/t.py"});
    TS::CDBuildContainers(graph, 1);
    TS::CDLayoutHierarchical(graph, kHierarchy);

    for (const TS::CDContainer& c : graph.containers) {
        if (!c.is_file) continue;
        for (int ni : c.child_nodes) {
            const TS::CDNode& n   = graph.nodes[static_cast<size_t>(ni)];
            const TS::CDBox   box = TS::CDNodeSize(n, kMetrics);
            EXPECT_GE(n.pos.x,             c.pos.x);
            EXPECT_GE(n.pos.y,             c.pos.y);
            EXPECT_LE(n.pos.x + box.w,     c.pos.x + c.size.x + 0.01f);
            EXPECT_LE(n.pos.y + box.h,     c.pos.y + c.size.y + 0.01f);
        }
    }
}

TEST(Hierarchical, FilesStayInsideTheirFolder)
{
    TS::CDGraph graph = GraphFromFileIds({"pkg/a.py", "pkg/b.py", "pkg/c.py", "tests/t.py"});
    TS::CDBuildContainers(graph, 1);
    TS::CDLayoutHierarchical(graph, kHierarchy);

    for (size_t i = 0; i < graph.containers.size(); ++i) {
        const TS::CDContainer& folder = graph.containers[i];
        if (folder.is_file) continue;
        for (int ci : folder.child_containers) {
            const TS::CDContainer& file = graph.containers[static_cast<size_t>(ci)];
            EXPECT_GE(file.pos.x,                folder.pos.x);
            EXPECT_GE(file.pos.y,                folder.pos.y);
            EXPECT_LE(file.pos.x + file.size.x,  folder.pos.x + folder.size.x + 0.01f);
            EXPECT_LE(file.pos.y + file.size.y,  folder.pos.y + folder.size.y + 0.01f);
        }
    }
}

TEST(Hierarchical, FoldersDoNotOverlap)
{
    TS::CDGraph graph = GraphFromFileIds({
        "a/x.py", "a/y.py", "b/x.py", "c/x.py", "c/y.py", "c/z.py", "d/x.py",
    });
    TS::CDBuildContainers(graph, 1);
    TS::CDLayoutHierarchical(graph, kHierarchy);

    std::vector<TS::CDBox> boxes;
    std::vector<ImVec2>    pos;
    for (const TS::CDContainer& c : graph.containers) {
        if (c.is_file) continue;
        boxes.push_back({c.size.x, c.size.y});
        pos.push_back(c.pos);
    }
    ExpectNoOverlap(boxes, pos);
}

TEST(Hierarchical, IsDeterministic)
{
    const std::vector<std::string> files = {
        "pkg/a.py", "pkg/a.py", "pkg/b.py", "tests/t.py", "tests/u.py",
    };
    std::vector<ImVec2> run[2];

    for (int r = 0; r < 2; ++r) {
        TS::CDGraph graph = GraphFromFileIds(files);
        graph.edges = {{0, 0, 2, TS::CDEdgeType::Association},
                       {1, 2, 3, TS::CDEdgeType::Dependency}};
        TS::CDBuildContainers(graph, 1);
        TS::CDLayoutHierarchical(graph, kHierarchy);
        for (const TS::CDNode& n : graph.nodes) run[r].push_back(n.pos);
    }

    ASSERT_EQ(run[0].size(), run[1].size());
    for (size_t i = 0; i < run[0].size(); ++i) {
        EXPECT_FLOAT_EQ(run[0][i].x, run[1][i].x);
        EXPECT_FLOAT_EQ(run[0][i].y, run[1][i].y);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RefreshContainerBounds
// ─────────────────────────────────────────────────────────────────────────────

namespace {

TS::CDGraph LaidOutGraph()
{
    TS::CDGraph graph = GraphFromFileIds({"pkg/a.py", "pkg/a.py", "pkg/b.py", "tests/t.py"});
    TS::CDBuildContainers(graph, 1);
    TS::CDLayoutHierarchical(graph, kHierarchy);
    return graph;
}

const TS::CDContainer& FindContainer(const TS::CDGraph& g, const std::string& label)
{
    for (const TS::CDContainer& c : g.containers)
        if (c.label == label) return c;
    ADD_FAILURE() << "no container labelled " << label;
    return g.containers.front();
}

} // namespace

TEST(RefreshContainerBounds, ReproducesTheLayoutWhenNothingMoved)
{
    TS::CDGraph graph = LaidOutGraph();

    std::vector<ImVec2> pos, size;
    for (const TS::CDContainer& c : graph.containers) { pos.push_back(c.pos); size.push_back(c.size); }

    TS::CDRefreshContainerBounds(graph, kHierarchy);

    for (size_t i = 0; i < graph.containers.size(); ++i) {
        EXPECT_NEAR(pos[i].x,  graph.containers[i].pos.x,  0.01f) << "container " << i;
        EXPECT_NEAR(pos[i].y,  graph.containers[i].pos.y,  0.01f) << "container " << i;
        EXPECT_NEAR(size[i].x, graph.containers[i].size.x, 0.01f) << "container " << i;
        EXPECT_NEAR(size[i].y, graph.containers[i].size.y, 0.01f) << "container " << i;
    }
}

TEST(RefreshContainerBounds, IsIdempotent)
{
    TS::CDGraph graph = LaidOutGraph();
    graph.nodes[0].pos = {2000.0f, 1500.0f};   // as if dragged

    TS::CDRefreshContainerBounds(graph, kHierarchy);
    const ImVec2 once_pos  = FindContainer(graph, "a.py").pos;
    const ImVec2 once_size = FindContainer(graph, "a.py").size;

    TS::CDRefreshContainerBounds(graph, kHierarchy);
    EXPECT_FLOAT_EQ(once_pos.x,  FindContainer(graph, "a.py").pos.x);
    EXPECT_FLOAT_EQ(once_size.x, FindContainer(graph, "a.py").size.x);
}

TEST(RefreshContainerBounds, FileBoxFollowsADraggedNode)
{
    // Without this, dragging a class would leave it sitting outside its own
    // file boundary.
    TS::CDGraph graph = LaidOutGraph();
    const float before = FindContainer(graph, "a.py").size.x;

    graph.nodes[0].pos.x += 900.0f;
    TS::CDRefreshContainerBounds(graph, kHierarchy);

    const TS::CDContainer& file = FindContainer(graph, "a.py");
    EXPECT_GT(file.size.x, before + 800.0f);

    for (int ni : file.child_nodes) {
        const TS::CDNode& n   = graph.nodes[static_cast<size_t>(ni)];
        const TS::CDBox   box = TS::CDNodeSize(n, kMetrics);
        EXPECT_GE(n.pos.x,         file.pos.x);
        EXPECT_LE(n.pos.x + box.w, file.pos.x + file.size.x + 0.01f);
    }
}

TEST(RefreshContainerBounds, FolderBoxFollowsItsFiles)
{
    TS::CDGraph graph = LaidOutGraph();
    graph.nodes[0].pos.y += 1200.0f;
    TS::CDRefreshContainerBounds(graph, kHierarchy);

    const TS::CDContainer& folder = FindContainer(graph, "pkg");
    for (int ci : folder.child_containers) {
        const TS::CDContainer& file = graph.containers[static_cast<size_t>(ci)];
        EXPECT_GE(file.pos.y,                folder.pos.y);
        EXPECT_LE(file.pos.y + file.size.y,  folder.pos.y + folder.size.y + 0.01f);
    }
}

TEST(RefreshContainerBounds, LeavesTheHeaderStripAboveTheChildren)
{
    // Phase 2 draws the label there; it must never be covered by a node.
    TS::CDGraph graph = LaidOutGraph();
    TS::CDRefreshContainerBounds(graph, kHierarchy);

    for (const TS::CDContainer& c : graph.containers) {
        if (!c.is_file) continue;
        for (int ni : c.child_nodes)
            EXPECT_GE(graph.nodes[static_cast<size_t>(ni)].pos.y,
                      c.pos.y + kHierarchy.file_header);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SemanticZoom
// ─────────────────────────────────────────────────────────────────────────────

TEST(SemanticZoom, FadeIsClampedOutsideTheBand)
{
    EXPECT_FLOAT_EQ(0.0f, TS::CDFade(0.10f, 0.20f, 0.40f));
    EXPECT_FLOAT_EQ(0.0f, TS::CDFade(0.20f, 0.20f, 0.40f));
    EXPECT_FLOAT_EQ(1.0f, TS::CDFade(0.40f, 0.20f, 0.40f));
    EXPECT_FLOAT_EQ(1.0f, TS::CDFade(9.00f, 0.20f, 0.40f));
}

TEST(SemanticZoom, FadeRisesSmoothlyThroughTheBand)
{
    EXPECT_FLOAT_EQ(0.5f, TS::CDFade(0.30f, 0.20f, 0.40f));   // smoothstep midpoint

    float prev = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float v = TS::CDFade(0.20f + 0.01f * static_cast<float>(i), 0.20f, 0.40f);
        EXPECT_GE(v, prev) << "fade went backwards";
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
        prev = v;
    }
}

TEST(SemanticZoom, MemberRowsGoBeforeTheNodesDo)
{
    // Detail has to drop out first: nodes vanishing while still showing their
    // rows would skip the summary step the whole scheme is built around.
    EXPECT_LT(TS::CD_NODE_FADE_HI, TS::CD_MEMBER_FADE_LO);
    EXPECT_LT(TS::CD_NODE_FADE_LO, TS::CD_NODE_FADE_HI);
    EXPECT_LT(TS::CD_MEMBER_FADE_LO, TS::CD_MEMBER_FADE_HI);
}

TEST(SemanticZoom, NodesAreGoneAtMinimumZoom)
{
    // ZOOM_MIN must land below the node fade, or the overview never reaches the
    // file-structure view it exists for.
    EXPECT_FLOAT_EQ(0.0f, TS::CDFade(TS::ZOOM_MIN, TS::CD_NODE_FADE_LO, TS::CD_NODE_FADE_HI));
    EXPECT_FLOAT_EQ(1.0f, TS::CDFade(TS::ZOOM_MAX, TS::CD_MEMBER_FADE_LO, TS::CD_MEMBER_FADE_HI));
}

TEST(SemanticZoom, LabelFloorBeatsScalingAtMinimumZoom)
{
    // The whole point of the floor: at ZOOM_MIN a scaled label is about a pixel.
    const float scaled = TS::FONT_SIZE_BASE * TS::ZOOM_MIN;
    EXPECT_LT(scaled, 2.0f);
    EXPECT_GT(TS::CD_LABEL_MIN_PX, scaled * 4.0f);
}
