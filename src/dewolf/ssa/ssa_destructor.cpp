#include "ssa_destructor.hpp"
#include "phi_dependency_resolver.hpp"
#include "minimal_variable_renamer.hpp"
#include "conditional_variable_renamer.hpp"
#include "liveness/liveness.hpp"
#include "../pipeline/pipeline.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dewolf {

namespace {

struct SimpleKey {
    std::string name;
    std::size_t version{};

    bool operator==(const SimpleKey& other) const {
        return name == other.name && version == other.version;
    }
};

struct SimpleKeyHash {
    std::size_t operator()(const SimpleKey& key) const {
        std::size_t h1 = std::hash<std::string>{}(key.name);
        std::size_t h2 = std::hash<std::size_t>{}(key.version);
        return h1 ^ (h2 << 1);
    }
};

SimpleKey to_key(const Variable* var) {
    return SimpleKey{var->name(), var->ssa_version()};
}

void remove_identity_assignments(ControlFlowGraph& cfg) {
    for (BasicBlock* block : cfg.blocks()) {
        std::vector<Instruction*> rewritten;
        rewritten.reserve(block->instructions().size());
        for (Instruction* inst : block->instructions()) {
            auto* assign = dynamic_cast<Assignment*>(inst);
            if (assign != nullptr) {
                auto* dst = dynamic_cast<Variable*>(assign->destination());
                auto* src = dynamic_cast<Variable*>(assign->value());
                if (dst != nullptr && src != nullptr && dst == src) {
                    continue;
                }
            }
            rewritten.push_back(inst);
        }
        block->set_instructions(std::move(rewritten));
    }
}

void apply_simple_renamer(DecompilerArena& arena, ControlFlowGraph& cfg) {
    std::unordered_map<SimpleKey, Variable*, SimpleKeyHash> replacement_for;

    auto replacement_for_var = [&](Variable* original) -> Variable* {
        SimpleKey key = to_key(original);
        auto it = replacement_for.find(key);
        if (it != replacement_for.end()) {
            return it->second;
        }

        std::string new_name = original->name();
        if (original->ssa_version() > 0) {
            new_name += "_" + std::to_string(original->ssa_version());
        }

        auto* replacement = arena.create<Variable>(new_name, original->size_bytes);
        replacement->set_ssa_version(0);
        replacement->set_aliased(original->is_aliased());
        replacement->set_ir_type(original->ir_type());

        replacement_for.emplace(std::move(key), replacement);
        return replacement;
    };

    for (BasicBlock* block : cfg.blocks()) {
        for (Instruction* inst : block->instructions()) {
            std::vector<Variable*> vars = inst->requirements();
            auto defs = inst->definitions();
            vars.insert(vars.end(), defs.begin(), defs.end());

            std::unordered_set<SimpleKey, SimpleKeyHash> seen;
            for (Variable* old_var : vars) {
                if (old_var == nullptr) {
                    continue;
                }
                SimpleKey key = to_key(old_var);
                if (!seen.insert(key).second) {
                    continue;
                }

                Variable* replacement = replacement_for_var(old_var);
                if (replacement != old_var) {
                    inst->substitute(old_var, replacement);
                }
            }
        }
    }

    remove_identity_assignments(cfg);
}

std::string normalize_mode_token(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) || c == '-' || c == '_') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(uc)));
    }
    return out;
}

} // namespace

void SsaDestructor::execute(DecompilerTask& task) {
    if (!task.cfg()) return;

    ControlFlowGraph& cfg = *task.cfg();
    const OutOfSsaMode mode = task.out_of_ssa_mode();

    // Pre-color first for minimization strategy (chordal SSA graph).
    if (mode == OutOfSsaMode::Minimization) {
        MinimalVariableRenamer::rename(task.arena(), cfg);
    }

    PhiDependencyResolver::resolve(task.arena(), cfg);
    LivenessAnalysis liveness(cfg);
    eliminate_phi_nodes(task.arena(), cfg, liveness);

    switch (mode) {
        case OutOfSsaMode::Simple:
            apply_simple_renamer(task.arena(), cfg);
            break;
        case OutOfSsaMode::Conditional:
            ConditionalVariableRenamer::rename(task.arena(), cfg);
            break;
        case OutOfSsaMode::Sreedhar:
            break;  // keep raw out-of-SSA lifting result
        case OutOfSsaMode::Minimization:
            break;  // pre-color already applied above
        case OutOfSsaMode::LiftMinimal:
        default:
            MinimalVariableRenamer::rename(task.arena(), cfg);
            break;
    }
}

std::optional<OutOfSsaMode> SsaDestructor::parse_mode(std::string_view text) {
    const std::string tok = normalize_mode_token(text);
    if (tok.empty()) {
        return std::nullopt;
    }

    if (tok == "simple") {
        return OutOfSsaMode::Simple;
    }
    if (tok == "min" || tok == "minimization") {
        return OutOfSsaMode::Minimization;
    }
    if (tok == "liftminimal") {
        return OutOfSsaMode::LiftMinimal;
    }
    if (tok == "conditional") {
        return OutOfSsaMode::Conditional;
    }
    if (tok == "sreedhar") {
        return OutOfSsaMode::Sreedhar;
    }

    return std::nullopt;
}

void SsaDestructor::eliminate_phi_nodes(DecompilerArena& arena, ControlFlowGraph& cfg, const LivenessAnalysis& liveness) {
    std::size_t next_block_id = 0;
    for (BasicBlock* block : cfg.blocks()) {
        next_block_id = std::max(next_block_id, block->id() + 1);
    }

    const std::vector<BasicBlock*> original_blocks = cfg.blocks();
    for (BasicBlock* bb : original_blocks) {
        std::vector<Instruction*> new_insts;
        std::unordered_map<BasicBlock*, BasicBlock*> split_block_for_pred;

        auto find_edge = [](BasicBlock* src, BasicBlock* dst) -> Edge* {
            if (!src || !dst) return nullptr;
            for (Edge* edge : src->successors()) {
                if (edge && edge->target() == dst) {
                    return edge;
                }
            }
            return nullptr;
        };

        auto insert_copy_into_block = [](BasicBlock* block, Assignment* copy_assign) {
            auto insts = block->instructions();
            if (!insts.empty()) {
                Instruction* last_inst = insts.back();
                if (is_branch(last_inst) || is_return(last_inst)) {
                    insts.insert(insts.end() - 1, copy_assign);
                } else {
                    insts.push_back(copy_assign);
                }
            } else {
                insts.push_back(copy_assign);
            }
            block->set_instructions(std::move(insts));
        };

        auto ensure_edge_split_block = [&](BasicBlock* pred_block) -> BasicBlock* {
            if (!pred_block) return nullptr;
            if (auto it = split_block_for_pred.find(pred_block); it != split_block_for_pred.end()) {
                return it->second;
            }

            Edge* pred_to_bb = find_edge(pred_block, bb);
            if (!pred_to_bb) return nullptr;

            const bool conditional_edge = pred_to_bb->type() != EdgeType::Unconditional ||
                                          pred_block->successors().size() > 1;
            if (!conditional_edge) {
                return nullptr;
            }

            auto* split_block = arena.create<BasicBlock>(next_block_id++);
            cfg.add_block(split_block);

            cfg.remove_edge(pred_to_bb);

            Edge* pred_to_split = nullptr;
            if (auto* switch_edge = dynamic_cast<SwitchEdge*>(pred_to_bb)) {
                pred_to_split = arena.create<SwitchEdge>(pred_block, split_block, switch_edge->case_value());
            } else {
                pred_to_split = arena.create<Edge>(pred_block, split_block, pred_to_bb->type());
            }
            pred_block->add_successor(pred_to_split);
            split_block->add_predecessor(pred_to_split);

            auto* split_to_phi = arena.create<Edge>(split_block, bb, EdgeType::Unconditional);
            split_block->add_successor(split_to_phi);
            bb->add_predecessor(split_to_phi);

            split_block_for_pred[pred_block] = split_block;
            return split_block;
        };

        auto place_phi_copy = [&](BasicBlock* pred_block, Expression* final_target, Expression* source_expr) {
            if (!pred_block || !source_expr || !final_target) return;

            BasicBlock* insertion_block = ensure_edge_split_block(pred_block);
            if (!insertion_block) {
                insertion_block = pred_block;
            }

            auto* copy_assign = arena.create<Assignment>(final_target, source_expr);
            insert_copy_into_block(insertion_block, copy_assign);
        };

        for (Instruction* inst : bb->instructions()) {
            auto* phi = dynamic_cast<Phi*>(inst);
            if (!phi) {
                new_insts.push_back(inst);
                continue;
            }

            Variable* target = phi->dest_var();
            if (!target) continue;

            auto* op_list = phi->operand_list();
            if (!op_list || op_list->empty()) continue;

            // Use origin_block if available, otherwise fall back to positional matching.
            if (!phi->origin_block().empty()) {
                for (auto& [pred_block, source_expr] : phi->origin_block()) {
                    if (!pred_block || !source_expr) continue;

                    bool interference = liveness.live_out(pred_block).contains(target->name());

                    Expression* final_target = target;
                    if (interference) {
                        auto* tmp = arena.create<Variable>(target->name() + "_tmp", target->size_bytes);
                        final_target = tmp;
                        auto* bb_copy = arena.create<Assignment>(target, tmp);
                        new_insts.push_back(bb_copy);
                    }

                    place_phi_copy(pred_block, final_target, source_expr);
                }
            } else {
                // Fallback: positional matching (predecessor index -> operand index)
                for (size_t i = 0; i < op_list->operands().size() && i < bb->predecessors().size(); ++i) {
                    Expression* source = op_list->operands()[i];
                    BasicBlock* pred = bb->predecessors()[i]->source();
                    if (!pred || !source) continue;

                    bool interference = liveness.live_out(pred).contains(target->name());

                    Expression* final_target = target;
                    if (interference) {
                        auto* tmp = arena.create<Variable>(target->name() + "_tmp", target->size_bytes);
                        final_target = tmp;
                        auto* bb_copy = arena.create<Assignment>(target, tmp);
                        new_insts.push_back(bb_copy);
                    }

                    place_phi_copy(pred, final_target, source);
                }
            }
        }

        bb->set_instructions(std::move(new_insts));
    }
}

} // namespace dewolf
