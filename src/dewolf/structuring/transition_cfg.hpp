#pragma once
#include "../../common/arena_allocated.hpp"
#include "ast.hpp"
#include "../../dewolf_logic/z3_logic.hpp"
#include "../structures/cfg.hpp" // For EdgeProperty
#include <vector>
#include <algorithm>
#include <optional>

namespace dewolf {

class TransitionBlock;

class TransitionEdge : public ArenaAllocated {
public:
    TransitionEdge(TransitionBlock* source, TransitionBlock* sink, dewolf_logic::LogicCondition tag, std::optional<EdgeProperty> property = std::nullopt)
        : source_(source), sink_(sink), tag_(tag), property_(property) {}

    TransitionBlock* source() const { return source_; }
    TransitionBlock* sink() const { return sink_; }
    dewolf_logic::LogicCondition tag() const { return tag_; }
    std::optional<EdgeProperty> property() const { return property_; }

    void set_source(TransitionBlock* b) { source_ = b; }
    void set_sink(TransitionBlock* b) { sink_ = b; }
    void set_tag(dewolf_logic::LogicCondition t) { tag_ = t; }
    void set_property(EdgeProperty p) { property_ = p; }

private:
    TransitionBlock* source_;
    TransitionBlock* sink_;
    dewolf_logic::LogicCondition tag_;
    std::optional<EdgeProperty> property_;
};

class TransitionBlock : public ArenaAllocated {
public:
    explicit TransitionBlock(AstNode* ast_node) : ast_node_(ast_node) {}

    AstNode* ast_node() const { return ast_node_; }
    void set_ast_node(AstNode* node) { ast_node_ = node; }

    void add_predecessor_edge(TransitionEdge* e) { predecessors_.push_back(e); }
    void add_successor_edge(TransitionEdge* e) { successors_.push_back(e); }

    void remove_predecessor_edge(TransitionEdge* e) {
        predecessors_.erase(std::remove(predecessors_.begin(), predecessors_.end(), e), predecessors_.end());
    }

    void remove_successor_edge(TransitionEdge* e) {
        successors_.erase(std::remove(successors_.begin(), successors_.end(), e), successors_.end());
    }

    const std::vector<TransitionEdge*>& predecessors() const { return predecessors_; }
    const std::vector<TransitionEdge*>& successors() const { return successors_; }

    // Compatibility methods
    std::vector<TransitionBlock*> predecessors_blocks() const {
        std::vector<TransitionBlock*> res;
        for (auto* e : predecessors_) res.push_back(e->source());
        return res;
    }

    std::vector<TransitionBlock*> successors_blocks() const {
        std::vector<TransitionBlock*> res;
        for (auto* e : successors_) res.push_back(e->sink());
        return res;
    }

    // These modify the underlying CFG, but we don't have edge context here!
    // They are used in places where we just add/remove block links without edge tags.
    // We should probably NOT have them and update callers to use TransitionCFG::add_edge.
    // However, to keep it building for now we can create synthetic edges.
    // Wait, TransitionBlock doesn't have an arena to create new edges.
    // Let's remove add_successor_block and force using CFG methods!

private:
    AstNode* ast_node_;
    std::vector<TransitionEdge*> predecessors_;
    std::vector<TransitionEdge*> successors_;
};

class TransitionCFG : public ArenaAllocated {
public:
    TransitionCFG(DecompilerArena& arena) : arena_(arena) {}

    void set_entry(TransitionBlock* entry) { entry_ = entry; }
    TransitionBlock* entry() const { return entry_; }

    void add_block(TransitionBlock* block) { blocks_.push_back(block); }
    
    void remove_block(TransitionBlock* block) {
        blocks_.erase(std::remove(blocks_.begin(), blocks_.end(), block), blocks_.end());
        if (entry_ == block) entry_ = nullptr;
    }

    const std::vector<TransitionBlock*>& blocks() const { return blocks_; }

    // Edge management
    TransitionEdge* add_edge(TransitionBlock* src, TransitionBlock* dst, dewolf_logic::LogicCondition tag, std::optional<EdgeProperty> prop = std::nullopt) {
        TransitionEdge* edge = arena_.create<TransitionEdge>(src, dst, tag, prop);
        src->add_successor_edge(edge);
        dst->add_predecessor_edge(edge);
        return edge;
    }

    
    void remove_edge_between(TransitionBlock* src, TransitionBlock* dst) {
        TransitionEdge* to_remove = nullptr;
        for (auto* e : src->successors()) {
            if (e->sink() == dst) {
                to_remove = e;
                break;
            }
        }
        if (to_remove) remove_edge(to_remove);
    }

    void remove_edge(TransitionEdge* edge) {
        edge->source()->remove_successor_edge(edge);
        edge->sink()->remove_predecessor_edge(edge);
    }

    // Utility methods
    std::vector<TransitionEdge*> get_in_edges(TransitionBlock* block) const {
        return block->predecessors();
    }
    
    std::vector<TransitionEdge*> get_out_edges(TransitionBlock* block) const {
        return block->successors();
    }

private:
    DecompilerArena& arena_;
    TransitionBlock* entry_ = nullptr;
    std::vector<TransitionBlock*> blocks_;
};

} // namespace dewolf
