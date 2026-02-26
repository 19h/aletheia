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
    }
}

void LivenessAnalysis::init_usages_definitions_of_blocks() {
    for (BasicBlock* block : cfg_.blocks()) {
        for (Instruction* inst : block->instructions()) {
            Operation* op = inst->operation();
            if (!op) continue;

            if (op->type() == OperationType::phi) {
                // Phi structure: target is op->operands()[0], sources are the rest (usually pair of block, var)
                // For simplicity, we just extract all uses from sources
                if (!op->operands().empty()) {
                    std::unordered_set<std::string> defs;
                    extract_vars(op->operands()[0], defs);
                    for (const auto& d : defs) defs_phi_block_[block].insert(d);

                    for (size_t i = 1; i < op->operands().size(); ++i) {
                        std::unordered_set<std::string> uses;
                        extract_vars(op->operands()[i], uses);
                        for (const auto& u : uses) {
                            // We need to associate this with predecessor block, but for now we just track it.
                            // In a real Phi node, operands contain info about which predecessor it comes from.
                            // We will approximate it here.
                            uses_phi_block_[block].insert(u);
                        }
                    }
                }
            } else if (op->type() == OperationType::assign && op->operands().size() == 2) {
                std::unordered_set<std::string> defs;
                extract_vars(op->operands()[0], defs);
                for (const auto& d : defs) defs_block_[block].insert(d);

                std::unordered_set<std::string> uses;
                extract_vars(op->operands()[1], uses);
                for (const auto& u : uses) uses_block_[block].insert(u);
            } else {
                std::unordered_set<std::string> uses;
                extract_vars(op, uses);
                for (const auto& u : uses) uses_block_[block].insert(u);
            }
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
            // For true Phi logic, we trace back to specific predecessor.
            // Here we assume it flows to all predecessors uniformly for simplicity in this stub.
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
