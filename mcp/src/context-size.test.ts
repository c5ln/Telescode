import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { QuizStore, sourceReader } from './quiz.ts'
import { LearningStore } from './learning.ts'
import { buildQuizContext, buildQuizContexts } from '../../frontend/src/quiz/model.ts'
import { compare, measure, repeatedSource, type Example } from './context-size.ts'

const fixturePath = fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url))
const nodeA = 'file:sherlock_project/notify.py'
const modules = { TourStore, QuizStore, LearningStore, sourceReader, buildQuizContext, buildQuizContexts }

test('repeated source counts every numbered line seen before, in either numbering, and never the omission lines', () => {
  const contexts = [
    { excerpts: [{ fileId: 'a.py', text: '1| import os\n2| x = 1\n…| lines 3–9 omitted: f()' }] },
    // The numbering before #84 padded the line number.
    { excerpts: [{ fileId: 'a.py', text: '    2| x = 1\n   10| y = 2' }, { fileId: 'b.py', text: '1| import os' }] },
  ]
  assert.deepEqual(repeatedSource(contexts), { sourceLines: 5, repeatedLines: 1, repeatedChars: '    2| x = 1'.length + 1 })
})

/** The example a measurement replays: a goal, the first prepared action, and a tour to quiz on. */
const example: Example = {
  requirements: 'Understand how results reach notifications',
  tour: { plan: { stops: [{ nodeId: nodeA }, { nodeId: 'file:sherlock_project/result.py' }] } },
  curriculum: {
    version: 1, relations: [],
    concepts: [{ id: 'goal:requirements:ID', name: 'Notification state', definition: 'Explain how notification consumers receive the search outcome', version: 1 }],
    items: [{ id: 'a-easy', kind: 'read', nodeId: nodeA, conceptId: 'goal:requirements:ID', level: 'easy', caption: 'Read the notifier.',
      questions: ['first', 'second'].map(s => ({ type: 'multiple_choice', difficulty: 'easy', concept: 'Notification state',
        question: `Why preserve the result before notifying? ${s}`, answer: 'So the receiver can inspect the outcome',
        distractors: ['To start another network request', 'To mutate the site configuration'],
        explanation: 'The stored result supplies the outcome to notification consumers.', evidenceNodeIds: [nodeA] })) }],
  },
}

test('a measurement reports the quiz, first preparation and follow-up material contexts an agent would receive, without any agent', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-context-size-'))
  const source = join(dir, 'source')
  await mkdir(join(source, 'sherlock_project'), { recursive: true })
  await writeFile(join(source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = result\n')
  await writeFile(join(source, 'sherlock_project/result.py'), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  const result = await measure(modules, { path: fixturePath, sourceRoot: source, rank: false, example, workDir: join(dir, 'work') })

  assert.deepEqual(result.quiz.stops.map(s => s.nodeId), example.tour.plan.stops.map(s => s.nodeId))
  assert.ok(result.quiz.stops.every(s => s.chars > 0 && s.sourceChars > 0))
  assert.equal(result.quiz.chars, result.quiz.stops.reduce((n, s) => n + s.chars, 0))
  assert.ok(result.preparation.chars > 0 && result.preparation.candidates > 0)
  // A wrong first answer asks for both outcomes of the next question.
  assert.equal(result.material.shouldRun, true)
  assert.deepEqual(result.material.slots.map(s => s.level), ['medium', 'easy'])
  assert.deepEqual(result.material.slots[0]!.nodeIds.slice(0, 1), [nodeA])
  assert.ok(result.material.chars! > 0)
  // The current code sends each source line once.
  assert.equal(result.material.repeatedLines, 0)
  assert.equal(result.preparation.repeatedLines, 0)
})

test('the comparison shows each size before and after with its change', () => {
  const run = (quiz: number, prep: number, material: number) => ({
    quiz: { chars: quiz, sourceChars: quiz / 2, repeatedChars: 0, stops: [] },
    preparation: { chars: prep, sourceChars: prep / 2, candidates: 6, truncated: 3, repeatedLines: 0, repeatedChars: 0, perCandidate: [] },
    material: { shouldRun: true, chars: material, sourceChars: material / 2, repeatedLines: 0, repeatedChars: material / 4, slots: [] },
  })
  const table = compare(run(1000, 2000, 4000), run(1000, 2000, 2000))
  assert.match(table, /\| Material context \(chars\) \| 4,000 \| 2,000 \| −50\.0% \|/)
  assert.match(table, /\| Quiz contexts \(chars\) \| 1,000 \| 1,000 \| 0\.0% \|/)
})
