import { describe, expect, it } from 'vitest'

import { parseGitHubRepositoryUrl, readRepositoryInput } from './repositoryUrl'

describe('parseGitHubRepositoryUrl', () => {
  it.each([
    'https://github.com/c5ln/Telescode',
    'https://github.com/c5ln/Telescode/',
    'https://github.com/c5ln/Telescode.git',
    'http://www.github.com/c5ln/Telescode',
    ' https://GitHub.com/c5ln/Telescode?tab=readme#top ',
    'https://github.com/c5ln/Telescode/tree/3c7baaa05ab4aa49cc819ce3a819d260de19c157',
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

describe('readRepositoryInput', () => {
  it.each([
    'https://github.com/psf/requests',
    'github.com/psf/requests',
    'www.github.com/psf/requests/',
    'psf/requests',
    'psf/requests.git',
    'git@github.com:psf/requests.git',
    'https://github.com/psf/requests/blob/main/README.md',
    'https://github.com/psf/requests/tree/main/src',
    ' https://github.com/psf/requests?tab=readme ',
  ])('opens %j as the repository itself', (value) =>
    expect(readRepositoryInput(value)).toEqual({ url: 'https://github.com/psf/requests', repository: { owner: 'psf', name: 'requests' } }))

  it.each([
    'https://github.com/psf/requests/tree/3C7BAAA05AB4AA49CC819CE3A819D260DE19C157',
    'github.com/psf/requests/tree/3c7baaa05ab4aa49cc819ce3a819d260de19c157/',
    'https://github.com/psf/requests.git/tree/3c7baaa05ab4aa49cc819ce3a819d260de19c157?tab=readme',
  ])('keeps the commit %j is pinned to', (value) =>
    expect(readRepositoryInput(value)).toEqual({
      url: 'https://github.com/psf/requests/tree/3c7baaa05ab4aa49cc819ce3a819d260de19c157',
      repository: { owner: 'psf', name: 'requests' },
    }))

  it.each([
    ['https://gitlab.com/psf/requests', 'Only GitHub is supported. Use an address like github.com/owner/repo.'],
    ['github.com/psf', 'Add the repository name, as in github.com/owner/repo.'],
    ['requests', 'That is not a repository address. Use one like github.com/owner/repo.'],
    ['github.com/psf/re$quests', 'That is not a valid repository name. Use one like github.com/owner/repo.'],
  ])('says how to fix %j', (value, problem) => expect(readRepositoryInput(value)).toEqual({ problem }))
})
