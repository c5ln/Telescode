// Top bar over a full-bleed canvas. No sidebar: orientation comes from the
// breadcrumbs, which follow where the user is in the code map.

import { useCallback, useRef, useState } from 'react'

import type { TelescodeApi } from '../bridge'
import { TopBar } from '../components/TopBar'
import { WorkspaceCanvas } from '../components/WorkspaceCanvas'
import type { GraphHandle } from '../graph/GraphCanvas'
import type { GraphNode } from '../graph/model'
import type { Crumb } from '../ui/Breadcrumbs'
import type { DropdownItem } from '../ui/Dropdown'
import { tauriTourLibrary, type TourLibrary } from '../tour/tutorial'
import styles from './AppShell.module.css'
import { useTutorial } from './useTutorial'
import { repositoryName, useWorkspace, type WorkspaceState } from './useWorkspace'

interface AppShellProps {
  api?: TelescodeApi
  initialState?: WorkspaceState
  /**
   * Let the empty state open a Telescode database by path. Development only:
   * users start from a repository URL and never see the database.
   */
  allowLocalDatabase?: boolean
  /** Where saved code tours are read from, for tutorial mode. */
  tours?: TourLibrary
}

export function AppShell({ api, initialState, allowLocalDatabase = false, tours = tauriTourLibrary }: AppShellProps) {
  const workspace = useWorkspace(api, initialState)
  const { state } = workspace
  const graphRef = useRef<GraphHandle | null>(null)
  const tutorial = useTutorial(tours, state.status === 'ready' ? state.snapshot : null, graphRef)
  /** Where the user is in the map, below the repository. */
  const [trail, setTrail] = useState<Crumb[]>([])
  /** Complexity mode: on by default, kept across repositories. */
  const [complexity, setComplexity] = useState(true)
  const [lastDbPath, setLastDbPath] = useState(state.status === 'empty' ? '' : state.dbPath)

  const open = (dbPath: string) => {
    setLastDbPath(dbPath)
    setTrail([])
    workspace.open(dbPath)
  }

  const close = () => {
    setTrail([])
    workspace.close()
  }

  const onContextChange = useCallback((path: GraphNode[]) => {
    setTrail(path.map((n) => ({ id: n.id, label: n.label })))
  }, [])

  const crumbs: Crumb[] =
    state.status === 'empty'
      ? [{ id: 'home', label: 'Telescode' }]
      : [{ id: 'root', label: repositoryName(state.dbPath), title: state.dbPath }, ...(state.status === 'ready' ? trail : [])]

  const menuItems: DropdownItem[] = [
    { id: 'open', label: 'Open repository…', onSelect: close },
    {
      id: 'reload',
      label: 'Reload analysis',
      onSelect: workspace.reload,
      disabled: state.status === 'empty' || state.status === 'loading',
    },
    { type: 'separator', id: 'view' },
    { type: 'toggle', id: 'complexity', label: 'Complexity', checked: complexity, onChange: setComplexity },
  ]

  return (
    <div className={styles.shell}>
      <TopBar
        crumbs={crumbs}
        onNavigate={(crumb) => graphRef.current?.navigate(crumb.id)}
        menuItems={menuItems}
        tutorial={{
          active: tutorial.state.status !== 'off',
          disabled: state.status !== 'ready',
          onToggle: tutorial.state.status === 'off' ? tutorial.start : tutorial.exit,
        }}
      />
      <WorkspaceCanvas
        state={state}
        onOpen={open}
        onRetry={workspace.reload}
        onClose={close}
        allowLocalDatabase={allowLocalDatabase}
        lastDbPath={lastDbPath}
        graphRef={graphRef}
        onContextChange={onContextChange}
        complexity={complexity}
        tutorial={tutorial}
      />
    </div>
  )
}
