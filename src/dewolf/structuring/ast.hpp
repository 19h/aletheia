#pragma once
#include "../../common/arena_allocated.hpp"
#include "../structures/cfg.hpp"
#include <vector>
#include <memory>

namespace dewolf {

class AstNode : public ArenaAllocated {
public:
    virtual ~AstNode() = default;
};

class CodeNode : public AstNode {
public:
    explicit CodeNode(BasicBlock* block) : block_(block) {}
    BasicBlock* block() const { return block_; }
private:
    BasicBlock* block_;
};

class SeqNode : public AstNode {
public:
    void add_node(AstNode* node) { nodes_.push_back(node); }
    const std::vector<AstNode*>& nodes() const { return nodes_; }
private:
    std::vector<AstNode*> nodes_;
};

class IfNode : public AstNode {
public:
    IfNode(AstNode* cond, AstNode* true_branch, AstNode* false_branch = nullptr)
        : cond_(cond), true_branch_(true_branch), false_branch_(false_branch) {}
private:
    AstNode* cond_;
    AstNode* true_branch_;
    AstNode* false_branch_;
};

class LoopNode : public AstNode {
public:
    explicit LoopNode(AstNode* body) : body_(body) {}
private:
    AstNode* body_;
};

class CaseNode : public AstNode {
public:
    CaseNode(std::uint64_t val, AstNode* body) : value_(val), body_(body) {}
private:
    std::uint64_t value_;
    AstNode* body_;
};

class SwitchNode : public AstNode {
public:
    explicit SwitchNode(AstNode* cond) : cond_(cond) {}
    void add_case(CaseNode* c) { cases_.push_back(c); }
private:
    AstNode* cond_;
    std::vector<CaseNode*> cases_;
};

class BreakNode : public AstNode {};
class ContinueNode : public AstNode {};

class AbstractSyntaxForest {
public:
    AbstractSyntaxForest() = default;
    
    void set_root(AstNode* root) { root_ = root; }
    AstNode* root() const { return root_; }

private:
    AstNode* root_ = nullptr;
};

} // namespace dewolf
