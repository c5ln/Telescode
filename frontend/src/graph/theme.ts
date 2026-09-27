// The canvas cannot use CSS variables directly, so the renderer reads the
// design tokens once (and again if the theme ever changes).

export interface GraphTheme {
  region: string
  regionOpen: string
  regionStroke: string
  node: string
  nodeStroke: string
  symbol: string
  symbolStroke: string
  edge: string
  edgeStrong: string
  label: string
  labelSecondary: string
  labelMuted: string
  hover: string
  hoverFill: string
  selected: string
  selectedFill: string
  tipBackground: string
  tipText: string
  font: string
}

const TOKENS: Record<Exclude<keyof GraphTheme, 'font'>, string> = {
  region: '--graph-region',
  regionOpen: '--graph-region-open',
  regionStroke: '--graph-region-stroke',
  node: '--graph-node',
  nodeStroke: '--graph-node-stroke',
  symbol: '--graph-symbol',
  symbolStroke: '--graph-symbol-stroke',
  edge: '--graph-edge',
  edgeStrong: '--graph-edge-strong',
  label: '--graph-label',
  labelSecondary: '--graph-label-secondary',
  labelMuted: '--graph-label-muted',
  hover: '--graph-hover',
  hoverFill: '--graph-hover-fill',
  selected: '--graph-selected',
  selectedFill: '--graph-selected-fill',
  tipBackground: '--surface-inverse',
  tipText: '--text-inverse',
}

/** Neutral stand-ins for environments without styles (tests). */
const FALLBACK = '#808080'

export function readTheme(element: Element): GraphTheme {
  const style = getComputedStyle(element)
  const read = (name: string) => style.getPropertyValue(name).trim() || FALLBACK
  const theme = { font: style.getPropertyValue('--font-ui').trim() || 'sans-serif' } as GraphTheme
  for (const [key, token] of Object.entries(TOKENS)) theme[key as keyof typeof TOKENS] = read(token)
  return theme
}
