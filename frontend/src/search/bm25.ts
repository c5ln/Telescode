// Okapi BM25 ported from `ltr/retrieval/bm25.py` (D2–D6).
//
// Parameters: k1 = 1.2, b = 0.75, linear query term weighting (k3 = None),
// and Lucene smoothed IDF: ln(1 + (N - df + 0.5) / (df + 0.5)).
//
// Uses an inverted index (`Map<term, Array<[docIdx, tf]>>`) and iterates query
// terms in first-appearance order (matching Python's `Counter` insertion order)
// so floating-point accumulation order matches `bm25.py`.

export const K1_DEFAULT = 1.2
export const B_DEFAULT = 0.75

export type Posting = readonly [docIdx: number, tf: number]

export class Bm25Index {
  readonly docIds: string[]
  private readonly docLens: Int32Array
  private readonly postings: ReadonlyMap<string, readonly Posting[]>
  private readonly avgdl: number
  private readonly k1: number
  private readonly b: number

  constructor(
    docIds: string[],
    docLens: Int32Array,
    postings: ReadonlyMap<string, readonly Posting[]>,
    avgdl: number,
    k1 = K1_DEFAULT,
    b = B_DEFAULT,
  ) {
    this.docIds = docIds
    this.docLens = docLens
    this.postings = postings
    this.avgdl = avgdl
    this.k1 = k1
    this.b = b
  }

  idf(term: string): number {
    const df = this.postings.get(term)?.length ?? 0
    return Math.log(1.0 + (this.docIds.length - df + 0.5) / (df + 0.5))
  }

  score(queryTokens: readonly string[]): Float64Array {
    const n = this.docIds.length
    const scores = new Float64Array(n)
    if (n === 0 || this.avgdl <= 0) return scores

    // Insertion order matches Python's `Counter(query_tokens)`.
    const qtf = new Map<string, number>()
    for (const token of queryTokens) {
      qtf.set(token, (qtf.get(token) ?? 0) + 1)
    }

    for (const [term, qn] of qtf) {
      const posting = this.postings.get(term)
      if (!posting || posting.length === 0) continue
      const w = this.idf(term) * qn
      for (const [docIdx, f] of posting) {
        const denom = f + this.k1 * (1.0 - this.b + (this.b * this.docLens[docIdx]) / this.avgdl)
        scores[docIdx] += (w * f * (this.k1 + 1.0)) / denom
      }
    }

    return scores
  }

  top(queryTokens: readonly string[], limit: number): Array<{ docId: string; score: number }> {
    if (limit <= 0) return []
    const scores = this.score(queryTokens)
    const positive: number[] = []
    for (let i = 0; i < scores.length; i++) {
      if (scores[i] > 0) positive.push(i)
    }
    positive.sort((a, b) => scores[b] - scores[a] || a - b)
    return positive.slice(0, limit).map((i) => ({ docId: this.docIds[i], score: scores[i] }))
  }
}

export class Bm25Builder {
  private readonly k1: number
  private readonly b: number
  private readonly docIds: string[] = []
  private readonly docLens: number[] = []
  private readonly postings = new Map<string, Array<[number, number]>>()
  private totalLen = 0

  constructor(opts?: { k1?: number; b?: number }) {
    this.k1 = opts?.k1 ?? K1_DEFAULT
    this.b = opts?.b ?? B_DEFAULT
  }

  add(docId: string, tokens: readonly string[]): void {
    const docIdx = this.docIds.length
    this.docIds.push(docId)
    this.docLens.push(tokens.length)
    this.totalLen += tokens.length

    const tf = new Map<string, number>()
    for (const token of tokens) {
      tf.set(token, (tf.get(token) ?? 0) + 1)
    }

    for (const [term, count] of tf) {
      let list = this.postings.get(term)
      if (!list) {
        list = []
        this.postings.set(term, list)
      }
      list.push([docIdx, count])
    }
  }

  finish(): Bm25Index {
    const n = this.docIds.length
    const avgdl = n > 0 ? this.totalLen / n : 0
    return new Bm25Index(
      this.docIds.slice(),
      Int32Array.from(this.docLens),
      this.postings,
      avgdl,
      this.k1,
      this.b,
    )
  }
}
