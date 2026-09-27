import { describe, expect, it } from 'vitest'

import { isRepositoryUrl } from './repositoryUrl'

describe('isRepositoryUrl', () => {
  it.each([
    'https://github.com/c5ln/Telescode',
    'https://github.com/c5ln/Telescode.git',
    'http://gitlab.example.com/group/sub/repo',
    'ssh://git@github.com/c5ln/Telescode.git',
    'git://example.com/repo.git',
    'git@github.com:c5ln/Telescode.git',
  ])('accepts %s', (url) => expect(isRepositoryUrl(url)).toBe(true))

  it.each(['', 'Telescode', 'github.com/c5ln/Telescode', 'https://github.com', 'ftp://example.com/repo', 'C:\repo'])(
    'rejects %j',
    (url) => expect(isRepositoryUrl(url)).toBe(false),
  )
})
