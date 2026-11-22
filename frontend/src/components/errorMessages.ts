// What an error tells the user: what happened, why, what to do next, and
// which actions fit. Every failure the workspace shows goes through here, so
// they all read the same way.
//
// Errors fall into groups by what the user can do about them:
//   input    the address or path needs fixing: edit it, or try an example
//   network  trying again may work: try again, or go back
//   scope    outside what Telescode reads: open another repository
//   app      something is wrong with the app itself: report it
// The core's own message is kept for the details, not the headline.

import type { RepositorySource, TelescodeError } from '../bridge'
import { parseGitHubRepositoryUrl } from './repositoryUrl'

export type ErrorAction = 'edit' | 'example' | 'retry' | 'back' | 'another' | 'report'

export interface ErrorDescription {
  /** Red for failures, amber for things outside what Telescode supports. */
  tone: 'error' | 'warning'
  title: string
  cause: string
  next: string
  /** The first is the main action. */
  actions: ErrorAction[]
}

const REPORT_NEXT = 'If it happens again, report it with the details below.'

function repositoryName(source?: RepositorySource): string {
  if (!source) return 'This repository'
  if (source.kind === 'folder') return source.path
  const repository = parseGitHubRepositoryUrl(source.url)
  return repository ? `${repository.owner}/${repository.name}` : source.url
}

export function describeError(error: TelescodeError, source?: RepositorySource): ErrorDescription {
  const name = repositoryName(source)
  const folder = source?.kind === 'folder'
  const editInput: ErrorAction[] = ['edit', 'example']

  switch (error.code) {
    // ---- Input --------------------------------------------------------
    case 'invalid_repository_url':
      return { tone: 'error', title: 'Not a GitHub repository address', cause: `${name} is not an address Telescode can open.`,
        next: 'Use an address like github.com/owner/repo.', actions: editInput }
    case 'repository_not_found':
      return { tone: 'error', title: 'Repository not found', cause: `${name} does not exist, or it is private.`,
        next: 'Check the spelling, or open a public repository instead.', actions: editInput }
    case 'folder_not_found':
      return { tone: 'error', title: 'Folder not found', cause: `${name} does not exist.`,
        next: 'Check the path, then open it again.', actions: editInput }
    case 'not_a_folder':
      return { tone: 'error', title: 'Not a folder', cause: `${name} is a file, not a folder.`,
        next: "Enter the repository's folder rather than a file inside it.", actions: editInput }
    case 'invalid_argument':
      return { tone: 'error', title: 'Check the address', cause: error.message,
        next: folder ? "Enter the folder's full path." : 'Use an address like github.com/owner/repo.', actions: editInput }

    // ---- Network ------------------------------------------------------
    case 'repository_unreachable':
      return /limiting requests/i.test(error.message)
        ? { tone: 'error', title: 'GitHub is busy', cause: 'GitHub is limiting requests from this computer for now.',
            next: 'Wait a few minutes, then try again.', actions: ['retry', 'back'] }
        : { tone: 'error', title: 'Could not reach GitHub', cause: 'Telescode could not connect to GitHub.',
            next: 'Check your internet connection, then try again.', actions: ['retry', 'back'] }
    case 'repository_fetch_failed':
      return { tone: 'error', title: 'Download did not finish', cause: 'The download stopped before it finished.',
        next: 'Check your internet connection, then try again.', actions: ['retry', 'back'] }

    // ---- Outside what Telescode reads ----------------------------------
    case 'repository_too_large':
      return { tone: 'warning', title: 'Repository too large', cause: `${name} is larger than Telescode can open (500 MB to download, 2 GB unpacked).`,
        next: 'Try a smaller repository, or one of the examples.', actions: ['another', 'example'] }
    case 'repository_empty':
      return { tone: 'warning', title: 'No Python code found', cause: `${name} has no .py files Telescode can read.`,
        next: 'Telescode reads Python for now. Try a repository written in Python.', actions: ['another', 'example'] }

    // ---- The app itself -------------------------------------------------
    case 'bridge_unavailable':
      return { tone: 'error', title: 'Desktop app required', cause: 'This page is running in a browser, which cannot analyze code.',
        next: 'Open Telescode as a desktop app.', actions: ['back'] }
    case 'sidecar_missing':
      return { tone: 'error', title: 'Telescode core not found', cause: 'The analysis engine that ships with the app is missing.',
        next: `Reinstall the app. ${REPORT_NEXT}`, actions: ['report', 'back'] }
    case 'sidecar_spawn_failed':
      return { tone: 'error', title: 'Telescode core could not start', cause: 'The analysis engine that ships with the app did not run.',
        next: `Reinstall the app. ${REPORT_NEXT}`, actions: ['report', 'back'] }
    case 'sidecar_failed':
      return { tone: 'error', title: 'Analysis failed', cause: `The analysis engine stopped with an error while reading ${name}.`,
        next: `Try again. ${REPORT_NEXT}`, actions: ['retry', 'report'] }
    case 'db_not_found':
    case 'db_not_a_file':
    case 'db_invalid':
      return { tone: 'error', title: 'Could not open the analysis', cause: 'The saved analysis is missing or damaged.',
        next: 'Open the repository again to analyze it from scratch.', actions: ['retry', 'back'] }
    case 'unsupported_schema_version':
    case 'malformed_json':
    case 'unexpected_schema':
    case 'output_not_utf8':
      return { tone: 'error', title: 'Could not read the analysis', cause: 'The analysis engine and the app do not match.',
        next: `Reinstall the app. ${REPORT_NEXT}`, actions: ['report', 'back'] }
    default:
      return { tone: 'error', title: 'Something went wrong', cause: `Telescode could not open ${name}.`,
        next: `Try again. ${REPORT_NEXT}`, actions: ['retry', 'report'] }
  }
}

/** Everything a bug report needs, as plain text. */
export function errorDetails(error: TelescodeError): string {
  const { path, exitCode, stderr } = error.details
  return [
    error.code,
    error.message,
    path && `path: ${path}`,
    exitCode != null && `exit code: ${exitCode}`,
    stderr?.trim() && `stderr:\n${stderr.trim().split('\n').slice(-20).join('\n')}`,
  ]
    .filter(Boolean)
    .join('\n')
}

const ISSUES = 'https://github.com/c5ln/Telescode/issues/new'

/** A new GitHub issue, filled in with the error. */
export function reportUrl(title: string, details: string): string {
  const body = `**What happened**\n${title}\n\n**Details**\n\`\`\`\n${details}\n\`\`\`\n\n**What I was doing**\n\n`
  return `${ISSUES}?labels=bug&title=${encodeURIComponent(`[bug] ${title}`)}&body=${encodeURIComponent(body)}`
}
