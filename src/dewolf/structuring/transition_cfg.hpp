#pragma once
#include "../../common/arena_allocated.hpp"
#include "ast.hpp"
#include <vector>
#include <algorithm>

namespace dewolf {

class TransitionBlock : public ArenaAllocated {
public:
    explicit TransitionBlock(AstNode* ast_node) : ast_node_(ast_node) {}

    AstNode* ast_node() const { return ast_node_; }
    void set_ast_node(AstNode* node) { ast_node_ = node; }

    void add_predecessor(TransitionBlock* b) { predecessors_.push_back(b); }
    void add_successor(TransitionBlock* b) { successors_.push_back(b); }

    void remove_predecessor(TransitionBlock* b) {
        predecessors_.erase(std::remove(predecessors_.begin(), predecessors_.end(), b), predecessors_.end());
    }

    void remove_successor(TransitionBlock* b) {
        successors_.erase(std::remove(successors_.begin(), successors_.end(), b), successors_.end());
    }

    const std::vector<TransitionBlock*>& predecessors() const { return predecessors_; }
    const std::vector<TransitionBlock*>& successors() const { return successors_; }

private:
    AstNode* ast_node_;
    std::vector<TransitionBlock*> predecessors_;
    std::vector<TransitionBlock*> successors_;
};

class TransitionCFG : public ArenaAllocated {
public:
    TransitionCFG() = default;

    void set_entry(TransitionBlock* entry) { entry_ = entry; }
    TransitionBlock* entry() const { return entry_; }

    void add_block(TransitionBlock* block) { blocks_.push_back(block); }
    
    void remove_block(TransitionBlock* block) {
        blocks_.erase(std::remove(blocks_.begin(), blocks_.end(), block), blocks_.end());
        if (entry_ == block) entry_ = nullptr;
    }

    const std::vector<TransitionBlock*>& blocks() const { return blocks_; }

private:
    TransitionBlock* entry_ = nullptr;
    std::vector<TransitionBlock*> blocks_;
};

} // namespace dewolf
