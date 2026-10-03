#include "algo/AlgoRunner.h"
#include "algo/Graph.h"
#include <gtest/gtest.h>
#include <algorithm>

// ── 헬퍼 ──────────────────────────────────────────────────────────────────────

static const ReadingEntry* find_entry(const AlgoRunResult& r, const std::string& eid)
{
    for (const auto& e : r.entries)
        if (e.entity_id == eid) return &e;
    return nullptr;
}

// 엔트리를 file_rank(파일) 또는 local_rank(함수) 기준으로 정렬해 반환
static std::vector<const ReadingEntry*> entries_for_file(const AlgoRunResult& r,
                                                          const std::string& fid)
{
    std::vector<const ReadingEntry*> out;
    for (const auto& e : r.entries)
        if (e.file_id == fid && e.entity_type != "file") out.push_back(&e);
    std::sort(out.begin(), out.end(),
              [](const ReadingEntry* a, const ReadingEntry* b) {
                  return a->local_rank < b->local_rank;
              });
    return out;
}

// AlgoPassResult를 균일 점수로 간단히 생성
static AlgoPassResult flat_pass(const Graph& g, std::vector<NodeId> seq, double score = 0.5)
{
    int N = g.size();
    return {
        std::vector<double>(N, score),
        std::vector<double>(N, score),
        std::vector<double>(N, score),
        std::move(seq),
    };
}

// ── 파일 엔트리 ───────────────────────────────────────────────────────────────

// seq 순서대로 file_rank 1, 2, ... 가 부여되는지 확인
TEST(AlgoMerge, file_rank_follows_seq_order)
{
    Graph file_g;
    NodeId f1 = file_g.get_or_add("f1");
    NodeId f2 = file_g.get_or_add("f2");

    GraphBuilderResult gbr;
    gbr.file_loc_map = {{"f1", 100}, {"f2", 200}};

    // seq: f2 먼저 → file_rank 1, f1 → file_rank 2
    auto file_res = flat_pass(file_g, {f2, f1});
    auto func_res = flat_pass(Graph{}, {});

    auto out = AlgoRunner::merge(file_res, file_g, func_res, Graph{}, gbr);
    ASSERT_EQ(out.entries.size(), 2u);

    const auto* ef2 = find_entry(out, "f2");
    const auto* ef1 = find_entry(out, "f1");
    ASSERT_NE(ef2, nullptr);
    ASSERT_NE(ef1, nullptr);
    EXPECT_EQ(ef2->file_rank, 1);
    EXPECT_EQ(ef1->file_rank, 2);
}

// file_loc_map에 없는 외부 노드를 걸러낸 뒤 file_rank에 갭이 없어야 함
TEST(AlgoMerge, external_nodes_filtered_and_rank_is_gap_free)
{
    Graph file_g;
    NodeId f1  = file_g.get_or_add("f1");
    NodeId ext = file_g.get_or_add("os");   // 외부 패키지
    NodeId f2  = file_g.get_or_add("f2");

    GraphBuilderResult gbr;
    gbr.file_loc_map = {{"f1", 100}, {"f2", 200}};  // "os" 없음

    // seq: os(외부) → f1 → f2
    auto file_res = flat_pass(file_g, {ext, f1, f2});
    auto func_res = flat_pass(Graph{}, {});

    auto out = AlgoRunner::merge(file_res, file_g, func_res, Graph{}, gbr);

    // "os"는 제외되어야 하고, f1=1, f2=2 (갭 없음)
    EXPECT_EQ(out.entries.size(), 2u);
    EXPECT_EQ(find_entry(out, "os"), nullptr);

    const auto* ef1 = find_entry(out, "f1");
    const auto* ef2 = find_entry(out, "f2");
    ASSERT_NE(ef1, nullptr);
    ASSERT_NE(ef2, nullptr);
    EXPECT_EQ(ef1->file_rank, 1);
    EXPECT_EQ(ef2->file_rank, 2);
}

// 파일 엔트리의 entity_type, file_id, local_rank 필드 확인
TEST(AlgoMerge, file_entry_fields)
{
    Graph file_g;
    NodeId f1 = file_g.get_or_add("f1");

    GraphBuilderResult gbr;
    gbr.file_loc_map = {{"f1", 50}};

    auto file_res = flat_pass(file_g, {f1});
    auto out = AlgoRunner::merge(file_res, file_g, flat_pass(Graph{}, {}), Graph{}, gbr);

    ASSERT_EQ(out.entries.size(), 1u);
    const auto& e = out.entries[0];
    EXPECT_EQ(e.entity_type, "file");
    EXPECT_EQ(e.file_id,     "f1");
    EXPECT_EQ(e.local_rank,  0);   // 파일 엔트리는 local_rank=0
}

// ── 함수/클래스 엔트리 ─────────────────────────────────────────────────────────

// 같은 파일 내 함수들이 global_rank(= func seq 순서) 대로 local_rank를 받는지 확인
TEST(AlgoMerge, local_rank_follows_global_seq)
{
    Graph func_g;
    NodeId fn1 = func_g.get_or_add("fn1");
    NodeId fn2 = func_g.get_or_add("fn2");

    GraphBuilderResult gbr;
    gbr.entity_file_map  = {{"fn1", "f1"}, {"fn2", "f1"}};
    gbr.entity_type_map  = {{"fn1", "function"}, {"fn2", "function"}};

    // seq: fn2가 global_rank 0 (먼저 읽음) → local_rank 1
    auto func_res = flat_pass(func_g, {fn2, fn1});
    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    ASSERT_EQ(out.entries.size(), 2u);
    const auto* e1 = find_entry(out, "fn1");
    const auto* e2 = find_entry(out, "fn2");
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->local_rank, 1);
    EXPECT_EQ(e1->local_rank, 2);
}

// 두 파일 각각 독립적으로 local_rank 1부터 시작하는지 확인
TEST(AlgoMerge, local_rank_independent_per_file)
{
    Graph func_g;
    NodeId fn1 = func_g.get_or_add("fn1");   // f1 소속
    NodeId fn2 = func_g.get_or_add("fn2");   // f1 소속
    NodeId fn3 = func_g.get_or_add("fn3");   // f2 소속

    GraphBuilderResult gbr;
    gbr.entity_file_map = {{"fn1","f1"}, {"fn2","f1"}, {"fn3","f2"}};
    gbr.entity_type_map = {{"fn1","function"}, {"fn2","function"}, {"fn3","function"}};

    // seq: fn3(global 0) → fn1(global 1) → fn2(global 2)
    auto func_res = flat_pass(func_g, {fn3, fn1, fn2});
    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    ASSERT_EQ(out.entries.size(), 3u);

    // f2: fn3만 있으므로 local_rank=1
    const auto* e3 = find_entry(out, "fn3");
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->file_id,    "f2");
    EXPECT_EQ(e3->local_rank, 1);

    // f1: fn1 global 1 → local 1, fn2 global 2 → local 2
    auto f1_entries = entries_for_file(out, "f1");
    ASSERT_EQ(f1_entries.size(), 2u);
    EXPECT_EQ(f1_entries[0]->entity_id, "fn1");
    EXPECT_EQ(f1_entries[0]->local_rank, 1);
    EXPECT_EQ(f1_entries[1]->entity_id, "fn2");
    EXPECT_EQ(f1_entries[1]->local_rank, 2);
}

// entity_file_map에 없는 엔티티는 묵음 처리(결과에 포함 안 됨)
TEST(AlgoMerge, entity_not_in_file_map_is_dropped)
{
    Graph func_g;
    NodeId fn1 = func_g.get_or_add("fn1");
    NodeId fn2 = func_g.get_or_add("fn2");  // entity_file_map에 없음

    GraphBuilderResult gbr;
    gbr.entity_file_map = {{"fn1", "f1"}};  // fn2 누락
    gbr.entity_type_map = {{"fn1", "function"}};

    auto func_res = flat_pass(func_g, {fn1, fn2});
    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    EXPECT_EQ(out.entries.size(), 1u);
    EXPECT_NE(find_entry(out, "fn1"), nullptr);
    EXPECT_EQ(find_entry(out, "fn2"), nullptr);
}

// entity_type_map에 없으면 기본값 "function"
TEST(AlgoMerge, entity_type_defaults_to_function)
{
    Graph func_g;
    NodeId fn1 = func_g.get_or_add("fn1");

    GraphBuilderResult gbr;
    gbr.entity_file_map = {{"fn1", "f1"}};
    // entity_type_map 비어 있음

    auto func_res = flat_pass(func_g, {fn1});
    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    ASSERT_EQ(out.entries.size(), 1u);
    EXPECT_EQ(out.entries[0].entity_type, "function");
}

// entity_type_map에 "class"가 있으면 그대로 반영
TEST(AlgoMerge, entity_type_class_from_map)
{
    Graph func_g;
    NodeId cls1 = func_g.get_or_add("cls1");

    GraphBuilderResult gbr;
    gbr.entity_file_map = {{"cls1", "f1"}};
    gbr.entity_type_map = {{"cls1", "class"}};

    auto func_res = flat_pass(func_g, {cls1});
    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    ASSERT_EQ(out.entries.size(), 1u);
    EXPECT_EQ(out.entries[0].entity_type, "class");
}

// ── 동점 보조 정렬: start_line ─────────────────────────────────────────────────

// func seq에 없는 엔티티(global_rank=INT_MAX)끼리는 start_line ASC로 정렬
TEST(AlgoMerge, tie_break_by_start_line)
{
    Graph func_g;
    NodeId fn1 = func_g.get_or_add("fn1");  // start_line=20
    NodeId fn2 = func_g.get_or_add("fn2");  // start_line=10

    GraphBuilderResult gbr;
    gbr.entity_file_map  = {{"fn1","f1"}, {"fn2","f1"}};
    gbr.entity_type_map  = {{"fn1","function"}, {"fn2","function"}};
    gbr.entity_start_line = {{"fn1", 20}, {"fn2", 10}};

    // seq가 비어 있어 두 함수 모두 global_rank=INT_MAX
    auto func_res = flat_pass(func_g, {});
    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    auto f1_entries = entries_for_file(out, "f1");
    ASSERT_EQ(f1_entries.size(), 2u);
    // start_line 10인 fn2가 먼저
    EXPECT_EQ(f1_entries[0]->entity_id, "fn2");
    EXPECT_EQ(f1_entries[0]->local_rank, 1);
    EXPECT_EQ(f1_entries[1]->entity_id, "fn1");
    EXPECT_EQ(f1_entries[1]->local_rank, 2);
}

// ── 점수 전달 ──────────────────────────────────────────────────────────────────

// func_result.pr/bc/sc 값이 NodeId 인덱스 기준으로 올바르게 엔트리에 복사되는지 확인
TEST(AlgoMerge, func_scores_assigned_by_node_id)
{
    Graph func_g;
    NodeId fn1 = func_g.get_or_add("fn1");  // NodeId 0
    NodeId fn2 = func_g.get_or_add("fn2");  // NodeId 1

    GraphBuilderResult gbr;
    gbr.entity_file_map = {{"fn1","f1"}, {"fn2","f1"}};
    gbr.entity_type_map = {{"fn1","function"}, {"fn2","function"}};

    AlgoPassResult func_res;
    func_res.pr  = {0.3, 0.7};   // fn1=0.3, fn2=0.7
    func_res.bc  = {0.1, 0.9};
    func_res.sc  = {0.2, 0.8};
    func_res.seq = {fn2, fn1};   // fn2 먼저

    auto out = AlgoRunner::merge(flat_pass(Graph{}, {}), Graph{}, func_res, func_g, gbr);

    const auto* e1 = find_entry(out, "fn1");
    const auto* e2 = find_entry(out, "fn2");
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);

    EXPECT_NEAR(e1->pagerank_score, 0.3, 1e-9);
    EXPECT_NEAR(e1->bc_score,       0.1, 1e-9);
    EXPECT_NEAR(e1->combined_score, 0.2, 1e-9);

    EXPECT_NEAR(e2->pagerank_score, 0.7, 1e-9);
    EXPECT_NEAR(e2->bc_score,       0.9, 1e-9);
    EXPECT_NEAR(e2->combined_score, 0.8, 1e-9);
}

// file_result.pr/bc/sc 값이 파일 엔트리에 올바르게 복사되는지 확인
TEST(AlgoMerge, file_scores_assigned_by_node_id)
{
    Graph file_g;
    NodeId f1 = file_g.get_or_add("f1");  // NodeId 0
    NodeId f2 = file_g.get_or_add("f2");  // NodeId 1

    GraphBuilderResult gbr;
    gbr.file_loc_map = {{"f1", 10}, {"f2", 20}};

    AlgoPassResult file_res;
    file_res.pr  = {0.4, 0.6};
    file_res.bc  = {0.1, 0.9};
    file_res.sc  = {0.3, 0.7};
    file_res.seq = {f1, f2};

    auto out = AlgoRunner::merge(file_res, file_g, flat_pass(Graph{}, {}), Graph{}, gbr);

    const auto* ef1 = find_entry(out, "f1");
    const auto* ef2 = find_entry(out, "f2");
    ASSERT_NE(ef1, nullptr);
    ASSERT_NE(ef2, nullptr);

    EXPECT_NEAR(ef1->pagerank_score, 0.4, 1e-9);
    EXPECT_NEAR(ef1->bc_score,       0.1, 1e-9);
    EXPECT_NEAR(ef1->combined_score, 0.3, 1e-9);

    EXPECT_NEAR(ef2->pagerank_score, 0.6, 1e-9);
    EXPECT_NEAR(ef2->bc_score,       0.9, 1e-9);
    EXPECT_NEAR(ef2->combined_score, 0.7, 1e-9);
}
