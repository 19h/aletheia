#include "ast.hpp"

#include <unordered_set>

namespace aletheia {

namespace {

constexpr std::size_t kMaxAstTraversalExpansions = 1'000'000;

std::vector<const AstNode*> structural_children(const AstNode* node) {
    std::vector<const AstNode*> result;
    if (const auto* sequence = ast_dyn_cast<SeqNode>(node)) {
        result.reserve(sequence->nodes().size());
        for (const AstNode* child : sequence->nodes()) {
            if (child) result.push_back(child);
        }
    } else if (const auto* conditional = ast_dyn_cast<IfNode>(node)) {
        if (conditional->true_branch()) {
            result.push_back(conditional->true_branch());
        }
        if (conditional->false_branch()) {
            result.push_back(conditional->false_branch());
        }
    } else if (const auto* loop = ast_dyn_cast<LoopNode>(node)) {
        if (loop->body()) result.push_back(loop->body());
    } else if (const auto* switch_node = ast_dyn_cast<SwitchNode>(node)) {
        result.reserve(switch_node->cases().size());
        for (const CaseNode* case_node : switch_node->cases()) {
            if (case_node) result.push_back(case_node);
        }
    } else if (const auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        if (case_node->body()) result.push_back(case_node->body());
    }
    return result;
}

enum class TerminalKind {
    Break,
    Continue,
    Return,
};

void collect_end_nodes_iterative(
    AstNode* root,
    std::vector<AstNode*>& out) {
    if (!root) return;
    std::unordered_set<AstNode*> visited;
    std::vector<AstNode*> work{root};
    while (!work.empty()) {
        AstNode* node = work.back();
        work.pop_back();
        if (!node || !visited.insert(node).second) continue;
        if (auto* sequence = ast_dyn_cast<SeqNode>(node)) {
            if (sequence->last()) work.push_back(sequence->last());
        } else if (auto* conditional = ast_dyn_cast<IfNode>(node)) {
            if (conditional->false_branch()) {
                work.push_back(conditional->false_branch());
            }
            if (conditional->true_branch()) {
                work.push_back(conditional->true_branch());
            }
        } else {
            // Code, loop, switch, case, expression, break, and continue nodes
            // retain their historical leaf/end-node behavior.
            out.push_back(node);
        }
    }
}

bool code_ends_with(const CodeNode* code, TerminalKind kind) {
    if (!code || !code->block()) return false;
    const auto& instructions = code->block()->instructions();
    if (instructions.empty()) return false;
    switch (kind) {
        case TerminalKind::Break:
            return isa<BreakInstr>(instructions.back());
        case TerminalKind::Continue:
            return isa<ContinueInstr>(instructions.back());
        case TerminalKind::Return:
            return isa<Return>(instructions.back());
    }
    return false;
}

bool subtree_ends_with(const AstNode* root, TerminalKind kind) {
    std::vector<AstNode*> ends;
    collect_end_nodes_iterative(const_cast<AstNode*>(root), ends);
    if (ends.empty()) return false;
    return std::all_of(
        ends.begin(), ends.end(),
        [&](const AstNode* node) {
            return code_ends_with(ast_dyn_cast<CodeNode>(node), kind);
        });
}

void collect_interrupting_code_nodes_iterative(
    AstNode* root,
    std::vector<CodeNode*>& out) {
    if (!root) return;
    std::unordered_set<AstNode*> visited;
    std::vector<AstNode*> work{root};
    while (!work.empty()) {
        AstNode* node = work.back();
        work.pop_back();
        if (!node || !visited.insert(node).second) continue;
        if (auto* code = ast_dyn_cast<CodeNode>(node)) {
            if (code_ends_with(code, TerminalKind::Break)
                || code_ends_with(code, TerminalKind::Continue)) {
                out.push_back(code);
            }
        } else if (auto* sequence = ast_dyn_cast<SeqNode>(node)) {
            for (auto it = sequence->nodes().rbegin();
                 it != sequence->nodes().rend();
                 ++it) {
                if (*it) work.push_back(*it);
            }
        } else if (auto* conditional = ast_dyn_cast<IfNode>(node)) {
            if (conditional->false_branch()) {
                work.push_back(conditional->false_branch());
            }
            if (conditional->true_branch()) {
                work.push_back(conditional->true_branch());
            }
        }
        // Nested loops and switches intentionally terminate this query:
        // their breaks/continues do not interrupt the ancestor loop.
    }
}

bool subtree_contains_break(const AstNode* root) {
    std::vector<CodeNode*> interrupting;
    collect_interrupting_code_nodes_iterative(
        const_cast<AstNode*>(root), interrupting);
    return std::any_of(
        interrupting.begin(), interrupting.end(),
        [](const CodeNode* code) {
            return code_ends_with(code, TerminalKind::Break);
        });
}

} // namespace

bool ast_contains_node(const AstNode* root, const AstNode* target) {
    if (!root || !target) return false;
    std::unordered_set<const AstNode*> visited;
    std::vector<const AstNode*> work{root};
    std::size_t expansions = 0;
    while (!work.empty()) {
        const AstNode* node = work.back();
        work.pop_back();
        if (!node || !visited.insert(node).second) continue;
        if (++expansions > kMaxAstTraversalExpansions) return false;
        if (node == target) return true;
        auto children = structural_children(node);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            work.push_back(*it);
        }
    }
    return false;
}

bool ast_contains_original_block(
    const AstNode* root,
    const BasicBlock* target) {
    if (!root || !target) return false;
    std::unordered_set<const AstNode*> visited;
    std::vector<const AstNode*> work{root};
    std::size_t expansions = 0;
    while (!work.empty()) {
        const AstNode* node = work.back();
        work.pop_back();
        if (!node || !visited.insert(node).second) continue;
        if (++expansions > kMaxAstTraversalExpansions) return false;
        if (const auto* code = ast_dyn_cast<CodeNode>(node);
            code && code->block() == target) {
            return true;
        }
        auto children = structural_children(node);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            work.push_back(*it);
        }
    }
    return false;
}

bool collect_ast_original_block_ids(
    const AstNode* root,
    std::vector<std::uint64_t>& ids) {
    if (!root) return true;
    struct Frame {
        const AstNode* node = nullptr;
        std::vector<const AstNode*> children;
        std::size_t next_child = 0;
    };
    std::vector<std::uint64_t> collected;
    std::unordered_set<const AstNode*> active;
    std::vector<Frame> stack;
    active.insert(root);
    stack.push_back({root, structural_children(root), 0});
    std::size_t expansions = 1;
    if (const BasicBlock* block = root->get_original_block()) {
        collected.push_back(static_cast<std::uint64_t>(block->id()));
    }
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            active.erase(frame.node);
            stack.pop_back();
            continue;
        }
        const AstNode* child = frame.children[frame.next_child++];
        if (!child) continue;
        if (active.contains(child)
            || ++expansions > kMaxAstTraversalExpansions) {
            return false;
        }
        if (const BasicBlock* block = child->get_original_block()) {
            collected.push_back(static_cast<std::uint64_t>(block->id()));
        }
        active.insert(child);
        stack.push_back({child, structural_children(child), 0});
    }
    ids.insert(ids.end(), collected.begin(), collected.end());
    return true;
}

// =============================================================================
// AstNode property query implementations
// =============================================================================

bool AstNode::is_endless_loop() const {
    if (auto* loop = ast_dyn_cast<LoopNode>(this))
        return loop->is_endless();
    return false;
}

bool AstNode::is_break_node() const {
    auto* code = ast_dyn_cast<CodeNode>(this);
    if (!code || !code->block()) return false;
    auto& instrs = code->block()->instructions();
    return instrs.size() == 1 && isa<BreakInstr>(instrs[0]);
}

bool AstNode::is_break_condition() const {
    auto* ifn = ast_dyn_cast<IfNode>(this);
    if (!ifn) return false;
    // Must be single-branch (no false/else arm)
    if (ifn->false_branch() != nullptr) return false;
    // The true branch must be a break node
    if (!ifn->true_branch()) return false;
    return ifn->true_branch()->is_break_node();
}

bool AstNode::is_single_branch() const {
    auto* ifn = ast_dyn_cast<IfNode>(this);
    if (!ifn) return false;
    return ifn->false_branch() == nullptr;
}

bool AstNode::does_end_with_break() const {
    return subtree_ends_with(this, TerminalKind::Break);
}

bool AstNode::does_contain_break() const {
    return subtree_contains_break(this);
}

bool AstNode::does_end_with_continue() const {
    return subtree_ends_with(this, TerminalKind::Continue);
}

bool AstNode::does_end_with_return() const {
    return subtree_ends_with(this, TerminalKind::Return);
}

bool AstNode::is_code_node_ending_with_break() const {
    return ast_dyn_cast<CodeNode>(this) && does_end_with_break();
}

bool AstNode::is_code_node_ending_with_continue() const {
    return ast_dyn_cast<CodeNode>(this) && does_end_with_continue();
}

void AstNode::get_end_nodes(std::vector<AstNode*>& out) {
    collect_end_nodes_iterative(this, out);
}

void AstNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    collect_interrupting_code_nodes_iterative(this, out);
}

bool AstNode::has_descendant_code_node_breaking_ancestor_loop() {
    return subtree_contains_break(this);
}

// =============================================================================
// CodeNode
// =============================================================================

bool CodeNode::does_end_with_break() const {
    if (!block_) return false;
    auto& instrs = block_->instructions();
    return !instrs.empty() && isa<BreakInstr>(instrs.back());
}

bool CodeNode::does_contain_break() const {
    return does_end_with_break();
}

bool CodeNode::does_end_with_continue() const {
    if (!block_) return false;
    auto& instrs = block_->instructions();
    return !instrs.empty() && isa<ContinueInstr>(instrs.back());
}

bool CodeNode::does_end_with_return() const {
    if (!block_) return false;
    auto& instrs = block_->instructions();
    return !instrs.empty() && isa<Return>(instrs.back());
}

void CodeNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    if (does_end_with_break() || does_end_with_continue()) {
        out.push_back(this);
    }
}

void CodeNode::clean() {
    if (!block_) return;
    auto& instrs = block_->mutable_instructions();
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        if (isa<BreakInstr>(instrs[i]) ||
            isa<Return>(instrs[i]) ||
            isa<ContinueInstr>(instrs[i])) {
            instrs.resize(i + 1);
            break;
        }
    }
}

void CodeNode::remove_last_instruction() {
    if (!block_) return;
    auto& instrs = block_->mutable_instructions();
    if (!instrs.empty()) {
        instrs.pop_back();
    }
}

// =============================================================================
// SeqNode
// =============================================================================

bool SeqNode::does_end_with_break() const {
    return subtree_ends_with(this, TerminalKind::Break);
}

bool SeqNode::does_contain_break() const {
    return subtree_contains_break(this);
}

void SeqNode::get_end_nodes(std::vector<AstNode*>& out) {
    collect_end_nodes_iterative(this, out);
}

void SeqNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    collect_interrupting_code_nodes_iterative(this, out);
}

// =============================================================================
// IfNode
// =============================================================================

bool IfNode::does_end_with_break() const {
    return subtree_ends_with(this, TerminalKind::Break);
}

bool IfNode::does_contain_break() const {
    return subtree_contains_break(this);
}

void IfNode::get_end_nodes(std::vector<AstNode*>& out) {
    collect_end_nodes_iterative(this, out);
}

void IfNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    collect_interrupting_code_nodes_iterative(this, out);
}

bool ast_has_unique_code_node_ownership(const AstNode* root) {
    if (!root) return true;
    constexpr std::size_t kMaxOwnershipExpansions = 1'000'000;
    const auto children = [](const AstNode* node) {
        std::vector<const AstNode*> result;
        if (const auto* sequence = ast_dyn_cast<SeqNode>(node)) {
            result.reserve(sequence->nodes().size());
            for (const AstNode* child : sequence->nodes()) {
                if (child) result.push_back(child);
            }
        } else if (const auto* conditional = ast_dyn_cast<IfNode>(node)) {
            if (conditional->cond()) result.push_back(conditional->cond());
            if (conditional->true_branch()) {
                result.push_back(conditional->true_branch());
            }
            if (conditional->false_branch()) {
                result.push_back(conditional->false_branch());
            }
        } else if (const auto* loop = ast_dyn_cast<LoopNode>(node)) {
            if (loop->body()) result.push_back(loop->body());
        } else if (const auto* switch_node = ast_dyn_cast<SwitchNode>(node)) {
            if (switch_node->cond()) result.push_back(switch_node->cond());
            for (const CaseNode* case_node : switch_node->cases()) {
                if (case_node) result.push_back(case_node);
            }
        } else if (const auto* case_node = ast_dyn_cast<CaseNode>(node)) {
            if (case_node->body()) result.push_back(case_node->body());
        }
        return result;
    };
    struct Frame {
        const AstNode* node = nullptr;
        std::vector<const AstNode*> children;
        std::size_t next_child = 0;
    };
    std::unordered_set<const BasicBlock*> owned_blocks;
    std::unordered_set<const AstNode*> active;
    std::vector<Frame> stack;
    active.insert(root);
    stack.push_back({root, children(root), 0});
    std::size_t expansions = 1;
    if (const auto* code = ast_dyn_cast<CodeNode>(root);
        code && code->block()) {
        owned_blocks.insert(code->block());
    }
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            active.erase(frame.node);
            stack.pop_back();
            continue;
        }
        const AstNode* child = frame.children[frame.next_child++];
        if (!child) continue;
        if (active.contains(child)
            || ++expansions > kMaxOwnershipExpansions) {
            return false;
        }
        if (const auto* code = ast_dyn_cast<CodeNode>(child);
            code && code->block()
            && !owned_blocks.insert(code->block()).second) {
            return false;
        }
        active.insert(child);
        stack.push_back({child, children(child), 0});
    }
    return true;
}

} // namespace aletheia
