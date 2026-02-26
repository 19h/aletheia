#pragma once
#include "../../common/arena_allocated.hpp"
#include "../../common/types.hpp"
#include "dataflow.hpp"
#include <vector>
#include <cstdint>
#include <memory>

namespace dewolf {

class BasicBlock;

enum class EdgeType {
    Unconditional,
    True,
    False,
    Switch,
    Fallthrough
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
        : Edge(source, target, EdgeType::Switch), case_value_(case_value) {}

    std::uint64_t case_value() const { return case_value_; }

private:
    std::uint64_t case_value_;
};

class Instruction : public ArenaAllocated {
public:
    Instruction(Address address, Operation* op) : address_(address), op_(op) {}
    
    Address address() const { return address_; }
    Operation* operation() const { return op_; }

private:
    Address address_;
    Operation* op_;
};

class BasicBlock : public ArenaAllocated {
public:
    explicit BasicBlock(std::size_t id) : id_(id) {}

    std::size_t id() const { return id_; }

    void add_instruction(Instruction* inst) {
        instructions_.push_back(inst);
    }

    const std::vector<Instruction*>& instructions() const { return instructions_; }
    void set_instructions(std::vector<Instruction*> insts) { instructions_ = std::move(insts); }

    void add_predecessor(Edge* edge) { predecessors_.push_back(edge); }
    void add_successor(Edge* edge) { successors_.push_back(edge); }

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

    // Traversals
    std::vector<BasicBlock*> post_order() const;
    std::vector<BasicBlock*> reverse_post_order() const;
    std::vector<BasicBlock*> dfs() const;

private:
    BasicBlock* entry_block_ = nullptr;
    std::vector<BasicBlock*> blocks_;
};

} // namespace dewolf
