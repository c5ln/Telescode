import { agents, type AgentProvider } from '../agent'
import type { Learning } from '../app/useLearning'
import { Button } from '../ui/Button'
import { IconButton } from '../ui/IconButton'
import { CloseIcon } from '../ui/icons'
import { ErrorState } from '../ui/ErrorState'
import { Spinner } from '../ui/Spinner'
import { AgentError } from './AgentError'
import { AgentProgress } from './AgentProgress'
import panel from './TutorialPanel.module.css'
import quiz from './QuizCard.module.css'

export function LearningPanel({ learning, provider = 'claude', onSwitchAgent }: {
  learning: Learning
  provider?: AgentProvider
  onSwitchAgent?: (provider: AgentProvider) => void
}) {
  const { state } = learning
  if (state.status === 'off') return null
  const close = <IconButton label="Back to tour" tooltipAlign="end" onClick={learning.close}><CloseIcon /></IconButton>
  if (state.status === 'loading') return <section className={panel.panel} aria-label="Adaptive learning">
    <header className={panel.header}><p className={panel.eyebrow}>Adaptive learning</p>{close}</header>
    {state.error && state.prepared
      ? <ErrorState layout="inline" title="The learning path did not open" cause="Telescode could not load this example's prepared learning path."
          next="Try again." details={state.error}><Button variant="primary" onClick={learning.retry}>Try again</Button></ErrorState>
      : state.error
      ? <AgentError what="learning path" message={state.error} provider={provider} onRetry={learning.retry} onSwitchAgent={onSwitchAgent} />
      : state.activity
        ? <AgentProgress work="learning path" agent={agents[provider].name} activity={state.activity} />
        : <p className={panel.waiting}><Spinner /> Preparing a learning path…</p>}
    {state.pending && <div className={panel.actions}><Button onClick={() => learning.advance('stop')}>End session</Button></div>}
  </section>
  const { session, concepts, knowledge } = state.view
  const step = session.current
  const concept = concepts.find(c => c.id === step?.conceptId)
  const estimate = knowledge.find(k => k.conceptId === step?.conceptId && k.level === step?.level)
  const locked = state.busy || Boolean(state.result)
  // An example's path is as long as its prepared material, so the step budget is not shown for it.
  const prepared = learning.isPrepared(session.goal.goalId)
  const progress = step
    ? prepared ? `Step ${session.spent + 1}` : `Step ${session.spent + 1} of up to ${session.goal.budget}`
    : prepared ? `${session.spent} ${session.spent === 1 ? 'step' : 'steps'} done` : `${session.spent} of up to ${session.goal.budget} steps done`
  const asked = state.view.answers.reduce((n, a) => n + a.asked, 0)
  const correct = state.view.answers.reduce((n, a) => n + a.correct, 0)
  return <section className={panel.panel} aria-label="Adaptive learning">
    <header className={panel.header}><p className={panel.eyebrow}>Adaptive learning</p>{close}</header>
    <p className={panel.text}>{progress}</p>
    <h2 className={panel.title}>{concept?.name ?? 'Learning path'}</h2>
    <p className={panel.text}>{session.goal.rawQuery}</p>
    {step && <>
      <p className={panel.caption}>{step.caption}</p>
      <p className={panel.text}>{(estimate?.observations ?? 0) < 2 ? 'Understanding unconfirmed' : 'Understanding estimate'} · {estimate?.observations ?? 0} independent answers · {step.level}</p>
      <p className={panel.text}>Model estimates are provisional. Reading alone does not confirm understanding.</p>
      {state.question ? <>
        <h3 className={panel.title}>{state.question.question}</h3>
        <fieldset className={quiz.choices} disabled={locked}>
          <legend className={quiz.hidden}>Choices</legend>
          {state.question.choices.map((choice, i) => <label className={quiz.choice} key={i}
            data-state={state.result && (choice === state.result.feedback.answer ? 'correct' : i === state.selected ? 'incorrect' : undefined)}>
            <input type="radio" name={state.question!.id} checked={state.selected === i} onChange={() => learning.select(i)} />{choice}
          </label>)}
        </fieldset>
        {state.result ? <div className={quiz.feedback} role="status" data-correct={state.result.feedback.correct}>
          <p className={quiz.verdict}>{state.result.feedback.correct ? 'Correct.' : `Not quite. The answer is “${state.result.feedback.answer}”.`}</p>
          <p className={panel.text}>{state.result.feedback.explanation}</p>
          <div className={quiz.evidence}>{state.result.feedback.evidence.map(e => <button className={quiz.node} key={e.nodeId} onClick={() => learning.showNode(e.nodeId)}>{e.title}</button>)}</div>
          <p className={panel.text}>The next step will use this answer. Continue when ready.</p>
        </div> : <div className={panel.actions}><Button variant="primary" disabled={state.busy || state.selected === null} onClick={learning.submit}>Check answer</Button></div>}
      </> : <p className={panel.text}>No fresh question is available. Complete the reading as a provisional learning step, or skip it.</p>}
      <div className={panel.actions}>
        <Button disabled={state.busy} onClick={() => learning.advance('stop')}>End session</Button>
        <Button disabled={state.busy} onClick={() => learning.advance('skip')}>Skip concept</Button>
        <Button variant="primary" disabled={state.busy || Boolean(state.question && !state.result)} onClick={() => learning.advance('continue')}>Continue learning</Button>
      </div>
    </>}
    {!step && session.status === 'generation-needed' && learning.isPrepared(session.goal.goalId) && <>
      <p className={panel.text}>You have answered every question prepared for this example. End the session to see how it went.</p>
      <div className={panel.actions}>
        <Button variant="primary" disabled={state.busy} onClick={() => learning.advance('stop')}>End session</Button>
      </div>
    </>}
    {!step && state.waiting && <>
      <p className={panel.waiting}><Spinner /> Writing the next question for your level…</p>
      <AgentProgress work="next question" agent={agents[provider].name} activity={state.waiting} />
    </>}
    {!step && !state.waiting && session.status === 'generation-needed' && !learning.isPrepared(session.goal.goalId) && <>
      <p className={panel.text}>More independent material is needed. Your answers and remaining budget are saved.</p>
      <div className={panel.actions}>
        <Button disabled={state.busy} onClick={() => learning.advance('stop')}>End session</Button>
        <Button variant="primary" disabled={state.busy} onClick={learning.prepareMore}>Prepare more material</Button>
      </div>
    </>}
    {!step && session.status === 'preparing' && <p className={panel.waiting}><Spinner /> Preparing learning material…</p>}
    {!step && ['finished', 'budget-exhausted', 'stopped'].includes(session.status) && <>
      <p className={panel.text}>{session.status === 'budget-exhausted' ? (prepared ? "You reached the end of this example's learning path." : 'Step budget reached.')
        : session.status === 'stopped' ? 'Session ended.' : 'Planned steps complete.'}</p>
      {asked > 0 && <p className={panel.text}>You answered {correct} of {asked} {asked === 1 ? 'question' : 'questions'} correctly.</p>}
      <ul className={panel.text}>{session.goal.targetConcepts.map(t => {
        const k = knowledge.find(k => k.conceptId === t.conceptId && k.level === t.level)
        const about = state.view.answers.find(a => a.conceptId === t.conceptId && a.level === t.level)
        // Understood takes two independent answers and a confident estimate, as the planner's goal does.
        const understood = (k?.observations ?? 0) >= 2 && (k?.probability ?? 0) >= .8
        return <li key={`${t.conceptId}:${t.level}`}>
          <strong>{concepts.find(c => c.id === t.conceptId)?.name ?? t.conceptId}</strong>: {understood ? 'Understood' : 'Not confirmed yet'}
          {about ? ` (${about.correct} of ${about.asked} ${about.asked === 1 ? 'answer' : 'answers'} about it correct)` : ' (no questions answered about it yet)'}
        </li>
      })}</ul>
    </>}
    {state.busy && <p className={panel.waiting}><Spinner /> Working…</p>}
    {state.error && <p className={panel.error} role="alert">{state.error}</p>}
  </section>
}
