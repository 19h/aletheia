#pragma once
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <unordered_set>

namespace logos {

// LLVM-style RTTI tag for DagNode hierarchy
enum class DagKind : std::uint8_t {
    Variable,
    Constant,
    Operation,
};

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

    /// LLVM-style RTTI tag.
    DagKind dag_kind() const { return dag_kind_; }

protected:
    DagKind dag_kind_;

private:
    std::vector<DagNode*> children_;
};

class DagVariable : public DagNode {
public:
    explicit DagVariable(std::string name) : name_(std::move(name)) { dag_kind_ = DagKind::Variable; }

    std::string to_string() const override {
        return name_;
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
};

class DagConstant : public DagNode {
public:
    explicit DagConstant(std::uint64_t value) : value_(value) { dag_kind_ = DagKind::Constant; }

    std::string to_string() const override {
        return std::to_string(value_);
    }

    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_;
};

enum class LogicOp {
    // Logical
    And,
    Or,
    Not,
    
    // Relational
    Eq,
    Neq,
    Lt,
    Le,
    Gt,
    Ge,

    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Negate,
    
    // Bitwise
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    
    // Shifts
    Shl,
    Shr,
    ShrUs
};

class DagOperation : public DagNode {
public:
    explicit DagOperation(LogicOp op) : op_(op) { dag_kind_ = DagKind::Operation; }

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

// =============================================================================
// LLVM-style RTTI helpers for DagNode hierarchy
// =============================================================================

template <typename T> bool dag_isa(const DagNode* n);

template <> inline bool dag_isa<DagVariable>(const DagNode* n)  { return n && n->dag_kind() == DagKind::Variable; }
template <> inline bool dag_isa<DagConstant>(const DagNode* n)  { return n && n->dag_kind() == DagKind::Constant; }
template <> inline bool dag_isa<DagOperation>(const DagNode* n) { return n && n->dag_kind() == DagKind::Operation; }

template <typename T>
T* dag_dyn_cast(DagNode* n) {
    return dag_isa<T>(n) ? static_cast<T*>(n) : nullptr;
}

template <typename T>
const T* dag_dyn_cast(const DagNode* n) {
    return dag_isa<T>(n) ? static_cast<const T*>(n) : nullptr;
}

template <typename T>
T* dag_cast(DagNode* n) {
    return static_cast<T*>(n);
}

template <typename T>
const T* dag_cast(const DagNode* n) {
    return static_cast<const T*>(n);
}

} // namespace logos
