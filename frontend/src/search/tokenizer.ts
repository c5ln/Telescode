// Code-aware tokenizer ported line-for-line from `ltr/retrieval/tokenizer.py`.
//
// Keeps both the original identifier (lowercased, edge underscores stripped)
// and its snake_case / camelCase / CONST_CASE subtokens so exact matches and
// notation-shifted matches both score.

const WORD_RE = /[A-Za-z_][A-Za-z0-9_]*|\d+/g
const CAMEL_RE = /[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+/g

export const MIN_LEN = 2

export const STOPWORDS: ReadonlySet<string> = new Set(
  `
a an the and or but if then else of to in on at by for with without from as is are was were
be been being do does did done have has had having will would shall should can could may might
must not no nor so such than that this these those it its it's i we you he she they them us
what which who whom whose when where why how all any both each few more most other some only
own same too very just also into out up down over under again further once here there
i'm i've don't doesn't isn't aren't wasn't weren't didn't won't can't cannot
self cls none true false def class return import lambda pass raise yield with try except
finally elif while for print len str int float bool list dict set tuple type object super
py python version issue bug error problem expected actual example code line lines file files
please thanks hi hello reproduce reproducible steps environment details description
`
    .trim()
    .split(/\s+/),
)

export function tokenize(text: string, opts?: { keepStopwords?: boolean }): string[] {
  const keepStopwords = opts?.keepStopwords ?? false
  const tokens: string[] = []

  for (const match of text.matchAll(WORD_RE)) {
    const raw = match[0]
    const low = raw.toLowerCase().replace(/^_+|_+$/g, '')
    if (!low) continue

    const parts: string[] = []
    for (const partMatch of raw.matchAll(CAMEL_RE)) {
      const part = partMatch[0]
      if (part) parts.push(part.toLowerCase())
    }

    if (parts.length > 1) {
      if (low.length >= MIN_LEN && (keepStopwords || !STOPWORDS.has(low))) {
        tokens.push(low)
      }
      for (const p of parts) {
        if (p.length >= MIN_LEN && (keepStopwords || !STOPWORDS.has(p))) {
          tokens.push(p)
        }
      }
    } else if (low.length >= MIN_LEN && (keepStopwords || !STOPWORDS.has(low))) {
      tokens.push(low)
    }
  }

  return tokens
}

export function tokenizePath(fileId: string): string[] {
  return tokenize(fileId.replaceAll('/', ' ').replaceAll('.', ' ').replaceAll('-', ' '))
}
