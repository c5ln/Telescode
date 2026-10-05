// Repository URL entry: the product's front door.
//
// URL → download and scan → internal database → analysis. The form only
// checks that the URL names a GitHub repository; everything after that
// happens once it is submitted.

import { useState, type FormEvent } from 'react'

import { Button } from '../ui/Button'
import styles from './InlineForm.module.css'
import { parseGitHubRepositoryUrl } from './repositoryUrl'

interface RepositoryUrlFormProps {
  onSubmit: (url: string) => void
  /** URL to prefill, e.g. the repository opened last. */
  initialUrl?: string
}

export function RepositoryUrlForm({ onSubmit, initialUrl = '' }: RepositoryUrlFormProps) {
  const [url, setUrl] = useState(initialUrl)
  const [invalid, setInvalid] = useState(false)
  const trimmed = url.trim()

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (parseGitHubRepositoryUrl(trimmed)) onSubmit(trimmed)
    else setInvalid(true)
  }

  return (
    <div className={styles.stack}>
      <form className={styles.form} onSubmit={submit}>
        <input
          className={styles.input}
          value={url}
          onChange={(e) => {
            setUrl(e.target.value)
            setInvalid(false)
          }}
          placeholder="https://github.com/owner/repo"
          aria-label="Repository URL"
          aria-invalid={invalid || undefined}
          aria-describedby={invalid ? 'repository-url-notice' : undefined}
          spellCheck={false}
          autoComplete="off"
          autoFocus
        />
        <Button type="submit" variant="primary" disabled={!trimmed}>
          Analyze
        </Button>
      </form>
      {invalid && (
        <p id="repository-url-notice" className={styles.notice} data-kind="invalid" role="status">
          Enter a public GitHub repository URL, such as https://github.com/owner/repo.
        </p>
      )}
    </div>
  )
}
