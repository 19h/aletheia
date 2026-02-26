#include "ast.hpp"

namespace dewolf {

// =============================================================================
// AstNode property query implementations
// =============================================================================

bool AstNode::is_endless_loop() const {
    if (auto* loop = dynamic_cast<const LoopNode*>(this))
        return loop->is_endless();
    return false;
}

bool AstNode::is_break_node() const {
    auto* code = dynamic_cast<const CodeNode*>(this);
    if (!code || !code->block()) return false;
    auto& instrs = code->block()->instructions();
    return instrs.size() == 1 && dynamic_cast<BreakInstr*>(instrs[0]) != nullptr;
}

bool AstNode::is_break_condition() const {
    auto* ifn = dynamic_cast<const IfNode*>(this);
    if (!ifn) return false;
    // Must be single-branch (no false/else arm)
    if (ifn->false_branch() != nullptr) return false;
    // The true branch must be a break node
    if (!ifn->true_branch()) return false;
    return ifn->true_branch()->is_break_node();
}

bool AstNode::is_single_branch() const {
    auto* ifn = dynamic_cast<const IfNode*>(this);
    if (!ifn) return false;
    return ifn->false_branch() == nullptr;
}

bool AstNode::does_end_with_break() const {
    // Default: check all end nodes
    std::vector<AstNode*> ends;
    const_cast<AstNode*>(this)->get_end_nodes(ends);
    if (ends.empty()) return false;
    return std::all_of(ends.begin(), ends.end(),
        [](AstNode* n) { return n->does_end_with_break(); });
}

bool AstNode::does_contain_break() const {
    // Default: check all descendant code nodes
    std::vector<CodeNode*> descendants;
    const_cast<AstNode*>(this)->get_descendant_code_nodes_interrupting_ancestor_loop(descendants);
    return std::any_of(descendants.begin(), descendants.end(),
        [](CodeNode* cn) { return cn->does_end_with_break(); });
}

bool AstNode::does_end_with_continue() const {
    std::vector<AstNode*> ends;
    const_cast<AstNode*>(this)->get_end_nodes(ends);
    if (ends.empty()) return false;
    return std::all_of(ends.begin(), ends.end(),
        [](AstNode* n) { return n->does_end_with_continue(); });
}

bool AstNode::does_end_with_return() const {
    std::vector<AstNode*> ends;
    const_cast<AstNode*>(this)->get_end_nodes(ends);
    if (ends.empty()) return false;
    return std::all_of(ends.begin(), ends.end(),
        [](AstNode* n) { return n->does_end_with_return(); });
}

bool AstNode::is_code_node_ending_with_break() const {
    return dynamic_cast<const CodeNode*>(this) && does_end_with_break();
}

bool AstNode::is_code_node_ending_with_continue() const {
    return dynamic_cast<const CodeNode*>(this) && does_end_with_continue();
}

void AstNode::get_end_nodes(std::vector<AstNode*>& out) {
    // Default: yield self (leaf)
    out.push_back(this);
}

void AstNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    // Default: nothing
    (void)out;
}

bool AstNode::has_descendant_code_node_breaking_ancestor_loop() {
    std::vector<CodeNode*> descendants;
    get_descendant_code_nodes_interrupting_ancestor_loop(descendants);
    return std::any_of(descendants.begin(), descendants.end(),
        [](CodeNode* cn) { return cn->does_end_with_break(); });
}

// =============================================================================
// CodeNode
// =============================================================================

bool CodeNode::does_end_with_break() const {
    if (!block_) return false;
    auto& instrs = block_->instructions();
    return !instrs.empty() && dynamic_cast<BreakInstr*>(instrs.back()) != nullptr;
}

bool CodeNode::does_contain_break() const {
    return does_end_with_break();
}

bool CodeNode::does_end_with_continue() const {
    if (!block_) return false;
    auto& instrs = block_->instructions();
    return !instrs.empty() && dynamic_cast<ContinueInstr*>(instrs.back()) != nullptr;
}

bool CodeNode::does_end_with_return() const {
    if (!block_) return false;
    auto& instrs = block_->instructions();
    return !instrs.empty() && dynamic_cast<Return*>(instrs.back()) != nullptr;
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
        if (dynamic_cast<BreakInstr*>(instrs[i]) ||
            dynamic_cast<Return*>(instrs[i]) ||
            dynamic_cast<ContinueInstr*>(instrs[i])) {
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
    // A sequence ends with break if its last child ends with break
    if (nodes_.empty()) return false;
    return nodes_.back()->does_end_with_break();
}

bool SeqNode::does_contain_break() const {
    return std::any_of(nodes_.begin(), nodes_.end(),
        [](AstNode* n) { return n->does_contain_break(); });
}

void SeqNode::get_end_nodes(std::vector<AstNode*>& out) {
    // End nodes come from the last child
    if (!nodes_.empty()) {
        nodes_.back()->get_end_nodes(out);
    }
}

void SeqNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    for (auto* child : nodes_) {
        child->get_descendant_code_nodes_interrupting_ancestor_loop(out);
    }
}

// =============================================================================
// IfNode
// =============================================================================

bool IfNode::does_end_with_break() const {
    if (!true_branch_ && !false_branch_) return false;
    // Both branches (when they exist) must end with break
    bool true_ends = true_branch_ ? true_branch_->does_end_with_break() : false;
    bool false_ends = false_branch_ ? false_branch_->does_end_with_break() : false;
    if (!false_branch_) return true_ends; // single-branch: only true matters
    return true_ends && false_ends;
}

bool IfNode::does_contain_break() const {
    bool t = true_branch_ ? true_branch_->does_contain_break() : false;
    bool f = false_branch_ ? false_branch_->does_contain_break() : false;
    return t || f;
}

void IfNode::get_end_nodes(std::vector<AstNode*>& out) {
    // End nodes come from both branches
    if (true_branch_) true_branch_->get_end_nodes(out);
    if (false_branch_) false_branch_->get_end_nodes(out);
    // If only single branch, the if-node itself is also an end-node
    // (execution may fall through if the condition is false)
    if (!false_branch_) {
        // For single-branch if nodes, the condition itself provides a
        // potential fall-through. However, matching the Python reference,
        // for loop structuring purposes we only yield from existing branches.
        // The caller handles missing branches.
    }
}

void IfNode::get_descendant_code_nodes_interrupting_ancestor_loop(
    std::vector<CodeNode*>& out) {
    if (true_branch_) true_branch_->get_descendant_code_nodes_interrupting_ancestor_loop(out);
    if (false_branch_) false_branch_->get_descendant_code_nodes_interrupting_ancestor_loop(out);
}

} // namespace dewolf
