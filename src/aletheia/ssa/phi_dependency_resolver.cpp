#include "phi_dependency_resolver.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>
#include <tuple>

namespace aletheia {

namespace {
    std::string phi_var_key(const Variable* var) {
        if (!var) {
            return "";
        }
        return var->name() + "#" + std::to_string(var->ssa_version());
    }

    std::tuple<std::string, std::size_t> phi_order_key(Phi* phi) {
        if (!phi || !phi->dest_var()) {
            return std::make_tuple(std::string(""), std::size_t{0});
        }
        Variable* dst = phi->dest_var();
        return std::make_tuple(dst->name(), dst->ssa_version());
    }

    bool phi_less(Phi* lhs, Phi* rhs) {
        const auto lhs_key = phi_order_key(lhs);
        const auto rhs_key = phi_order_key(rhs);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        return false;
    }

    struct PhiGraph {
        std::vector<Phi*> nodes;
        std::unordered_map<Phi*, std::vector<Phi*>> edges;

        void add_edge(Phi* u, Phi* v) {
            edges[u].push_back(v);
        }
    };

    // Performs DFS to output nodes in post-order.
    void dfs_postorder(Phi* u, PhiGraph& graph, std::unordered_set<Phi*>& visited, std::vector<Phi*>& postorder) {
        visited.insert(u);
        auto succs = graph.edges[u];
        std::sort(succs.begin(), succs.end(), phi_less);
        succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
        for (Phi* v : succs) {
            if (visited.find(v) == visited.end()) {
                dfs_postorder(v, graph, visited, postorder);
            }
        }
        postorder.push_back(u);
    }
}

void PhiDependencyResolver::resolve(DecompilerArena& arena, ControlFlowGraph& cfg) {
    for (BasicBlock* bb : cfg.blocks()) {
        std::vector<Phi*> phis;
        std::vector<Instruction*> non_phis;

        for (Instruction* inst : bb->instructions()) {
            if (auto* phi = dyn_cast<Phi>(inst)) {
                phis.push_back(phi);
            } else {
                non_phis.push_back(inst);
            }
        }

        if (phis.empty()) {
            continue;
        }
        std::sort(phis.begin(), phis.end(), phi_less);

        bool cycle_broken = true;
        std::vector<Assignment*> added_copies;

        while (cycle_broken) {
            cycle_broken = false;
            PhiGraph graph;
            graph.nodes = phis;

            // Map fully-versioned definitions to their Phi instructions.
            std::unordered_map<std::string, Phi*> def_map;
            for (Phi* phi : phis) {
                if (phi->dest_var()) {
                    def_map[phi_var_key(phi->dest_var())] = phi;
                }
            }

            // Add edges: u -> v if u requires what v defines
            for (Phi* u : phis) {
                for (Variable* req : u->requirements()) {
                    auto it = def_map.find(phi_var_key(req));
                    if (it != def_map.end() && it->second != u) {
                        graph.add_edge(u, it->second);
                    }
                }
            }

            for (auto& [phi, succs] : graph.edges) {
                std::sort(succs.begin(), succs.end(), phi_less);
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }

            // Compute topological order via DFS post-order reversal
            std::unordered_set<Phi*> visited;
            std::vector<Phi*> postorder;
            for (Phi* phi : phis) {
                if (visited.find(phi) == visited.end()) {
                    dfs_postorder(phi, graph, visited, postorder);
                }
            }
            
            std::vector<Phi*> topological_order = postorder;
            std::reverse(topological_order.begin(), topological_order.end());

            // Compute approximate directed feedback vertex set
            std::unordered_set<Phi*> smaller_order;
            std::unordered_set<Phi*> directed_fvs;
            for (Phi* phi : topological_order) {
                bool has_back_edge = false;
                for (Phi* succ : graph.edges[phi]) {
                    if (smaller_order.find(succ) != smaller_order.end()) {
                        has_back_edge = true;
                        break;
                    }
                }
                if (has_back_edge) {
                    directed_fvs.insert(phi);
                } else {
                    smaller_order.insert(phi);
                }
            }

            if (!directed_fvs.empty()) {
                cycle_broken = true;
                // Break cycle for the first FVS node and restart
                Phi* fvs_node = *std::min_element(directed_fvs.begin(), directed_fvs.end(), phi_less);
                Variable* orig_var = fvs_node->dest_var();
                
                auto* copy_var = arena.create<Variable>("copy_" + orig_var->name(), orig_var->size_bytes);
                copy_var->set_ssa_version(orig_var->ssa_version());
                copy_var->set_ir_type(orig_var->ir_type());
                
                fvs_node->rename_destination(orig_var, copy_var);
                
                auto* copy_assign = arena.create<Assignment>(orig_var, copy_var);
                added_copies.push_back(copy_assign);
            } else {
                // No cycles, we have a valid topological sort.
                // Reconstruct instructions in BB: sorted phis + added copies + non_phis
                std::vector<Instruction*> final_insts;
                for (Phi* phi : topological_order) {
                    final_insts.push_back(phi);
                }
                for (Assignment* copy : added_copies) {
                    final_insts.push_back(copy);
                }
                for (Instruction* inst : non_phis) {
                    final_insts.push_back(inst);
                }
                bb->set_instructions(std::move(final_insts));
            }
        }
    }
}

} // namespace aletheia
