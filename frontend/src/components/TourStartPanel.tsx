import { useId, useState } from 'react'
import { MAX_REQUIREMENTS_LENGTH } from '../learning/goal'
import { Button } from '../ui/Button'
import { IconButton } from '../ui/IconButton'
import { CloseIcon } from '../ui/icons'
import panel from './TutorialPanel.module.css'
import styles from './TourStartPanel.module.css'

interface TourStartPanelProps {
  onStart: (requirements: string) => void
  onClose: () => void
}

export function TourStartPanel({ onStart, onClose }: TourStartPanelProps) {
  const id = useId()
  const [requirements, setRequirements] = useState('')
  const [submitted, setSubmitted] = useState(false)
  return <section className={panel.panel} aria-label="Start tutorial" onKeyDown={event => {
    if (event.key === 'Escape') { event.preventDefault(); onClose() }
  }}>
    <div className={panel.header}>
      <p className={panel.eyebrow}>Tutorial</p>
      <IconButton label="Close tour setup" tooltipAlign="end" onClick={onClose}><CloseIcon /></IconButton>
    </div>
    <h2 className={panel.title}>What would you like to learn?</h2>
    <p className={panel.text} id={`${id}-hint`}>Leave blank for onboarding, or describe a feature or code flow to explore.</p>
    <form onSubmit={event => {
      event.preventDefault()
      if (submitted) return
      setSubmitted(true)
      onStart(requirements.trim())
    }}>
      <label className={styles.label} htmlFor={id}>Requirements <span>(optional)</span></label>
      <textarea id={id} className={styles.input} rows={3} value={requirements}
        aria-describedby={`${id}-hint`} maxLength={MAX_REQUIREMENTS_LENGTH} disabled={submitted}
        placeholder="Explain how a login request is authenticated."
        onChange={event => setRequirements(event.target.value)} />
      <div className={panel.actions}><Button type="submit" variant="primary" disabled={submitted}>Start tour</Button></div>
    </form>
  </section>
}
