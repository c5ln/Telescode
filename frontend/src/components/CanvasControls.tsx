// Zoom in / zoom out / fit, floating in the canvas corner, and a sheet of
// every way to move around the map.

import { useEffect, useRef, useState } from 'react'

import { FitIcon, HelpIcon, MinusIcon, PlusIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import styles from './CanvasControls.module.css'

interface CanvasControlsProps {
  disabled?: boolean
  onZoomIn?: () => void
  onZoomOut?: () => void
  onFit?: () => void
}

const MOUSE: [string, string][] = [
  ['Zoom', 'Scroll, or pinch'],
  ['Move', 'Drag'],
  ["See a file's links", 'Click'],
  ['Zoom into a folder or file', 'Double-click'],
]

const KEYS: [string, string[]][] = [
  ['Zoom in, out', ['+', '−']],
  ['Fit the whole map', ['0']],
  ['Move', ['←', '→', '↑', '↓']],
  ['Zoom to the selection, clear it', ['Enter', 'Esc']],
  ['Start or leave a code tour', ['T']],
]

export function CanvasControls({ disabled = false, onZoomIn, onZoomOut, onFit }: CanvasControlsProps) {
  const [help, setHelp] = useState(false)
  const root = useRef<HTMLDivElement>(null)

  // Closes on Escape or a press anywhere else.
  useEffect(() => {
    if (!help) return
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && setHelp(false)
    const onPress = (e: PointerEvent) => {
      if (!root.current?.contains(e.target as Node)) setHelp(false)
    }
    window.addEventListener('keydown', onKey)
    document.addEventListener('pointerdown', onPress)
    return () => {
      window.removeEventListener('keydown', onKey)
      document.removeEventListener('pointerdown', onPress)
    }
  }, [help])

  return (
    <div className={styles.dock} ref={root}>
      {help && (
        <section className={styles.help} aria-label="Moving around the map">
          <h2>Moving around the map</h2>
          <p className={styles.section}>Mouse</p>
          <dl>
            {MOUSE.map(([what, how]) => (
              <div key={what}>
                <dt>{what}</dt>
                <dd>{how}</dd>
              </div>
            ))}
          </dl>
          <p className={styles.section}>Keyboard, once the map has focus</p>
          <dl>
            {KEYS.map(([what, keys]) => (
              <div key={what}>
                <dt>{what}</dt>
                <dd>
                  {keys.map((k) => (
                    <kbd key={k}>{k}</kbd>
                  ))}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      )}
      <div className={styles.controls} role="toolbar" aria-label="Canvas" aria-orientation="vertical">
        <IconButton label="Zoom in" shortcut="+" tooltip="left" disabled={disabled} onClick={onZoomIn}>
          <PlusIcon />
        </IconButton>
        <IconButton label="Zoom out" shortcut="−" tooltip="left" disabled={disabled} onClick={onZoomOut}>
          <MinusIcon />
        </IconButton>
        <div className={styles.divider} role="separator" />
        <IconButton label="Fit to view" shortcut="0" tooltip="left" disabled={disabled} onClick={onFit}>
          <FitIcon />
        </IconButton>
        <div className={styles.divider} role="separator" />
        <IconButton
          label="How to move around"
          tooltip={help ? false : 'left'}
          disabled={disabled}
          aria-expanded={help}
          onClick={() => setHelp((open) => !open)}
        >
          <HelpIcon />
        </IconButton>
      </div>
    </div>
  )
}
