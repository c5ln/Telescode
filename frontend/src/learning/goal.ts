import type { GoalSeed } from './api'

export const MAX_REQUIREMENTS_LENGTH = 4000
const DEFAULT_QUERY = 'Understand the application architecture, main entry points, core modules, configuration, dependencies, and how to load, process and return data.'

export const defaultGoal = (): GoalSeed => ({
  goalId: 'repository-onboarding', goalRevision: 1, rawQuery: DEFAULT_QUERY, retrievalQuery: DEFAULT_QUERY,
  targetConcepts: [{ conceptId: 'goal:architecture', level: 'easy', weight: 1 }], budget: 12,
})

/** Canonical requirements identify a single assessment target across session resumes. */
export async function requirementsGoal(requirements: string): Promise<GoalSeed> {
  const query = requirements.normalize('NFKC').trim().replace(/\s+/g, ' ')
  if (requirements.length > MAX_REQUIREMENTS_LENGTH || query.length > MAX_REQUIREMENTS_LENGTH) {
    throw new Error(`Keep requirements within ${MAX_REQUIREMENTS_LENGTH.toLocaleString('en-US')} characters.`)
  }
  if (!query) return defaultGoal()
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(query))
  const id = Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('')
  return {
    goalId: `requirements:${id}`, goalRevision: 1, rawQuery: query, retrievalQuery: query,
    targetConcepts: [{ conceptId: `goal:requirements:${id}`, level: 'easy', weight: 1 }], budget: 12,
  }
}
