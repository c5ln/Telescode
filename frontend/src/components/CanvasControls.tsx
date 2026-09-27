// Zoom in / zoom out / fit, floating in the canvas corner.
//
// Shell only: the graph viewport will supply the handlers. Until then the
// buttons are enabled when a workspace is ready and do nothing.

import { FitIcon, MinusIcon, PlusIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import styles from './CanvasControls.module.css'

interface CanvasControlsProps {
  disabled?: boolean
  onZoomIn?: () => void
  onZoomOut?: () => void
  onFit?: () => void
}

export function CanvasControls({ disabled = false, onZoomIn, onZoomOut, onFit }: CanvasControlsProps) {
  return (
    <div className={styles.controls} role="toolbar" aria-label="Canvas" aria-orientation="vertical">
      <IconButton label="Zoom in" tooltip="left" disabled={disabled} onClick={onZoomIn}>
        <PlusIcon />
      </IconButton>
      <IconButton label="Zoom out" tooltip="left" disabled={disabled} onClick={onZoomOut}>
        <MinusIcon />
      </IconButton>
      <div className={styles.divider} role="separator" />
      <IconButton label="Fit to view" tooltip="left" disabled={disabled} onClick={onFit}>
        <FitIcon />
      </IconButton>
    </div>
  )
}
