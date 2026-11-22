import { describe, expect, it } from 'vitest'

import { Bm25Builder } from './bm25'
import fixture from './fixtures/bm25.fixture.json'
import { STOPWORDS, tokenize, tokenizePath } from './tokenizer'

describe('tokenizer (Python parity)', () => {
  it('exposes the Python STOPWORDS set', () => {
    for (const word of ['error', 'type', 'class', 'file', 'self', "don't", 'python']) {
      expect(STOPWORDS.has(word)).toBe(true)
    }
  })

  it('matches Python tokenize for all fixture queries in both stopword modes', () => {
    for (const q of fixture.queries) {
      expect(tokenize(q.query, { keepStopwords: true }), `keepStopwords=true: "${q.query}"`).toEqual(
        q.tokensKeepStopwords,
      )
      expect(tokenize(q.query), `default: "${q.query}"`).toEqual(q.tokensDefault)
    }
  })

  it('matches Python tokenizePath and tokenize for all corpus files', () => {
    for (const file of fixture.files) {
      expect(tokenizePath(file.fileId), `path: ${file.fileId}`).toEqual(file.pathTokens)
      expect(tokenize(file.source, { keepStopwords: true }), `source keepStopwords: ${file.fileId}`).toEqual(
        file.sourceTokensKeepStopwords,
      )
      expect(tokenize(file.source), `source default: ${file.fileId}`).toEqual(file.sourceTokensDefault)
    }
  })
})

describe('Bm25Builder / Bm25Index (Python parity)', () => {
  function buildFixtureIndex() {
    const builder = new Bm25Builder()
    for (const file of fixture.files) {
      builder.add(file.fileId, [...tokenizePath(file.fileId), ...tokenize(file.source, { keepStopwords: true })])
    }
    return builder.finish()
  }

  it('matches Python idf values', () => {
    const index = buildFixtureIndex()
    for (const [term, expectedIdf] of Object.entries(fixture.idfs)) {
      expect(Math.abs(index.idf(term) - expectedIdf), `idf(${term})`).toBeLessThanOrEqual(1e-12)
    }
  })

  it('matches Python BM25 scores for all fixture queries within 1e-12', () => {
    const index = buildFixtureIndex()
    expect(index.docIds).toEqual(fixture.files.map((f) => f.fileId))

    for (const q of fixture.queries) {
      const tokens = tokenize(q.query, { keepStopwords: true })
      const scores = index.score(tokens)
      expect(scores).toBeInstanceOf(Float64Array)
      expect(scores.length).toBe(q.scores.length)
      for (let i = 0; i < q.scores.length; i++) {
        expect(
          Math.abs(scores[i] - q.scores[i]),
          `query="${q.query}" doc="${index.docIds[i]}" got=${scores[i]} expected=${q.scores[i]}`,
        ).toBeLessThanOrEqual(1e-12)
      }
    }
  })

  it('top() returns only positive-scoring docs sorted descending, breaking ties by input order', () => {
    const builder = new Bm25Builder()
    builder.add('first.py', ['alpha', 'beta'])
    builder.add('second.py', ['gamma'])
    builder.add('third.py', ['alpha', 'beta'])
    builder.add('fourth.py', ['alpha', 'alpha', 'alpha'])
    const index = builder.finish()

    const all = index.top(['alpha'], 10)
    expect(all.map((r) => r.docId)).toEqual(['fourth.py', 'first.py', 'third.py'])
    expect(all.every((r) => r.score > 0)).toBe(true)
    expect(all[1].score).toBe(all[2].score)

    expect(index.top(['alpha'], 2).map((r) => r.docId)).toEqual(['fourth.py', 'first.py'])
    expect(index.top(['alpha'], 0)).toEqual([])
    expect(index.top(['missing'], 10)).toEqual([])
  })

  it('returns all zeros for empty corpus or all-empty documents (avgdl <= 0)', () => {
    const emptyIndex = new Bm25Builder().finish()
    expect(Array.from(emptyIndex.score(['foo']))).toEqual([])
    expect(emptyIndex.top(['foo'], 5)).toEqual([])

    const zeroLenBuilder = new Bm25Builder()
    zeroLenBuilder.add('a.py', [])
    zeroLenBuilder.add('b.py', [])
    const zeroLenIndex = zeroLenBuilder.finish()
    expect(Array.from(zeroLenIndex.score(['foo']))).toEqual([0, 0])
    expect(zeroLenIndex.top(['foo'], 5)).toEqual([])
  })
})
