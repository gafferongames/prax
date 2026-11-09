#include "pr/graph.h"

int prTestGraphCApi(void);

// The public header compiled as C. A graph of one node lays out as that node at the origin.
int prTestGraphCApi(void) {
    PrGraph* graph = prGraphCreate();
    PrGraphNodeDesc desc = prGraphNodeDescDefault();
    desc.extent.x = 40;
    desc.extent.y = 20;
    PrGraphNode node = prGraphAddNode(graph, &desc);
    prGraphLayoutCompute(graph, NULL);
    PrRect rect = prGraphNodeRect(graph, node);
    PrRect bounds = prGraphBounds(graph);
    int failed = !prGraphHasLayout(graph) || rect.left != 0 || rect.top != 0 || rect.right != 40 || bounds.bottom != 20;
    prGraphDestroy(graph);
    prGraphDestroy(NULL);
    return failed;
}
