#include "structurer.hpp"
#include "ast_processor.hpp"
#include "loop_structurer.hpp"
#include "reachability.hpp"
#include "../ssa/dominators.hpp"
#include "../../logos/z3_logic.hpp"
#include "graph_slice/graph_slice.hpp"
#include "reaching_conditions/reaching_conditions.hpp"
#include "cbr/cbr.hpp"
#include "car/car.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <unordered_set>
#include <string>

namespace aletheia {

namespace {

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string v(value);
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

void collect_original_block_ids(AstNode* node, std::vector<std::uint64_t>& ids) {
    if (!node) {
        return;
    }

    if (BasicBlock* bb = node->get_original_block()) {
        ids.push_back(static_cast<std::uint64_t>(bb->id()));
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            collect_original_block_ids(child, ids);
        }
        return;
    }
    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        collect_original_block_ids(if_node->true_branch(), ids);
        collect_original_block_ids(if_node->false_branch(), ids);
        return;
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        collect_original_block_ids(loop->body(), ids);
        return;
    }
    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->cases()) {
            collect_original_block_ids(case_node->body(), ids);
        }
        return;
    }
    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        collect_original_block_ids(case_node->body(), ids);
    }
}

std::uint64_t transition_block_order_key(TransitionBlock* node) {
    if (!node || !node->ast_node()) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    if (BasicBlock* bb = node->ast_node()->get_original_block()) {
        return static_cast<std::uint64_t>(bb->id());
    }

    std::vector<std::uint64_t> ids;
    collect_original_block_ids(node->ast_node(), ids);
    if (!ids.empty()) {
        return *std::min_element(ids.begin(), ids.end());
    }

    const auto kind_bias = static_cast<std::uint64_t>(node->ast_node()->ast_kind());
    return std::numeric_limits<std::uint64_t>::max() - kind_bias;
}

std::string transition_block_signature(TransitionBlock* node) {
    if (!node || !node->ast_node()) {
        return "<null>";
    }

    std::vector<std::uint64_t> ids;
    collect_original_block_ids(node->ast_node(), ids);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::string out = "k" + std::to_string(static_cast<int>(node->ast_node()->ast_kind())) + ":";
    if (ids.empty()) {
        out += "none";
    } else {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i != 0) {
                out += ",";
            }
            out += std::to_string(ids[i]);
        }
    }
    return out;
}

void sort_transition_blocks_deterministically(std::vector<TransitionBlock*>& blocks) {
    std::stable_sort(blocks.begin(), blocks.end(), [](TransitionBlock* lhs, TransitionBlock* rhs) {
        const std::uint64_t lhs_key = transition_block_order_key(lhs);
        const std::uint64_t rhs_key = transition_block_order_key(rhs);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        return transition_block_signature(lhs) < transition_block_signature(rhs);
    });
}

std::vector<TransitionBlock*> sorted_transition_blocks(const std::vector<TransitionBlock*>& blocks) {
    std::vector<TransitionBlock*> sorted = blocks;
    sort_transition_blocks_deterministically(sorted);
    return sorted;
}

std::unordered_set<TransitionBlock*> compute_dominated_region(TransitionCFG& cfg, TransitionBlock* header) {
    std::unordered_set<TransitionBlock*> dominated;
    if (!header) return dominated;
    for (TransitionBlock* node : cfg.blocks()) {
        if (cfg.dominates(header, node)) {
            dominated.insert(node);
        }
    }
    return dominated;
}

std::unordered_set<TransitionBlock*> compute_region_exits(
    const std::unordered_set<TransitionBlock*>& region) {
    std::unordered_set<TransitionBlock*> exits;
    for (TransitionBlock* node : region) {
        for (TransitionBlock* succ : node->successors_blocks()) {
            if (!region.contains(succ)) {
                exits.insert(succ);
            }
        }
    }
    return exits;
}

std::unordered_set<TransitionBlock*> compute_dominated_subset(
    TransitionCFG& cfg,
    TransitionBlock* dominator,
    const std::unordered_set<TransitionBlock*>& candidates) {
    std::unordered_set<TransitionBlock*> subset;
    if (!dominator) return subset;
    for (TransitionBlock* node : candidates) {
        if (cfg.dominates(dominator, node)) {
            subset.insert(node);
        }
    }
    return subset;
}

bool is_restructurable_region(
    TransitionCFG& cfg,
    TransitionBlock* header,
    const std::unordered_set<TransitionBlock*>& region,
    TransitionBlock** out_exit) {
    if (!header || !region.contains(header) || region.size() <= 1) return false;

    // Connectivity inside region from header.
    std::unordered_set<TransitionBlock*> reachable;
    std::vector<TransitionBlock*> stack = {header};
    while (!stack.empty()) {
        TransitionBlock* cur = stack.back();
        stack.pop_back();
        if (!cur || reachable.contains(cur) || !region.contains(cur)) continue;
        reachable.insert(cur);
        for (TransitionBlock* succ : cur->successors_blocks()) {
            if (region.contains(succ) && !reachable.contains(succ)) {
                stack.push_back(succ);
            }
        }
    }
    if (reachable.size() != region.size()) return false;

    // Closed predecessors (except header entry).
    for (TransitionBlock* node : region) {
        if (node == header) continue;
        for (TransitionBlock* pred : node->predecessors_blocks()) {
            if (!region.contains(pred)) {
                return false;
            }
        }
    }

    auto exits = compute_region_exits(region);
    if (exits.size() > 1) return false;
    if (out_exit) {
        *out_exit = exits.empty() ? nullptr : *exits.begin();
    }
    return true;
}

} // namespace

void CyclicRegionFinder::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;

    cfg.refresh_edge_properties();
    auto loop_heads = cfg.get_loop_heads();

    while (!loop_heads.empty()) {
        TransitionBlock* head = loop_heads.back();
        loop_heads.pop_back();
        if (!head) {
            continue;
        }

        size_t initial_nodes = cfg.blocks().size();

        // 1. Compute initial loop nodes (GraphSlice from head to latching nodes)
        std::vector<TransitionBlock*> latching_nodes;
        for (auto* e : head->predecessors()) {
            if (e->property() == EdgeProperty::Back || e->property() == EdgeProperty::Retreating) {
                latching_nodes.push_back(e->source());
            }
        }
        
        TransitionCFG* loop_region = GraphSlice::compute_graph_slice_for_sink_nodes(arena_, &cfg, head, latching_nodes, false);
        if (!loop_region) {
            continue;
        }
        std::unordered_set<TransitionBlock*> region_set(loop_region->blocks().begin(), loop_region->blocks().end());
        region_set.erase(nullptr);

        // 2. Restructure Abnormal Entry (TODO)
        // If there's a retreating edge into head
        bool has_retreating = false;
        for (auto* e : head->predecessors()) {
            if (e->property() == EdgeProperty::Retreating) has_retreating = true;
        }
        if (has_retreating) {
            std::unordered_map<TransitionBlock*, std::vector<TransitionEdge*>> entry_edges;
            std::vector<TransitionBlock*> sorted_region_nodes(region_set.begin(), region_set.end());
            sort_transition_blocks_deterministically(sorted_region_nodes);
            for (auto* node : sorted_region_nodes) {
                if (!node) {
                    continue;
                }
                for (auto* e : node->predecessors()) {
                    if (!e || !e->source()) {
                        continue;
                    }
                    if (!region_set.contains(e->source())) {
                        entry_edges[node].push_back(e);
                    }
                }
            }

            if (entry_edges.empty()) {
                // Nothing enters the loop from outside this region; skip abnormal-entry rewriting.
                goto skip_abnormal_entry;
            }

            std::vector<TransitionBlock*> ordered_entries;
            ordered_entries.reserve(entry_edges.size());
            if (entry_edges.contains(head)) {
                ordered_entries.push_back(head);
            }
            std::vector<TransitionBlock*> non_head_entries;
            for (auto& pair : entry_edges) {
                if (pair.first && pair.first != head) {
                    non_head_entries.push_back(pair.first);
                }
            }
            sort_transition_blocks_deterministically(non_head_entries);
            ordered_entries.insert(ordered_entries.end(), non_head_entries.begin(), non_head_entries.end());

            if (ordered_entries.empty()) {
                goto skip_abnormal_entry;
            }

            const size_t num_entries = ordered_entries.size();
            std::string var_name = "entry_" + std::to_string(reinterpret_cast<uintptr_t>(head));
            auto* entry_var = arena_.create<Variable>(var_name, 4); entry_var->set_ir_type(Integer::int32_t());

            std::vector<std::pair<TransitionBlock*, logos::LogicCondition>> condition_nodes;
            for (size_t i = 0; i + 1 < num_entries; ++i) {
                auto* cond_op = arena_.create<Condition>(OperationType::eq, entry_var, arena_.create<Constant>(i, 4));
                auto* branch = arena_.create<Branch>(cond_op);
                auto* bb = arena_.create<BasicBlock>(9000 + i);
                bb->add_instruction(branch);
                auto* tb = arena_.create<TransitionBlock>(arena_.create<CodeNode>(bb));
                cfg.add_block(tb);
                loop_region->add_block(tb);
                
                logos::Z3Converter z3_conv(task_.z3_ctx());
                condition_nodes.push_back({tb, z3_conv.convert_to_condition(cond_op)});
            }

            std::vector<TransitionBlock*> code_nodes;
            for (size_t i = 0; i < num_entries; ++i) {
                auto* assign = arena_.create<Assignment>(entry_var, arena_.create<Constant>(i, 4));
                auto* bb = arena_.create<BasicBlock>(9100 + i);
                bb->add_instruction(assign);
                auto* tb = arena_.create<TransitionBlock>(arena_.create<CodeNode>(bb));
                cfg.add_block(tb);
                // Note: code_nodes are outside the loop region
                code_nodes.push_back(tb);
            }

            TransitionBlock* new_head = condition_nodes.empty() ? head : condition_nodes[0].first;

            for (auto* cn : code_nodes) {
                cfg.add_edge(cn, new_head, logos::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
            }
            for (size_t i = 0; i + 1 < condition_nodes.size(); ++i) {
                cfg.add_edge(condition_nodes[i].first, condition_nodes[i+1].first, condition_nodes[i].second.negate(), EdgeProperty::NonLoop);
            }

            // Redirect loop edges (back/retreating) to new_head
            std::vector<TransitionEdge*> loop_edges;
            for (auto* e : head->predecessors()) {
                if (!e) {
                    continue;
                }
                if (e->property() == EdgeProperty::Back || e->property() == EdgeProperty::Retreating) {
                    loop_edges.push_back(e);
                }
            }
            for (auto* e : loop_edges) {
                auto* src = e->source();
                auto tag = e->tag();
                auto prop = e->property();
                cfg.remove_edge(e);
                cfg.add_edge(src, new_head, tag, prop);
            }

            std::vector<std::pair<TransitionBlock*, logos::LogicCondition>> ext_conds = condition_nodes;
            if (!condition_nodes.empty()) {
                ext_conds.push_back({condition_nodes.back().first, condition_nodes.back().second.negate()});
            } else {
                // If there's only 1 entry (head), but it had a retreating edge, num_entries = 1, condition_nodes = 0
                // Wait, if num_entries=1, new_head=head, no conditions. We still redirect the entry edges to code_nodes[0] -> head.
            }

            for (size_t i = 0; i < ordered_entries.size(); ++i) {
                auto* tb = ordered_entries[i];
                auto* cn = code_nodes[i];
                if (!tb || !cn) {
                    continue;
                }
                if (i < ext_conds.size()) {
                    cfg.add_edge(ext_conds[i].first, tb, ext_conds[i].second, EdgeProperty::NonLoop);
                }
                for (auto* e : entry_edges[tb]) {
                    if (!e || !e->source()) {
                        continue;
                    }
                    auto* src = e->source();
                    auto tag = e->tag();
                    auto prop = e->property();
                    cfg.remove_edge(e);
                    cfg.add_edge(src, cn, tag, prop);
                }
            }

            for (size_t i = 1; i < ordered_entries.size(); ++i) {
                auto* tb = ordered_entries[i];
                if (!tb) {
                    continue;
                }
                auto* assign = arena_.create<Assignment>(entry_var, arena_.create<Constant>(0, 4));
                
                auto* ast = tb->ast_node();
                if (auto* code_node = ast_dyn_cast<CodeNode>(ast)) {
                    code_node->block()->add_instruction(assign);
                } else if (auto* seq_node = ast_dyn_cast<SeqNode>(ast)) {
                    auto* new_bb = arena_.create<BasicBlock>(9200 + i);
                    new_bb->add_instruction(assign);
                    auto* new_cn = arena_.create<CodeNode>(new_bb);
                    seq_node->add_node(new_cn); // append to end
                } else {
                    auto* new_seq = arena_.create<SeqNode>();
                    new_seq->add_node(ast);
                    auto* new_bb = arena_.create<BasicBlock>(9200 + i);
                    new_bb->add_instruction(assign);
                    new_seq->add_node(arena_.create<CodeNode>(new_bb));
                    tb->set_ast_node(new_seq);
                }
            }
            
            loop_region->set_entry(new_head);
            head = new_head;
            region_set.insert(new_head);
            loop_region->add_block(new_head);
            for(auto& p : condition_nodes) {
                region_set.insert(p.first);
                loop_region->add_block(p.first);
            }
        }

skip_abnormal_entry:


        // 3. Compute Loop Successors
        std::vector<TransitionBlock*> loop_successors;
        std::unordered_set<TransitionBlock*> succ_set;
        for (auto* node : sorted_transition_blocks(loop_region->blocks())) {
            if (!node) {
                continue;
            }
            for (auto* succ : node->successors_blocks()) {
                if (!succ) {
                    continue;
                }
                if (!region_set.contains(succ) && !succ_set.contains(succ)) {
                    loop_successors.push_back(succ);
                    succ_set.insert(succ);
                }
            }
        }
        sort_transition_blocks_deterministically(loop_successors);

        // 4. Restructure Abnormal Exit (TODO)
        if (loop_successors.size() > 1) {
            size_t num_exits = loop_successors.size();
            std::string var_name = "exit_" + std::to_string(reinterpret_cast<uintptr_t>(head));
            auto* exit_var = arena_.create<Variable>(var_name, 4); exit_var->set_ir_type(Integer::int32_t());

            std::vector<std::pair<TransitionBlock*, logos::LogicCondition>> condition_nodes;
            for (size_t i = 0; i < num_exits - 1; ++i) {
                auto* cond_op = arena_.create<Condition>(OperationType::eq, exit_var, arena_.create<Constant>(i, 4));
                auto* branch = arena_.create<Branch>(cond_op);
                auto* bb = arena_.create<BasicBlock>(9300 + i);
                bb->add_instruction(branch);
                auto* tb = arena_.create<TransitionBlock>(arena_.create<CodeNode>(bb));
                cfg.add_block(tb);
                
                logos::Z3Converter z3_conv(task_.z3_ctx());
                condition_nodes.push_back({tb, z3_conv.convert_to_condition(cond_op)});
            }

            std::vector<TransitionBlock*> code_nodes;
            for (size_t i = 0; i < num_exits; ++i) {
                auto* assign = arena_.create<Assignment>(exit_var, arena_.create<Constant>(i, 4));
                auto* bb = arena_.create<BasicBlock>(9400 + i);
                bb->add_instruction(assign);
                auto* tb = arena_.create<TransitionBlock>(arena_.create<CodeNode>(bb));
                cfg.add_block(tb);
                loop_region->add_block(tb);
                region_set.insert(tb);
                code_nodes.push_back(tb);
            }

            TransitionBlock* new_successor = condition_nodes.empty() ? loop_successors[0] : condition_nodes[0].first;

            for (auto* cn : code_nodes) {
                cfg.add_edge(cn, new_successor, logos::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
            }
            for (size_t i = 0; i + 1 < condition_nodes.size(); ++i) {
                cfg.add_edge(condition_nodes[i].first, condition_nodes[i+1].first, condition_nodes[i].second.negate(), EdgeProperty::NonLoop);
            }

            std::vector<TransitionBlock*> sorted_successors = loop_successors; // python topological sorts them
            sort_transition_blocks_deterministically(sorted_successors);

            std::vector<std::pair<TransitionBlock*, logos::LogicCondition>> ext_conds = condition_nodes;
            if (!condition_nodes.empty()) {
                ext_conds.push_back({condition_nodes.back().first, condition_nodes.back().second.negate()});
            }

            // Reverse sorted successors according to python
            std::vector<TransitionBlock*> reversed_succs = sorted_successors;
            std::reverse(reversed_succs.begin(), reversed_succs.end());

            for (size_t i = 0; i < reversed_succs.size(); ++i) {
                auto* tb = reversed_succs[i];
                auto* cn = code_nodes[i];
                if (i < ext_conds.size()) {
                    cfg.add_edge(ext_conds[i].first, tb, ext_conds[i].second, EdgeProperty::NonLoop);
                }
                
                std::vector<TransitionEdge*> exit_edges;
                for(auto* e : tb->predecessors()) {
                    if (!e || !e->source()) {
                        continue;
                    }
                    if (region_set.contains(e->source())) exit_edges.push_back(e);
                }
                for (auto* e : exit_edges) {
                    auto* src = e->source();
                    auto tag = e->tag();
                    cfg.remove_edge(e);
                    cfg.add_edge(src, cn, tag, EdgeProperty::NonLoop);
                }
            }

            loop_successors = {new_successor}; // only 1 successor now!
        }


        // 5. Restructure acyclic loop body
        auto* loop_cfg = arena_.create<TransitionCFG>(arena_);
        std::unordered_map<TransitionBlock*, TransitionBlock*> block_map;
        for (auto* node : sorted_transition_blocks(loop_region->blocks())) {
            if (!node) {
                continue;
            }
            auto* clone = arena_.create<TransitionBlock>(node->ast_node());
            block_map[node] = clone;
            loop_cfg->add_block(clone);
        }
        if (!block_map.contains(head) || block_map[head] == nullptr) {
            continue;
        }
        loop_cfg->set_entry(block_map[head]);

        for (auto* node : sorted_transition_blocks(loop_region->blocks())) {
            if (!node) {
                continue;
            }
            auto* clone_src = block_map[node];
            if (!clone_src) {
                continue;
            }
            for (auto* e : node->successors()) {
                if (!e) {
                    continue;
                }
                auto* succ = e->sink();
                if (succ == head && node != head && region_set.contains(node)) {
                    // Back edge -> Continue node
                    auto* cont_block = arena_.create<BasicBlock>(9999);
                    cont_block->add_instruction(arena_.create<ContinueInstr>());
                    auto* cont_trans = arena_.create<TransitionBlock>(arena_.create<CodeNode>(cont_block));
                    loop_cfg->add_block(cont_trans);
                    loop_cfg->add_edge(clone_src, cont_trans, e->tag(), EdgeProperty::NonLoop);
                } else if (!region_set.contains(succ)) {
                    // Exit edge -> Break node
                    auto* brk_block = arena_.create<BasicBlock>(9998);
                    brk_block->add_instruction(arena_.create<BreakInstr>());
                    auto* brk_trans = arena_.create<TransitionBlock>(arena_.create<CodeNode>(brk_block));
                    loop_cfg->add_block(brk_trans);
                    loop_cfg->add_edge(clone_src, brk_trans, e->tag(), EdgeProperty::NonLoop);
                } else if (block_map.contains(succ)) {
                    auto* clone_dst = block_map[succ];
                    if (clone_dst) {
                        loop_cfg->add_edge(clone_src, clone_dst, e->tag(), e->property());
                    }
                }
            }
        }

        AcyclicRegionRestructurer acyclic(task_);
        acyclic.process(*loop_cfg);

        if (!loop_cfg->entry()) {
            continue;
        }
        AstNode* loop_body_ast = loop_cfg->entry()->ast_node();
        auto* endless_loop = arena_.create<WhileLoopNode>(loop_body_ast);
        AstNode* refined = LoopStructurer::refine_loop(arena_, endless_loop);
        head->set_ast_node(refined);

        for (auto* succ : loop_successors) {
            if (!succ) {
                continue;
            }
            for (auto* node : loop_region->blocks()) {
                if (node && node != head) {
                    cfg.remove_edge_between(node, succ);
                }
            }
            bool linked = false;
            for (auto* s : head->successors_blocks()) if (s == succ) { linked = true; break; }
            if (!linked) cfg.add_edge(head, succ, logos::LogicCondition(task_.z3_ctx().bool_val(true)));
        }

        for (auto* node : loop_region->blocks()) {
            if (!node || node == head) continue;
            // Need to remove edges properly
            std::vector<TransitionEdge*> out_edges = node->successors();
            for (auto* e : out_edges) cfg.remove_edge(e);
            std::vector<TransitionEdge*> in_edges = node->predecessors();
            for (auto* e : in_edges) cfg.remove_edge(e);
            cfg.remove_block(node);
        }
        cfg.remove_edge_between(head, head); // remove self loop

        if (cfg.blocks().size() != initial_nodes) {
            cfg.refresh_edge_properties();
            loop_heads = cfg.get_loop_heads();
        }
    }
}

void AcyclicRegionRestructurer::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;

    bool reduced = true;
    constexpr std::size_t max_iterations = 500;
    std::size_t iteration = 0;
    while (reduced && cfg.blocks().size() > 1 && iteration < max_iterations) {
        reduced = false;
        ++iteration;

        std::unordered_set<TransitionBlock*> best_region;
        TransitionBlock* best_header = nullptr;
        TransitionBlock* best_exit = nullptr;

        for (TransitionBlock* header : cfg.blocks()) {
            std::unordered_set<TransitionBlock*> full_region = compute_dominated_region(cfg, header);
            TransitionBlock* full_exit = nullptr;
            if (!is_restructurable_region(cfg, header, full_region, &full_exit)) {
                continue;
            }

            std::unordered_set<TransitionBlock*> candidate_region = full_region;
            TransitionBlock* candidate_exit = full_exit;

            // Improved DREAM subset search:
            // try trimming each dominated subtree rooted at a potential exit node.
            for (TransitionBlock* possible_exit : full_region) {
                if (possible_exit == header) continue;

                std::unordered_set<TransitionBlock*> trimmed = full_region;
                std::unordered_set<TransitionBlock*> dominated_by_exit =
                    compute_dominated_subset(cfg, possible_exit, full_region);
                for (TransitionBlock* node : dominated_by_exit) {
                    trimmed.erase(node);
                }

                TransitionBlock* trimmed_exit = nullptr;
                if (is_restructurable_region(cfg, header, trimmed, &trimmed_exit) &&
                    trimmed.size() < candidate_region.size()) {
                    candidate_region = std::move(trimmed);
                    candidate_exit = trimmed_exit;
                }
            }

            if (best_region.empty() || candidate_region.size() < best_region.size()) {
                best_region = std::move(candidate_region);
                best_header = header;
                best_exit = candidate_exit;
            }
        }

        if (!best_region.empty()) {
            restructure_region(&cfg, best_header, best_region);
            
            TransitionBlock* collapsed_block = arena_.create<TransitionBlock>(best_header->ast_node());
            
            std::vector<TransitionBlock*> incoming_preds;
            for (auto* p : best_header->predecessors_blocks()) {
                if (!best_region.contains(p)) incoming_preds.push_back(p);
            }
            for (auto* p : incoming_preds) {
                cfg.remove_edge_between(p, best_header);
                cfg.add_edge(p, collapsed_block, logos::LogicCondition(task_.z3_ctx().bool_val(true)));
            }
            
            if (best_exit) {
                cfg.add_edge(collapsed_block, best_exit, logos::LogicCondition(task_.z3_ctx().bool_val(true)));
                
                for (auto* node : best_region) {
                    cfg.remove_edge_between(node, best_exit);
                }
            }
            
            bool was_entry = (cfg.entry() == best_header);
            for (auto* node : best_region) {
                cfg.remove_block(node);
            }
            cfg.add_block(collapsed_block);
            if (was_entry) {
                cfg.set_entry(collapsed_block);
            }

            reduced = true;
        } else {
            // Force collapse everything remaining into a single sequential
            // block. Before calling restructure_region (which requires a DAG
            // for topological sort), strip any remaining back/retreating
            // edges to break cycles. Without this, Kahn's algorithm in
            // ReachingConditions::compute() silently drops cyclic nodes,
            // causing CBR to fail to classify them and produce empty ASTs.

            cfg.refresh_edge_properties();
            {
                std::vector<TransitionEdge*> back_edges;
                for (auto* b : cfg.blocks()) {
                    if (!b) continue;
                    for (auto* e : b->successors()) {
                        if (!e) continue;
                        if (e->property() == EdgeProperty::Back ||
                            e->property() == EdgeProperty::Retreating) {
                            back_edges.push_back(e);
                        }
                    }
                }
                for (auto* e : back_edges) {
                    cfg.remove_edge(e);
                }
            }

            std::unordered_set<TransitionBlock*> all_blocks;
            for (auto* b : cfg.blocks()) all_blocks.insert(b);
            
            if (!cfg.entry() || all_blocks.empty()) break;
            restructure_region(&cfg, cfg.entry(), all_blocks);
            
            TransitionBlock* collapsed_block = arena_.create<TransitionBlock>(cfg.entry()->ast_node());
            for (auto* node : all_blocks) cfg.remove_block(node);
            cfg.add_block(collapsed_block);
            cfg.set_entry(collapsed_block);
            
            break;
        }
    }
}

void AcyclicRegionRestructurer::restructure_region(TransitionCFG* t_cfg, TransitionBlock* header, const std::unordered_set<TransitionBlock*>& region) {
    TransitionCFG* slice = GraphSlice::compute_graph_slice_for_region(arena_, t_cfg, header, region, false);

    z3::context& ctx = task_.z3_ctx();
    auto reaching_conditions = ReachingConditions::compute(ctx, slice, header, t_cfg);

    ReachabilityGraph reachability(slice);
    SiblingReachability sibling_reachability(reachability);
    std::vector<TransitionBlock*> ordered_blocks = sibling_reachability.order_blocks(slice->blocks());

    SeqNode* seq = arena_.create<SeqNode>();
    for (TransitionBlock* block : ordered_blocks) {
        seq->add_node(block->ast_node());
    }

    // 4. Condition-Based Refinement (CBR) - De Morgan's Law Application & Branch Synthesis
    AstNode* cbr_root = AstProcessor::preprocess_acyclic(arena_, seq);
    if (!env_flag_enabled("ALETHEIA_DISABLE_CBR")) {
        cbr_root = ConditionBasedRefinement::refine(arena_, ctx, cbr_root, reaching_conditions);
    }

    // 5. Condition-Aware Refinement (CAR) - Switch Statement Extraction
    AstNode* car_root = cbr_root;
    if (!env_flag_enabled("ALETHEIA_DISABLE_CAR")) {
        car_root = ConditionAwareRefinement::refine(arena_, ctx, cbr_root, reaching_conditions);
    }

    car_root = AstProcessor::postprocess_acyclic(arena_, car_root);

    if (header) {
        header->set_ast_node(car_root);
    }
}

} // namespace aletheia
