// src/ui/class_diagram/cd_view.h
//
// Call order per frame:
//   ImNodes::BeginNodeEditor();
//     DrawClassDiagramContent(graph, zoom);   ← nodes + edges
//   ImNodes::EndNodeEditor();
//   SyncClassDiagramPositions(graph, zoom);   ← capture drags back into the model
//   UpdateClassDiagramInteraction(graph);     ← sync hover/selection state

#pragma once
#include "cd_model.h"

namespace TS {

void DrawClassDiagramContent(CDGraph& graph, float zoom);
// Call after ImNodes::EndNodeEditor() — draws arrowheads on top of all imnodes channels.
void DrawClassDiagramArrowheads(CDGraph& graph, float zoom);
// Call after ImNodes::EndNodeEditor() — writes dragged node positions back into
// CDNode::pos in logical space. Skipping it makes nodes snap back on drag.
void SyncClassDiagramPositions(CDGraph& graph, float zoom);
void UpdateClassDiagramInteraction(CDGraph& graph);

} // namespace TS
