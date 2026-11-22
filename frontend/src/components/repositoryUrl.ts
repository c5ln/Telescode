// Reading a repository URL: which public GitHub repository it names.
//
// Mirrors `parse_url` in src-tauri/src/repository.rs, which has the final say;
// this copy only lets the form point out a mistake before anything is fetched.

export interface GitHubRepository {
  owner: string
  name: string
}

const GITHUB_URL = /^https?:\/\/(?:www\.)?github\.com\/([a-z0-9-]+)\/([\w.-]+?)(?:\.git)?\/?(?:[?#].*)?$/i

/** `owner/name` from https://github.com/owner/name (optionally `.git`, a trailing slash, a query or fragment). */
export function parseGitHubRepositoryUrl(value: string): GitHubRepository | null {
  const match = GITHUB_URL.exec(value.trim())
  if (!match) return null
  const [, owner, name] = match
  return name === '.' || name === '..' ? null : { owner: owner!, name: name! }
}

/** What the user typed, read as a repository: the URL to open, or what to fix. */
export type RepositoryInput = { url: string; repository: GitHubRepository } | { problem: string }

const EXAMPLE = 'github.com/owner/repo'
const SHORTHAND = /^([a-z0-9-]+)\/([\w.-]+?)(?:\.git)?$/i
const SSH = /^git@github\.com:([a-z0-9-]+)\/([\w.-]+?)(?:\.git)?$/i

/**
 * Reads the common ways people write a repository: the full URL, the URL
 * without https://, `owner/repo`, an SSH remote, or a link to a file or pull
 * request inside it. Anything else gets a sentence on how to fix it.
 */
export function readRepositoryInput(value: string): RepositoryInput {
  const text = value.trim()
  const ssh = SSH.exec(text)
  const short = SHORTHAND.exec(text)
  let url = ssh ? `https://github.com/${ssh[1]}/${ssh[2]}` : short ? `https://github.com/${short[1]}/${short[2]}` : text
  if (!/^https?:\/\//i.test(url)) url = `https://${url}`

  let host = ''
  let segments: string[] = []
  try {
    const parsed = new URL(url)
    host = parsed.hostname.toLowerCase()
    segments = parsed.pathname.split('/').filter(Boolean)
  } catch {
    return { problem: `That is not a repository address. Use one like ${EXAMPLE}.` }
  }
  if (host !== 'github.com' && host !== 'www.github.com') {
    return host.includes('.')
      ? { problem: `Only GitHub is supported. Use an address like ${EXAMPLE}.` }
      : { problem: `That is not a repository address. Use one like ${EXAMPLE}.` }
  }
  if (segments.length < 2) return { problem: `Add the repository name, as in ${EXAMPLE}.` }

  const canonical = `https://github.com/${segments[0]}/${segments[1]!.replace(/\.git$/i, '')}`
  const repository = parseGitHubRepositoryUrl(canonical)
  return repository ? { url: canonical, repository } : { problem: `That is not a valid repository name. Use one like ${EXAMPLE}.` }
}
