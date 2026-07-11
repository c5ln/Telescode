#!/usr/bin/env bash
# Parser 벤치마크: -O2 바이너리를 빌드해 "파싱 vs DB insert" 시간을 측정한다.
# 기존 build/는 Debug라 측정이 왜곡되므로, fetch된 소스를 재사용해 별도 컴파일한다.
#
# 사용법: scripts/bench_parser.sh [대상경로] [반복횟수]
#   대상경로  기본값 /usr/lib/python3.11
#   반복횟수  기본값 3
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-/usr/lib/python3.11}"
RUNS="${2:-3}"

DEPS="$ROOT/build/_deps"
OUT="$ROOT/build/bench"

if [ ! -d "$DEPS/tree-sitter-src" ]; then
    echo "error: $DEPS/tree-sitter-src 가 없습니다. 먼저 CMake 빌드를 한 번 실행하세요." >&2
    exit 1
fi

mkdir -p "$OUT"

# 외부 라이브러리는 바뀌지 않으므로 .o가 있으면 재사용한다.
[ -f "$OUT/ts.o" ] || gcc -O2 -c "$DEPS/tree-sitter-src/lib/src/lib.c" \
    -I"$DEPS/tree-sitter-src/lib/include" -I"$DEPS/tree-sitter-src/lib/src" -o "$OUT/ts.o"
[ -f "$OUT/tsp_parser.o" ] || gcc -O2 -c "$DEPS/tree-sitter-python-src/src/parser.c" \
    -I"$DEPS/tree-sitter-python-src/src" -o "$OUT/tsp_parser.o"
[ -f "$OUT/tsp_scanner.o" ] || gcc -O2 -c "$DEPS/tree-sitter-python-src/src/scanner.c" \
    -I"$DEPS/tree-sitter-python-src/src" -o "$OUT/tsp_scanner.o"
[ -f "$OUT/sqlite3.o" ] || gcc -O2 -c "$ROOT/third_party/sqlite/sqlite3.c" \
    -I"$ROOT/third_party/sqlite" -o "$OUT/sqlite3.o"

# 프로젝트 소스는 개발 중 바뀌므로 항상 다시 컴파일한다.
g++ -O2 -std=c++17 -I"$ROOT/src" -I"$DEPS/tree-sitter-src/lib/include" -I"$ROOT/third_party/sqlite" \
    "$ROOT/src/scan_main.cpp" "$ROOT/src/db/DbInserter.cpp" "$ROOT/src/db/db.cpp" \
    "$ROOT/src/parser/PythonParser.cpp" "$ROOT/src/parser/ParserRegistry.cpp" \
    "$OUT"/ts.o "$OUT"/tsp_parser.o "$OUT"/tsp_scanner.o "$OUT"/sqlite3.o \
    -lpthread -ldl -o "$OUT/scanner_bench"

# 페이지 캐시 워밍: 첫 실행만 디스크 I/O 때문에 느려지는 것을 방지
find "$TARGET" -name '*.py' -exec cat {} + > /dev/null 2>&1 || true

echo "== bench: $TARGET (${RUNS}회) =="
for _ in $(seq "$RUNS"); do
    rm -f "$OUT"/bench.db*
    "$OUT/scanner_bench" "$TARGET" "$OUT/bench.db" | grep -E "Parsed|Timing"
done
