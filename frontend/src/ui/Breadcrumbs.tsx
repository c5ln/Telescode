// Where you are, as a path of any depth: `src › core › dependency.cpp`.
//
// Ancestors are buttons that navigate back up; the last item is the current
// location and is emphasised. Long paths collapse their middle into a `…`
// menu, and each remaining label truncates with an ellipsis, ancestors first.

import { Dropdown } from './Dropdown'
import { ChevronRightIcon } from './icons'
import styles from './Breadcrumbs.module.css'

export interface Crumb {
  id: string
  label: string
  /** Full text for the tooltip, if different from the label (e.g. a path). */
  title?: string
}

interface BreadcrumbsProps {
  items: readonly Crumb[]
  onNavigate?: (crumb: Crumb, index: number) => void
  /** Most items shown before the middle collapses. Minimum 3. */
  maxVisible?: number
}

type Slot = { kind: 'crumb'; crumb: Crumb; index: number } | { kind: 'overflow'; hidden: { crumb: Crumb; index: number }[] }

/** First item, a `…` holding the middle, then the last `maxVisible - 2` items. */
function layoutCrumbs(items: readonly Crumb[], maxVisible: number): Slot[] {
  const all = items.map((crumb, index) => ({ kind: 'crumb' as const, crumb, index }))
  const max = Math.max(3, maxVisible)
  if (all.length <= max) return all
  const tail = max - 2
  return [all[0], { kind: 'overflow', hidden: all.slice(1, all.length - tail) }, ...all.slice(all.length - tail)]
}

export function Breadcrumbs({ items, onNavigate, maxVisible = 5 }: BreadcrumbsProps) {
  if (items.length === 0) return <nav aria-label="Breadcrumb" className={styles.nav} />
  const last = items.length - 1
  const slots = layoutCrumbs(items, maxVisible)

  return (
    <nav aria-label="Breadcrumb" className={styles.nav}>
      <ol className={styles.list}>
        {slots.map((slot, i) => {
          const separator = i > 0 && <ChevronRightIcon className={styles.separator} />
          if (slot.kind === 'overflow') {
            return (
              <li key="overflow" className={styles.item} data-kind="overflow">
                {separator}
                <Dropdown
                  label="Hidden path levels"
                  items={slot.hidden.map(({ crumb, index }) => ({
                    id: crumb.id,
                    label: crumb.label,
                    title: crumb.title ?? crumb.label,
                    onSelect: () => onNavigate?.(crumb, index),
                  }))}
                  trigger={(props) => (
                    <button
                      {...props}
                      type="button"
                      className={styles.crumb}
                      aria-label={`Show ${slot.hidden.length} hidden levels`}
                      title={slot.hidden.map(({ crumb }) => crumb.label).join(' › ')}
                    >
                      …
                    </button>
                  )}
                />
              </li>
            )
          }
          const { crumb, index } = slot
          const current = index === last
          return (
            <li key={crumb.id} className={styles.item} data-kind={current ? 'current' : 'ancestor'}>
              {separator}
              {current ? (
                <span className={styles.current} aria-current="location" title={crumb.title ?? crumb.label}>
                  {crumb.label}
                </span>
              ) : (
                <button
                  type="button"
                  className={styles.crumb}
                  title={crumb.title ?? crumb.label}
                  disabled={!onNavigate}
                  onClick={() => onNavigate?.(crumb, index)}
                >
                  <span className={styles.text}>{crumb.label}</span>
                </button>
              )}
            </li>
          )
        })}
      </ol>
    </nav>
  )
}

