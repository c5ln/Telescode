// Shape check only: whether a string looks like something `git clone` takes.

/** https://host/owner/repo, ssh://..., git://..., or scp-style git@host:owner/repo. */
export function isRepositoryUrl(value: string): boolean {
  return /^(https?|ssh|git):\/\/[^\s/]+\/\S+$/i.test(value) || /^[\w.-]+@[\w.-]+:\S+$/.test(value)
}
