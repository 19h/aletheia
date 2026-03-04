#include "ssa_constructor.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <stack>

namespace aletheia {

namespace {

std::uint64_t block_order_key(BasicBlock* block) {
    if (!block) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(block->id());
}

std::vector<BasicBlock*> sorted_blocks_by_id(const std::vector<BasicBlock*>& blocks) {
    std::vector<BasicBlock*> sorted = blocks;
    std::stable_sort(sorted.begin(), sorted.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
        return block_order_key(lhs) < block_order_key(rhs);
    });
    return sorted;
}

std::vector<Edge*> sorted_successor_edges(const std::vector<Edge*>& edges) {
    std::vector<Edge*> sorted = edges;
    std::stable_sort(sorted.begin(), sorted.end(), [](Edge* lhs, Edge* rhs) {
        BasicBlock* lhs_target = lhs ? lhs->target() : nullptr;
        BasicBlock* rhs_target = rhs ? rhs->target() : nullptr;

        const std::uint64_t lhs_key = block_order_key(lhs_target);
        const std::uint64_t rhs_key = block_order_key(rhs_target);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }

        const int lhs_type = lhs ? static_cast<int>(lhs->type()) : std::numeric_limits<int>::max();
        const int rhs_type = rhs ? static_cast<int>(rhs->type()) : std::numeric_limits<int>::max();
        if (lhs_type != rhs_type) {
            return lhs_type < rhs_type;
        }

        const int lhs_kind = lhs ? static_cast<int>(lhs->edge_kind()) : std::numeric_limits<int>::max();
        const int rhs_kind = rhs ? static_cast<int>(rhs->edge_kind()) : std::numeric_limits<int>::max();
        if (lhs_kind != rhs_kind) {
            return lhs_kind < rhs_kind;
        }

        return false;
    });
    return sorted;
}

std::vector<std::string> sorted_var_names(const std::unordered_map<std::string, std::vector<BasicBlock*>>& defs) {
    std::vector<std::string> names;
    names.reserve(defs.size());
    for (const auto& [name, _] : defs) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

void SsaConstructor::execute(DecompilerTask& task) {
    if (!task.cfg()) return;
    var_defs_.clear();
    phi_nodes_.clear();
    DominatorTree dom_tree(*task.cfg());
    gather_definitions(*task.cfg());
    insert_phi_nodes(task.arena(), *task.cfg(), dom_tree);
    rename_variables(task.arena(), *task.cfg(), dom_tree);
}

void SsaConstructor::gather_definitions(ControlFlowGraph& cfg) {
    for (BasicBlock* block : sorted_blocks_by_id(cfg.blocks())) {
        for (Instruction* inst : block->instructions()) {
            // Use the new Assignment type to identify definitions
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (auto* var = dyn_cast<Variable>(assign->destination())) {
                    var_defs_[var->name()].push_back(block);
                }
            }
        }
    }
}

void SsaConstructor::insert_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const DominatorTree& dom_tree) {
    for (const std::string& var_name : sorted_var_names(var_defs_)) {
        std::vector<BasicBlock*> def_blocks = var_defs_[var_name];
        std::stable_sort(def_blocks.begin(), def_blocks.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
            return block_order_key(lhs) < block_order_key(rhs);
        });
        def_blocks.erase(std::unique(def_blocks.begin(), def_blocks.end()), def_blocks.end());

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
                    // Create a Phi node: dest = phi()
                    // Initially the phi has no source operands -- they are
                    // added during the renaming phase when successors are processed.
                    Variable* target = arena.create<Variable>(var_name, 8);
                    auto* phi_operands = arena.create<ListOperation>(std::vector<Expression*>{});
                    Phi* phi_node = arena.create<Phi>(target, phi_operands);
                    
                    phi_nodes_[y].push_back(phi_node);
                    
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

    for (const std::string& var_name : sorted_var_names(var_defs_)) {
        counters[var_name].push(0);
        counts[var_name] = 0;
    }

    // Recursively update SSA versions on Variable uses within an Expression tree.
    auto update_uses = [&](Expression* expr, auto& update_uses_ref) -> void {
        if (!expr) return;
        if (auto* v = dyn_cast<Variable>(expr)) {
            // GlobalVariables are resolved symbol names — never SSA-versioned.
            if (!isa<GlobalVariable>(v) && counters.contains(v->name())) {
                v->set_ssa_version(counters[v->name()].top());
            }
        } else if (auto* op = dyn_cast<Operation>(expr)) {
            for (Expression* child : op->operands()) {
                update_uses_ref(child, update_uses_ref);
            }
        } else if (auto* list = dyn_cast<ListOperation>(expr)) {
            for (Expression* child : list->operands()) {
                update_uses_ref(child, update_uses_ref);
            }
        }
    };

    auto rename_block = [&](BasicBlock* block, auto& rename_block_ref) -> void {
        std::unordered_map<std::string, int> pushed_in_this_block;

        // Process phi nodes placed at this block
        if (phi_nodes_.contains(block)) {
            for (Phi* phi : phi_nodes_[block]) {
                Variable* def_var = phi->dest_var();
                if (def_var) {
                    std::size_t count = counts[def_var->name()]++;
                    counters[def_var->name()].push(count);
                    def_var->set_ssa_version(count);
                    pushed_in_this_block[def_var->name()]++;
                }
            }
        }

        // Process regular instructions
        for (Instruction* inst : block->instructions()) {
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                // For assignments: first rename uses (RHS), then define (LHS)
                update_uses(assign->value(), update_uses);
                
                // If destination is a complex expression (e.g., deref), update
                // uses in the destination too
                if (!isa<Variable>(assign->destination())) {
                    update_uses(assign->destination(), update_uses);
                }
                
                // Rename the definition
                if (auto* def_var = dyn_cast<Variable>(assign->destination())) {
                    std::size_t count = counts[def_var->name()]++;
                    counters[def_var->name()].push(count);
                    def_var->set_ssa_version(count);
                    pushed_in_this_block[def_var->name()]++;
                }
            } else if (auto* branch = dyn_cast<Branch>(inst)) {
                // Branches only have uses (in the condition)
                update_uses(branch->condition(), update_uses);
            } else if (auto* ret = dyn_cast<Return>(inst)) {
                // Return values are uses
                for (auto* val : ret->values()) {
                    update_uses(val, update_uses);
                }
            }
            // Break, Continue, Comment have no variable references
        }

        // Add phi operands in successor blocks
        for (Edge* edge : sorted_successor_edges(block->successors())) {
            if (!edge) {
                continue;
            }
            BasicBlock* succ = edge->target();
            if (phi_nodes_.contains(succ)) {
                for (std::size_t i = 0; i < phi_nodes_[succ].size(); ++i) {
                    Phi* phi = phi_nodes_[succ][i];
                    Variable* target = phi->dest_var();
                    if (target && counters.contains(target->name())) {
                        std::size_t current_ver = counters[target->name()].top();
                        
                        // Create a new source variable with the current SSA version
                        Variable* source_var = arena.create<Variable>(target->name(), target->size_bytes);
                        source_var->set_ssa_version(current_ver);
                        
                        // Add to the phi's operand list
                        phi->operand_list()->mutable_operands().push_back(source_var);
                        
                        // Track which predecessor block supplies this operand
                        phi->mutable_origin_block()[block] = source_var;
                    }
                }
            }
        }

        // Recurse into dominator tree children
        for (BasicBlock* child : dom_tree.children(block)) {
            rename_block_ref(child, rename_block_ref);
        }

        // Pop the SSA version counters pushed in this block
        std::vector<std::pair<std::string, int>> pushed;
        pushed.reserve(pushed_in_this_block.size());
        for (const auto& [var_name, pushes] : pushed_in_this_block) {
            pushed.emplace_back(var_name, pushes);
        }
        std::sort(pushed.begin(), pushed.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });

        for (const auto& [var_name, pushes] : pushed) {
            for (int i = 0; i < pushes; ++i) {
                counters[var_name].pop();
            }
        }
    };

    if (cfg.entry_block()) {
        rename_block(cfg.entry_block(), rename_block);
    }

    // Insert phi nodes into the actual block instruction lists (at the front)
    for (BasicBlock* block : sorted_blocks_by_id(cfg.blocks())) {
        if (!phi_nodes_.contains(block)) {
            continue;
        }
        auto& phis = phi_nodes_[block];
        auto& insts = block->mutable_instructions();
        // Phi* is-a Instruction* but vector iterators differ, so copy explicitly
        std::vector<Instruction*> phi_insts(phis.begin(), phis.end());
        insts.insert(insts.begin(), phi_insts.begin(), phi_insts.end());
    }
}

} // namespace aletheia
