// Repositories offered on the first screen, so a new user can see the map
// without finding a repository first.
//
// Chosen to open fast and read well (measured with TelescodeHeadless on
// 2026-11-22, download included):
//   psf/requests   37 Python files, about 1 s. Small and well known; its source
//                  and tests split cleanly, so the first map is easy to read.
//   pallets/flask  83 files, about 1 s. A web framework with a clear core.
//   httpie/cli    133 files, about 1.5 s. A command-line app with plugins.
// Avoid repositories whose Python files are mostly docs examples (fastapi/typer
// has 643, most of them snippets) or that download slowly.
//
// Each is pinned to a commit, and its tour, quizzes and learning material for
// `requirements` are written in advance (scripts/build-examples.ts), so an
// example plays at once instead of waiting for an agent.

export interface ExampleRepository {
  owner: string
  name: string
  /** The commit the example's tour and quizzes were written for. */
  commit: string
  /** Suggested for "About you and your goal" when the example is open. */
  requirements: string
}

export const EXAMPLE_REPOSITORIES: readonly ExampleRepository[] = [
  {
    owner: 'psf',
    name: 'requests',
    commit: '3c7baaa05ab4aa49cc819ce3a819d260de19c157',
    requirements:
      "I'm a backend developer who calls requests every day. Show me how requests.get() becomes an HTTP request, and where sessions and adapters fit in.",
  },
  {
    owner: 'pallets',
    name: 'flask',
    commit: '49943b5a94413a530761350f04e896f76e8d620e',
    requirements:
      "I'm a Python developer new to Flask's internals. Show me how an incoming request reaches a view function: routing, the request context and the app object.",
  },
  {
    owner: 'httpie',
    name: 'cli',
    commit: '5b604c37c6c67e18e7c3e9aee6c88a8c22b98345',
    requirements:
      "I'm a frontend developer who uses HTTPie in the terminal. Show me how a command line becomes an HTTP request, and how the response is formatted and printed.",
  },
]

/** The URL that opens an example at its pinned commit. */
export function exampleUrl(example: ExampleRepository): string {
  return `https://github.com/${example.owner}/${example.name}/tree/${example.commit}`
}

/** The example a repository URL opens, if any. */
export function exampleFor(url: string): ExampleRepository | undefined {
  return EXAMPLE_REPOSITORIES.find((e) => url === exampleUrl(e))
}
