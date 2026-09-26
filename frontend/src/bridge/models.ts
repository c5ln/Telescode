// TypeScript view of the JSON printed by TelescodeHeadless.
//
// Mirrors src/core/json/AnalysisJson.cpp (schemaVersion 1) field for field.
// These are shapes only: every value is computed by the C++ core, and nothing
// in the frontend derives, recomputes or reinterprets it.
//
// Floating-point fields are `number | null` because the core writes NaN and
// infinity as null (see JsonWriter.cpp).

export const SUPPORTED_SCHEMA_VERSION = 1

export interface AnalysisTotals {
  fileCount: number
  classCount: number
  classEdgeCount: number
  /** Nodes in the file-level graph. */
  fileNodeCount: number
  fileEdgeCount: number
  /** Nodes in the function/class-level graph. */
  funcNodeCount: number
  funcEdgeCount: number
  /** Rows in the reading_sequence table. */
  sequenceCount: number
}

export interface FileMetrics {
  rawLoc: number
  logicalLoc: number
  maxCyclomaticComplexity: number
  avgCyclomaticComplexity: number | null
  maxBlockDepth: number
  avgBlockDepth: number | null
  maxFunctionLoc: number
  avgFunctionLoc: number | null
  complexityScore: number | null
}

export interface FileEntry {
  fileId: string
  fileName: string
  language: string
  isGenerated: boolean
  metrics: FileMetrics
}

export interface FileGraphNode {
  fileId: string
  inbound: number
  outbound: number
}

export interface FileGraphEdge {
  /** fileId of the importing file. */
  source: string
  /** fileId of the imported file. */
  target: string
}

export interface FileGraph {
  nodes: FileGraphNode[]
  edges: FileGraphEdge[]
}

/** '+' public, '-' private, '#' protected, '~' package. */
export type Access = '+' | '-' | '#' | '~'

export interface ClassField {
  access: Access
  name: string
}

export interface ClassMethod {
  access: Access
  name: string
  /** Already-joined parameter list, e.g. "req, qty". */
  params: string
  /** "void" when the source declares none. */
  returnType: string
}

export interface ClassNode {
  /** Stable database key; what classEdges reference. */
  classId: string
  /** Integer handle assigned by the core's class-graph builder. */
  nodeId: number
  fileId: string
  className: string
  package: string
  fields: ClassField[]
  methods: ClassMethod[]
}

export type ClassEdgeType = 'dependency' | 'association' | 'unknown'

export interface ClassEdge {
  edgeId: number
  /** classId, or "" if the core could not resolve the endpoint. */
  source: string
  target: string
  type: ClassEdgeType
}

export type EntityType = 'file' | 'class' | 'function'

export interface ReadingSequenceScores {
  pagerank: number | null
  betweenness: number | null
  combined: number | null
}

export interface ReadingSequenceEntry {
  entityId: string
  entityType: EntityType
  fileId: string
  /** Set for file rows, null otherwise. */
  fileRank: number | null
  /** Set for class/function rows, null for file rows. */
  localRank: number | null
  scores: ReadingSequenceScores
}

interface ResponseBase {
  schemaVersion: number
  dbPath: string
  totals: AnalysisTotals
}

/** `TelescodeHeadless graph` */
export interface GraphResponse extends ResponseBase {
  files: FileEntry[]
  fileGraph: FileGraph
  classes: ClassNode[]
  classEdges: ClassEdge[]
}

/** `TelescodeHeadless sequence` */
export interface SequenceResponse extends ResponseBase {
  readingSequence: ReadingSequenceEntry[]
}

/** `TelescodeHeadless analyze` -- the whole snapshot. */
export interface AnalysisSnapshot extends GraphResponse, SequenceResponse {}

export type AnalysisResponse = AnalysisSnapshot
