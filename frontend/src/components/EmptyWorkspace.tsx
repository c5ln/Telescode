// What the canvas shows before anything is open: what the app does, how to
// use it, a repository URL field and a few examples to try.
//
// Users never deal with Telescode's internal database. In development builds
// a quiet link switches to entering the path of a repository folder on this
// computer, which is scanned the same way as a downloaded repository.

import { useState } from 'react'

import type { RepositorySource } from '../bridge'
import { EmptyState } from '../ui/EmptyState'
import styles from './InlineForm.module.css'
import { OpenFolderForm } from './OpenFolderForm'
import { RepositoryUrlForm } from './RepositoryUrlForm'
import welcome from './EmptyWorkspace.module.css'
import { EXAMPLE_REPOSITORIES, exampleFor, exampleUrl } from './examples'

interface EmptyWorkspaceProps {
  /** Offer the development-only folder entry. */
  allowLocalFolder: boolean
  onOpen: (source: RepositorySource) => void
  /** Last repository opened, to prefill; a folder also starts in folder mode. */
  lastSource?: RepositorySource
}

export function EmptyWorkspace({ allowLocalFolder, onOpen, lastSource }: EmptyWorkspaceProps) {
  const [mode, setMode] = useState<'repository' | 'folder'>(
    allowLocalFolder && lastSource?.kind === 'folder' ? 'folder' : 'repository',
  )

  if (mode === 'folder' && allowLocalFolder) {
    return (
      <EmptyState title="Open a local folder" description="Development only: a repository folder on this computer.">
        <div className={styles.stack}>
          <OpenFolderForm
            initialPath={lastSource?.kind === 'folder' ? lastSource.path : undefined}
            onOpen={(path) => onOpen({ kind: 'folder', path })}
          />
          <button type="button" className={styles.link} onClick={() => setMode('repository')}>
            Use a repository URL
          </button>
        </div>
      </EmptyState>
    )
  }

  return (
    <div className={welcome.hero}>
      <h1 className={welcome.title}>See a Python repository as a map</h1>
      <p className={welcome.lede}>
        Telescode draws the code as a map you can zoom into, picks what to read first, and has your AI agent walk
        you through it with quizzes.
      </p>
      <ol className={welcome.steps} aria-label="How it works">
        <li>
          <span className={welcome.number}>1</span>
          <strong>Paste a GitHub URL</strong>
          <span>Any public repository with Python code.</span>
        </li>
        <li>
          <span className={welcome.number}>2</span>
          <strong>Explore the map</strong>
          <span>Folders, files, classes and methods, zoomable.</span>
        </li>
        <li>
          <span className={welcome.number}>3</span>
          <strong>Take a tour</strong>
          <span>Read in a good order, then check yourself with a quiz.</span>
        </li>
      </ol>
      <div className={welcome.form}>
        <RepositoryUrlForm
          initialUrl={lastSource?.kind === 'github' ? shownUrl(lastSource.url) : undefined}
          onSubmit={(url) => onOpen({ kind: 'github', url })}
        />
        <div className={welcome.examples}>
          <span className={welcome.examplesLabel} id="example-repositories">
            Try an example
          </span>
          <ul aria-labelledby="example-repositories">
            {EXAMPLE_REPOSITORIES.map((example) => (
              <li key={example.name}>
                <button
                  type="button"
                  className={welcome.example}
                  aria-label={`Open ${example.owner}/${example.name}`}
                  onClick={() => onOpen({ kind: 'github', url: exampleUrl(example) })}
                >
                  <span className={welcome.owner}>{example.owner}/</span>
                  {example.name}
                </button>
              </li>
            ))}
          </ul>
        </div>
      </div>
      <p className={welcome.note}>
        Most repositories open in a few seconds, and you will see each step as it happens.
      </p>
      {allowLocalFolder && (
        <button type="button" className={styles.link} onClick={() => setMode('folder')}>
          Open a local folder (dev)
        </button>
      )}
    </div>
  )
}

/** An example is shown by its plain address, not the commit it is pinned to. */
function shownUrl(url: string): string {
  const example = exampleFor(url)
  return example ? `https://github.com/${example.owner}/${example.name}` : url
}
