#pragma once
#include "ast.hpp"
#include "../../common/arena.hpp"

namespace dewolf {

// =============================================================================
// Loop Structuring Rules
// =============================================================================
// Each rule checks whether it can be applied to an endless WhileLoopNode and,
// if so, restructures the loop into the appropriate type (while-with-condition,
// do-while, sequence, etc.).
//
// Port of the Python reference:
//   ref/dewolf/.../loop_structuring_rules.py
//   ref/dewolf/.../loop_structurer.py

/// Abstract base for loop structuring rules.
class LoopStructuringRule {
public:
    virtual ~LoopStructuringRule() = default;

    /// Check whether this rule can be applied to the given loop node.
    virtual bool can_be_applied(LoopNode* loop) const = 0;

    /// Apply the rule, potentially replacing `loop` in the AST.
    /// Returns the new root node that replaces the loop (or the loop itself
    /// if it was modified in place).
    virtual AstNode* restructure(DecompilerArena& arena, LoopNode* loop) = 0;
};

// ---- Rule 1: WhileLoopRule ----
// Converts: while(true) { if(cond) break; ... } -> while(!cond) { ... }
// Also: while(true) { if(cond) break; } -> while(!cond) {}
class WhileLoopRule : public LoopStructuringRule {
public:
    bool can_be_applied(LoopNode* loop) const override;
    AstNode* restructure(DecompilerArena& arena, LoopNode* loop) override;
};

// ---- Rule 2: DoWhileLoopRule ----
// Converts: while(true) { ...; if(cond) break; } -> do { ... } while(!cond)
class DoWhileLoopRule : public LoopStructuringRule {
public:
    bool can_be_applied(LoopNode* loop) const override;
    AstNode* restructure(DecompilerArena& arena, LoopNode* loop) override;
};

// ---- Rule 3: NestedDoWhileLoopRule ----
// Converts: while(true) { ...; if(cond) { X } } where no other child breaks
// -> do { ... } while(!cond); X
class NestedDoWhileLoopRule : public LoopStructuringRule {
public:
    bool can_be_applied(LoopNode* loop) const override;
    AstNode* restructure(DecompilerArena& arena, LoopNode* loop) override;
};

// ---- Rule 4: SequenceRule ----
// Converts: while(true) { ...; break; } (all paths break) -> { ... }
class SequenceRule : public LoopStructuringRule {
public:
    bool can_be_applied(LoopNode* loop) const override;
    AstNode* restructure(DecompilerArena& arena, LoopNode* loop) override;
};

// ---- Rule 5: ConditionToSequenceRule ----
// Converts: while(true) { if(cond) { A } else { B; break; } }
// -> while(cond) { A }; B
class ConditionToSequenceRule : public LoopStructuringRule {
public:
    bool can_be_applied(LoopNode* loop) const override;
    AstNode* restructure(DecompilerArena& arena, LoopNode* loop) override;
};

// =============================================================================
// LoopStructurer -- Orchestrator
// =============================================================================
// Iterates over rules in priority order until no rule matches.
// Port of the Python reference's LoopStructurer class.

class LoopStructurer {
public:
    /// Refine an endless WhileLoopNode into the correct loop type.
    /// Returns the new root node (which may be different from the input).
    static AstNode* refine_loop(DecompilerArena& arena, LoopNode* loop);

private:
    /// Try all rules in order; return the first matching rule, or nullptr.
    static LoopStructuringRule* match_rule(LoopNode* loop);

    // Static rule instances (stateless, safe to share)
    static WhileLoopRule while_rule_;
    static DoWhileLoopRule do_while_rule_;
    static NestedDoWhileLoopRule nested_do_while_rule_;
    static SequenceRule sequence_rule_;
    static ConditionToSequenceRule condition_to_sequence_rule_;
};

} // namespace dewolf
