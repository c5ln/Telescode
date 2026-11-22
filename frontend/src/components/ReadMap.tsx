// A small map of the repository that fills in as the core reads it: one box
// per top-level folder, one square per file (or per few files, in a large
// repository), dark once read. It shows that reading is moving, and where.

import styles from './ReadMap.module.css'

/** Squares drawn at most; above this each square stands for several files. */
const MAX_CELLS = 1200

export interface ReadMapGroup {
  /** The top-level folder, or '' for files at the top. */
  name: string
  total: number
  read: number
  /** Squares drawn, and how many of them are read. */
  cells: number
  filled: number
}

/** The boxes for `found` with `read` filled in, the largest folder first. */
export function readMapGroups(found: readonly string[], read: readonly string[], maxCells = MAX_CELLS): ReadMapGroup[] {
  const done = new Set(read)
  const groups = new Map<string, { total: number; read: number }>()
  for (const file of found) {
    const slash = file.indexOf('/')
    const name = slash < 0 ? '' : file.slice(0, slash)
    const group = groups.get(name) ?? { total: 0, read: 0 }
    group.total++
    if (done.has(file)) group.read++
    groups.set(name, group)
  }
  const perCell = Math.max(1, Math.ceil(found.length / maxCells))
  return [...groups]
    .sort(([a, x], [b, y]) => y.total - x.total || a.localeCompare(b))
    .map(([name, { total, read }]) => {
      const cells = Math.ceil(total / perCell)
      return { name, total, read, cells, filled: read === total ? cells : Math.floor(read / perCell) }
    })
}

export function ReadMap({ found, read }: { found: readonly string[]; read: readonly string[] }) {
  const groups = readMapGroups(found, read)
  const total = groups.reduce((n, g) => n + g.total, 0)
  const done = groups.reduce((n, g) => n + g.read, 0)
  return (
    <figure className={styles.map}>
      <div className={styles.groups} aria-hidden="true">
        {groups.map((g) => (
          <div key={g.name} className={styles.group} title={`${g.name || 'Top level'}: ${g.read} of ${g.total} read`}>
            <span className={styles.name}>{g.name || '/'}</span>
            <span className={styles.cells}>
              {Array.from({ length: g.cells }, (_, i) => (
                <span key={i} className={styles.cell} data-read={i < g.filled || undefined} />
              ))}
            </span>
          </div>
        ))}
      </div>
      <figcaption className={styles.caption}>
        {done.toLocaleString('en-US')} of {total.toLocaleString('en-US')} files read
      </figcaption>
    </figure>
  )
}
