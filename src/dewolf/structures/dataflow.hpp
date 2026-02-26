#pragma once
#include "../../common/arena_allocated.hpp"
#include "../../common/types.hpp"
#include "types.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dewolf {

// Forward declarations -- Expressions
class Constant;
class Variable;
class Operation;
class ListOperation;
class Condition;

// Forward declarations -- Instructions
class Instruction;
class Assignment;
class Branch;
class IndirectBranch;
class Return;
class Phi;
class BreakInstr;
class ContinueInstr;
class Comment;
class Relation;

class BasicBlock; // Forward from cfg.hpp for Phi::origin_block

// =============================================================================
// Visitor Interface
// =============================================================================
// The full visitor with methods for every concrete Expression and Instruction
// type, matching the Python reference's 18+ method interface. Concrete visitors
// implement the methods they care about; by default all dispatch to a fallback
// that does nothing (so existing visitors continue to compile during the
// incremental refactoring period).

class DataflowObjectVisitorInterface {
public:
    virtual ~DataflowObjectVisitorInterface() = default;

    // --- Expression visitors ---
    virtual void visit(Constant* c) = 0;
    virtual void visit(Variable* v) = 0;
    virtual void visit(Operation* o) = 0;

    // Extended expression visitors (default to visit(Operation*) for
    // backward compatibility during incremental migration)
    virtual void visit(ListOperation* o);
    virtual void visit(Condition* o);

    // --- Instruction visitors ---
    virtual void visit_assignment(Assignment* i);
    virtual void visit_branch(Branch* i);
    virtual void visit_indirect_branch(IndirectBranch* i);
    virtual void visit_return(Return* i);
    virtual void visit_phi(Phi* i);
    virtual void visit_break(BreakInstr* i);
    virtual void visit_continue(ContinueInstr* i);
    virtual void visit_comment(Comment* i);
    virtual void visit_relation(Relation* i);
};

// =============================================================================
// DataflowObject -- Abstract root of the entire IR hierarchy
// =============================================================================

class DataflowObject : public ArenaAllocated {
public:
    virtual ~DataflowObject() = default;
    virtual void accept(DataflowObjectVisitorInterface& visitor) = 0;

    /// Collect all Variable* that this object reads from (its requirements).
    /// Subclasses override to add their specific variable references.
    virtual void collect_requirements(std::unordered_set<Variable*>& out) const {
        (void)out;
    }

    /// Size of the object in bytes (register width, constant width, etc.)
    std::size_t size_bytes = 0;

    /// The type of this IR node (may be null if not yet resolved).
    TypePtr ir_type() const { return type_; }
    void set_ir_type(TypePtr t) { type_ = std::move(t); }

private:
    TypePtr type_;
};

// =============================================================================
// Expression -- Base class for all value-producing IR nodes
// =============================================================================
// Expressions are pure values: constants, variables, arithmetic/logic
// operations, function calls, conditions. They do NOT have side effects
// (assignments, branches, returns are Instructions, not Expressions).

class Expression : public DataflowObject {
public:
    virtual ~Expression() = default;
};

// =============================================================================
// Constant -- Literal integer/address value
// =============================================================================

class Constant : public Expression {
public:
    Constant(std::uint64_t value, std::size_t size) : value_(value) {
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_;
};

// =============================================================================
// Variable -- Named storage location (register, stack slot, temp, etc.)
// =============================================================================

class Variable : public Expression {
public:
    Variable(std::string name, std::size_t size) : name_(std::move(name)) {
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        // A variable requires itself (it must be defined somewhere for this
        // use to be valid).
        out.insert(const_cast<Variable*>(this));
    }

    const std::string& name() const { return name_; }

    // SSA fields
    std::size_t ssa_version() const { return ssa_version_; }
    void set_ssa_version(std::size_t version) { ssa_version_ = version; }

    /// Whether this variable is aliased (may be modified through a pointer).
    bool is_aliased() const { return is_aliased_; }
    void set_aliased(bool aliased) { is_aliased_ = aliased; }

private:
    std::string name_;
    std::size_t ssa_version_ = 0;
    bool is_aliased_ = false;
};

// =============================================================================
// OperationType -- Enum for expression-level operations
// =============================================================================
// This enum covers ONLY expression-level operations (arithmetic, bitwise,
// comparison, memory, calls). Statement-level constructs (assign, phi, branch,
// return, break, continue) are now represented by distinct Instruction
// subclasses and are NOT in this enum.

enum class OperationType {
    // Arithmetic (signed)
    add,
    sub,
    mul,
    div,
    mod,

    // Bitwise
    bit_and,
    bit_or,
    bit_xor,
    bit_not,

    // Shifts
    shl,
    shr,
    sar,

    // Logical
    logical_and,
    logical_or,
    logical_not,

    // Comparison (signed)
    eq,
    neq,
    lt,
    le,
    gt,
    ge,

    // Memory
    deref,
    address_of,

    // Function calls (expression-level: the call itself produces a value)
    call,

    // Unknown / unmapped
    unknown
};

// =============================================================================
// Operation -- Expression node for arithmetic, bitwise, comparison, etc.
// =============================================================================

class Operation : public Expression {
public:
    Operation(OperationType type, std::vector<Expression*> operands, std::size_t size)
        : type_(type), operands_(std::move(operands)) {
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        for (auto* op : operands_) {
            if (op) op->collect_requirements(out);
        }
    }

    OperationType type() const { return type_; }
    void set_type(OperationType t) { type_ = t; }
    const std::vector<Expression*>& operands() const { return operands_; }
    std::vector<Expression*>& mutable_operands() { return operands_; }

private:
    OperationType type_;
    std::vector<Expression*> operands_;
};

// =============================================================================
// ListOperation -- Ordered list of expressions (phi args, call params, etc.)
// =============================================================================

class ListOperation : public Expression {
public:
    explicit ListOperation(std::vector<Expression*> operands, std::size_t size = 0)
        : operands_(std::move(operands)) {
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        for (auto* op : operands_) {
            if (op) op->collect_requirements(out);
        }
    }

    const std::vector<Expression*>& operands() const { return operands_; }
    std::vector<Expression*>& mutable_operands() { return operands_; }
    bool empty() const { return operands_.empty(); }
    std::size_t size() const { return operands_.size(); }
    Expression* operator[](std::size_t i) const { return operands_[i]; }

private:
    std::vector<Expression*> operands_;
};

// =============================================================================
// Condition -- Binary comparison expression (produces a boolean)
// =============================================================================
// Condition is a specialization of Operation for comparison operations.
// It always has exactly 2 operands and its OperationType is one of:
// eq, neq, lt, le, gt, ge (and their unsigned variants when added).

class Condition : public Operation {
public:
    Condition(OperationType cmp_type, Expression* lhs, Expression* rhs, std::size_t size = 1)
        : Operation(cmp_type, {lhs, rhs}, size) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    Expression* lhs() const { return operands()[0]; }
    Expression* rhs() const { return operands()[1]; }

    /// Return the negated comparison type (e.g., eq -> neq, lt -> ge).
    static OperationType negate_comparison(OperationType op) {
        switch (op) {
            case OperationType::eq:  return OperationType::neq;
            case OperationType::neq: return OperationType::eq;
            case OperationType::lt:  return OperationType::ge;
            case OperationType::ge:  return OperationType::lt;
            case OperationType::le:  return OperationType::gt;
            case OperationType::gt:  return OperationType::le;
            default: return op;
        }
    }

    /// Is this an equality check with a constant on one side?
    bool is_equality_with_constant() const {
        return type() == OperationType::eq &&
               (dynamic_cast<Constant*>(operands()[0]) ||
                dynamic_cast<Constant*>(operands()[1]));
    }
};

// =============================================================================
// Instruction -- Abstract base for all statement-level IR nodes
// =============================================================================
// Instructions represent actions with side effects: assignments, branches,
// returns, phi functions, break/continue. They are stored in BasicBlock
// instruction lists. Unlike Expressions, Instructions do not produce a value
// that can be used as an operand in another expression.
//
// The Python reference hierarchy:
//   Instruction (ABC)
//     +-- Comment
//     +-- BaseAssignment -> Assignment -> Phi
//     |                  -> Relation
//     +-- GenericBranch  -> Branch (typed Condition)
//     |                  -> IndirectBranch (any Expression)
//     +-- Return
//     +-- Break
//     +-- Continue

class Instruction : public DataflowObject {
public:
    virtual ~Instruction() = default;

    /// Address in the original binary that this instruction was lifted from.
    /// 0 if synthetic (e.g., phi nodes, inserted copies).
    Address address() const { return address_; }
    void set_address(Address addr) { address_ = addr; }

    /// Variables defined (written) by this instruction.
    /// Default: empty (branches, break, continue, comments define nothing).
    virtual void collect_definitions(std::unordered_set<Variable*>& out) const {
        (void)out;
    }

    // Convenience: get definitions as a vector
    std::vector<Variable*> definitions() const {
        std::unordered_set<Variable*> s;
        collect_definitions(s);
        return {s.begin(), s.end()};
    }

    // Convenience: get requirements as a vector
    std::vector<Variable*> requirements() const {
        std::unordered_set<Variable*> s;
        collect_requirements(s);
        return {s.begin(), s.end()};
    }

protected:
    Address address_ = 0;
};

// =============================================================================
// Assignment -- dest = value
// =============================================================================
// The workhorse instruction: assigns the result of an Expression to a
// destination (usually a Variable, but can be a dereference for memory stores).

class Assignment : public Instruction {
public:
    Assignment(Expression* destination, Expression* value)
        : destination_(destination), value_(value) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_assignment(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        // The value (RHS) is always a requirement.
        if (value_) value_->collect_requirements(out);
        // If the destination is NOT a plain Variable (e.g., *(ptr+off) = val),
        // the destination's sub-expressions are also requirements (the pointer
        // address must be computed).
        if (destination_ && !dynamic_cast<Variable*>(destination_)) {
            destination_->collect_requirements(out);
        }
    }

    void collect_definitions(std::unordered_set<Variable*>& out) const override {
        if (auto* var = dynamic_cast<Variable*>(destination_)) {
            out.insert(var);
        }
        // ListOperation destination (e.g., [a,b] = f()) -- each element is a def
        if (auto* list = dynamic_cast<ListOperation*>(destination_)) {
            list->collect_requirements(out); // variables in the list are defs
        }
    }

    Expression* destination() const { return destination_; }
    Expression* value() const { return value_; }

    void set_destination(Expression* dest) { destination_ = dest; }
    void set_value(Expression* val) { value_ = val; }

    /// Rename the destination variable (for SSA renaming).
    /// Only works when destination is a Variable.
    void rename_destination(Variable* old_var, Variable* new_var) {
        if (auto* var = dynamic_cast<Variable*>(destination_)) {
            if (var == old_var) {
                destination_ = new_var;
            }
        }
    }

protected:
    Expression* destination_;
    Expression* value_;
};

// =============================================================================
// Relation -- aliased memory assignment (dest -> value, value may have changed)
// =============================================================================

class Relation : public Instruction {
public:
    Relation(Variable* destination, Variable* value)
        : destination_(destination), value_(value) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_relation(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        if (value_) out.insert(value_);
    }

    void collect_definitions(std::unordered_set<Variable*>& out) const override {
        if (destination_) out.insert(destination_);
    }

    Variable* destination() const { return destination_; }
    Variable* value() const { return value_; }

private:
    Variable* destination_;
    Variable* value_;
};

// =============================================================================
// Branch -- conditional branch: if (condition) then take true edge
// =============================================================================

class Branch : public Instruction {
public:
    explicit Branch(Condition* condition) : condition_(condition) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_branch(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        if (condition_) condition_->collect_requirements(out);
    }

    Condition* condition() const { return condition_; }
    void set_condition(Condition* cond) { condition_ = cond; }

private:
    Condition* condition_;
};

// =============================================================================
// IndirectBranch -- computed jump (switch dispatch via jump table)
// =============================================================================

class IndirectBranch : public Instruction {
public:
    explicit IndirectBranch(Expression* expression) : expression_(expression) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_indirect_branch(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        if (expression_) expression_->collect_requirements(out);
    }

    Expression* expression() const { return expression_; }

private:
    Expression* expression_;
};

// =============================================================================
// Return -- return statement with optional return values
// =============================================================================

class Return : public Instruction {
public:
    explicit Return(std::vector<Expression*> values = {})
        : values_(std::move(values)) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_return(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        for (auto* val : values_) {
            if (val) val->collect_requirements(out);
        }
    }

    const std::vector<Expression*>& values() const { return values_; }
    bool has_value() const { return !values_.empty(); }

private:
    std::vector<Expression*> values_;
};

// =============================================================================
// Phi -- SSA phi function: dest = phi(src_from_block_A, src_from_block_B, ...)
// =============================================================================
// Phi extends Assignment. The destination is a Variable. The value is a
// ListOperation of the phi operands. Additionally, origin_block maps each
// predecessor BasicBlock* to the specific operand that flows from that
// predecessor.

class Phi : public Assignment {
public:
    /// Construct a Phi with destination variable and a pre-built ListOperation.
    /// The ListOperation must be arena-allocated by the caller.
    Phi(Variable* destination, ListOperation* operands)
        : Assignment(destination, operands) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_phi(this);
    }

    /// The phi destination variable.
    Variable* dest_var() const {
        return static_cast<Variable*>(destination_);
    }

    /// The list of phi operands.
    ListOperation* operand_list() const {
        return static_cast<ListOperation*>(value_);
    }

    /// Map from predecessor BasicBlock to the operand that flows from it.
    const std::unordered_map<BasicBlock*, Expression*>& origin_block() const {
        return origin_block_;
    }
    std::unordered_map<BasicBlock*, Expression*>& mutable_origin_block() {
        return origin_block_;
    }

    /// Populate the origin_block map.
    void update_phi_function(
        const std::unordered_map<BasicBlock*, Expression*>& block_to_var) {
        for (auto& [block, var] : block_to_var) {
            origin_block_[block] = var;
        }
    }

    /// Remove the entry for a given predecessor block.
    void remove_from_origin_block(BasicBlock* block) {
        origin_block_.erase(block);
    }

    /// Rename the phi destination variable.
    void rename_phi_destination(Variable* new_dest) {
        destination_ = new_dest;
    }

private:
    std::unordered_map<BasicBlock*, Expression*> origin_block_;
};

// =============================================================================
// BreakInstr -- break statement (exits a loop)
// =============================================================================
// Named BreakInstr to avoid conflict with C++ `break` keyword and the AST
// BreakNode class.

class BreakInstr : public Instruction {
public:
    BreakInstr() = default;

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_break(this);
    }
};

// =============================================================================
// ContinueInstr -- continue statement (jumps to loop head)
// =============================================================================

class ContinueInstr : public Instruction {
public:
    ContinueInstr() = default;

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_continue(this);
    }
};

// =============================================================================
// Comment -- annotation / debug note (no semantic effect)
// =============================================================================

class Comment : public Instruction {
public:
    explicit Comment(std::string message) : message_(std::move(message)) {}

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_comment(this);
    }

    const std::string& message() const { return message_; }

private:
    std::string message_;
};

// =============================================================================
// Visitor default implementations (do nothing -- backward compatibility)
// =============================================================================
// These are defined inline here so that existing visitor subclasses that only
// override the original 3 methods (Constant, Variable, Operation) continue to
// compile without changes.

inline void DataflowObjectVisitorInterface::visit(ListOperation* o) {
    // ListOperation is not an Operation, so we can't delegate.
    // Do nothing by default. Visitors that care will override.
    (void)o;
}

inline void DataflowObjectVisitorInterface::visit(Condition* o) {
    // Fall back to generic Operation visitor since Condition IS-A Operation.
    visit(static_cast<Operation*>(o));
}

inline void DataflowObjectVisitorInterface::visit_assignment(Assignment* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_branch(Branch* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_indirect_branch(IndirectBranch* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_return(Return* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_phi(Phi* i) {
    // Default: treat phi as an assignment
    visit_assignment(static_cast<Assignment*>(i));
}
inline void DataflowObjectVisitorInterface::visit_break(BreakInstr* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_continue(ContinueInstr* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_comment(Comment* i) { (void)i; }
inline void DataflowObjectVisitorInterface::visit_relation(Relation* i) { (void)i; }

} // namespace dewolf
