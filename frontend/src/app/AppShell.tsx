// Top bar over a full-bleed canvas. No sidebar: orientation comes from the
// breadcrumbs, which follow where the user is in the code map.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react'

import { savedAgent, saveAgent, type AgentProvider } from '../agent'
import type { RepositoryOpener, RepositorySource, TelescodeApi } from '../bridge'
import { TopBar } from '../components/TopBar'
import { WorkspaceCanvas } from '../components/WorkspaceCanvas'
import type { GraphHandle } from '../graph/GraphCanvas'
import type { GraphNode } from '../graph/model'
import type { Crumb } from '../ui/Breadcrumbs'
import type { DropdownItem } from '../ui/Dropdown'
import { createQuizLibrary, type QuizLibrary } from '../quiz/quiz'
import { createTourLibrary, type TourLibrary } from '../tour/tutorial'
import styles from './AppShell.module.css'
import { useQuiz } from './useQuiz'
import { useTutorial } from './useTutorial'
import { createLearningLibrary, type LearningLibrary } from '../learning/api'
import { useLearning } from './useLearning'
import { exampleFor, type ExampleRepository } from '../components/examples'
import { loadExample, withExample, type ExampleData } from '../examples'
import { sourceLocation, useWorkspace, workspaceName, type WorkspaceState } from './useWorkspace'

interface AppShellProps {
  api?: TelescodeApi
  initialState?: WorkspaceState
  /**
   * Let the empty state open a repository folder on this computer.
   * Development only: users start from a repository URL.
   */
  allowLocalFolder?: boolean
  /** Where saved code tours are read from, for tutorial mode. */
  tours?: TourLibrary
  /** Where quizzes on tutorial steps are read, written and answered. */
  quizzes?: QuizLibrary
  /** Scans a repository, from its URL or folder, into a database. */
  repositories?: RepositoryOpener
  learning?: LearningLibrary
}

export function AppShell({
  api,
  initialState,
  allowLocalFolder = false,
  tours,
  quizzes,
  repositories,
  learning: providedLearning,
}: AppShellProps) {
  const [provider, setProvider] = useState(savedAgent)
  const [pending, setPending] = useState(0)
  const libraries = useMemo(() => {
    // Capture the provider for the whole request, even if the panel is closed.
    const track = <T,>(work: () => Promise<T>): Promise<T> => {
      setPending(count => count + 1)
      return work().finally(() => setPending(count => count - 1))
    }
    const tour = createTourLibrary(provider)
    const quiz = createQuizLibrary(provider)
    const learning = createLearningLibrary(provider)
    return {
      tour: { ...tour, signIn: () => track(tour.signIn!), generate: (...args: Parameters<NonNullable<TourLibrary['generate']>>) => track(() => tour.generate!(...args)) },
      quiz: { ...quiz, signIn: () => track(quiz.signIn!), generate: (...args: Parameters<NonNullable<QuizLibrary['generate']>>) => track(() => quiz.generate!(...args)) },
      learning: { ...learning, start: (input: Parameters<LearningLibrary['start']>[0]) => track(() => learning.start(input)),
        prepare: (...args: Parameters<LearningLibrary['prepare']>) => track(() => learning.prepare(...args)) },
    }
  }, [provider])
  const workspace = useWorkspace(api, initialState, repositories)
  const { state } = workspace
  const graphRef = useRef<GraphHandle | null>(null)
  const ready = state.status === 'ready' ? state : null

  // An example repository plays the tour, quizzes and learning material
  // written for it in advance.
  const example = state.status !== 'empty' && state.source?.kind === 'github' ? exampleFor(state.source.url) : undefined
  const [loadedExample, setLoadedExample] = useState<{ example: ExampleRepository; data: ExampleData } | null>(null)
  useEffect(() => {
    if (!example) return
    let live = true
    loadExample(example).then(
      (data) => { if (live && data) setLoadedExample({ example, data }) },
      () => {},
    )
    return () => { live = false }
  }, [example])
  const exampleData = loadedExample?.example === example ? loadedExample?.data : undefined
  const content = useMemo(
    () => (exampleData && ready ? withExample(libraries, exampleData, ready.snapshot) : libraries),
    [exampleData, ready, libraries],
  )

  const tutorial = useTutorial(tours ?? content.tour, ready?.snapshot ?? null, ready?.dbPath ?? null, graphRef)
  const quiz = useQuiz(quizzes ?? content.quiz, ready?.snapshot ?? null, ready?.dbPath ?? null, graphRef)
  const learning = useLearning(providedLearning ?? content.learning, ready?.snapshot ?? null, ready?.dbPath ?? null, graphRef)
  /** Where the user is in the map, below the repository. */
  const [trail, setTrail] = useState<Crumb[]>([])
  /** Complexity mode: on by default, kept across repositories. */
  const [complexity, setComplexity] = useState(true)
  /** Offered again when returning to the empty state. */
  const [lastSource, setLastSource] = useState(state.status === 'empty' ? undefined : state.source)

  const open = (source: RepositorySource) => {
    setLastSource(source)
    setTrail([])
    workspace.open(source)
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
      : [{ id: 'root', label: workspaceName(state), title: state.source ? sourceLocation(state.source) : state.dbPath }, ...(state.status === 'ready' ? trail : [])]

  const menuItems: DropdownItem[] = [
    { id: 'open', label: 'Open repository…', onSelect: close },
    {
      id: 'reload',
      label: 'Reload analysis',
      onSelect: workspace.reload,
      disabled: state.status === 'empty' || state.status === 'loading',
    },
  ]

  const switchAgent = (next: AgentProvider) => { saveAgent(next); setProvider(next) }

  const toggleTour =
    tutorial.state.status === 'off'
      ? tutorial.open
      : () => {
          quiz.close()
          learning.close()
          tutorial.exit()
        }
  const tourDisabled = state.status !== 'ready'

  // T starts or leaves the code tour, unless the user is typing.
  const tourShortcut = useRef<(() => void) | null>(null)
  tourShortcut.current = tourDisabled ? null : toggleTour
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 't' && e.key !== 'T') return
      if (e.ctrlKey || e.metaKey || e.altKey || e.repeat) return
      const target = e.target as HTMLElement | null
      if (target?.closest('input, textarea, select, [contenteditable="true"]')) return
      if (!tourShortcut.current) return
      e.preventDefault()
      tourShortcut.current()
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [])

  return (
    <div className={styles.shell}>
      <TopBar
        crumbs={crumbs}
        agent={{ provider, disabled: pending > 0 || tutorial.state.status === 'searching' || quiz.state.status === 'searching',
          onChange: switchAgent }}
        onNavigate={(crumb) => graphRef.current?.navigate(crumb.id)}
        menuItems={menuItems}
        tutorial={{ active: tutorial.state.status !== 'off', disabled: tourDisabled, onToggle: toggleTour }}
      />
      <WorkspaceCanvas
        state={state}
        onOpen={open}
        onRetry={workspace.reload}
        onClose={close}
        allowLocalFolder={allowLocalFolder}
        lastSource={lastSource}
        graphRef={graphRef}
        onContextChange={onContextChange}
        complexity={complexity}
        onComplexityChange={setComplexity}
        tutorial={tutorial}
        quiz={quiz}
        provider={provider}
        onSwitchAgent={switchAgent}
        suggestedRequirements={example?.requirements}
        learning={learning}
      />
    </div>
  )
}
