#include "pr/mermaid.h"

int prTestMermaidCApi(void);

// The public header compiled as C. A source that names no diagram fails to parse, and leaves no diagram.
int prTestMermaidCApi(void) {
    PrMermaidDesc desc = {0};
    desc.source = PR_SV("not a diagram");
    PrMermaid* diagram = NULL;
    PrMermaidError error = {0};
    PrStatus status = prMermaidCreate(&desc, NULL, &diagram, &error);
    prMermaidDestroy(NULL);
    return status == PrStatus_Syntax && diagram == NULL && error.line == 1 ? 0 : 1;
}
