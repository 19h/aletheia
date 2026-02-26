#include "structurer.hpp"
#include <unordered_set>

namespace dewolf {

void CyclicRegionFinder::process(TransitionCFG& cfg) {
    detect_back_edges(cfg);
    formulate_loop_bodies();
    synthesize_breaks_and_continues();
}

void CyclicRegionFinder::detect_back_edges(TransitionCFG& cfg) {
}

void CyclicRegionFinder::formulate_loop_bodies() {
}

void CyclicRegionFinder::synthesize_breaks_and_continues() {
}

void AcyclicRegionRestructurer::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;

    // Simple ad-hoc traverser to create an AST seq for the demo.
    // Real implementation would collapse nodes in the transition CFG until it's just the root node.

    SeqNode* seq = arena_.create<SeqNode>();

    std::unordered_set<TransitionBlock*> visited;
    TransitionBlock* current = cfg.entry();

    while (current && visited.find(current) == visited.end()) {
        visited.insert(current);
        
        // 2-way branch
        if (current->successors().size() == 2) {
            TransitionBlock* true_succ = current->successors()[0];
            TransitionBlock* false_succ = current->successors()[1];
            
            // Heuristic to check if they merge
            TransitionBlock* merge = nullptr;
            if (true_succ->successors().size() == 1 && false_succ->successors().size() == 1) {
                if (true_succ->successors()[0] == false_succ->successors()[0]) {
                    merge = true_succ->successors()[0];
                }
            }

            IfNode* if_node = arena_.create<IfNode>(
                nullptr, // cond
                true_succ->ast_node(),
                false_succ->ast_node()
            );

            // Create a sequence that wraps the branch
            SeqNode* bseq = arena_.create<SeqNode>();
            bseq->add_node(current->ast_node());
            bseq->add_node(if_node);
            
            seq->add_node(bseq);

            visited.insert(true_succ);
            visited.insert(false_succ);

            current = merge; // Continue from merge point
        } else if (current->successors().size() == 1) {
            // Unconditional edge
            seq->add_node(current->ast_node());
            current = current->successors()[0];
        } else {
            // End of CFG or unsupported
            seq->add_node(current->ast_node());
            current = nullptr;
        }
    }

    // Replace the entry node's AST with this new sequence.
    // (This works around the structuring fallback that only picks the entry node)
    cfg.entry()->set_ast_node(seq);
}

} // namespace dewolf
