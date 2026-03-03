#include "minimal_variable_renamer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aletheia {

namespace {

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

struct VarInfo {
    Variable* sample = nullptr;
    std::size_t size_bytes = 0;
    bool aliased = false;
    TypePtr type = nullptr;
};

using VarSet = std::unordered_set<VarKey, VarKeyHash>;
using Adjacency = std::unordered_map<VarKey, VarSet, VarKeyHash>;

std::vector<VarKey> sorted_var_keys(const VarSet& vars);
std::vector<VarKey> sorted_var_keys(const std::vector<VarKey>& vars);

VarKey key_of(const Variable* var) {
    return VarKey{var->name(), var->ssa_version()};
}

std::vector<VarKey> keys_from_variables(const std::vector<Variable*>& vars) {
    std::vector<VarKey> result;
    result.reserve(vars.size());
    VarSet seen;
    for (Variable* var : vars) {
        if (var == nullptr) {
            continue;
        }
        VarKey key = key_of(var);
        if (seen.insert(key).second) {
            result.push_back(std::move(key));
        }
    }
    return result;
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
    std::vector<VarKey> items = sorted_var_keys(vars);
    for (std::size_t i = 0; i < items.size(); ++i) {
        add_vertex_if_missing(graph, items[i]);
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            add_edge(graph, items[i], items[j]);
        }
    }
}

std::string compatibility_group_of(const VarKey& key, const VarInfo& info) {
    if (info.aliased) {
        return "alias:" + key.name + "#" + std::to_string(key.version);
    }
    if (info.type) {
        return "type:" + info.type->to_string();
    }
    return "size:" + std::to_string(info.size_bytes);
}

bool label_greater(const std::vector<int>& lhs, const std::vector<int>& rhs) {
    const std::size_t n = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (lhs[i] != rhs[i]) {
            return lhs[i] > rhs[i];
        }
    }
    return lhs.size() > rhs.size();
}

bool key_less(const VarKey& lhs, const VarKey& rhs) {
    if (lhs.name != rhs.name) {
        return lhs.name < rhs.name;
    }
    return lhs.version < rhs.version;
}

std::uint64_t block_order_key(BasicBlock* block) {
    if (!block) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(block->id());
}

std::vector<BasicBlock*> sorted_blocks_by_id(const std::vector<BasicBlock*>& blocks) {
    std::vector<BasicBlock*> ordered = blocks;
    std::stable_sort(ordered.begin(), ordered.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
        return block_order_key(lhs) < block_order_key(rhs);
    });
    return ordered;
}

std::vector<Edge*> sorted_successor_edges(const std::vector<Edge*>& edges) {
    std::vector<Edge*> ordered = edges;
    std::stable_sort(ordered.begin(), ordered.end(), [](Edge* lhs, Edge* rhs) {
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
    return ordered;
}

std::vector<VarKey> sorted_var_keys(const VarSet& vars) {
    std::vector<VarKey> ordered(vars.begin(), vars.end());
    std::stable_sort(ordered.begin(), ordered.end(), key_less);
    return ordered;
}

std::vector<VarKey> sorted_var_keys(const std::vector<VarKey>& vars) {
    std::vector<VarKey> ordered = vars;
    std::stable_sort(ordered.begin(), ordered.end(), key_less);
    return ordered;
}

std::vector<VarKey> reverse_lex_bfs(const std::vector<VarKey>& vertices, const Adjacency& graph) {
    std::unordered_map<VarKey, std::vector<int>, VarKeyHash> labels;
    VarSet unnumbered(vertices.begin(), vertices.end());
    std::vector<VarKey> candidate_order = sorted_var_keys(vertices);
    std::vector<VarKey> order;
    order.reserve(vertices.size());

    const auto label_for = [&](const VarKey& key) -> const std::vector<int>& {
        static const std::vector<int> empty_label;
        auto it = labels.find(key);
        return it != labels.end() ? it->second : empty_label;
    };

    int step = static_cast<int>(vertices.size());
    while (!unnumbered.empty()) {
        bool first = true;
        VarKey best{};
        for (const VarKey& candidate : candidate_order) {
            if (!unnumbered.contains(candidate)) {
                continue;
            }
            if (first) {
                best = candidate;
                first = false;
                continue;
            }
            const auto& cand_label = label_for(candidate);
            const auto& best_label = label_for(best);
            if (label_greater(cand_label, best_label)
                || (!label_greater(best_label, cand_label) && key_less(candidate, best))) {
                best = candidate;
            }
        }

        if (first) {
            break;
        }

        unnumbered.erase(best);
        order.push_back(best);

        auto neigh_it = graph.find(best);
        if (neigh_it != graph.end()) {
            for (const VarKey& neigh : sorted_var_keys(neigh_it->second)) {
                if (unnumbered.contains(neigh)) {
                    labels[neigh].push_back(step);
                }
            }
        }
        --step;
    }

    return order;
}

void remove_identity_assignments(ControlFlowGraph& cfg) {
    for (BasicBlock* block : sorted_blocks_by_id(cfg.blocks())) {
        std::vector<Instruction*> rewritten;
        rewritten.reserve(block->instructions().size());
        for (Instruction* inst : block->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (assign != nullptr) {
                auto* dst = dyn_cast<Variable>(assign->destination());
                auto* src = dyn_cast<Variable>(assign->value());
                if (dst != nullptr && src != nullptr && dst == src) {
                    continue;
                }
            }
            rewritten.push_back(inst);
        }
        block->set_instructions(std::move(rewritten));
    }
}

} // namespace

void MinimalVariableRenamer::rename(DecompilerArena& arena, ControlFlowGraph& cfg) {
    std::unordered_map<VarKey, VarInfo, VarKeyHash> var_info;
    std::unordered_map<BasicBlock*, VarSet> uses_block;
    std::unordered_map<BasicBlock*, VarSet> defs_block;
    std::unordered_map<BasicBlock*, VarSet> live_in;
    std::unordered_map<BasicBlock*, VarSet> live_out;
    Adjacency interference;
    const std::vector<BasicBlock*> ordered_blocks = sorted_blocks_by_id(cfg.blocks());

    auto record_var = [&](Variable* var) {
        if (var == nullptr) {
            return;
        }
        VarKey key = key_of(var);
        auto& info = var_info[key];
        if (info.sample == nullptr) {
            info.sample = var;
            info.size_bytes = var->size_bytes;
            info.aliased = var->is_aliased();
        } else {
            info.aliased = info.aliased || var->is_aliased();
            info.size_bytes = std::max(info.size_bytes, var->size_bytes);
        }

        if (TypePtr candidate_type = var->ir_type()) {
            if (!info.type) {
                info.type = candidate_type;
            } else {
                const std::string current_sig = info.type->to_string();
                const std::string candidate_sig = candidate_type->to_string();
                if (candidate_sig < current_sig) {
                    info.type = candidate_type;
                }
            }
        }
        add_vertex_if_missing(interference, key);
    };

    for (BasicBlock* block : ordered_blocks) {
        VarSet uses;
        VarSet defs;

        for (Instruction* inst : block->instructions()) {
            auto req_keys = keys_from_variables(inst->requirements());
            auto def_keys = keys_from_variables(inst->definitions());

            for (Variable* v : inst->requirements()) {
                record_var(v);
            }
            for (Variable* v : inst->definitions()) {
                record_var(v);
            }

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

    bool changed = true;
    auto order = cfg.reverse_post_order();
    std::stable_sort(order.begin(), order.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
        return block_order_key(lhs) < block_order_key(rhs);
    });
    while (changed) {
        changed = false;

        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            BasicBlock* block = *it;

            VarSet out;
            for (Edge* edge : sorted_successor_edges(block->successors())) {
                if (edge != nullptr && edge->target() != nullptr) {
                    const VarSet& succ_in = live_in[edge->target()];
                    out.insert(succ_in.begin(), succ_in.end());
                }
            }

            VarSet in = uses_block[block];
            VarSet out_minus_def = out;
            for (const VarKey& d : sorted_var_keys(defs_block[block])) {
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

    for (BasicBlock* block : ordered_blocks) {
        add_clique(interference, live_in[block]);
        add_clique(interference, live_out[block]);

        VarSet current = live_out[block];
        auto insts = block->instructions();
        for (auto it = insts.rbegin(); it != insts.rend(); ++it) {
            Instruction* inst = *it;
            VarSet defs;
            VarSet reqs;
            for (const VarKey& k : keys_from_variables(inst->definitions())) {
                defs.insert(k);
            }
            for (const VarKey& k : keys_from_variables(inst->requirements())) {
                reqs.insert(k);
            }

            const std::vector<VarKey> sorted_defs = sorted_var_keys(defs);
            const std::vector<VarKey> sorted_current = sorted_var_keys(current);
            for (const VarKey& d : sorted_defs) {
                for (const VarKey& live : sorted_current) {
                    add_edge(interference, d, live);
                }
            }
            for (const VarKey& d : sorted_defs) {
                current.erase(d);
            }
            current.insert(reqs.begin(), reqs.end());
        }
    }

    std::unordered_map<std::string, std::vector<VarKey>> groups;
    for (const auto& [key, info] : var_info) {
        groups[compatibility_group_of(key, info)].push_back(key);
    }

    std::vector<std::string> group_names;
    group_names.reserve(groups.size());
    for (const auto& [name, _] : groups) {
        group_names.push_back(name);
    }
    std::sort(group_names.begin(), group_names.end());

    std::unordered_map<VarKey, int, VarKeyHash> class_of;
    std::unordered_map<int, std::vector<VarKey>> members_of;
    int next_class = 0;

    for (const std::string& group_name : group_names) {
        std::vector<VarKey> vertices = sorted_var_keys(groups[group_name]);
        auto order_in_group = reverse_lex_bfs(vertices, interference);

        std::unordered_map<VarKey, int, VarKeyHash> color_of;
        std::unordered_map<std::string, std::unordered_map<int, std::size_t>> name_color_count;
        int next_color = 0;

        for (const VarKey& var : order_in_group) {
            std::unordered_set<int> blocked;
            auto neigh_it = interference.find(var);
            if (neigh_it != interference.end()) {
                for (const VarKey& neigh : sorted_var_keys(neigh_it->second)) {
                    auto c_it = color_of.find(neigh);
                    if (c_it != color_of.end()) {
                        blocked.insert(c_it->second);
                    }
                }
            }

            std::vector<int> possible;
            for (int c = 0; c < next_color; ++c) {
                if (!blocked.contains(c)) {
                    possible.push_back(c);
                }
            }
            possible.push_back(next_color);

            int chosen = possible.front();
            std::size_t best_occ = 0;
            bool found_preferred = false;
            auto dist_it = name_color_count.find(var.name);
            for (int c : possible) {
                std::size_t occ = 0;
                if (dist_it != name_color_count.end()) {
                    auto occ_it = dist_it->second.find(c);
                    if (occ_it != dist_it->second.end()) {
                        occ = occ_it->second;
                    }
                }
                if (occ > best_occ) {
                    best_occ = occ;
                    chosen = c;
                    found_preferred = true;
                }
            }

            if (!found_preferred) {
                chosen = *std::min_element(possible.begin(), possible.end());
            }

            if (chosen == next_color) {
                ++next_color;
            }

            color_of[var] = chosen;
            name_color_count[var.name][chosen]++;
        }

        for (const VarKey& var : order_in_group) {
            auto color_it = color_of.find(var);
            if (color_it == color_of.end()) {
                continue;
            }
            const int color = color_it->second;
            const int global_class = next_class + color;
            class_of[var] = global_class;
            members_of[global_class].push_back(var);
        }
        next_class += next_color;
    }

    std::unordered_map<VarKey, Variable*, VarKeyHash> replacement_for;
    int fresh_name = 0;
    std::vector<int> class_ids;
    class_ids.reserve(members_of.size());
    for (const auto& [klass, _] : members_of) {
        class_ids.push_back(klass);
    }
    std::sort(class_ids.begin(), class_ids.end());

    for (int klass : class_ids) {
        auto& members = members_of[klass];
        if (members.empty()) {
            continue;
        }

        std::stable_sort(members.begin(), members.end(), key_less);

        const VarInfo& info = var_info[members.front()];
        if (info.aliased) {
            continue;
        }
        const std::size_t size = info.size_bytes > 0 ? info.size_bytes : 4;
        std::string replacement_name = "var_" + std::to_string(fresh_name++);
        auto* replacement = arena.create<Variable>(replacement_name, size);
        replacement->set_ssa_version(0);
        replacement->set_aliased(info.aliased);
        replacement->set_ir_type(info.type);

        for (const VarKey& key : members) {
            replacement_for[key] = replacement;
        }
    }

    for (BasicBlock* block : ordered_blocks) {
        for (Instruction* inst : block->instructions()) {
            std::vector<Variable*> vars = inst->requirements();
            auto defs = inst->definitions();
            vars.insert(vars.end(), defs.begin(), defs.end());

            VarSet seen;
            for (Variable* old_var : vars) {
                if (old_var == nullptr) {
                    continue;
                }
                VarKey key = key_of(old_var);
                if (!seen.insert(key).second) {
                    continue;
                }
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
