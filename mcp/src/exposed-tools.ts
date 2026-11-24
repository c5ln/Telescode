// Which tools a server registers. An agent's server is started with its run's
// tools in TELESCODE_MCP_TOOLS (frontend/src-tauri/src/agent.rs), so the model
// is never sent the definitions of tools the run cannot call. The app's own
// server, started without it, registers every tool.

export const TOOLS_ENV = 'TELESCODE_MCP_TOOLS'

/** What to register, and afterwards which named tools no registration asked for. */
export function exposure(env: Record<string, string | undefined> = process.env) {
  const value = env[TOOLS_ENV]
  const wanted = value === undefined ? null : new Set(value.split(',').map(name => name.trim()).filter(Boolean))
  const asked = new Set<string>()
  const has = (name: string) => !wanted || wanted.has(name)
  return {
    /** Whether the server offers the tool `name`, without counting it as registered. */
    has,
    /** Whether to register the tool `name`. */
    exposes(name: string) {
      asked.add(name)
      return has(name)
    },
    /** Names in the variable that are no tool's, such as a renamed tool's old name. */
    unknown() {
      return wanted ? [...wanted].filter(name => !asked.has(name)) : []
    },
  }
}

/** Whether a server started with `env` registers a tool, by name. */
export function exposedTools(env: Record<string, string | undefined> = process.env) {
  const { exposes } = exposure(env)
  return exposes
}

const TOUR_TOOLS = ['create_tour_draft', 'revise_tour_draft', 'render_approved_tour']
const QUIZ_TOOLS = ['get_quiz_context', 'create_quiz']
const LEARNING_TOOLS = ['get_learning_preparation_context', 'save_learning_curriculum', 'get_learning_material_context', 'save_learning_material']

/**
 * The server's instructions for the tools it offers (`has`): the tour
 * instructions for a tour writer, the quiz flow for a quiz writer, the
 * learning instructions for a learning writer, and how to write questions for
 * both of the last two. A server with every tool gets all of them.
 */
export function instructionsFor(has: (name: string) => boolean, parts: { onboarding: string; quiz: string; questions: string; learning: string }) {
  const any = (names: string[]) => names.some(has)
  const quiz = any(QUIZ_TOOLS), learning = any(LEARNING_TOOLS)
  return [any(TOUR_TOOLS) && parts.onboarding, quiz && parts.quiz, (quiz || learning) && parts.questions, learning && parts.learning]
    .filter((part): part is string => Boolean(part)).join('\n\n')
}
