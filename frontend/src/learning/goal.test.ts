import { describe, expect, it } from 'vitest'
import { defaultGoal, MAX_REQUIREMENTS_LENGTH, requirementsGoal } from './goal'

describe('requirementsGoal', () => {
  it('uses onboarding for blank requirements', async () => {
    expect(await requirementsGoal(' \t\n ')).toEqual(defaultGoal())
  })

  it('resumes the same goal for equivalent whitespace and Unicode forms', async () => {
    const goal = await requirementsGoal('Auth flow')
    expect(await requirementsGoal('  Ａuth\u3000flow\n')).toEqual(goal)
    expect(goal.rawQuery).toBe('Auth flow')
    expect(goal.retrievalQuery).toBe('Auth flow')
  })

  it('separates assessment targets for different requirements', async () => {
    const auth = await requirementsGoal('Explain authentication')
    const results = await requirementsGoal('Explain search results')
    expect(auth.goalId).not.toBe(results.goalId)
    expect(auth.targetConcepts[0]!.conceptId).not.toBe(results.targetConcepts[0]!.conceptId)
    expect(auth.targetConcepts[0]!.conceptId).not.toBe(defaultGoal().targetConcepts[0]!.conceptId)
  })

  it('accepts the server length limit and rejects overflow, including normalization expansion', async () => {
    await expect(requirementsGoal('a'.repeat(MAX_REQUIREMENTS_LENGTH))).resolves.toBeDefined()
    await expect(requirementsGoal('a'.repeat(MAX_REQUIREMENTS_LENGTH + 1))).rejects.toThrow(/4,000/)
    await expect(requirementsGoal('㍿'.repeat(1001))).rejects.toThrow(/4,000/)
  })
})
