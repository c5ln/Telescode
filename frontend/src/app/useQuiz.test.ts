import { describe, expect, it } from 'vitest'

import type { QuizQuestion, QuizSet } from '../quiz/model'
import { questionsOf } from './useQuiz'

const question = (id: string, difficulty: QuizQuestion['difficulty']) => ({ id, difficulty }) as QuizQuestion

describe('questionsOf', () => {
  it('asks every question of the step, easiest first, keeping the set order within a difficulty', () => {
    const set = { questions: [question('m1', 'medium'), question('e1', 'easy'), question('h1', 'hard'), question('e2', 'easy')] } as QuizSet
    expect(questionsOf(set).map((q) => q.id)).toEqual(['e1', 'e2', 'm1', 'h1'])
  })
})
