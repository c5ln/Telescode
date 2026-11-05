import type { Difficulty, QuestionInput, QuizFeedback, QuizEvidence } from '../quiz/model'
import type { Decision, Knowledge, LearningAction, LearningGoal } from './model'

export interface Concept { id: string; name: string; definition: string; version: number }
export interface PreparedItem extends LearningAction { questions: QuestionInput[] }
export interface Curriculum {
  version: number
  concepts: Concept[]
  items: PreparedItem[]
  relations: { from: string; to: string; kind: 'prerequisite' | 'similar'; reason: string; evidenceNodeIds: string[] }[]
}
export interface LearningStep { id: string; actionId: string; nodeId: string; conceptId: string; level: Difficulty; kind: LearningAction['kind']; caption: string }
export interface LearningSession {
  id: string; revision: number; learnerId: string; goal: LearningGoal; spent: number
  status: 'preparing' | 'generation-needed' | 'active' | 'finished' | 'budget-exhausted' | 'stopped'
  current: LearningStep | null
  history: { step: LearningStep; outcome: 'continue' | 'skip'; responseId?: string }[]
  policyVersion: string; modelVersion: string
}
export interface LearningView {
  session: LearningSession
  knowledge: Knowledge[]
  concepts: Concept[]
  decision: Decision | null
  question: { id: string; question: string; choices: string[]; difficulty: Difficulty; concept: string; evidence: QuizEvidence[] } | null
  score: number
  reason: string
}
export interface LearningAnswer {
  sessionId: string; revision: number; stopId: string; submissionId: string; questionId: string; selectedIndex: number
}
export interface LearningResult { view: LearningView; feedback: QuizFeedback; responseId: string; counted: boolean }
export interface PreparationJob { id: string; sessionId: string; revision: number; status: 'running' | 'ready' | 'failed'; attempt: number; shouldRun: boolean; error?: string }
