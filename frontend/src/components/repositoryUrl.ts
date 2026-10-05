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
