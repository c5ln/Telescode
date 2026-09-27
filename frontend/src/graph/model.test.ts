import { describe, expect, it } from 'vitest'

import type { GraphResponse } from '../bridge'
import sherlock from './fixtures/sherlock.graph.json'
import { buildGraphModel, contains, type GraphModel } from './model'

// Real `TelescodeHeadless graph` output for repos/sherlock (16 Python files).
const graph = sherlock as GraphResponse

const labels = (model: GraphModel, id: string) => model.byId.get(id)!.children.map((c) => c.label)

describe('buildGraphModel', () => {
  const model = buildGraphModel(graph, 'sherlock')

  it('derives directories from file paths, directories first, then by name', () => {
    expect(labels(model, 'root')).toEqual(['devel', 'sherlock_project', 'tests'])
    expect(labels(model, 'dir:sherlock_project')).toEqual([
      '__init__.py',
      '__main__.py',
      'notify.py',
      'result.py',
      'sherlock.py',
      'sites.py',
    ])
    expect(model.root.fileCount).toBe(16)
    expect(model.root.classCount).toBe(11)
  })

  it('nests classes in their files and members in their classes', () => {
    const cls = model.byId.get('class:sherlock_project/notify.py::QueryNotify')!
    expect(cls.parent!.id).toBe('file:sherlock_project/notify.py')
    expect(cls.children.map((m) => `${m.memberKind}:${m.label}`)).toEqual([
      'field:result',
      'method:__init__',
      'method:start',
      'method:update',
      'method:finish',
      'method:__str__',
    ])
    expect(cls.children[1].detail).toBe('result')
  })

  it('numbers nodes depth-first so a subtree is a contiguous range', () => {
    model.nodes.forEach((n, i) => expect(n.order).toBe(i))
    const dir = model.byId.get('dir:sherlock_project')!
    for (const n of model.nodes) {
      let inside = false
      for (let p: typeof n | null = n; p; p = p.parent) if (p === dir) inside = true
      expect(contains(dir, n)).toBe(inside)
    }
  })

  it('keeps the core edges as they are', () => {
    const imports = model.edges.filter((e) => e.kind === 'import')
    const classes = model.edges.filter((e) => e.kind === 'class')
    expect(imports).toHaveLength(graph.fileGraph.edges.length)
    expect(classes.map((e) => [e.source.label, e.target.label])).toEqual([['QueryNotifyPrint', 'QueryNotify']])
  })

  it('merges single-child directory chains into one region', () => {
    const m = buildGraphModel(
      {
        ...graph,
        files: [graph.files[0], { ...graph.files[0], fileId: 'src/core/deep/a.py' }, { ...graph.files[0], fileId: 'src/core/deep/b.py' }],
        classes: [],
        classEdges: [],
        fileGraph: { nodes: [], edges: [] },
      },
      'repo',
    )
    expect(labels(m, 'root')).toEqual(['devel', 'src/core/deep'])
    expect(labels(m, 'dir:src/core/deep')).toEqual(['a.py', 'b.py'])
  })

  it('gives same-named members distinct ids', () => {
    const [file] = graph.files
    const m = buildGraphModel(
      {
        ...graph,
        files: [file],
        classes: [
          {
            classId: `${file.fileId}::C`,
            nodeId: 0,
            fileId: file.fileId,
            className: 'C',
            package: '',
            fields: [],
            methods: [
              { access: '+', name: 'value', params: '', returnType: 'void' },
              { access: '+', name: 'value', params: 'v', returnType: 'void' },
            ],
          },
        ],
        classEdges: [],
        fileGraph: { nodes: [], edges: [] },
      },
      'repo',
    )
    const ids = m.nodes.filter((n) => n.kind === 'member').map((n) => n.id)
    expect(new Set(ids).size).toBe(2)
  })
})
