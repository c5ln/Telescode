import { describe, expect, it } from 'vitest'

import type { GraphResponse } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import { buildGraphModel } from '../graph/model'
import { buildNameIndex, searchNames } from './names'

const sherlockGraph = sherlock as GraphResponse

function makeFile(fileId: string): GraphResponse['files'][number] {
  const fileName = fileId.split('/').pop() ?? fileId
  return {
    ...sherlockGraph.files[0],
    fileId,
    fileName,
  }
}

function makeGraph(
  files: string[],
  classes: Array<{
    fileId: string
    className: string
    fields?: string[]
    methods?: string[]
  }> = [],
): GraphResponse {
  return {
    ...sherlockGraph,
    files: files.map(makeFile),
    classes: classes.map((c, idx) => ({
      classId: `${c.fileId}::${c.className}`,
      nodeId: idx,
      fileId: c.fileId,
      className: c.className,
      package: '',
      fields: (c.fields ?? []).map((name) => ({ access: '+' as const, name })),
      methods: (c.methods ?? []).map((name) => ({ access: '+' as const, name, params: '', returnType: 'void' })),
    })),
    classEdges: [],
    fileGraph: { nodes: [], edges: [] },
  }
}

describe('buildNameIndex', () => {
  it('excludes root and dir nodes and classifies file, class, method, and field', () => {
    const model = buildGraphModel(sherlockGraph, '')
    const index = buildNameIndex(model)

    expect(index.some((e) => e.id === 'root' || e.id.startsWith('dir:'))).toBe(false)

    const fileEntry = index.find((e) => e.id === 'file:sherlock_project/notify.py')
    expect(fileEntry).toMatchObject({
      id: 'file:sherlock_project/notify.py',
      kind: 'file',
      label: 'notify.py',
      subtitle: 'sherlock_project/notify.py',
      fileId: 'sherlock_project/notify.py',
    })

    const classEntry = index.find((e) => e.id === 'class:sherlock_project/notify.py::QueryNotify')
    expect(classEntry).toMatchObject({
      id: 'class:sherlock_project/notify.py::QueryNotify',
      kind: 'class',
      label: 'QueryNotify',
      subtitle: 'sherlock_project/notify.py',
      fileId: 'sherlock_project/notify.py',
      pieces: ['query', 'notify'],
    })

    const fieldEntry = index.find((e) => e.id === 'member:sherlock_project/notify.py::QueryNotify::.result')
    expect(fieldEntry).toMatchObject({
      kind: 'field',
      label: 'result',
      subtitle: 'QueryNotify · sherlock_project/notify.py',
      fileId: 'sherlock_project/notify.py',
    })

    const methodEntry = index.find((e) => e.id === 'member:sherlock_project/notify.py::QueryNotify::update')
    expect(methodEntry).toMatchObject({
      kind: 'method',
      label: 'update',
      subtitle: 'QueryNotify · sherlock_project/notify.py',
      fileId: 'sherlock_project/notify.py',
    })
  })

  it('splits snake_case, camelCase, PascalCase, and acronyms into lowercase pieces (excluding file extension)', () => {
    const model = buildGraphModel(
      makeGraph(['src/query_notify.py', 'src/AppShell.test.tsx', '.gitignore'], [
        {
          fileId: 'src/query_notify.py',
          className: 'HTMLParser',
          methods: ['getHTTPResponse', '__init__', 'tour_store'],
        },
      ]),
      '',
    )
    const index = buildNameIndex(model)
    const piecesOf = (label: string) => index.find((e) => e.label === label)?.pieces

    expect(piecesOf('query_notify.py')).toEqual(['query', 'notify'])
    expect(piecesOf('AppShell.test.tsx')).toEqual(['app', 'shell', 'test'])
    expect(piecesOf('.gitignore')).toEqual(['gitignore'])
    expect(piecesOf('HTMLParser')).toEqual(['html', 'parser'])
    expect(piecesOf('getHTTPResponse')).toEqual(['get', 'http', 'response'])
    expect(piecesOf('__init__')).toEqual(['init'])
    expect(piecesOf('tour_store')).toEqual(['tour', 'store'])
  })

  it('does not promote file extensions to piece-prefix matches', () => {
    const model = buildGraphModel(
      makeGraph(['src/a.py'], [
        { fileId: 'src/a.py', className: 'Runner', methods: ['runPy'] }, // tier 3: piece prefix ("py"), method < file in kind order
      ]),
      '',
    )
    const results = searchNames(buildNameIndex(model), 'py')
    // If a.py matched at tier 3 via ".py", it would rank above method "runPy" (file > method).
    expect(results.map((r) => r.label)).toEqual(['runPy', 'a.py'])
  })
})

describe('searchNames', () => {
  it('returns empty array for blank or whitespace-only queries', () => {
    const index = buildNameIndex(buildGraphModel(sherlockGraph, ''))
    expect(searchNames(index, '')).toEqual([])
    expect(searchNames(index, '   \t\n  ')).toEqual([])
  })

  it('ranks results by match tier: exact > prefix > piece prefix > substring', () => {
    const model = buildGraphModel(
      makeGraph(['src/app.ts'], [
        { fileId: 'src/app.ts', className: 'RestoreService' }, // tier 4: substring ("restore" contains "store")
        { fileId: 'src/app.ts', className: 'TourStore' }, // tier 3: piece prefix ("store")
        { fileId: 'src/app.ts', className: 'StoreManager' }, // tier 2: label prefix
        { fileId: 'src/app.ts', className: 'Store' }, // tier 1: exact (case-insensitive)
      ]),
      '',
    )
    const index = buildNameIndex(model)
    const results = searchNames(index, '  store  ')

    expect(results.map((r) => r.label)).toEqual(['Store', 'StoreManager', 'TourStore', 'RestoreService'])
    expect(results.every((r) => r.source === 'name')).toBe(true)
  })

  it('matches snake_case and camelCase pieces case-insensitively', () => {
    const model = buildGraphModel(
      makeGraph(['src/tour_store.ts'], [
        {
          fileId: 'src/tour_store.ts',
          className: 'TourStore',
          methods: ['load_store_state', 'resetStorage'],
        },
      ]),
      '',
    )
    const index = buildNameIndex(model)
    const results = searchNames(index, 'STO')

    expect(results.map((r) => r.label)).toEqual(['TourStore', 'tour_store.ts', 'resetStorage', 'load_store_state'])
  })

  it('matches fileId paths containing "/" only for file nodes', () => {
    const model = buildGraphModel(
      makeGraph(['frontend/src/graph/model.ts', 'frontend/src/other/util.ts'], [
        {
          fileId: 'frontend/src/graph/model.ts',
          className: 'NodeBuilder',
          methods: ['build'],
        },
      ]),
      '',
    )
    const index = buildNameIndex(model)
    const results = searchNames(index, 'graph/model')

    expect(results).toEqual([
      {
        id: 'file:frontend/src/graph/model.ts',
        kind: 'file',
        label: 'model.ts',
        subtitle: 'frontend/src/graph/model.ts',
        source: 'name',
      },
    ])
  })

  it('sorts within the same tier by kind (class > file > method > field), then label length, then lexicographically', () => {
    const model = buildGraphModel(
      makeGraph(['src/item'], [
        {
          fileId: 'src/item',
          className: 'item',
          fields: ['item'],
          methods: ['item'],
        },
      ]),
      '',
    )
    const kindResults = searchNames(buildNameIndex(model), 'item')
    expect(kindResults.map((r) => r.kind)).toEqual(['class', 'file', 'method', 'field'])

    const tieModel = buildGraphModel(
      makeGraph(['src/a.ts'], [
        { fileId: 'src/a.ts', className: 'NodeZebra' },
        { fileId: 'src/a.ts', className: 'NodeBeta' },
        { fileId: 'src/a.ts', className: 'NodeAlpha' },
        { fileId: 'src/a.ts', className: 'NodeB' },
        { fileId: 'src/a.ts', className: 'NodeA' },
      ]),
      '',
    )
    const tieResults = searchNames(buildNameIndex(tieModel), 'node')
    expect(tieResults.map((r) => r.label)).toEqual(['NodeA', 'NodeB', 'NodeBeta', 'NodeAlpha', 'NodeZebra'])
  })

  it('caps results at 15 by default and honors a custom limit', () => {
    const classes = Array.from({ length: 20 }, (_, i) => ({
      fileId: 'src/many.ts',
      className: `Item${String(i).padStart(2, '0')}`,
    }))
    const index = buildNameIndex(buildGraphModel(makeGraph(['src/many.ts'], classes), ''))

    const defaultResults = searchNames(index, 'item')
    expect(defaultResults).toHaveLength(15)
    expect(defaultResults[0].label).toBe('Item00')
    expect(defaultResults[14].label).toBe('Item14')

    expect(searchNames(index, 'item', 5)).toHaveLength(5)
  })

  it('preserves distinct ids for same-named members including #1 suffix', () => {
    const model = buildGraphModel(
      makeGraph(['src/prop.py'], [
        {
          fileId: 'src/prop.py',
          className: 'Config',
          fields: ['value'],
          methods: ['value', 'value'],
        },
      ]),
      '',
    )
    const index = buildNameIndex(model)
    const results = searchNames(index, 'value')

    expect(results.map((r) => ({ id: r.id, kind: r.kind }))).toEqual([
      { id: 'member:src/prop.py::Config::value', kind: 'method' },
      { id: 'member:src/prop.py::Config::value#1', kind: 'method' },
      { id: 'member:src/prop.py::Config::.value', kind: 'field' },
    ])
  })
})
