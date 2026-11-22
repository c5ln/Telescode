import { useId, useState } from 'react'
import { MAX_REQUIREMENTS_LENGTH } from '../learning/goal'
import { Button } from '../ui/Button'
import { IconButton } from '../ui/IconButton'
import { CloseIcon } from '../ui/icons'
import panel from './TutorialPanel.module.css'
import styles from './TourStartPanel.module.css'

interface TourStartPanelProps {
  /** A goal written in advance, e.g. an example repository's; filled in and editable. */
  suggested?: string
  onStart: (requirements: string) => void
  onClose: () => void
}

export function TourStartPanel({ suggested, onStart, onClose }: TourStartPanelProps) {
  const id = useId()
  const [requirements, setRequirements] = useState(suggested ?? '')
  const [submitted, setSubmitted] = useState(false)
  return <section className={panel.panel} aria-label="Start a code tour" onKeyDown={event => {
    if (event.key === 'Escape') { event.preventDefault(); onClose() }
  }}>
    <div className={panel.header}>
      <p className={panel.eyebrow}>Code tour</p>
      <IconButton label="Close tour setup" tooltipAlign="end" onClick={onClose}><CloseIcon /></IconButton>
    </div>
    <h2 className={panel.title}>What would you like to learn?</h2>
    <p className={panel.text} id={`${id}-hint`}>
      {suggested
        ? 'This example comes with a goal, and its learning path is ready. Edit it, or clear it for a general onboarding tour.'
        : 'Leave it blank for a general onboarding tour. Or tell us who you are and what you want to understand, and the tour starts from there.'}
    </p>
    <form onSubmit={event => {
      event.preventDefault()
      if (submitted) return
      setSubmitted(true)
      onStart(requirements.trim())
    }}>
      <label className={styles.label} htmlFor={id}>About you and your goal <span>(optional)</span></label>
      <textarea id={id} className={styles.input} rows={4} value={requirements}
        aria-describedby={`${id}-hint`} maxLength={MAX_REQUIREMENTS_LENGTH} disabled={submitted}
        placeholder="I'm a backend developer who just joined this project. Show me how a request flows from the entry point to the response, and how errors are handled."
        onChange={event => setRequirements(event.target.value)} />
      {suggested && requirements.trim() && requirements.trim() !== suggested.trim() && (
        <p className={panel.text} role="status">A changed goal gets a new learning path, written by your AI agent with your account.</p>
      )}
      <div className={panel.actions}><Button type="submit" variant="primary" disabled={submitted}>Start tour</Button></div>
    </form>
  </section>
}
