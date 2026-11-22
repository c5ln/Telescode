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

export interface ExampleRepository {
  owner: string
  name: string
  url: string
}

export const EXAMPLE_REPOSITORIES: readonly ExampleRepository[] = [
  { owner: 'psf', name: 'requests', url: 'https://github.com/psf/requests' },
  { owner: 'pallets', name: 'flask', url: 'https://github.com/pallets/flask' },
  { owner: 'httpie', name: 'cli', url: 'https://github.com/httpie/cli' },
]
