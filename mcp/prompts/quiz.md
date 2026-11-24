# Telescode tour quiz instructions

목적은 사용자가 투어의 한 정차 지점에서 본 코드를 **개념적으로** 이해했는지 확인하는 것이다. 문제는 그 코드베이스에 합류한 개발자가 기술 면접에서 받을 법한 질문처럼 쓴다. 퀴즈 결과는 이해도 자체가 아니라 이해 상태를 추정하기 위한 관측값으로 저장된다.
문제를 쓰는 기준은 "Telescode question writing"을 따른다.

## Context 구성 (GraphRAG)

1. `open_project`로 투어와 같은 DB를 연다.
2. 요청받은 난이도를 한 번에 `get_quiz_context(projectId, nodeId, difficulties: [...])`로 받는다(투어 퀴즈는 `["easy", "medium"]`). 공통 소스는 한 번만 온다. 요청받지 않은 난이도의 context는 받지도, 문제를 쓰지도 않는다. 이 결과가 유일한 근거이고, 난이도 L 문제의 `evidenceNodeIds`는 `evidence[L]`에서만 고르며 정차 노드를 반드시 포함한다.
   - `excerpts`는 실제 소스 코드이다(`줄번호| 코드`). 문제는 주로 여기서 나온다. 긴 파일은 관련 정의만 남기고, 나머지는 `…| lines a–b omitted: 이름` 줄로 표시된다. 생략된 코드의 동작은 묻지 않는다.
   - `relations`는 코드 그래프(포함, import, 클래스 관계)이고, `nodes[].facts`는 시그니처 같은 구조 정보이다.
   - `learning`은 reading sequence 기반 학습 순서이다. import 관계를 학습 선행 관계라고 단정하지 않는다.
3. `excerpts`가 비어 있으면(소스를 찾지 못한 프로젝트) 시그니처와 관계로 답할 수 있는 개념 문제만 만들고, 그것도 어렵다면 해당 난이도는 만들지 않는다.

## 저장

`create_quiz`에 투어 ID, revision, 정차 인덱스, nodeId와 함께 저장한다. 검증 오류가 나면 근거를 고쳐 다시 저장한다. 에이전트는 퀴즈에 답하거나 응답을 기록하지 않는다.
