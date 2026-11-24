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
  return {
    /** Whether to register the tool `name`. */
    exposes(name: string) {
      asked.add(name)
      return !wanted || wanted.has(name)
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
