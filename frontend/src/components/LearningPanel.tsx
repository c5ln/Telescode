import type { Learning } from '../app/useLearning'
import { Button } from '../ui/Button'
import { IconButton } from '../ui/IconButton'
import { CloseIcon } from '../ui/icons'
import { Spinner } from '../ui/Spinner'
import panel from './TutorialPanel.module.css'
import quiz from './QuizCard.module.css'

export function LearningPanel({ learning }: { learning: Learning }) {
  const { state } = learning
  if (state.status === 'off') return null
  const close = <IconButton label="Back to tutorial" tooltipAlign="end" onClick={learning.close}><CloseIcon /></IconButton>
  if (state.status === 'loading') return <section className={panel.panel} aria-label="Adaptive learning">
    <header className={panel.header}><p className={panel.eyebrow}>Adaptive learning</p>{close}</header>
    {state.error ? <p className={panel.error} role="alert">{state.error}</p> : <p className={panel.waiting}><Spinner /> Preparing a learning path…</p>}
    {(state.pending || state.error) && <div className={panel.actions}>
      {state.pending && <Button onClick={() => learning.advance('stop')}>End session</Button>}
      {state.error && <Button variant="primary" onClick={learning.retry}>Try again</Button>}
    </div>}
  </section>
  const { session, concepts, knowledge } = state.view
  const step = session.current
  const concept = concepts.find(c => c.id === step?.conceptId)
  const estimate = knowledge.find(k => k.conceptId === step?.conceptId && k.level === step?.level)
  const locked = state.busy || Boolean(state.result)
  return <section className={panel.panel} aria-label="Adaptive learning">
    <header className={panel.header}><p className={panel.eyebrow}>Adaptive learning</p>{close}</header>
    <p className={panel.text}>{session.spent} / {session.goal.budget} steps used</p>
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
          {state.question.choices.map((choice, i) => <label className={quiz.choice} key={i}>
            <input type="radio" name={state.question!.id} checked={state.selected === i} onChange={() => learning.select(i)} />{choice}
          </label>)}
        </fieldset>
        {state.result ? <div className={quiz.feedback} role="status">
          <p className={quiz.verdict}>{state.result.feedback.correct ? 'Correct.' : `The answer is “${state.result.feedback.answer}”.`}</p>
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
    {!step && session.status === 'generation-needed' && <>
      <p className={panel.text}>More independent material is needed. Your answers and remaining budget are saved.</p>
      <div className={panel.actions}>
        <Button disabled={state.busy} onClick={() => learning.advance('stop')}>End session</Button>
        <Button variant="primary" disabled={state.busy} onClick={learning.prepareMore}>Prepare more material</Button>
      </div>
    </>}
    {!step && session.status === 'preparing' && <p className={panel.waiting}><Spinner /> Preparing learning material…</p>}
    {!step && ['finished', 'budget-exhausted', 'stopped'].includes(session.status) && <>
      <p className={panel.text}>{session.status === 'budget-exhausted' ? 'Step budget reached.' : session.status === 'stopped' ? 'Session ended.' : 'Planned steps complete.'}</p>
      <ul className={panel.text}>{session.goal.targetConcepts.map(t => {
        const k = knowledge.find(k => k.conceptId === t.conceptId && k.level === t.level)
        return <li key={`${t.conceptId}:${t.level}`}>{concepts.find(c => c.id === t.conceptId)?.name ?? t.conceptId}: {(k?.observations ?? 0) >= 2 && (k?.probability ?? 0) >= .8 ? 'supporting evidence' : 'needs more independent evidence'}</li>
      })}</ul>
    </>}
    {state.busy && <p className={panel.waiting}><Spinner /> Working…</p>}
    {state.error && <p className={panel.error} role="alert">{state.error}</p>}
  </section>
}
