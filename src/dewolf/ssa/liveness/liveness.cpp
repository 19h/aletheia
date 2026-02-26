#include "liveness.hpp"

namespace dewolf {

static const std::unordered_set<std::string> EMPTY_SET;

LivenessAnalysis::LivenessAnalysis(ControlFlowGraph& cfg) : cfg_(cfg) {
    create_live_sets();
}

const std::unordered_set<std::string>& LivenessAnalysis::live_in(BasicBlock* block) const {
    auto it = live_in_block_.find(block);
    return it != live_in_block_.end() ? it->second : EMPTY_SET;
}

const std::unordered_set<std::string>& LivenessAnalysis::live_out(BasicBlock* block) const {
    auto it = live_out_block_.find(block);
    return it != live_out_block_.end() ? it->second : EMPTY_SET;
}

const std::unordered_set<std::string>& LivenessAnalysis::defs_phi(BasicBlock* block) const {
    auto it = defs_phi_block_.find(block);
    return it != defs_phi_block_.end() ? it->second : EMPTY_SET;
}

void LivenessAnalysis::extract_vars(Expression* expr, std::unordered_set<std::string>& out) {
    if (!expr) return;
    if (auto* v = dynamic_cast<Variable*>(expr)) {
        out.insert(v->name());
    } else if (auto* op = dynamic_cast<Operation*>(expr)) {
        for (auto* child : op->operands()) {
            extract_vars(child, out);
        }
    } else if (auto* list = dynamic_cast<ListOperation*>(expr)) {
        for (auto* child : list->operands()) {
            extract_vars(child, out);
        }
    }
}

void LivenessAnalysis::init_usages_definitions_of_blocks() {
    for (BasicBlock* block : cfg_.blocks()) {
        for (Instruction* inst : block->instructions()) {
            if (auto* phi = dynamic_cast<Phi*>(inst)) {
                // Phi: destination is a definition, operands are uses
                if (Variable* def_var = phi->dest_var()) {
                    defs_phi_block_[block].insert(def_var->name());
                }
                if (auto* op_list = phi->operand_list()) {
                    for (auto* operand : op_list->operands()) {
                        std::unordered_set<std::string> uses;
                        extract_vars(operand, uses);
                        for (const auto& u : uses) {
                            uses_phi_block_[block].insert(u);
                        }
                    }
                }
            } else if (auto* assign = dynamic_cast<Assignment*>(inst)) {
                // Assignment: destination is a definition, value is a use
                std::unordered_set<std::string> defs;
                extract_vars(assign->destination(), defs);
                for (const auto& d : defs) defs_block_[block].insert(d);

                std::unordered_set<std::string> uses;
                extract_vars(assign->value(), uses);
                for (const auto& u : uses) uses_block_[block].insert(u);
            } else if (auto* branch = dynamic_cast<Branch*>(inst)) {
                // Branch: condition variables are uses
                std::unordered_set<std::string> uses;
                extract_vars(branch->condition(), uses);
                for (const auto& u : uses) uses_block_[block].insert(u);
            } else if (auto* ret = dynamic_cast<Return*>(inst)) {
                // Return: return values are uses
                for (auto* val : ret->values()) {
                    std::unordered_set<std::string> uses;
                    extract_vars(val, uses);
                    for (const auto& u : uses) uses_block_[block].insert(u);
                }
            }
            // Break, Continue, Comment have no variable references
        }
    }
}

void LivenessAnalysis::explore_all_paths(BasicBlock* block, const std::string& variable) {
    if (!block) return;
    
    if (defs_block_[block].contains(variable) || live_in_block_[block].contains(variable)) {
        return;
    }
    
    live_in_block_[block].insert(variable);
    
    if (defs_phi_block_[block].contains(variable)) {
        return;
    }
    
    for (Edge* edge : block->predecessors()) {
        BasicBlock* pred = edge->source();
        if (pred) {
            live_out_block_[pred].insert(variable);
            explore_all_paths(pred, variable);
        }
    }
}

void LivenessAnalysis::create_live_sets() {
    init_usages_definitions_of_blocks();
    
    for (BasicBlock* block : cfg_.blocks()) {
        for (const std::string& var : uses_phi_block_[block]) {
            for (Edge* e : block->predecessors()) {
                BasicBlock* pred = e->source();
                if (pred) {
                    live_out_block_[pred].insert(var);
                    explore_all_paths(pred, var);
                }
            }
        }
        for (const std::string& var : uses_block_[block]) {
            explore_all_paths(block, var);
        }
    }
}

} // namespace dewolf
