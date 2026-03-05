#pragma once
#include "../../common/arena_allocated.hpp"
#include "../structures/cfg.hpp"
#include <algorithm>
#include <vector>
#include <memory>

namespace aletheia {

// Forward declarations for recursive type queries
class CodeNode;
class SeqNode;
class IfNode;
class LoopNode;
class WhileLoopNode;
class DoWhileLoopNode;
class ForLoopNode;
class SwitchNode;
class CaseNode;

// =============================================================================
// LoopType enum -- matches the Python reference's LoopType
// =============================================================================

enum class LoopType {
    While,
    DoWhile,
    For
};

// =============================================================================
// AstKind -- LLVM-style RTTI tag for AstNode hierarchy
// =============================================================================
// O(1) integer comparison replaces dynamic_cast's O(N) string matching.

enum class AstKind : std::uint8_t {
    CodeNode,
    ExprAstNode,
    SeqNode,
    IfNode,
    WhileLoopNode,
    DoWhileLoopNode,
    ForLoopNode,
    CaseNode,
    SwitchNode,
    BreakNode,
    ContinueNode,
};

// =============================================================================
// AstNode -- Abstract base for all AST nodes
// =============================================================================
// Enhanced with property query methods matching the Python reference's
// AbstractSyntaxTreeNode. These are needed by loop structuring rules,
// the LoopStructurer, and the LoopProcessor.

class AstNode : public ArenaAllocated {
public:
    virtual ~AstNode() = default;
    virtual BasicBlock* get_original_block() const { return nullptr; }

    /// LLVM-style RTTI tag.
    AstKind ast_kind() const { return ast_kind_; }

protected:
    AstKind ast_kind_; // Set by each concrete constructor

public:
    // ---- Property queries (match Python reference) ----

    /// Is this node a LoopNode with condition == true (i.e., while(true))?
    bool is_endless_loop() const;

    /// Is this node a CodeNode whose only instruction is Break?
    bool is_break_node() const;

    /// Is this a ConditionNode (IfNode) with one branch (true only) whose
    /// child is a break-only CodeNode?
    bool is_break_condition() const;

    /// Is this a ConditionNode with exactly one branch (no false/else)?
    bool is_single_branch() const;

    /// Does this subtree end with break on all paths?
    virtual bool does_end_with_break() const;

    /// Does any descendant CodeNode end with break?
    virtual bool does_contain_break() const;

    /// Does this subtree end with continue on all paths?
    virtual bool does_end_with_continue() const;

    /// Does this subtree end with return on all paths?
    virtual bool does_end_with_return() const;

    /// Is this a CodeNode that ends with break?
    bool is_code_node_ending_with_break() const;

    /// Is this a CodeNode that ends with continue?
    bool is_code_node_ending_with_continue() const;

    // ---- End-node and descendant queries ----

    /// Yields all leaf nodes where execution terminates in this subtree.
    virtual void get_end_nodes(std::vector<AstNode*>& out);

    /// Yields all CodeNodes that end with break or continue, which would
    /// interrupt an ancestor loop. LoopNode overrides to yield nothing
    /// (breaks/continues inside it don't escape).
    virtual void get_descendant_code_nodes_interrupting_ancestor_loop(
        std::vector<CodeNode*>& out);

    /// Check whether any descendant code-node ends with a break that
    /// would interrupt the parent loop.
    bool has_descendant_code_node_breaking_ancestor_loop();
};

// =============================================================================
// CodeNode -- Leaf node wrapping a BasicBlock
// =============================================================================

class CodeNode : public AstNode {
public:
    explicit CodeNode(BasicBlock* block) : block_(block) { ast_kind_ = AstKind::CodeNode; }
    BasicBlock* block() const { return block_; }
    BasicBlock* get_original_block() const override { return block_; }

    bool does_end_with_break() const override;
    bool does_contain_break() const override;
    bool does_end_with_continue() const override;
    bool does_end_with_return() const override;

    void get_end_nodes(std::vector<AstNode*>& out) override { out.push_back(this); }

    void get_descendant_code_nodes_interrupting_ancestor_loop(
        std::vector<CodeNode*>& out) override;

    /// Remove the last instruction if it is a Break/Continue/Return,
    /// and remove any instructions after the first such instruction.
    void clean();

    /// Remove the last instruction unconditionally.
    void remove_last_instruction();

private:
    BasicBlock* block_;
};

// =============================================================================
// ExprAstNode -- Wraps an Expression* for use in AST conditions
// =============================================================================

class ExprAstNode : public AstNode {
public:
    explicit ExprAstNode(Expression* expr) : expr_(expr) { ast_kind_ = AstKind::ExprAstNode; }
    Expression* expr() const { return expr_; }
private:
    Expression* expr_;
};

// =============================================================================
// SeqNode -- Sequence of child nodes (executed in order)
// =============================================================================

class SeqNode : public AstNode {
public:
    SeqNode() { ast_kind_ = AstKind::SeqNode; }
    void add_node(AstNode* node) { nodes_.push_back(node); }
    const std::vector<AstNode*>& nodes() const { return nodes_; }
    std::vector<AstNode*>& mutable_nodes() { return nodes_; }

    bool empty() const { return nodes_.empty(); }
    std::size_t size() const { return nodes_.size(); }
    AstNode* first() const { return nodes_.empty() ? nullptr : nodes_.front(); }
    AstNode* last() const { return nodes_.empty() ? nullptr : nodes_.back(); }

    void remove_node(AstNode* node) {
        nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
    }

    bool does_end_with_break() const override;
    bool does_contain_break() const override;

    void get_end_nodes(std::vector<AstNode*>& out) override;

    void get_descendant_code_nodes_interrupting_ancestor_loop(
        std::vector<CodeNode*>& out) override;

private:
    std::vector<AstNode*> nodes_;
};

// =============================================================================
// IfNode -- Conditional branch (if/else)
// =============================================================================
// Maps to the Python reference's ConditionNode. The condition is stored
// as an ExprAstNode wrapping a Condition* (or any boolean Expression*).
// true_branch and false_branch are the two arms. false_branch may be null
// for single-branch conditions.

class IfNode : public AstNode {
public:
    IfNode(AstNode* cond, AstNode* true_branch, AstNode* false_branch = nullptr)
        : cond_(cond), true_branch_(true_branch), false_branch_(false_branch) { ast_kind_ = AstKind::IfNode; }

    AstNode* cond() const { return cond_; }
    AstNode* true_branch() const { return true_branch_; }
    AstNode* false_branch() const { return false_branch_; }

    void set_cond(AstNode* c) { cond_ = c; }
    void set_true_branch(AstNode* b) { true_branch_ = b; }
    void set_false_branch(AstNode* b) { false_branch_ = b; }

    /// Swap true and false branches.
    void switch_branches() { std::swap(true_branch_, false_branch_); }

    /// Get the condition Expression (unwraps ExprAstNode if present).
    Expression* condition_expr() const {
        if (cond_ && cond_->ast_kind() == AstKind::ExprAstNode)
            return static_cast<ExprAstNode*>(cond_)->expr();
        return nullptr;
    }

    bool does_end_with_break() const override;
    bool does_contain_break() const override;

    void get_end_nodes(std::vector<AstNode*>& out) override;

    void get_descendant_code_nodes_interrupting_ancestor_loop(
        std::vector<CodeNode*>& out) override;

private:
    AstNode* cond_;
    AstNode* true_branch_;
    AstNode* false_branch_;
};

// =============================================================================
// LoopNode -- Base class for all loop types (while, do-while, for)
// =============================================================================
// Enhanced from the skeleton's flat struct: now carries a condition expression,
// a loop type discriminator, and a mutable body pointer. Endless loops have
// condition_ == nullptr (representing condition=True).

class LoopNode : public AstNode {
public:
    LoopNode(AstNode* body, Expression* condition = nullptr)
        : body_(body), condition_(condition) {}

    AstNode* body() const { return body_; }
    void set_body(AstNode* b) { body_ = b; }

    Expression* condition() const { return condition_; }
    void set_condition(Expression* c) { condition_ = c; }

    /// Is this an endless loop (condition is null or always true)?
    bool is_endless() const { return condition_ == nullptr; }

    /// Subclasses return their specific loop type.
    virtual LoopType loop_type() const = 0;

    // Loop nodes are end-nodes themselves (we don't know if the loop
    // body executes, so the loop itself is where execution may terminate).
    bool does_end_with_break() const override { return false; }
    bool does_contain_break() const override { return false; }
    bool does_end_with_continue() const override { return false; }
    bool does_end_with_return() const override { return false; }

    void get_end_nodes(std::vector<AstNode*>& out) override {
        out.push_back(this);
    }

    // Breaks/continues inside a loop don't escape to the parent loop.
    void get_descendant_code_nodes_interrupting_ancestor_loop(
        std::vector<CodeNode*>& /*out*/) override {
        // Yield nothing -- all breaks/continues inside this loop are local to it.
    }

protected:
    AstNode* body_;
    Expression* condition_;
};

// =============================================================================
// WhileLoopNode -- while (condition) { body }
// =============================================================================

class WhileLoopNode : public LoopNode {
public:
    /// Endless while(true) loop.
    explicit WhileLoopNode(AstNode* body)
        : LoopNode(body, nullptr) { ast_kind_ = AstKind::WhileLoopNode; }

    /// While loop with a condition.
    WhileLoopNode(AstNode* body, Expression* condition)
        : LoopNode(body, condition) { ast_kind_ = AstKind::WhileLoopNode; }

    LoopType loop_type() const override { return LoopType::While; }
};

// =============================================================================
// DoWhileLoopNode -- do { body } while (condition)
// =============================================================================

class DoWhileLoopNode : public LoopNode {
public:
    DoWhileLoopNode(AstNode* body, Expression* condition)
        : LoopNode(body, condition) { ast_kind_ = AstKind::DoWhileLoopNode; }

    LoopType loop_type() const override { return LoopType::DoWhile; }
};

// =============================================================================
// ForLoopNode -- for (declaration; condition; modification) { body }
// =============================================================================

class ForLoopNode : public LoopNode {
public:
    ForLoopNode(AstNode* body, Expression* condition,
                Instruction* declaration, Instruction* modification)
        : LoopNode(body, condition),
          declaration_(declaration), modification_(modification) { ast_kind_ = AstKind::ForLoopNode; }

    Instruction* declaration() const { return declaration_; }
    Instruction* modification() const { return modification_; }

    LoopType loop_type() const override { return LoopType::For; }

private:
    Instruction* declaration_;
    Instruction* modification_;
};

// =============================================================================
// CaseNode, SwitchNode, BreakNode, ContinueNode
// =============================================================================

class CaseNode : public AstNode {
public:
    CaseNode(std::uint64_t val, AstNode* body, bool is_default = false, bool break_case = true)
        : value_(val), body_(body), is_default_(is_default), break_case_(break_case) { ast_kind_ = AstKind::CaseNode; }

    std::uint64_t value() const { return value_; }
    AstNode* body() const { return body_; }
    bool is_default() const { return is_default_; }
    bool break_case() const { return break_case_; }

    void set_body(AstNode* body) { body_ = body; }
    void set_break_case(bool value) { break_case_ = value; }

private:
    std::uint64_t value_;
    AstNode* body_;
    bool is_default_;
    bool break_case_;
};

class SwitchNode : public AstNode {
public:
    explicit SwitchNode(AstNode* cond) : cond_(cond) { ast_kind_ = AstKind::SwitchNode; }
    void add_case(CaseNode* c) { cases_.push_back(c); }
    AstNode* cond() const { return cond_; }
    const std::vector<CaseNode*>& cases() const { return cases_; }
    std::vector<CaseNode*>& mutable_cases() { return cases_; }

    bool does_end_with_break() const override { return false; }
    bool does_contain_break() const override { return false; }

    void get_end_nodes(std::vector<AstNode*>& out) override {
        out.push_back(this);
    }

    void get_descendant_code_nodes_interrupting_ancestor_loop(
        std::vector<CodeNode*>& /*out*/) override {
        // Switch breaks are local to the switch, not the parent loop.
    }

private:
    AstNode* cond_;
    std::vector<CaseNode*> cases_;
};

class BreakNode : public AstNode {
public:
    BreakNode() { ast_kind_ = AstKind::BreakNode; }
};
class ContinueNode : public AstNode {
public:
    ContinueNode() { ast_kind_ = AstKind::ContinueNode; }
};

// =============================================================================
// AbstractSyntaxForest
// =============================================================================

class AbstractSyntaxForest {
public:
    AbstractSyntaxForest() = default;

    void set_root(AstNode* root) { root_ = root; }
    AstNode* root() const { return root_; }

private:
    AstNode* root_ = nullptr;
};

// =============================================================================
// LLVM-style RTTI helpers for AstNode hierarchy
// =============================================================================
// isa<T>(ptr) -- returns bool, O(1) integer comparison
// cast<T>(ptr) -- asserts and returns T*, undefined behavior if wrong type
// dyn_cast<T>(ptr) -- returns T* or nullptr (replaces dynamic_cast)

template <typename T> bool ast_isa(const AstNode* n);

template <> inline bool ast_isa<CodeNode>(const AstNode* n)         { return n && n->ast_kind() == AstKind::CodeNode; }
template <> inline bool ast_isa<ExprAstNode>(const AstNode* n)      { return n && n->ast_kind() == AstKind::ExprAstNode; }
template <> inline bool ast_isa<SeqNode>(const AstNode* n)          { return n && n->ast_kind() == AstKind::SeqNode; }
template <> inline bool ast_isa<IfNode>(const AstNode* n)           { return n && n->ast_kind() == AstKind::IfNode; }
template <> inline bool ast_isa<WhileLoopNode>(const AstNode* n)    { return n && n->ast_kind() == AstKind::WhileLoopNode; }
template <> inline bool ast_isa<DoWhileLoopNode>(const AstNode* n)  { return n && n->ast_kind() == AstKind::DoWhileLoopNode; }
template <> inline bool ast_isa<ForLoopNode>(const AstNode* n)      { return n && n->ast_kind() == AstKind::ForLoopNode; }
template <> inline bool ast_isa<CaseNode>(const AstNode* n)         { return n && n->ast_kind() == AstKind::CaseNode; }
template <> inline bool ast_isa<SwitchNode>(const AstNode* n)       { return n && n->ast_kind() == AstKind::SwitchNode; }
template <> inline bool ast_isa<BreakNode>(const AstNode* n)        { return n && n->ast_kind() == AstKind::BreakNode; }
template <> inline bool ast_isa<ContinueNode>(const AstNode* n)     { return n && n->ast_kind() == AstKind::ContinueNode; }

// LoopNode matches any of the three loop subtypes
template <> inline bool ast_isa<LoopNode>(const AstNode* n) {
    return n && (n->ast_kind() == AstKind::WhileLoopNode ||
                 n->ast_kind() == AstKind::DoWhileLoopNode ||
                 n->ast_kind() == AstKind::ForLoopNode);
}

template <typename T>
T* ast_cast(AstNode* n) {
    return static_cast<T*>(n);
}

template <typename T>
const T* ast_cast(const AstNode* n) {
    return static_cast<const T*>(n);
}

template <typename T>
T* ast_dyn_cast(AstNode* n) {
    return ast_isa<T>(n) ? static_cast<T*>(n) : nullptr;
}

template <typename T>
const T* ast_dyn_cast(const AstNode* n) {
    return ast_isa<T>(n) ? static_cast<const T*>(n) : nullptr;
}

} // namespace aletheia
