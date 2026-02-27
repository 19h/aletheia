#include "structurer.hpp"
#include "loop_structurer.hpp"
#include "../ssa/dominators.hpp"
#include "../../dewolf_logic/z3_logic.hpp"
#include "graph_slice/graph_slice.hpp"
#include "reaching_conditions/reaching_conditions.hpp"
#include "cbr/cbr.hpp"
#include "car/car.hpp"
#include <unordered_set>
#include <string>

namespace dewolf {

void CyclicRegionFinder::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;

    cfg.refresh_edge_properties();
    auto loop_heads = cfg.get_loop_heads();

    while (!loop_heads.empty()) {
        TransitionBlock* head = loop_heads.back();
        loop_heads.pop_back();

        size_t initial_nodes = cfg.blocks().size();

        // 1. Compute initial loop nodes (GraphSlice from head to latching nodes)
        std::vector<TransitionBlock*> latching_nodes;
        for (auto* e : head->predecessors()) {
            if (e->property() == EdgeProperty::Back || e->property() == EdgeProperty::Retreating) {
                latching_nodes.push_back(e->source());
            }
        }
        
        TransitionCFG* loop_region = GraphSlice::compute_graph_slice_for_sink_nodes(arena_, &cfg, head, latching_nodes, false);
        std::unordered_set<TransitionBlock*> region_set(loop_region->blocks().begin(), loop_region->blocks().end());

        // 2. Restructure Abnormal Entry (TODO)
        // If there's a retreating edge into head
        bool has_retreating = false;
        for (auto* e : head->predecessors()) {
            if (e->property() == EdgeProperty::Retreating) has_retreating = true;
        }
        if (has_retreating) {
            std::unordered_map<TransitionBlock*, std::vector<TransitionEdge*>> entry_edges;
            for (auto* node : region_set) {
                for (auto* e : node->predecessors()) {
                    if (!region_set.contains(e->source())) {
                        entry_edges[node].push_back(e);
                    }
                }
            }

            size_t num_entries = entry_edges.size();
            std::string var_name = "entry_" + std::to_string(reinterpret_cast<uintptr_t>(head));
            auto* entry_var = arena_.create<Variable>(var_name, 4); entry_var->set_ir_type(Integer::int32_t());

            std::vector<std::pair<TransitionBlock*, dewolf_logic::LogicCondition>> condition_nodes;
            for (size_t i = 0; i < num_entries - 1; ++i) {
                auto* cond_op = arena_.create<Condition>(OperationType::eq, entry_var, arena_.create<Constant>(i, 4));
                auto* branch = arena_.create<Branch>(cond_op);
                auto* bb = arena_.create<BasicBlock>(9000 + i);
                bb->add_instruction(branch);
                auto* tb = arena_.create<TransitionBlock>(arena_.create<CodeNode>(bb));
                cfg.add_block(tb);
                loop_region->add_block(tb);
                
                dewolf_logic::Z3Converter z3_conv(task_.z3_ctx());
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
                cfg.add_edge(cn, new_head, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
            }
            for (size_t i = 0; i + 1 < condition_nodes.size(); ++i) {
                cfg.add_edge(condition_nodes[i].first, condition_nodes[i+1].first, condition_nodes[i].second.negate(), EdgeProperty::NonLoop);
            }

            // Redirect loop edges (back/retreating) to new_head
            std::vector<TransitionEdge*> loop_edges;
            for (auto* e : head->predecessors()) {
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

            std::vector<TransitionBlock*> sorted_entries = {head};
            for (auto& pair : entry_edges) {
                if (pair.first != head) sorted_entries.push_back(pair.first);
            }

            std::vector<std::pair<TransitionBlock*, dewolf_logic::LogicCondition>> ext_conds = condition_nodes;
            if (!condition_nodes.empty()) {
                ext_conds.push_back({condition_nodes.back().first, condition_nodes.back().second.negate()});
            } else {
                // If there's only 1 entry (head), but it had a retreating edge, num_entries = 1, condition_nodes = 0
                // Wait, if num_entries=1, new_head=head, no conditions. We still redirect the entry edges to code_nodes[0] -> head.
            }

            for (size_t i = 0; i < sorted_entries.size(); ++i) {
                auto* tb = sorted_entries[i];
                auto* cn = code_nodes[i];
                if (i < ext_conds.size()) {
                    cfg.add_edge(ext_conds[i].first, tb, ext_conds[i].second, EdgeProperty::NonLoop);
                }
                for (auto* e : entry_edges[tb]) {
                    auto* src = e->source();
                    auto tag = e->tag();
                    auto prop = e->property();
                    cfg.remove_edge(e);
                    cfg.add_edge(src, cn, tag, prop);
                }
            }

            for (size_t i = 1; i < sorted_entries.size(); ++i) {
                auto* tb = sorted_entries[i];
                auto* assign = arena_.create<Assignment>(entry_var, arena_.create<Constant>(0, 4));
                
                auto* ast = tb->ast_node();
                if (auto* code_node = dynamic_cast<CodeNode*>(ast)) {
                    code_node->block()->add_instruction(assign);
                } else if (auto* seq_node = dynamic_cast<SeqNode*>(ast)) {
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
            for(auto& p : condition_nodes) region_set.insert(p.first);
        }


        // 3. Compute Loop Successors
        std::vector<TransitionBlock*> loop_successors;
        std::unordered_set<TransitionBlock*> succ_set;
        for (auto* node : loop_region->blocks()) {
            for (auto* succ : node->successors_blocks()) {
                if (!region_set.contains(succ) && !succ_set.contains(succ)) {
                    loop_successors.push_back(succ);
                    succ_set.insert(succ);
                }
            }
        }

        // 4. Restructure Abnormal Exit (TODO)
        if (loop_successors.size() > 1) {
            size_t num_exits = loop_successors.size();
            std::string var_name = "exit_" + std::to_string(reinterpret_cast<uintptr_t>(head));
            auto* exit_var = arena_.create<Variable>(var_name, 4); exit_var->set_ir_type(Integer::int32_t());

            std::vector<std::pair<TransitionBlock*, dewolf_logic::LogicCondition>> condition_nodes;
            for (size_t i = 0; i < num_exits - 1; ++i) {
                auto* cond_op = arena_.create<Condition>(OperationType::eq, exit_var, arena_.create<Constant>(i, 4));
                auto* branch = arena_.create<Branch>(cond_op);
                auto* bb = arena_.create<BasicBlock>(9300 + i);
                bb->add_instruction(branch);
                auto* tb = arena_.create<TransitionBlock>(arena_.create<CodeNode>(bb));
                cfg.add_block(tb);
                
                dewolf_logic::Z3Converter z3_conv(task_.z3_ctx());
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
                cfg.add_edge(cn, new_successor, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
            }
            for (size_t i = 0; i + 1 < condition_nodes.size(); ++i) {
                cfg.add_edge(condition_nodes[i].first, condition_nodes[i+1].first, condition_nodes[i].second.negate(), EdgeProperty::NonLoop);
            }

            std::vector<TransitionBlock*> sorted_successors = loop_successors; // python topological sorts them

            std::vector<std::pair<TransitionBlock*, dewolf_logic::LogicCondition>> ext_conds = condition_nodes;
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
        for (auto* node : loop_region->blocks()) {
            auto* clone = arena_.create<TransitionBlock>(node->ast_node());
            block_map[node] = clone;
            loop_cfg->add_block(clone);
        }
        loop_cfg->set_entry(block_map[head]);

        for (auto* node : loop_region->blocks()) {
            auto* clone_src = block_map[node];
            for (auto* e : node->successors()) {
                auto* succ = e->sink();
                if (succ == head && node != head && region_set.contains(node)) {
                    // Back edge -> Continue node
                    auto* cont_block = arena_.create<BasicBlock>(9999);
                    cont_block->add_instruction(arena_.create<ContinueInstr>());
                    auto* cont_trans = arena_.create<TransitionBlock>(arena_.create<CodeNode>(cont_block));
                    loop_cfg->add_block(cont_trans);
                    loop_cfg->add_edge(clone_src, cont_trans, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
                } else if (!region_set.contains(succ)) {
                    // Exit edge -> Break node
                    auto* brk_block = arena_.create<BasicBlock>(9998);
                    brk_block->add_instruction(arena_.create<BreakInstr>());
                    auto* brk_trans = arena_.create<TransitionBlock>(arena_.create<CodeNode>(brk_block));
                    loop_cfg->add_block(brk_trans);
                    loop_cfg->add_edge(clone_src, brk_trans, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
                } else if (block_map.contains(succ)) {
                    auto* clone_dst = block_map[succ];
                    loop_cfg->add_edge(clone_src, clone_dst, e->tag(), e->property());
                }
            }
        }

        AcyclicRegionRestructurer acyclic(task_);
        acyclic.process(*loop_cfg);

        AstNode* loop_body_ast = loop_cfg->entry()->ast_node();
        auto* endless_loop = arena_.create<WhileLoopNode>(loop_body_ast);
        AstNode* refined = LoopStructurer::refine_loop(arena_, endless_loop);
        head->set_ast_node(refined);

        for (auto* succ : loop_successors) {
            for (auto* node : loop_region->blocks()) {
                if (node != head) {
                    cfg.remove_edge_between(node, succ);
                }
            }
            bool linked = false;
            for (auto* s : head->successors_blocks()) if (s == succ) { linked = true; break; }
            if (!linked) cfg.add_edge(head, succ, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)));
        }

        for (auto* node : loop_region->blocks()) {
            if (node == head) continue;
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
    while (reduced && cfg.blocks().size() > 1) {
        reduced = false;

        std::unordered_set<TransitionBlock*> best_region;
        TransitionBlock* best_header = nullptr;
        TransitionBlock* best_exit = nullptr;

        for (TransitionBlock* header : cfg.blocks()) {
            std::unordered_set<TransitionBlock*> region;
            std::unordered_set<TransitionBlock*> visited;
            std::vector<TransitionBlock*> stack = {header};
            
            while (!stack.empty()) {
                TransitionBlock* curr = stack.back();
                stack.pop_back();
                
                if (visited.contains(curr)) continue;
                visited.insert(curr);
                
                bool all_preds_in = true;
                if (curr != header) {
                    for (auto* p : curr->predecessors_blocks()) {
                        if (!region.contains(p)) {
                            all_preds_in = false;
                            break;
                        }
                    }
                }
                
                if (all_preds_in) {
                    region.insert(curr);
                    for (auto* s : curr->successors_blocks()) {
                        stack.push_back(s);
                    }
                }
            }
            
            if (region.size() <= 1) continue;

            std::unordered_set<TransitionBlock*> exits;
            for (auto* node : region) {
                for (auto* s : node->successors_blocks()) {
                    if (!region.contains(s)) {
                        exits.insert(s);
                    }
                }
            }
            
            if (exits.size() <= 1) {
                if (best_region.empty() || region.size() < best_region.size()) {
                    best_region = region;
                    best_header = header;
                    best_exit = exits.empty() ? nullptr : *exits.begin();
                }
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
                cfg.add_edge(p, collapsed_block, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)));
            }
            
            if (best_exit) {
                cfg.add_edge(collapsed_block, best_exit, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)));
                
                for (auto* node : best_region) {
                    cfg.remove_edge_between(node, best_exit);
                }
            }
            
            for (auto* node : best_region) {
                cfg.remove_block(node);
            }
            cfg.add_block(collapsed_block);
            
            if (cfg.entry() == best_header) {
                cfg.set_entry(collapsed_block);
            }

            reduced = true;
        } else {
            // Force collapse everything remaining into a single sequential block to avoid truncation when there are unstructured jumps
            // But we must call restructure_region on this final set to process Conditions correctly!
            
            std::unordered_set<TransitionBlock*> all_blocks;
            for (auto* b : cfg.blocks()) all_blocks.insert(b);
            
            restructure_region(&cfg, cfg.entry(), all_blocks);
            
            // At this point, the root of cfg.entry() has the fully nested AST!
            break;
        }
    }
}

void AcyclicRegionRestructurer::restructure_region(TransitionCFG* t_cfg, TransitionBlock* header, const std::unordered_set<TransitionBlock*>& region) {
    TransitionCFG* slice = GraphSlice::compute_graph_slice_for_region(arena_, t_cfg, header, region, false);

    z3::context& ctx = task_.z3_ctx();
    auto reaching_conditions = ReachingConditions::compute(ctx, slice, header, t_cfg);

    SeqNode* seq = arena_.create<SeqNode>();
    for (TransitionBlock* block : slice->blocks()) {
        seq->add_node(block->ast_node());
    }

    // 4. Condition-Based Refinement (CBR) - De Morgan's Law Application & Branch Synthesis
    AstNode* cbr_root = ConditionBasedRefinement::refine(arena_, ctx, seq, reaching_conditions);

    // 5. Condition-Aware Refinement (CAR) - Switch Statement Extraction
    AstNode* car_root = ConditionAwareRefinement::refine(arena_, ctx, cbr_root, reaching_conditions);

    header->set_ast_node(car_root);
}

} // namespace dewolf
