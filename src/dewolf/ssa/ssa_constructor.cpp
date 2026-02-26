#include "ssa_constructor.hpp"
#include <queue>
#include <stack>

namespace dewolf {

// (Previous methods remain exactly as is, appending implementation for renaming)
void SsaConstructor::run() {
    gather_definitions();
    insert_phi_nodes();
    rename_variables();
}

void SsaConstructor::gather_definitions() {
    for (BasicBlock* block : cfg_.blocks()) {
        for (Instruction* inst : block->instructions()) {
            Operation* op = inst->operation();
            if (op->type() == OperationType::assign) {
                if (!op->operands().empty()) {
                    if (Variable* var = dynamic_cast<Variable*>(op->operands()[0])) {
                        var_defs_[var->name()].push_back(block);
                    }
                }
            }
        }
    }
}

void SsaConstructor::insert_phi_nodes() {
    for (const auto& [var_name, def_blocks] : var_defs_) {
        std::unordered_set<BasicBlock*> in_worklist;
        std::unordered_set<BasicBlock*> has_phi;
        std::queue<BasicBlock*> worklist;

        for (BasicBlock* b : def_blocks) {
            worklist.push(b);
            in_worklist.insert(b);
        }

        while (!worklist.empty()) {
            BasicBlock* x = worklist.front();
            worklist.pop();

            for (BasicBlock* y : dom_tree_.dominance_frontier(x)) {
                if (has_phi.find(y) == has_phi.end()) {
                    std::vector<Expression*> phi_operands;
                    Operation* phi_op = arena_.create<Operation>(OperationType::phi, std::move(phi_operands), 0 /*size*/);
                    Instruction* phi_inst = arena_.create<Instruction>(0 /*BadAddress*/, phi_op);
                    
                    phi_nodes_[y].push_back(phi_inst);
                    
                    has_phi.insert(y);
                    if (in_worklist.find(y) == in_worklist.end()) {
                        in_worklist.insert(y);
                        worklist.push(y);
                    }
                }
            }
        }
    }
}

void SsaConstructor::rename_variables() {
    std::unordered_map<std::string, std::stack<int>> counters;
    std::unordered_map<std::string, int> counts;

    for (const auto& pair : var_defs_) {
        counters[pair.first].push(0);
        counts[pair.first] = 0;
    }

    auto rename_block = [&](BasicBlock* block, auto& rename_block_ref) -> void {
        // 1. Process PHI nodes
        if (phi_nodes_.contains(block)) {
            for (Instruction* phi : phi_nodes_[block]) {
                // In real implementation we assign to a new version of the variable
            }
        }

        // 2. Process regular instructions
        for (Instruction* inst : block->instructions()) {
            Operation* op = inst->operation();
            // In real implementation, read uses first, then update definition
        }

        // 3. Update successors' PHI nodes
        for (Edge* edge : block->successors()) {
            BasicBlock* succ = edge->target();
            if (phi_nodes_.contains(succ)) {
                // Add the current version to the PHI arguments
            }
        }

        // 4. Recurse to dominated children
        // Need dominator tree children to do this cleanly
        // For now, it's just a top-down traversal stub
        // for (BasicBlock* child : dom_tree_children[block]) {
        //    rename_block_ref(child, rename_block_ref);
        // }

        // 5. Pop scopes from stacks
    };

    if (cfg_.entry_block()) {
        rename_block(cfg_.entry_block(), rename_block);
    }
}

} // namespace dewolf
