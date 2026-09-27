// Label measurement and font state for the canvas.
//
// Both measureText and assigning ctx.font (which parses a CSS font string)
// are slow enough to matter at hundreds of labels per frame. Widths are
// measured once per string and weight at a reference size and scaled (text
// width is linear in font size), and the font is only reassigned when it
// actually changes.

const REFERENCE_SIZE = 100

export class TextMeasurer {
  private readonly ctx: CanvasRenderingContext2D
  private readonly family: string
  /** weight → text → width at REFERENCE_SIZE. */
  private readonly widths = new Map<number, Map<string, number>>()
  private current = ''

  constructor(ctx: CanvasRenderingContext2D, family: string) {
    this.ctx = ctx
    this.family = family
  }

  /** Font sizes snap to quarter pixels so consecutive labels can share a font. */
  static quantize(size: number): number {
    return Math.round(size * 4) / 4
  }

  font(size: number, weight: number): string {
    return `${weight} ${size}px ${this.family}`
  }

  /** Make this the context's font, skipping the (costly) assignment if it already is. */
  use(size: number, weight: number) {
    const font = this.font(size, weight)
    if (font !== this.current) {
      this.ctx.font = font
      this.current = font
    }
  }

  width(text: string, size: number, weight: number): number {
    let byText = this.widths.get(weight)
    if (!byText) this.widths.set(weight, (byText = new Map()))
    let w = byText.get(text)
    if (w === undefined) {
      this.ctx.font = this.font(REFERENCE_SIZE, weight)
      this.current = ''
      w = this.ctx.measureText(text).width
      byText.set(text, w)
    }
    return (w * size) / REFERENCE_SIZE
  }

  /** The text, shortened with an ellipsis to fit `max` px; '' if not even a few characters fit. */
  fit(text: string, size: number, weight: number, max: number): string {
    const full = this.width(text, size, weight)
    if (full <= max) return text
    const ellipsis = this.width('…', size, weight)
    if (max < ellipsis + size * 1.2) return ''
    // Sum per-character widths (cached by character, so the cache stays
    // bounded however many prefixes get tried while zooming).
    let used = ellipsis
    let n = 0
    for (const ch of text) {
      const w = this.width(ch, size, weight)
      if (used + w > max) break
      used += w
      n += ch.length
    }
    return n >= 2 ? `${text.slice(0, n).trimEnd()}…` : ''
  }

  /** Drop cached widths, e.g. once web fonts finish loading. */
  reset() {
    this.widths.clear()
    this.current = ''
  }
}
