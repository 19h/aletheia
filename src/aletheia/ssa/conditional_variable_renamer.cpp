#include "conditional_variable_renamer.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aletheia {

namespace {

constexpr double kOperationPenalty = 0.9;

struct VarKey {
    std::string name;
    std::size_t version{};

    bool operator==(const VarKey& other) const {
        return name == other.name && version == other.version;
    }
};

struct VarKeyHash {
    std::size_t operator()(const VarKey& key) const {
        std::size_t h1 = std::hash<std::string>{}(key.name);
        std::size_t h2 = std::hash<std::size_t>{}(key.version);
        return h1 ^ (h2 << 1);
    }
};

struct PairKey {
    VarKey a;
    VarKey b;

    bool operator==(const PairKey& other) const {
        return a == other.a && b == other.b;
    }
};

struct PairKeyHash {
    std::size_t operator()(const PairKey& key) const {
        std::size_t h1 = VarKeyHash{}(key.a);
        std::size_t h2 = VarKeyHash{}(key.b);
        return h1 ^ (h2 << 1);
    }
};

struct GroupPair {
    int lhs = -1;
    int rhs = -1;

    bool operator==(const GroupPair& other) const {
        return lhs == other.lhs && rhs == other.rhs;
    }
};

struct GroupPairHash {
    std::size_t operator()(const GroupPair& p) const {
        std::size_t h1 = std::hash<int>{}(p.lhs);
        std::size_t h2 = std::hash<int>{}(p.rhs);
        return h1 ^ (h2 << 1);
    }
};

struct VarInfo {
    Variable* sample = nullptr;
    std::size_t size_bytes = 0;
    bool aliased = false;
    TypePtr type = nullptr;
    VariableKind kind = VariableKind::Register;
    int parameter_index = -1;
    std::int64_t stack_offset = 0;
};

using VarSet = std::unordered_set<VarKey, VarKeyHash>;
using Adjacency = std::unordered_map<VarKey, VarSet, VarKeyHash>;
using DefUseSetMap = std::unordered_map<BasicBlock*, VarSet>;
using DependencyWeightMap = std::unordered_map<PairKey, double, PairKeyHash>;

VarKey key_of(const Variable* var) {
    return VarKey{var->name(), var->ssa_version()};
}

void add_vertex_if_missing(Adjacency& graph, const VarKey& key) {
    graph.try_emplace(key, VarSet{});
}

void add_edge(Adjacency& graph, const VarKey& a, const VarKey& b) {
    if (a == b) {
        return;
    }
    add_vertex_if_missing(graph, a);
    add_vertex_if_missing(graph, b);
    graph[a].insert(b);
    graph[b].insert(a);
}

void add_clique(Adjacency& graph, const VarSet& vars) {
    std::vector<VarKey> items(vars.begin(), vars.end());
    for (std::size_t i = 0; i < items.size(); ++i) {
        add_vertex_if_missing(graph, items[i]);
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            add_edge(graph, items[i], items[j]);
        }
    }
}

std::vector<VarKey> keys_from_variables(const std::vector<Variable*>& vars) {
    std::vector<VarKey> out;
    out.reserve(vars.size());
    VarSet seen;
    for (Variable* var : vars) {
        if (var == nullptr || isa<GlobalVariable>(var)) {
            continue;
        }
        VarKey k = key_of(var);
        if (seen.insert(k).second) {
            out.push_back(std::move(k));
        }
    }
    return out;
}

void collect_var_info(ControlFlowGraph& cfg,
                      std::unordered_map<VarKey, VarInfo, VarKeyHash>& var_info,
                      Adjacency& interference,
                      DefUseSetMap& uses_block,
                      DefUseSetMap& defs_block) {
    auto record_var = [&](Variable* var) {
        if (var == nullptr || isa<GlobalVariable>(var)) {
            return;
        }
        VarKey key = key_of(var);
        auto& info = var_info[key];
        if (info.sample == nullptr) {
            info.sample = var;
            info.size_bytes = var->size_bytes;
            info.aliased = var->is_aliased();
            info.type = var->ir_type();
            info.kind = var->kind();
            info.parameter_index = var->parameter_index();
            info.stack_offset = var->stack_offset();
        } else {
            info.aliased = info.aliased || var->is_aliased();
            info.size_bytes = std::max(info.size_bytes, var->size_bytes);
            if (!info.type && var->ir_type()) {
                info.type = var->ir_type();
            }
            // Upgrade kind: Parameter > StackLocal/StackArgument > Register
            if (var->is_parameter() && !info.sample->is_parameter()) {
                info.kind = var->kind();
                info.parameter_index = var->parameter_index();
            }
            if ((var->kind() == VariableKind::StackLocal || var->kind() == VariableKind::StackArgument)
                && info.kind == VariableKind::Register) {
                info.kind = var->kind();
                info.stack_offset = var->stack_offset();
            }
        }
        add_vertex_if_missing(interference, key);
    };

    for (BasicBlock* block : cfg.blocks()) {
        VarSet uses;
        VarSet defs;

        for (Instruction* inst : block->instructions()) {
            for (Variable* v : inst->requirements()) {
                record_var(v);
            }
            for (Variable* v : inst->definitions()) {
                record_var(v);
            }

            auto req_keys = keys_from_variables(inst->requirements());
            auto def_keys = keys_from_variables(inst->definitions());

            for (const VarKey& rk : req_keys) {
                if (!defs.contains(rk)) {
                    uses.insert(rk);
                }
            }
            for (const VarKey& dk : def_keys) {
                defs.insert(dk);
            }
        }

        uses_block[block] = std::move(uses);
        defs_block[block] = std::move(defs);
    }
}

void compute_liveness(ControlFlowGraph& cfg,
                      const DefUseSetMap& uses_block,
                      const DefUseSetMap& defs_block,
                      DefUseSetMap& live_in,
                      DefUseSetMap& live_out) {
    bool changed = true;
    auto order = cfg.reverse_post_order();
    while (changed) {
        changed = false;

        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            BasicBlock* block = *it;

            VarSet out;
            for (Edge* edge : block->successors()) {
                if (edge != nullptr && edge->target() != nullptr) {
                    const VarSet& succ_in = live_in[edge->target()];
                    out.insert(succ_in.begin(), succ_in.end());
                }
            }

            VarSet in = uses_block.at(block);
            VarSet out_minus_def = out;
            const VarSet& defs = defs_block.at(block);
            for (const VarKey& d : defs) {
                out_minus_def.erase(d);
            }
            in.insert(out_minus_def.begin(), out_minus_def.end());

            if (in != live_in[block] || out != live_out[block]) {
                live_in[block] = std::move(in);
                live_out[block] = std::move(out);
                changed = true;
            }
        }
    }
}

void build_interference(ControlFlowGraph& cfg,
                        const DefUseSetMap& live_in,
                        const DefUseSetMap& live_out,
                        Adjacency& interference) {
    for (BasicBlock* block : cfg.blocks()) {
        add_clique(interference, live_in.at(block));
        add_clique(interference, live_out.at(block));

        VarSet current = live_out.at(block);
        auto insts = block->instructions();
        for (auto it = insts.rbegin(); it != insts.rend(); ++it) {
            Instruction* inst = *it;
            bool has_call_value = false;
            if (auto* assign = dyn_cast<Assignment>(inst)) {
                if (dyn_cast<Call>(assign->value()) != nullptr) {
                    has_call_value = true;
                } else if (auto* op = dyn_cast<Operation>(assign->value())) {
                    has_call_value = op->type() == OperationType::call;
                }
            }

            VarSet defs;
            for (const VarKey& k : keys_from_variables(inst->definitions())) {
                defs.insert(k);
            }

            VarSet reqs;
            for (const VarKey& k : keys_from_variables(inst->requirements())) {
                reqs.insert(k);
            }

            for (const VarKey& d : defs) {
                for (const VarKey& live : current) {
                    add_edge(interference, d, live);
                }
                if (has_call_value) {
                    for (const VarKey& r : reqs) {
                        add_edge(interference, d, r);
                    }
                }
            }
            for (const VarKey& d : defs) {
                current.erase(d);
            }
            current.insert(reqs.begin(), reqs.end());
        }
    }
}

using ExprDepMap = std::unordered_map<VarKey, double, VarKeyHash>;

ExprDepMap expression_dependencies(Expression* expr) {
    if (expr == nullptr) {
        return {};
    }

    if (auto* var = dyn_cast<Variable>(expr)) {
        return {{key_of(var), 1.0}};
    }

    auto* op = dyn_cast<Operation>(expr);
    if (op == nullptr) {
        return {};
    }

    switch (op->type()) {
        case OperationType::call:
        case OperationType::address_of:
        case OperationType::deref:
        case OperationType::member_access:
            return {};
        default:
            break;
    }

    std::vector<ExprDepMap> operand_maps;
    for (Expression* child : op->operands()) {
        ExprDepMap deps = expression_dependencies(child);
        if (!deps.empty()) {
            operand_maps.push_back(std::move(deps));
        }
    }
    if (operand_maps.empty()) {
        return {};
    }

    ExprDepMap out;
    const double divisor = static_cast<double>(operand_maps.size());
    for (const ExprDepMap& deps : operand_maps) {
        for (const auto& [key, score] : deps) {
            out[key] += (score / divisor) * kOperationPenalty;
        }
    }
    return out;
}

void build_dependency_weights(ControlFlowGraph& cfg, DependencyWeightMap& weights) {
    for (BasicBlock* block : cfg.blocks()) {
        for (Instruction* inst : block->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (assign == nullptr || assign->value() == nullptr) {
                continue;
            }

            auto defs = keys_from_variables(assign->definitions());
            ExprDepMap deps = expression_dependencies(assign->value());

            for (const VarKey& def_key : defs) {
                for (const auto& [used_key, score] : deps) {
                    if (score <= 0.0) {
                        continue;
                    }
                    weights[PairKey{def_key, used_key}] += score;
                }
            }
        }
    }
}

bool same_type_compat(const VarInfo& a, const VarInfo& b) {
    if (a.type && b.type) {
        return a.type->to_string() == b.type->to_string();
    }
    return a.size_bytes == b.size_bytes;
}

bool are_interfering(const Adjacency& graph, const VarKey& a, const VarKey& b) {
    auto it = graph.find(a);
    if (it == graph.end()) {
        return false;
    }
    return it->second.contains(b);
}

void remove_identity_assignments(ControlFlowGraph& cfg) {
    for (BasicBlock* block : cfg.blocks()) {
        std::vector<Instruction*> rewritten;
        rewritten.reserve(block->instructions().size());
        for (Instruction* inst : block->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (assign != nullptr) {
                auto* dst = dyn_cast<Variable>(assign->destination());
                auto* src = dyn_cast<Variable>(assign->value());
                if (dst != nullptr && src != nullptr
                    && (dst == src || (dst->name() == src->name()
                                      && dst->ssa_version() == src->ssa_version()))) {
                    continue;
                }
            }
            rewritten.push_back(inst);
        }
        block->set_instructions(std::move(rewritten));
    }
}

} // namespace

void ConditionalVariableRenamer::rename(DecompilerArena& arena, ControlFlowGraph& cfg) {
    std::unordered_map<VarKey, VarInfo, VarKeyHash> var_info;
    DefUseSetMap uses_block;
    DefUseSetMap defs_block;
    DefUseSetMap live_in;
    DefUseSetMap live_out;
    Adjacency interference;

    collect_var_info(cfg, var_info, interference, uses_block, defs_block);
    compute_liveness(cfg, uses_block, defs_block, live_in, live_out);
    build_interference(cfg, live_in, live_out, interference);

    DependencyWeightMap dep_weights;
    build_dependency_weights(cfg, dep_weights);

    struct Group {
        int id = -1;
        bool active = true;
        std::vector<VarKey> members;
        VarInfo repr;
    };

    std::vector<Group> groups;
    std::unordered_map<VarKey, int, VarKeyHash> group_of;

    groups.reserve(var_info.size());
    int gid = 0;
    for (const auto& [key, info] : var_info) {
        Group g;
        g.id = gid;
        g.members.push_back(key);
        g.repr = info;
        groups.push_back(g);
        group_of[key] = gid;
        ++gid;
    }

    auto can_merge = [&](const Group& lhs, const Group& rhs) -> bool {
        if (!same_type_compat(lhs.repr, rhs.repr)) {
            return false;
        }
        if (lhs.repr.aliased != rhs.repr.aliased) {
            return false;
        }
        if (lhs.repr.aliased && !lhs.members.empty() && !rhs.members.empty()) {
            if (lhs.members.front().name != rhs.members.front().name) {
                return false;
            }
        }
        for (const VarKey& a : lhs.members) {
            for (const VarKey& b : rhs.members) {
                if (are_interfering(interference, a, b)) {
                    return false;
                }
            }
        }
        return true;
    };

    while (true) {
        std::unordered_map<GroupPair, double, GroupPairHash> merged_scores;
        for (const auto& [edge_key, score] : dep_weights) {
            auto u_it = group_of.find(edge_key.a);
            auto v_it = group_of.find(edge_key.b);
            if (u_it == group_of.end() || v_it == group_of.end()) {
                continue;
            }
            int u = u_it->second;
            int v = v_it->second;
            if (u == v) {
                continue;
            }

            GroupPair pair{std::min(u, v), std::max(u, v)};
            merged_scores[pair] += score;
        }

        if (merged_scores.empty()) {
            break;
        }

        std::vector<std::pair<GroupPair, double>> sorted_pairs(merged_scores.begin(), merged_scores.end());
        std::sort(sorted_pairs.begin(), sorted_pairs.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            if (lhs.first.lhs != rhs.first.lhs) {
                return lhs.first.lhs < rhs.first.lhs;
            }
            return lhs.first.rhs < rhs.first.rhs;
        });

        bool merged = false;
        for (const auto& [pair, score] : sorted_pairs) {
            (void)score;
            if (pair.lhs < 0 || pair.rhs < 0 || pair.lhs >= static_cast<int>(groups.size()) || pair.rhs >= static_cast<int>(groups.size())) {
                continue;
            }

            Group& lhs = groups[static_cast<std::size_t>(pair.lhs)];
            Group& rhs = groups[static_cast<std::size_t>(pair.rhs)];
            if (!lhs.active || !rhs.active) {
                continue;
            }
            if (!can_merge(lhs, rhs)) {
                continue;
            }

            lhs.members.insert(lhs.members.end(), rhs.members.begin(), rhs.members.end());
            lhs.repr.aliased = lhs.repr.aliased || rhs.repr.aliased;
            lhs.repr.size_bytes = std::max(lhs.repr.size_bytes, rhs.repr.size_bytes);
            if (!lhs.repr.type && rhs.repr.type) {
                lhs.repr.type = rhs.repr.type;
            }
            // Merge provenance: Parameter > StackLocal/StackArgument > Register
            if (rhs.repr.kind == VariableKind::Parameter && rhs.repr.parameter_index >= 0
                && lhs.repr.kind != VariableKind::Parameter) {
                lhs.repr.kind = rhs.repr.kind;
                lhs.repr.parameter_index = rhs.repr.parameter_index;
            }
            if ((rhs.repr.kind == VariableKind::StackLocal || rhs.repr.kind == VariableKind::StackArgument)
                && lhs.repr.kind == VariableKind::Register) {
                lhs.repr.kind = rhs.repr.kind;
                lhs.repr.stack_offset = rhs.repr.stack_offset;
            }

            for (const VarKey& k : rhs.members) {
                group_of[k] = lhs.id;
            }
            rhs.active = false;
            rhs.members.clear();

            merged = true;
            break;
        }

        if (!merged) {
            break;
        }
    }

    std::unordered_map<VarKey, Variable*, VarKeyHash> replacement_for;
    int counter = 0;
    for (Group& g : groups) {
        if (!g.active || g.members.empty()) {
            continue;
        }

        // Scan all members for the best provenance metadata.
        VariableKind best_kind = g.repr.kind;
        int best_param_index = g.repr.parameter_index;
        std::int64_t best_stack_offset = g.repr.stack_offset;
        std::size_t best_size = g.repr.size_bytes;
        TypePtr best_type = g.repr.type;

        for (const VarKey& key : g.members) {
            auto it = var_info.find(key);
            if (it == var_info.end()) {
                continue;
            }
            const VarInfo& mi = it->second;
            best_size = std::max(best_size, mi.size_bytes);
            if (!best_type && mi.type) {
                best_type = mi.type;
            }
            if (mi.kind == VariableKind::Parameter && mi.parameter_index >= 0) {
                best_kind = VariableKind::Parameter;
                best_param_index = mi.parameter_index;
            }
            if ((mi.kind == VariableKind::StackLocal || mi.kind == VariableKind::StackArgument)
                && best_kind != VariableKind::Parameter) {
                best_kind = mi.kind;
                best_stack_offset = mi.stack_offset;
            }
        }

        const std::size_t size = best_size > 0 ? best_size : 4;
        auto* replacement = arena.create<Variable>("var_" + std::to_string(counter++), size);
        replacement->set_ssa_version(0);
        replacement->set_aliased(g.repr.aliased);
        replacement->set_ir_type(best_type);
        replacement->set_kind(best_kind);
        replacement->set_parameter_index(best_param_index);
        replacement->set_stack_offset(best_stack_offset);

        for (const VarKey& key : g.members) {
            replacement_for[key] = replacement;
        }
    }

    for (BasicBlock* block : cfg.blocks()) {
        for (Instruction* inst : block->instructions()) {
            std::vector<Variable*> vars = inst->requirements();
            auto defs = inst->definitions();
            vars.insert(vars.end(), defs.begin(), defs.end());

            std::unordered_set<Variable*> seen;
            for (Variable* old_var : vars) {
                if (old_var == nullptr) {
                    continue;
                }
                // GlobalVariables represent resolved symbol names and must
                // not be replaced with plain Variable objects.
                if (isa<GlobalVariable>(old_var)) {
                    continue;
                }
                if (!seen.insert(old_var).second) {
                    continue;
                }
                VarKey key = key_of(old_var);
                auto it = replacement_for.find(key);
                if (it != replacement_for.end() && it->second != old_var) {
                    inst->substitute(old_var, it->second);
                }
            }
        }
    }

    remove_identity_assignments(cfg);
}

} // namespace aletheia
