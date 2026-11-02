# Telescode tour quiz instructions

목적은 사용자가 투어의 한 정차 지점에서 본 코드 구조를 이해했는지 확인하는 퀴즈를 만드는 것이다. 퀴즈 결과는 이해도 자체가 아니라 이해 상태를 추정하기 위한 관측값으로 저장된다.

## Context 구성 (GraphRAG)

1. `open_project`로 투어와 같은 DB를 연다.
2. 난이도마다 `get_quiz_context(projectId, nodeId, difficulty)`를 호출한다. 이 결과가 해당 난이도 문제가 사용할 수 있는 유일한 근거이다.
   - `relations`는 코드 그래프(포함, import, 클래스 관계)이다.
   - `learning`은 reading sequence 기반 학습 순서이다. import 관계를 학습 선행 관계라고 단정하지 않는다.
3. 필요하면 `inspect_node`로 context 안의 노드를 더 확인한다. context 밖의 노드는 근거로 쓰지 않는다.

## 난이도

난이도는 사용자의 숙련도가 아니라 문제가 요구하는 추론 깊이, 그래프 hop 수, 추상화 수준이다.

- **easy**: 정차 노드 하나(와 그 자식)만 보면 답할 수 있다. 예: 이 파일이 선언하는 클래스, 이 메서드의 반환 타입.
- **medium**: 정차 노드와 직접 연결된 노드를 함께 봐야 한다. 예: 이 파일이 import하는 파일, 두 클래스의 관계 종류. 근거 노드 2개 이상.
- **hard**: 여러 노드와 관계를 종합해 구조적 결과나 읽기 순서를 추론해야 한다. 예: 이 클래스의 시그니처가 바뀌면 영향받는 파일, 이 파일을 이해하기 전에 읽어야 할 파일. 근거 노드 3개 이상.

## 문제 작성

- 유형은 `multiple_choice`(오답 2–4개) 또는 `true_false`(정답 "True"/"False")만 쓴다.
- 문제마다 `concept`(확인하려는 개념, 짧게), `question`, `answer`, `distractors`, `explanation`, `evidenceNodeIds`를 채운다.
- `evidenceNodeIds`는 해당 난이도 context의 nodeId만 쓰고, 정차 노드를 반드시 포함한다.
- 정답은 context의 facts와 relations로 확인 가능해야 한다. 소스 코드 본문은 제공되지 않으므로 런타임 동작, 구현 세부, 설계 의도를 추측해서 묻지 않는다.
- 오답은 그럴듯하되 context로 명확히 틀렸다고 확인할 수 있어야 한다. "모두 정답" 같은 선택지는 쓰지 않는다.
- `explanation`은 어떤 노드/관계 때문에 정답인지 1–2문장으로 설명한다.
- context가 부족해 어떤 난이도의 근거 있는 문제를 만들 수 없다면 그 난이도는 만들지 않는다.

## 저장

`create_quiz`에 투어 ID, revision, 정차 인덱스, nodeId와 함께 저장한다. 검증 오류가 나면 근거를 고쳐 다시 저장한다. 에이전트는 퀴즈에 답하거나 응답을 기록하지 않는다.
