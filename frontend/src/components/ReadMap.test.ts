import { describe, expect, it } from 'vitest'

import { readMapGroups } from './ReadMap'

describe('readMapGroups', () => {
  it('has a box per top-level folder, the largest first, filled as files are read', () => {
    const found = ['setup.py', 'src/a.py', 'src/pkg/b.py', 'src/c.py', 'tests/t.py']
    expect(readMapGroups(found, ['src/pkg/b.py', 'setup.py'])).toEqual([
      { name: 'src', total: 3, read: 1, cells: 3, filled: 1 },
      { name: '', total: 1, read: 1, cells: 1, filled: 1 },
      { name: 'tests', total: 1, read: 0, cells: 1, filled: 0 },
    ])
  })

  it('lets a square stand for several files in a large repository, and fills a folder once it is all read', () => {
    const found = Array.from({ length: 10 }, (_, i) => `pkg/m${i}.py`)
    expect(readMapGroups(found, found.slice(0, 5), 4)).toEqual([{ name: 'pkg', total: 10, read: 5, cells: 4, filled: 1 }])
    expect(readMapGroups(found, found, 4)[0]!.filled).toBe(4)
  })
})
