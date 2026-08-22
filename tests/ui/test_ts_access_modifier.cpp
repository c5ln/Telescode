// TDD: tests written before ts_access_modifier.h / ts_class_diagram.h exist.
// Pure-function tests only — no ImGui/imnodes context required.
//
// Suites:
//   PythonAccessStrategy  — Python naming-convention strategy
//   ExplicitAccessStrategy — language with DB raw_modifier column
//   DefaultStrategy        — fallback for unregistered languages
//   Registry               — RegisterAccessStrategy / GetAccessStrategy
//   BuildMemberDisplay     — display string builder
//   ClassDiagramGridPos    — grid layout helper

#include "ui/ts_access_modifier.h"
#include "ui/ts_class_diagram.h"
#include <imgui.h>
#include <gtest/gtest.h>

// ─────────────────────────────────────────────────────────────────────────────
// PythonAccessStrategy
// ─────────────────────────────────────────────────────────────────────────────

TEST(PythonAccessStrategy, double_underscore_prefix_is_private)
{
    auto s = TS::MakePythonAccessStrategy();
    auto info = s->resolve("__name", "");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '-');
    EXPECT_EQ(info.member_name, "__name");
}

TEST(PythonAccessStrategy, single_underscore_prefix_is_protected)
{
    auto s = TS::MakePythonAccessStrategy();
    auto info = s->resolve("_name", "");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '#');
    EXPECT_EQ(info.member_name, "_name");
}

TEST(PythonAccessStrategy, no_prefix_is_public)
{
    auto s = TS::MakePythonAccessStrategy();
    auto info = s->resolve("name", "");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '+');
    EXPECT_EQ(info.member_name, "name");
}

TEST(PythonAccessStrategy, dunder_method_is_private)
{
    // __init__, __str__ 등 dunder도 __ prefix이므로 private('-') 처리
    auto s = TS::MakePythonAccessStrategy();
    auto info = s->resolve("__init__", "");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '-');
}

TEST(PythonAccessStrategy, raw_modifier_is_ignored)
{
    // Python은 raw_modifier 컬럼이 없으므로 어떤 값을 전달해도 결과 동일
    auto s = TS::MakePythonAccessStrategy();
    auto info_empty   = s->resolve("_method", "");
    auto info_nonempty = s->resolve("_method", "public");
    ASSERT_TRUE(info_empty.symbol.has_value());
    ASSERT_TRUE(info_nonempty.symbol.has_value());
    EXPECT_EQ(info_empty.symbol.value(), info_nonempty.symbol.value());
    EXPECT_EQ(info_empty.member_name, info_nonempty.member_name);
}

// ─────────────────────────────────────────────────────────────────────────────
// ExplicitAccessStrategy
// ─────────────────────────────────────────────────────────────────────────────

TEST(ExplicitAccessStrategy, public_modifier_gives_plus)
{
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("myMethod", "public");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '+');
    EXPECT_EQ(info.member_name, "myMethod");
}

TEST(ExplicitAccessStrategy, private_modifier_gives_minus)
{
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("myMethod", "private");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '-');
}

TEST(ExplicitAccessStrategy, protected_modifier_gives_hash)
{
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("myMethod", "protected");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '#');
}

TEST(ExplicitAccessStrategy, empty_modifier_gives_nullopt)
{
    // modifier를 알 수 없으면 symbol을 표시하지 않음
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("myMethod", "");
    EXPECT_FALSE(info.symbol.has_value());
}

TEST(ExplicitAccessStrategy, uppercase_public_is_case_insensitive)
{
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("myMethod", "Public");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '+');
}

TEST(ExplicitAccessStrategy, mixed_case_private_is_case_insensitive)
{
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("myMethod", "PRIVATE");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '-');
}

TEST(ExplicitAccessStrategy, member_name_equals_function_name)
{
    auto s = TS::MakeExplicitAccessStrategy();
    auto info = s->resolve("doWork", "protected");
    EXPECT_EQ(info.member_name, "doWork");
}

// ─────────────────────────────────────────────────────────────────────────────
// DefaultStrategy (fallback)
// ─────────────────────────────────────────────────────────────────────────────

TEST(DefaultStrategy, unregistered_language_returns_nullopt_symbol)
{
    // "cobol"은 등록된 전략이 없으므로 Default 폴백 → symbol=nullopt
    const auto& strat = TS::GetAccessStrategy("cobol");
    auto info = strat.resolve("SomeProcedure", "");
    EXPECT_FALSE(info.symbol.has_value());
}

TEST(DefaultStrategy, unregistered_language_preserves_member_name)
{
    const auto& strat = TS::GetAccessStrategy("cobol_2");
    auto info = strat.resolve("COMPUTE", "");
    EXPECT_EQ(info.member_name, "COMPUTE");
}

// ─────────────────────────────────────────────────────────────────────────────
// Registry
// ─────────────────────────────────────────────────────────────────────────────

TEST(Registry, registered_python_strategy_behaves_as_python)
{
    // 고유 ID를 사용해 다른 테스트와 격리
    TS::RegisterAccessStrategy("python_reg_1", TS::MakePythonAccessStrategy());
    const auto& strat = TS::GetAccessStrategy("python_reg_1");

    auto pub  = strat.resolve("open_file", "");
    auto prot = strat.resolve("_helper", "");
    auto priv = strat.resolve("__internal", "");

    ASSERT_TRUE(pub.symbol.has_value());
    EXPECT_EQ(pub.symbol.value(), '+');

    ASSERT_TRUE(prot.symbol.has_value());
    EXPECT_EQ(prot.symbol.value(), '#');

    ASSERT_TRUE(priv.symbol.has_value());
    EXPECT_EQ(priv.symbol.value(), '-');
}

TEST(Registry, re_registration_overwrites_previous_strategy)
{
    // 먼저 Python 전략 등록, 그다음 Explicit 전략으로 덮어쓰기
    TS::RegisterAccessStrategy("lang_overwrite_test", TS::MakePythonAccessStrategy());
    TS::RegisterAccessStrategy("lang_overwrite_test", TS::MakeExplicitAccessStrategy());

    const auto& strat = TS::GetAccessStrategy("lang_overwrite_test");

    // Explicit 전략: raw_modifier="public" → '+'
    auto info = strat.resolve("anyFunc", "public");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '+');

    // Python 전략이 남아있다면 "__anyFunc"에 '-'를 반환하겠지만,
    // Explicit 전략은 raw_modifier="" → nullopt
    auto info2 = strat.resolve("__anyFunc", "");
    EXPECT_FALSE(info2.symbol.has_value());
}

TEST(Registry, unregistered_language_falls_back_to_default)
{
    // "nonexistent_xyz"는 등록한 적 없음 → Default 폴백 → nullopt
    const auto& strat = TS::GetAccessStrategy("nonexistent_xyz");
    auto info = strat.resolve("foo", "");
    EXPECT_FALSE(info.symbol.has_value());
}

TEST(Registry, registered_explicit_strategy_works_correctly)
{
    TS::RegisterAccessStrategy("cpp_reg_1", TS::MakeExplicitAccessStrategy());
    const auto& strat = TS::GetAccessStrategy("cpp_reg_1");

    auto info = strat.resolve("getValue", "private");
    ASSERT_TRUE(info.symbol.has_value());
    EXPECT_EQ(info.symbol.value(), '-');
    EXPECT_EQ(info.member_name, "getValue");
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildMemberDisplay
// ─────────────────────────────────────────────────────────────────────────────

TEST(BuildMemberDisplay, symbol_and_params)
{
    TS::AccessInfo info{ '+', "process" };
    std::string result = TS::BuildMemberDisplay(info, "data, opts");
    EXPECT_EQ(result, "+ process(data, opts)");
}

TEST(BuildMemberDisplay, private_symbol_empty_params)
{
    TS::AccessInfo info{ '-', "__parse" };
    std::string result = TS::BuildMemberDisplay(info, "");
    EXPECT_EQ(result, "- __parse()");
}

TEST(BuildMemberDisplay, no_symbol_with_params)
{
    // symbol=nullopt → symbol 접두사 없이 "name(params)"
    TS::AccessInfo info{ std::nullopt, "run" };
    std::string result = TS::BuildMemberDisplay(info, "x");
    EXPECT_EQ(result, "run(x)");
}

TEST(BuildMemberDisplay, no_symbol_empty_params)
{
    TS::AccessInfo info{ std::nullopt, "run" };
    std::string result = TS::BuildMemberDisplay(info, "");
    EXPECT_EQ(result, "run()");
}

TEST(BuildMemberDisplay, protected_symbol_multiple_params)
{
    TS::AccessInfo info{ '#', "compute" };
    std::string result = TS::BuildMemberDisplay(info, "a, b, c");
    EXPECT_EQ(result, "# compute(a, b, c)");
}

// ─────────────────────────────────────────────────────────────────────────────
// ClassDiagramGridPos
// ─────────────────────────────────────────────────────────────────────────────

// gap_x = NODE_WIDTH * 1.5f = 220.0f * 1.5f = 330.0f
// gap_y = 300.0f
static constexpr float GAP_X = 220.0f * 1.5f; // 330.0f
static constexpr float GAP_Y = 300.0f;

TEST(ClassDiagramGridPos, single_node_at_origin)
{
    // total=1 → cols=1, 유일한 노드는 (0, 0)
    ImVec2 pos = TS::ClassDiagramGridPos(0, 1);
    EXPECT_NEAR(pos.x, 0.0f, 1e-3f);
    EXPECT_NEAR(pos.y, 0.0f, 1e-3f);
}

TEST(ClassDiagramGridPos, four_nodes_two_columns_node0)
{
    // total=4 → cols=2
    ImVec2 pos = TS::ClassDiagramGridPos(0, 4);
    EXPECT_NEAR(pos.x, 0.0f, 1e-3f);
    EXPECT_NEAR(pos.y, 0.0f, 1e-3f);
}

TEST(ClassDiagramGridPos, four_nodes_two_columns_node1)
{
    ImVec2 pos = TS::ClassDiagramGridPos(1, 4);
    EXPECT_NEAR(pos.x, GAP_X, 1e-3f);
    EXPECT_NEAR(pos.y, 0.0f, 1e-3f);
}

TEST(ClassDiagramGridPos, four_nodes_two_columns_node2)
{
    ImVec2 pos = TS::ClassDiagramGridPos(2, 4);
    EXPECT_NEAR(pos.x, 0.0f, 1e-3f);
    EXPECT_NEAR(pos.y, GAP_Y, 1e-3f);
}

TEST(ClassDiagramGridPos, four_nodes_two_columns_node3)
{
    ImVec2 pos = TS::ClassDiagramGridPos(3, 4);
    EXPECT_NEAR(pos.x, GAP_X, 1e-3f);
    EXPECT_NEAR(pos.y, GAP_Y, 1e-3f);
}

TEST(ClassDiagramGridPos, nine_nodes_three_columns)
{
    // total=9 → cols=3
    // node 0: col=0, row=0 → (0, 0)
    // node 2: col=2, row=0 → (2*GAP_X, 0)
    // node 3: col=0, row=1 → (0, GAP_Y)
    // node 8: col=2, row=2 → (2*GAP_X, 2*GAP_Y)
    ImVec2 p0 = TS::ClassDiagramGridPos(0, 9);
    EXPECT_NEAR(p0.x, 0.0f,        1e-3f);
    EXPECT_NEAR(p0.y, 0.0f,        1e-3f);

    ImVec2 p2 = TS::ClassDiagramGridPos(2, 9);
    EXPECT_NEAR(p2.x, 2.0f * GAP_X, 1e-3f);
    EXPECT_NEAR(p2.y, 0.0f,          1e-3f);

    ImVec2 p3 = TS::ClassDiagramGridPos(3, 9);
    EXPECT_NEAR(p3.x, 0.0f,  1e-3f);
    EXPECT_NEAR(p3.y, GAP_Y, 1e-3f);

    ImVec2 p8 = TS::ClassDiagramGridPos(8, 9);
    EXPECT_NEAR(p8.x, 2.0f * GAP_X, 1e-3f);
    EXPECT_NEAR(p8.y, 2.0f * GAP_Y, 1e-3f);
}

TEST(ClassDiagramGridPos, edge_case_total_zero_does_not_crash)
{
    // total=0: 유효한 인덱스가 없으나 호출 시 crash가 없어야 함
    EXPECT_NO_THROW({
        TS::ClassDiagramGridPos(0, 0);
    });
}
