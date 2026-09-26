// Bridge proof: enter a database path, ask the C++ core for an analysis, show
// the totals it reports. Deliberately minimal -- the real UI comes later.

import { useState, type FormEvent } from 'react'

import { telescode, TelescodeError, type AnalysisSnapshot } from './bridge'

type State =
  | { status: 'idle' }
  | { status: 'loading' }
  | { status: 'ok'; snapshot: AnalysisSnapshot }
  | { status: 'error'; error: TelescodeError }

function toTelescodeError(e: unknown): TelescodeError {
  return e instanceof TelescodeError
    ? e
    : new TelescodeError('unknown', e instanceof Error ? e.message : String(e))
}

export default function App() {
  const [dbPath, setDbPath] = useState('')
  const [state, setState] = useState<State>({ status: 'idle' })

  async function load(event: FormEvent) {
    event.preventDefault()
    setState({ status: 'loading' })
    try {
      setState({ status: 'ok', snapshot: await telescode.analyze(dbPath) })
    } catch (e) {
      setState({ status: 'error', error: toTelescodeError(e) })
    }
  }

  return (
    <main style={{ maxWidth: 640, margin: '0 auto', padding: '2rem 1rem' }}>
      <h1 style={{ fontSize: '1.25rem' }}>Telescode — bridge check</h1>

      <form onSubmit={load} style={{ display: 'flex', gap: '0.5rem', flexWrap: 'wrap' }}>
        <label htmlFor="db-path" style={{ flexBasis: '100%' }}>
          Database path
        </label>
        <input
          id="db-path"
          value={dbPath}
          onChange={(e) => setDbPath(e.target.value)}
          placeholder="C:\path\to\telescode.db"
          spellCheck={false}
          style={{ flex: 1, minWidth: 0, padding: '0.4rem', font: 'inherit' }}
        />
        <button type="submit" disabled={state.status === 'loading'}>
          {state.status === 'loading' ? 'Loading…' : 'Load analysis'}
        </button>
      </form>

      {state.status === 'ok' && <Summary snapshot={state.snapshot} />}
      {state.status === 'error' && (
        <div role="alert" data-testid="error" style={{ marginTop: '1.5rem', color: '#cf222e' }}>
          <strong data-testid="error-code">{state.error.code}</strong>
          <pre style={{ whiteSpace: 'pre-wrap', margin: '0.25rem 0 0' }}>{state.error.message}</pre>
        </div>
      )}
    </main>
  )
}

function Summary({ snapshot }: { snapshot: AnalysisSnapshot }) {
  const t = snapshot.totals
  const rows: [string, string | number][] = [
    ['Schema version', snapshot.schemaVersion],
    ['Database', snapshot.dbPath],
    ['Files', t.fileCount],
    ['Classes', t.classCount],
    ['Class edges', t.classEdgeCount],
    ['File edges', t.fileEdgeCount],
    ['Function graph nodes', t.funcNodeCount],
    ['Function graph edges', t.funcEdgeCount],
    ['Reading sequence entries', t.sequenceCount],
  ]
  return (
    <dl data-testid="summary" style={{ marginTop: '1.5rem', display: 'grid', gridTemplateColumns: 'auto 1fr', gap: '0.25rem 1rem' }}>
      {rows.map(([label, value]) => (
        <div key={label} style={{ display: 'contents' }}>
          <dt>{label}</dt>
          <dd style={{ margin: 0, overflowWrap: 'anywhere' }} data-testid={`total-${label}`}>
            {value}
          </dd>
        </div>
      ))}
    </dl>
  )
}
