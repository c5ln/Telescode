// src/core/json/AnalysisJson.h
// AnalysisSnapshot -> JSON.
//
// Kept apart from AnalysisService on purpose: the snapshot is the core's data
// model, and this is one presentation of it. Adding a second format, or changing
// key names for a frontend, touches only this file.
//
// Key naming is lowerCamelCase for the JSON surface even though the C++ and the
// database columns are snake_case -- the consumer of this is a web frontend, and
// the mapping is applied here rather than leaking either convention into the
// other.

#pragma once

#include "core/AnalysisService.h"

#include <string>

namespace TS {

// The whole snapshot: files, classes, edges, metrics, readingSequence.
std::string AnalysisSnapshotToJson(const AnalysisSnapshot& snap, bool pretty);

// Just the graph portion -- files, their degrees, file edges, and the class
// graph. What a dependency view needs, without the reading order.
std::string GraphToJson(const AnalysisSnapshot& snap, bool pretty);

// Just the reading order, files first, with the scores each entry was ranked by.
std::string ReadingSequenceToJson(const AnalysisSnapshot& snap, bool pretty);

} // namespace TS
