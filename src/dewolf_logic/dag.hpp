#pragma once
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <unordered_set>

namespace dewolf_logic {

class DagNode {
public:
    virtual ~DagNode() = default;

    void add_child(DagNode* child) {
        children_.push_back(child);
    }

    const std::vector<DagNode*>& children() const {
        return children_;
    }

    virtual std::string to_string() const = 0;

private:
    std::vector<DagNode*> children_;
};

class DagVariable : public DagNode {
public:
    explicit DagVariable(std::string name) : name_(std::move(name)) {}

    std::string to_string() const override {
        return name_;
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
};

class DagConstant : public DagNode {
public:
    explicit DagConstant(std::uint64_t value) : value_(value) {}

    std::string to_string() const override {
        return std::to_string(value_);
    }

    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_;
};

enum class LogicOp {
    And,
    Or,
    Not,
    Eq,
    Neq,
    Lt,
    Le,
    Gt,
    Ge
};

class DagOperation : public DagNode {
public:
    explicit DagOperation(LogicOp op) : op_(op) {}

    std::string to_string() const override;

    LogicOp op() const { return op_; }

private:
    LogicOp op_;
};

class LogicDag {
public:
    template <typename T, typename... Args>
    T* create_node(Args&&... args) {
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = node.get();
        nodes_.push_back(std::move(node));
        return ptr;
    }

    const std::vector<std::unique_ptr<DagNode>>& nodes() const {
        return nodes_;
    }

private:
    std::vector<std::unique_ptr<DagNode>> nodes_;
};

} // namespace dewolf_logic
