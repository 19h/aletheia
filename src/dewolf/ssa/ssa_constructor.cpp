#include "ssa_constructor.hpp"
#include <queue>
#include <stack>

namespace dewolf {

void SsaConstructor::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    DominatorTree dom_tree(*task.cfg());
    gather_definitions(*task.cfg());
    insert_phi_nodes(task.arena(), *task.cfg(), dom_tree);
    rename_variables(task.arena(), *task.cfg(), dom_tree);
}

void SsaConstructor::gather_definitions(ControlFlowGraph& cfg) {
    for (BasicBlock* block : cfg.blocks()) {
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

void SsaConstructor::insert_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree) {
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

            for (BasicBlock* y : dom_tree.dominance_frontier(x)) {
                if (has_phi.find(y) == has_phi.end()) {
                    std::vector<Expression*> phi_operands;
                    // First operand is target variable
                    Variable* target = arena.create<Variable>(var_name, 8); // simplified size
                    phi_operands.push_back(target);
                    
                    Operation* phi_op = arena.create<Operation>(OperationType::phi, std::move(phi_operands), 8);
                    Instruction* phi_inst = arena.create<Instruction>(0 /*BadAddress*/, phi_op);
                    
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

void SsaConstructor::rename_variables(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree) {
    std::unordered_map<std::string, std::stack<std::size_t>> counters;
    std::unordered_map<std::string, std::size_t> counts;

    for (const auto& pair : var_defs_) {
        counters[pair.first].push(0);
        counts[pair.first] = 0;
    }

    auto update_uses = [&](Expression* expr, auto& update_uses_ref) -> void {
        if (!expr) return;
        if (Variable* v = dynamic_cast<Variable*>(expr)) {
            if (counters.contains(v->name())) {
                v->set_ssa_version(counters[v->name()].top());
            }
        } else if (Operation* op = dynamic_cast<Operation*>(expr)) {
            for (Expression* child : op->operands()) {
                update_uses_ref(child, update_uses_ref);
            }
        }
    };

    auto rename_block = [&](BasicBlock* block, auto& rename_block_ref) -> void {
        std::unordered_map<std::string, int> pushed_in_this_block;

        if (phi_nodes_.contains(block)) {
            for (Instruction* phi : phi_nodes_[block]) {
                if (!phi->operation()->operands().empty()) {
                    if (Variable* def_var = dynamic_cast<Variable*>(phi->operation()->operands()[0])) {
                        std::size_t count = counts[def_var->name()]++;
                        counters[def_var->name()].push(count);
                        def_var->set_ssa_version(count);
                        pushed_in_this_block[def_var->name()]++;
                    }
                }
            }
        }

        for (Instruction* inst : block->instructions()) {
            Operation* op = inst->operation();
            
            if (op->type() == OperationType::assign && op->operands().size() == 2) {
                update_uses(op->operands()[1], update_uses);
                
                if (Variable* def_var = dynamic_cast<Variable*>(op->operands()[0])) {
                    std::size_t count = counts[def_var->name()]++;
                    counters[def_var->name()].push(count);
                    def_var->set_ssa_version(count);
                    pushed_in_this_block[def_var->name()]++;
                }
            } else {
                update_uses(op, update_uses);
            }
        }

        for (Edge* edge : block->successors()) {
            BasicBlock* succ = edge->target();
            if (phi_nodes_.contains(succ)) {
                for (Instruction* phi : phi_nodes_[succ]) {
                    if (!phi->operation()->operands().empty()) {
                        if (Variable* target = dynamic_cast<Variable*>(phi->operation()->operands()[0])) {
                            std::size_t current_ver = counters[target->name()].top();
                            
                            std::vector<Expression*> new_ops = phi->operation()->operands();
                            Variable* source_var = arena.create<Variable>(target->name(), target->size_bytes);
                            source_var->set_ssa_version(current_ver);
                            new_ops.push_back(source_var);

                            Operation* new_phi_op = arena.create<Operation>(OperationType::phi, std::move(new_ops), phi->operation()->size_bytes);
                            Instruction* new_phi_inst = arena.create<Instruction>(phi->address(), new_phi_op);
                            // We logically swap the instruction pointer within the phi_nodes_ tracking block
                            phi = new_phi_inst;
                        }
                    }
                }
            }
        }

        for (BasicBlock* child : dom_tree.children(block)) {
            rename_block_ref(child, rename_block_ref);
        }

        for (const auto& [var_name, pushes] : pushed_in_this_block) {
            for (int i = 0; i < pushes; ++i) {
                counters[var_name].pop();
            }
        }
    };

    if (cfg.entry_block()) {
        rename_block(cfg.entry_block(), rename_block);
    }
}

} // namespace dewolf
