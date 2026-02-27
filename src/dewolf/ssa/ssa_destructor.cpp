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
    for (BasicBlock* bb : cfg.blocks()) {
        std::vector<Instruction*> new_insts;
        
        for (Instruction* inst : bb->instructions()) {
            auto* phi = dynamic_cast<Phi*>(inst);
            if (!phi) {
                new_insts.push_back(inst);
                continue;
            }

            // Process phi node: insert copy operations in predecessor blocks
            Variable* target = phi->dest_var();
            if (!target) continue;

            auto* op_list = phi->operand_list();
            if (!op_list || op_list->empty()) continue;

            // Use origin_block if available, otherwise fall back to positional matching
            if (!phi->origin_block().empty()) {
                for (auto& [pred_block, source_expr] : phi->origin_block()) {
                    if (!source_expr) continue;

                    // Check interference: is the target live-out at the predecessor?
                    bool interference = liveness.live_out(pred_block).contains(target->name());

                    Expression* final_target = target;
                    if (interference) {
                        // Create a temporary to break the interference
                        auto* tmp = arena.create<Variable>(target->name() + "_tmp", target->size_bytes);
                        final_target = tmp;
                        
                        // Insert copy from tmp to real target at the start of the phi's block
                        auto* bb_copy = arena.create<Assignment>(target, tmp);
                        new_insts.push_back(bb_copy);
                    }

                    // Insert copy at the end of predecessor block (before branch if present)
                    auto* copy_assign = arena.create<Assignment>(final_target, source_expr);
                    auto pred_insts = pred_block->instructions();
                    
                    if (!pred_insts.empty()) {
                        Instruction* last_inst = pred_insts.back();
                        // Insert before branch/return at end of predecessor
                        if (is_branch(last_inst) || is_return(last_inst)) {
                            pred_insts.insert(pred_insts.end() - 1, copy_assign);
                        } else {
                            pred_insts.push_back(copy_assign);
                        }
                    } else {
                        pred_insts.push_back(copy_assign);
                    }
                    
                    pred_block->set_instructions(std::move(pred_insts));
                }
            } else {
                // Fallback: positional matching (predecessor index -> operand index)
                for (size_t i = 0; i < op_list->operands().size() && i < bb->predecessors().size(); ++i) {
                    Expression* source = op_list->operands()[i];
                    BasicBlock* pred = bb->predecessors()[i]->source();
                    
                    bool interference = liveness.live_out(pred).contains(target->name());

                    Expression* final_target = target;
                    if (interference) {
                        auto* tmp = arena.create<Variable>(target->name() + "_tmp", target->size_bytes);
                        final_target = tmp;
                        auto* bb_copy = arena.create<Assignment>(target, tmp);
                        new_insts.push_back(bb_copy);
                    }

                    auto* copy_assign = arena.create<Assignment>(final_target, source);
                    auto pred_insts = pred->instructions();
                    
                    if (!pred_insts.empty()) {
                        Instruction* last_inst = pred_insts.back();
                        if (is_branch(last_inst) || is_return(last_inst)) {
                            pred_insts.insert(pred_insts.end() - 1, copy_assign);
                        } else {
                            pred_insts.push_back(copy_assign);
                        }
                    } else {
                        pred_insts.push_back(copy_assign);
                    }
                    
                    pred->set_instructions(std::move(pred_insts));
                }
            }
        }
        
        bb->set_instructions(std::move(new_insts));
    }
}

} // namespace dewolf
