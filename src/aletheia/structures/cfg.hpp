#pragma once
#include "../../common/arena_allocated.hpp"
#include "../../common/types.hpp"
#include "dataflow.hpp"
#include <vector>
#include <cstdint>
#include <memory>

namespace aletheia {

// Note: BasicBlock is forward-declared in dataflow.hpp for Phi::origin_block.
// The actual definition is here.

enum class EdgeType {
    Unconditional,
    True,
    False,
    Switch,
    Fallthrough
};

enum class EdgeProperty {
    Tree,
    Back,
    Forward,
    Cross,
    Retreating,
    NonLoop
};

class Edge : public ArenaAllocated {
public:
    Edge(BasicBlock* source, BasicBlock* target, EdgeType type)
        : source_(source), target_(target), type_(type) {}

    virtual ~Edge() = default;

    BasicBlock* source() const { return source_; }
    BasicBlock* target() const { return target_; }
    EdgeType type() const { return type_; }

private:
    BasicBlock* source_;
    BasicBlock* target_;
    EdgeType type_;
};

class SwitchEdge : public Edge {
public:
    SwitchEdge(BasicBlock* source, BasicBlock* target, std::uint64_t case_value)
        : Edge(source, target, EdgeType::Switch), case_values_{static_cast<std::int64_t>(case_value)} {}

    SwitchEdge(BasicBlock* source,
               BasicBlock* target,
               std::vector<std::int64_t> case_values,
               bool is_default = false)
        : Edge(source, target, EdgeType::Switch),
          case_values_(std::move(case_values)),
          is_default_(is_default) {}

    std::int64_t case_value() const {
        if (case_values_.empty()) {
            return 0;
        }
        return case_values_.front();
    }

    const std::vector<std::int64_t>& case_values() const { return case_values_; }
    bool is_default() const { return is_default_; }

    void add_case_value(std::int64_t value) { case_values_.push_back(value); }
    void set_default(bool value) { is_default_ = value; }

private:
    std::vector<std::int64_t> case_values_;
    bool is_default_ = false;
};

// =============================================================================
// BasicBlock -- A sequence of instructions with control-flow edges
// =============================================================================
// BasicBlock now stores Instruction* from the dataflow hierarchy (Assignment,
// Branch, Return, Phi, etc.) instead of the old thin wrapper class.

class BasicBlock : public ArenaAllocated {
public:
    explicit BasicBlock(std::size_t id) : id_(id) {}

    std::size_t id() const { return id_; }

    void add_instruction(Instruction* inst) {
        instructions_.push_back(inst);
    }

    const std::vector<Instruction*>& instructions() const { return instructions_; }
    std::vector<Instruction*>& mutable_instructions() { return instructions_; }
    void set_instructions(std::vector<Instruction*> insts) { instructions_ = std::move(insts); }

    void add_predecessor(Edge* edge) { predecessors_.push_back(edge); }
    void add_successor(Edge* edge) { successors_.push_back(edge); }

    void remove_predecessor(Edge* edge) {
        std::erase(predecessors_, edge);
    }

    void remove_successor(Edge* edge) {
        std::erase(successors_, edge);
    }

    void substitute_successor(Edge* old_edge, Edge* new_edge) {
        for (auto& edge : successors_) {
            if (edge == old_edge) {
                edge = new_edge;
                break;
            }
        }
    }

    const std::vector<Edge*>& predecessors() const { return predecessors_; }
    const std::vector<Edge*>& successors() const { return successors_; }

private:
    std::size_t id_;
    std::vector<Instruction*> instructions_;
    std::vector<Edge*> predecessors_;
    std::vector<Edge*> successors_;
};

class ControlFlowGraph {
public:
    ControlFlowGraph() = default;

    void set_entry_block(BasicBlock* block) { entry_block_ = block; }
    BasicBlock* entry_block() const { return entry_block_; }

    void add_block(BasicBlock* block) { blocks_.push_back(block); }
    const std::vector<BasicBlock*>& blocks() const { return blocks_; }
    std::vector<BasicBlock*>& mutable_blocks() { return blocks_; }

    void remove_edge(Edge* edge);
    void substitute_edge(Edge* old_edge, Edge* new_edge);
    void remove_nodes_from(const std::unordered_set<BasicBlock*>& dead_blocks);

    void classify_edges(class DominatorTree& dom_tree);
    const std::unordered_map<Edge*, EdgeProperty>& edge_properties() const { return edge_properties_; }
    std::unordered_map<BasicBlock*, std::unordered_set<Edge*>> back_edges() const;
    std::unordered_set<Edge*> retreating_edges() const;

    // Traversals
    std::vector<BasicBlock*> post_order() const;
    std::vector<BasicBlock*> reverse_post_order() const;
    std::vector<BasicBlock*> dfs() const;

private:
    BasicBlock* entry_block_ = nullptr;
    std::vector<BasicBlock*> blocks_;
    std::unordered_map<Edge*, EdgeProperty> edge_properties_;
};

// =============================================================================
// Migration helpers
// =============================================================================
// These helpers ease the transition from the old Operation-centric code to the
// new Instruction hierarchy. They will be removed once all consumers are fully
// migrated.

/// Extract the Operation* from an Instruction, if it is an Assignment whose
/// value is an Operation, or if the instruction wraps a known expression type.
/// Returns nullptr if the instruction does not contain an Operation.
inline Operation* get_operation(Instruction* inst) {
    if (auto* assign = dynamic_cast<Assignment*>(inst)) {
        return dynamic_cast<Operation*>(assign->value());
    }
    if (auto* branch = dynamic_cast<Branch*>(inst)) {
        return branch->condition();  // Condition IS-A Operation
    }
    return nullptr;
}

/// Check if an Instruction is an assignment (including Phi).
inline bool is_assignment(Instruction* inst) {
    return dynamic_cast<Assignment*>(inst) != nullptr;
}

/// Check if an Instruction is a phi function.
inline bool is_phi(Instruction* inst) {
    return dynamic_cast<Phi*>(inst) != nullptr;
}

/// Check if an Instruction is a branch (conditional or indirect).
inline bool is_branch(Instruction* inst) {
    return dynamic_cast<Branch*>(inst) != nullptr ||
           dynamic_cast<IndirectBranch*>(inst) != nullptr;
}

/// Check if an Instruction is a return.
inline bool is_return(Instruction* inst) {
    return dynamic_cast<Return*>(inst) != nullptr;
}

} // namespace aletheia
