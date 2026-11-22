// What the map means and how to move around it, shown without getting in
// the way: a legend that stays folded until asked for, a one-time hint that
// goes away once the user has tried the map, and a card with a file's own
// numbers while the pointer rests on it.

import { useLayoutEffect, useRef, useState, type ReactNode } from 'react'

import type { MapInteraction } from '../graph/GraphCanvas'
import type { GraphNode } from '../graph/model'
import { IconButton } from '../ui/IconButton'
import { CloseIcon, LegendIcon } from '../ui/icons'
import styles from './MapOverlays.module.css'

const COMPLEXITY_SHADES = [1, 2, 3, 4].map((i) => `var(--graph-complexity-${i})`)

function read(key: string): string | null {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}

function write(key: string, value: string) {
  try {
    localStorage.setItem(key, value)
  } catch {
    // Only a convenience: the map works the same without it.
  }
}

// ---- Legend ----------------------------------------------------------------

const LEGEND_KEY = 'telescode.map.legend.v1'

interface MapLegendProps {
  complexity: boolean
  onComplexityChange: (on: boolean) => void
  /** Files and classes on the map, e.g. "16 files, 11 classes". */
  summary: string
}

/** Bottom left: a folded legend with the complexity switch, then the map's totals. */
export function MapLegend({ complexity, onComplexityChange, summary }: MapLegendProps) {
  const [open, setOpen] = useState(() => read(LEGEND_KEY) === 'open')
  const toggle = (next: boolean) => {
    setOpen(next)
    write(LEGEND_KEY, next ? 'open' : 'closed')
  }

  return (
    <div className={styles.legendDock}>
      {open && (
        <section className={styles.legend} aria-label="Legend">
          <div className={styles.legendHeader}>
            <span>Legend</span>
            <IconButton label="Close legend" tooltip={false} className={styles.small} onClick={() => toggle(false)}>
              <CloseIcon />
            </IconButton>
          </div>
          <LegendRow
            sample={
              <span className={styles.sizes} aria-hidden="true">
                <span />
                <span />
              </span>
            }
            title="Size"
            text="Bigger box, more lines of code"
          />
          <LegendRow sample={<Shades />} title="Shade" text="Darker, more complex than the rest of this repository" />
          <LegendRow
            sample={
              <svg className={styles.arrow} viewBox="0 0 40 12" aria-hidden="true">
                <path d="M2 6h32M30 2.5L35 6l-5 3.5" />
              </svg>
            }
            title="Line"
            text="One file imports another"
          />
          <label className={styles.switchRow}>
            <span>Show complexity</span>
            <input
              type="checkbox"
              role="switch"
              className={styles.switch}
              checked={complexity}
              onChange={(e) => onComplexityChange(e.target.checked)}
            />
          </label>
        </section>
      )}
      <div className={styles.dockRow}>
        {!open && (
          <button type="button" className={styles.legendChip} aria-expanded={false} onClick={() => toggle(true)}>
            <LegendIcon />
            Legend
            <Shades small />
          </button>
        )}
        <p className={styles.summary}>{summary}</p>
      </div>
    </div>
  )
}

function LegendRow({ sample, title, text }: { sample: ReactNode; title: string; text: string }) {
  return (
    <div className={styles.legendRow}>
      <div className={styles.sample}>{sample}</div>
      <p>
        <strong>{title}</strong>
        {text}
      </p>
    </div>
  )
}

function Shades({ small = false }: { small?: boolean }) {
  return (
    <span className={styles.shades} data-small={small || undefined} aria-hidden="true">
      {COMPLEXITY_SHADES.map((shade) => (
        <span key={shade} style={{ background: shade }} />
      ))}
    </span>
  )
}

// ---- First-time hint ---------------------------------------------------------

const HINT_KEY = 'telescode.map.hint.v1'
/** Different things to try before the hint has done its job. */
const TRIED_ENOUGH = 2

/** Whether the hint is still due, and what to call as the user tries the map. */
export function useMapHint() {
  const [done, setDone] = useState(() => read(HINT_KEY) === 'done')
  const [tried, setTried] = useState<ReadonlySet<MapInteraction>>(new Set())
  const finish = () => {
    setDone(true)
    write(HINT_KEY, 'done')
  }
  const interacted = (kind: MapInteraction) => {
    if (done || tried.has(kind)) return
    const next = new Set(tried).add(kind)
    setTried(next)
    if (next.size >= TRIED_ENOUGH) finish()
  }
  return { due: !done, interacted, dismiss: finish }
}

export function MapHint({ onDismiss }: { onDismiss: () => void }) {
  return (
    <div className={styles.hint} role="note" aria-label="How to move around the map">
      <Hint icon={<MouseIcon />} verb="Scroll" text="to zoom" />
      <Hint icon={<DragIcon />} verb="Drag" text="to move" />
      <Hint icon={<PointerIcon />} verb="Click" text="a file to see its links" />
      <Hint icon={<PointerIcon double />} verb="Double-click" text="to zoom in" />
      <button type="button" className={styles.hintDone} onClick={onDismiss}>
        Got it
      </button>
    </div>
  )
}

function Hint({ icon, verb, text }: { icon: ReactNode; verb: string; text: string }) {
  return (
    <span className={styles.hintItem}>
      {icon}
      <span>
        <strong>{verb}</strong> {text}
      </span>
    </span>
  )
}

const glyph = (children: ReactNode) => (
  <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
    {children}
  </svg>
)
const MouseIcon = () => glyph(<><rect x="4.5" y="2" width="7" height="12" rx="3.5" /><path d="M8 4.5v2" /></>)
const DragIcon = () =>
  glyph(<path d="M8 2v12M2 8h12M8 2L6.5 3.5M8 2l1.5 1.5M8 14l-1.5-1.5M8 14l1.5-1.5M2 8l1.5-1.5M2 8l1.5 1.5M14 8l-1.5-1.5M14 8l-1.5 1.5" />)
const PointerIcon = ({ double = false }: { double?: boolean }) =>
  glyph(<><path d="M4 2.5l7 4-3 1.2-1.2 3.3z" />{double && <path d="M11.5 10.5l2 2M13.5 9l1 .2M10 12.5l.2 1" />}</>)

// ---- File card -----------------------------------------------------------------

const LEVEL_NAMES = ['Low', 'Moderate', 'High', 'Very high']

export interface FileFacts {
  node: GraphNode
  /** 0 to 3, or -1 without data. */
  level: number
  imports: number
  usedBy: number
}

const files = (n: number) => `${n} ${n === 1 ? 'file' : 'files'}`

/** The file card's numbers as one sentence, for a screen reader when the file is selected. */
export function fileFactsText({ node, level, imports, usedBy }: FileFacts): string {
  return [
    node.loc !== undefined && `${node.loc.toLocaleString('en-US')} lines`,
    level >= 0 && `${LEVEL_NAMES[level]} complexity for this repository`,
    `imports ${files(imports)}`,
    `used by ${files(usedBy)}`,
  ].filter(Boolean).join(', ')
}

/** Space kept between the card and the map's edges. */
const EDGE = 8

/**
 * A file's own numbers, by the pointer, so the legend's encodings read off
 * the map. Below the pointer when it fits, otherwise above it, and always
 * inside the map.
 */
export function FileCard({ facts, x, y, offset }: { facts: FileFacts; x: number; y: number; offset: number }) {
  const { node, level, imports, usedBy } = facts
  const path = node.id.replace(/^file:/, '')
  const ref = useRef<HTMLDivElement>(null)
  useLayoutEffect(() => {
    const card = ref.current
    const area = card?.offsetParent
    if (!card || !area) return
    const height = card.offsetHeight
    const space = area.clientHeight
    const below = y + offset
    const top = below + height <= space - EDGE ? below : y - offset - height
    card.style.top = `${Math.max(EDGE, Math.min(top, space - height - EDGE))}px`
  }, [x, y, offset, facts])
  return (
    <div ref={ref} className={styles.card} style={{ left: x, top: y + offset }} role="tooltip">
      <p className={styles.cardName}>{node.label}</p>
      {path !== node.label && <p className={styles.cardPath}>{path}</p>}
      <dl className={styles.cardFacts}>
        {node.loc !== undefined && (
          <>
            <dt>Lines</dt>
            <dd>{node.loc.toLocaleString('en-US')}</dd>
          </>
        )}
        {level >= 0 && (
          <>
            <dt>Complexity</dt>
            <dd>
              <span className={styles.cardShade} style={{ background: COMPLEXITY_SHADES[level] }} aria-hidden="true" />
              {LEVEL_NAMES[level]} for this repository
            </dd>
          </>
        )}
        <dt>Imports</dt>
        <dd>{files(imports)}</dd>
        <dt>Used by</dt>
        <dd>{files(usedBy)}</dd>
      </dl>
      <p className={styles.cardFoot}>Click to see its links. Double-click to zoom in.</p>
    </div>
  )
}
