// src/ui/class_diagram/cd_view.h
//
// Call order per frame:
//   ImNodes::BeginNodeEditor();
//     DrawClassDiagramContent(graph);   ← nodes + edges
//   ImNodes::EndNodeEditor();
//   UpdateClassDiagramInteraction(graph); ← sync hover/selection state

#pragma once
#include "cd_model.h"

namespace TS {

void DrawClassDiagramContent(CDGraph& graph);
void UpdateClassDiagramInteraction(CDGraph& graph);

} // namespace TS
