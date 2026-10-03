# Reading Sequence 추천 알고리즘 구현 계획

## 개요

PageRank와 Betweenness Centrality를 결합하여 코드베이스의 파일·함수 읽기 순서를 추천한다.

---

## 아키텍처

```
src/
├── parser/
│   ├── ParseResult.h
│   └── PythonParser.cpp
│
├── algo/
│   ├── AlgoConfig.h/.cpp
│   ├── AlgoResult.h
│   ├── Graph.h
│   ├── GraphBuilder.h/.cpp
│   ├── PageRank.h/.cpp
│   ├── BetweennessCentrality.h/.cpp
│   ├── SCCFinder.h/.cpp
│   ├── ScoreCombiner.h
│   ├── ReadingSequencer.h/.cpp
│   ├── AlgoRunner.h/.cpp
│   └── AlgoDbWriter.h/.cpp
```

---

## DB 스키마 확장

`db.cpp`의 `kInitSQL` 끝에 추가한다.

```sql
CREATE TABLE IF NOT EXISTS reading_sequence (
    entity_id      TEXT PRIMARY KEY,
    entity_type    TEXT NOT NULL CHECK(entity_type IN ('file', 'class', 'function')),
    file_id        TEXT NOT NULL REFERENCES file(file_id) ON DELETE CASCADE,
    file_rank      INTEGER,           -- entity_type='file'일 때만 NOT NULL
    local_rank     INTEGER,           -- entity_type!='file'일 때만 NOT NULL
    pagerank_score REAL NOT NULL,
    bc_score       REAL NOT NULL,
    combined_score REAL NOT NULL,
    CHECK(
        (entity_type = 'file'  AND file_rank  IS NOT NULL AND local_rank IS NULL) OR
        (entity_type != 'file' AND local_rank IS NOT NULL AND file_rank  IS NULL)
    )
);
CREATE INDEX IF NOT EXISTS idx_rs_file      ON reading_sequence(file_id);
CREATE INDEX IF NOT EXISTS idx_rs_file_rank ON reading_sequence(file_rank);

-- 파라미터 및 메타 (key 목록은 AlgoConfig 참조)
CREATE TABLE IF NOT EXISTS reading_sequence_config (
    config_key   TEXT PRIMARY KEY,
    config_value TEXT NOT NULL
);

```

파일 변경 시 `reading_sequence`는 CASCADE 삭제로 구멍이 생기므로 **전체 재계산 필수**.

---

## Graph 타입

```cpp
// Graph.h
using NodeId = uint32_t;

struct Graph {
    std::vector<std::vector<NodeId>> adj;    // 정방향
    std::vector<std::vector<NodeId>> radj;   // 역방향 (BC용)
    std::unordered_map<std::string, NodeId> id_to_node;
    std::vector<std::string>                node_to_id;

    NodeId get_or_add(const std::string& id);
    int    size() const { return node_to_id.size(); }
};
```

string → 정수 매핑으로 BFS 반복의 해시 오버헤드를 제거한다.

---

## 2-Pass 구조

### Pass 1 — 파일 레벨 그래프

노드: `file_id` / 간선: IMPORTS + CALLS + INHERITS (파일 레벨로 집계)

```sql
SELECT * FROM (
    SELECT DISTINCT
        substr(source_id, 1, instr(source_id||'::', '::')-1) AS src,
        substr(target_id, 1, instr(target_id||'::', '::')-1) AS tgt
    FROM link
    WHERE link_type IN ('IMPORTS', 'CALLS', 'INHERITS')
) WHERE src != tgt;
-- DECORATES: 읽기 순서 의존성 없음 → 의도적 제외
```

### Pass 2 — 함수/클래스 레벨 그래프

노드: `function_id`, `class_id` / 간선: CALLS + INHERITS

```sql
SELECT source_id, target_id FROM link
WHERE link_type IN ('CALLS', 'INHERITS');
-- IMPORTS: 파일 의존성은 Pass 1 담당 → 의도적 제외
```

### Pass 1·2 통합 — file_id 역참조

`function` 테이블은 `file_id` / `class_id` nullable FK 구조 (둘 중 하나만 NOT NULL).

```sql
SELECT function_id, COALESCE(f.file_id, c.file_id) AS file_id
FROM function f
LEFT JOIN class c ON f.class_id = c.class_id;
```

통합 절차:
1. Pass 1 완료 → `file_rank[file_id]` (1-indexed)
2. Pass 2 완료 → `global_rank[entity_id]`
3. 파일별 entity 그룹화 후 `global_rank ASC` 정렬 → `local_rank = 1, 2, 3, ...`
4. 동점 보조 정렬: `start_line ASC`
5. file 엔티티: `file_rank = pass1 결과`, `local_rank = NULL`

---

## 알고리즘

### PageRank — 댕글링 노드 처리 포함

```
PR(v) = (1-d)/N
      + d * Σ_{u→v} PR(u) / out(u)
      + d * (Σ_{dangling u} PR(u)) / N
```

- 간선 방향 A→B: A가 B를 import/call → PR(B) 누적 → B를 먼저 읽어야 함
- 수렴 기준: `Σ|PR_new - PR_old| < eps`

### Betweenness Centrality — 적응형 임계값

Pass별 임계값을 분리한다 (함수 레벨 그래프는 파일 레벨의 ~20배 규모).

| | Exact (Brandes) | k-sample | Fixed k |
|---|---|---|---|
| **Pass 1** (파일) | V < 200 | k = max(50, ⌈√V⌉) | k = min(64, V), V ≥ 2000 |
| **Pass 2** (함수) | V < 500 | k = max(50, ⌈√V⌉) | k = min(32, V), V ≥ 5000 |

- k 하한선 50: 최소 오차 보장
- `k = min(k_target, V)`: V < k 케이스 방지
- sampling 재현성: seed 주입

### ScoreCombiner

```
score(v) = (α / (α+β)) · PR_norm(v) + (β / (α+β)) · BC_norm(v)
```

- min-max 정규화, 분모=0(모든 값 동일)이면 0.5로 처리

### 위상정렬 — Kahn + SCCFinder(Tarjan)

- 후보 다수 시 max-heap(combined_score) 우선 (greedy, globally optimal 미보장)
- 사이클 → Tarjan SCC, 크기 > 10이면 경고 출력
- 고립 노드 동점 보조 정렬: LOC 내림차순 → 파일명 알파벳순
- `file_rank` 1-indexed

---

## AlgoConfig

```cpp
struct AlgoConfig {
    // 점수 결합
    double alpha = 0.6;
    double beta  = 0.4;

    // PageRank
    double   damping         = 0.85;
    int      max_iter        = 100;
    double   convergence_eps = 1e-6;

    // BC — Pass 1 (파일 레벨)
    int      bc_p1_exact_v   = 200;
    int      bc_p1_large_v   = 2000;
    int      bc_p1_fixed_k   = 64;

    // BC — Pass 2 (함수 레벨)
    bool     enable_p2_bc    = true;
    int      bc_p2_exact_v   = 500;
    int      bc_p2_large_v   = 5000;
    int      bc_p2_fixed_k   = 32;

    int      bc_k_min        = 50;
    uint64_t bc_seed         = 42;

    static AlgoConfig load_from_db(sqlite3* db);
    void               save_to_db(sqlite3* db) const;
};
```

`reading_sequence_config` 테이블 키: `alpha`, `beta`, `damping`, `max_iter`, `eps`,
`bc_p1_exact_v`, `bc_p1_large_v`, `bc_p1_fixed_k`, `bc_p2_exact_v`, `bc_p2_large_v`,
`bc_p2_fixed_k`, `bc_k_min`, `bc_seed`, `enable_p2_bc`, `last_computed_at`

---

## 디자인 패턴

### Strategy — BC 알고리즘 교체

```cpp
struct IBCStrategy {
    virtual std::vector<double> compute(const Graph& g) const = 0;
    virtual ~IBCStrategy() = default;
};
struct ExactBrandesStrategy    : IBCStrategy { ... };
struct SamplingBrandesStrategy : IBCStrategy {
    int k; uint64_t seed;
    SamplingBrandesStrategy(int k, uint64_t seed);
};

// AlgoConfig + PassLevel 기반 자동 선택
std::unique_ptr<IBCStrategy> make_bc_strategy(int V, const AlgoConfig&, PassLevel);
```

### Template Method — Pass 공통 구조

```cpp
enum class PassLevel { File, Function };

class AlgoPass {
public:
    AlgoPassResult run(const Graph& g, const AlgoConfig& cfg) {
        auto pr  = PageRank::compute(g, cfg);
        auto bc  = make_bc_strategy(g.size(), cfg, level())->compute(g);
        auto sc  = ScoreCombiner::combine(pr, bc, cfg.alpha, cfg.beta);
        auto seq = ReadingSequencer::sequence(g, sc, cfg);
        return {pr, bc, sc, seq};
    }
protected:
    virtual PassLevel level() const = 0;
};

class FilePass     : public AlgoPass { PassLevel level() const override { return PassLevel::File;     } };
class FunctionPass : public AlgoPass { PassLevel level() const override { return PassLevel::Function; } };
```

### Pipeline — AlgoRunner

```cpp
// AlgoRunner.cpp
AlgoRunResult AlgoRunner::run(const char* dbPath, const AlgoConfig& cfg) {
    sqlite3* db = nullptr;
    initDb(dbPath, &db);

    auto [file_graph, func_graph] = GraphBuilder::build(db);

    auto file_result = FilePass{}.run(file_graph, cfg);
    auto func_result = FunctionPass{}.run(func_graph, cfg);
    auto merged      = merge(file_result, func_result, db);

    sqlite3_close(db);

    AlgoDbWriter::write(dbPath, merged, cfg);
    return merged;
}
```

### 트랜잭션 — 기존 `goto rollback` 패턴

```cpp
// AlgoDbWriter.cpp — DbUpdater와 동일한 패턴
int AlgoDbWriter::write(const char* dbPath, const AlgoRunResult& result,
                        const AlgoConfig& cfg) {
    sqlite3* db = nullptr;
    int rc = initDb(dbPath, &db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); return rc; }

    rc = deleteExisting(db);
    if (rc != SQLITE_OK) goto rollback;

    rc = insertAll(db, result);
    if (rc != SQLITE_OK) goto rollback;

    rc = updateConfig(db, cfg);
    if (rc != SQLITE_OK) goto rollback;

    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return SQLITE_OK;

rollback:
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc;
}
```

## 구현 순서

1. `db.cpp` `kInitSQL` 확장 (새 테이블 4개)
2. `Graph.h` + `AlgoConfig`
3. `GraphBuilder` — SQL → Graph (Pass 1·2)
4. `PageRank` — power method + 댕글링 처리
5. `SCCFinder` — Tarjan + 크기 경고
6. `BetweennessCentrality` — Strategy + adaptive sampling
7. `ScoreCombiner` + `ReadingSequencer`
8. `AlgoRunner` — 2-pass + merge 로직
9. `AlgoDbWriter` — `goto rollback` 패턴
