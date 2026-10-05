import { describe, expect, it } from 'vitest'

import { parseGitHubRepositoryUrl } from './repositoryUrl'

describe('parseGitHubRepositoryUrl', () => {
  it.each([
    'https://github.com/c5ln/Telescode',
    'https://github.com/c5ln/Telescode/',
    'https://github.com/c5ln/Telescode.git',
    'http://www.github.com/c5ln/Telescode',
    ' https://GitHub.com/c5ln/Telescode?tab=readme#top ',
  ])('reads %s', (url) => expect(parseGitHubRepositoryUrl(url)).toEqual({ owner: 'c5ln', name: 'Telescode' }))

  it.each([
    '',
    'Telescode',
    'github.com/c5ln/Telescode',
    'git@github.com:c5ln/Telescode.git',
    'https://gitlab.com/c5ln/Telescode',
    'https://github.com.evil.example/c5ln/Telescode',
    'https://github.com/c5ln',
    'https://github.com/c5ln/Telescode/blob/main/README.md',
    'https://github.com/c5ln/Telescode/pull/1',
    'https://github.com/c5ln/..',
  ])('refuses %j', (url) => expect(parseGitHubRepositoryUrl(url)).toBeNull())
})
