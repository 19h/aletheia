#pragma once
#include "../../common/arena_allocated.hpp"
#include "../../common/arena.hpp"
#include "../../common/small_vector.hpp"
#include "../../common/types.hpp"
#include "types.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

namespace aletheia {

// Forward declarations -- Expressions
class Expression;
class Constant;
class Variable;
class GlobalVariable;
class Operation;
class Call;
class ListOperation;
class Condition;

// Forward declarations -- Instructions
class Instruction;
class Assignment;
class Branch;
class IndirectBranch;
class Return;
class Phi;
class MemPhi;
class BreakInstr;
class ContinueInstr;
class Comment;
class Relation;

class BasicBlock; // Forward from cfg.hpp for Phi::origin_block

// =============================================================================
// NodeKind -- LLVM-style RTTI tag for O(1) type checks
// =============================================================================
// Every concrete DataflowObject subclass carries a NodeKind tag set at
// construction time. Use isa<T>(ptr), cast<T>(ptr), dyn_cast<T>(ptr) instead
// of dynamic_cast<T*>(ptr) in hot paths -- the tag check is a single integer
// comparison vs. dynamic_cast's O(N) string comparisons on macOS.

enum class NodeKind : std::uint8_t {
    // Expressions
    Constant,
    Variable,
    GlobalVariable,
    Operation,
    Call,
    ListOperation,
    Condition,

    // Instructions
    Assignment,
    Branch,
    IndirectBranch,
    Return,
    Phi,
    MemPhi,
    BreakInstr,
    ContinueInstr,
    Comment,
    Relation,
};

// =============================================================================
// LLVM-style RTTI helpers: isa<T>, cast<T>, dyn_cast<T>
// =============================================================================
// These are O(1) integer comparisons. Use them instead of dynamic_cast in hot
// paths. They handle inheritance: isa<Variable> matches GlobalVariable,
// isa<Operation> matches Call/Condition, isa<Assignment> matches Phi/MemPhi.

class DataflowObject; // forward

/// Check if a DataflowObject pointer is of a specific concrete type (or a
/// subclass mapped to that kind). Returns false for nullptr.
template <typename T>
inline bool isa(const DataflowObject* obj);

/// Unconditional downcast -- UB if the type doesn't match. Use after isa<T>.
template <typename T>
inline T* cast(DataflowObject* obj) {
    return static_cast<T*>(obj);
}

template <typename T>
inline const T* cast(const DataflowObject* obj) {
    return static_cast<const T*>(obj);
}

/// Safe downcast -- returns nullptr if the type doesn't match.
template <typename T>
inline T* dyn_cast(DataflowObject* obj) {
    return (obj && isa<T>(obj)) ? static_cast<T*>(obj) : nullptr;
}

template <typename T>
inline const T* dyn_cast(const DataflowObject* obj) {
    return (obj && isa<T>(obj)) ? static_cast<const T*>(obj) : nullptr;
}

// Expression* and Instruction* overloads are defined after class definitions
// (see bottom of file) to avoid incomplete-type static_cast errors.

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
    virtual void visit(GlobalVariable* v);
    virtual void visit(Operation* o) = 0;

    // Extended expression visitors (default to visit(Operation*) for
    // backward compatibility during incremental migration)
    virtual void visit(Call* o);
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

    /// LLVM-style RTTI tag -- O(1) type identification without dynamic_cast.
    NodeKind node_kind() const { return node_kind_; }

    /// Collect all Variable* that this object reads from (its requirements).
    /// Subclasses override to add their specific variable references.
    virtual void collect_requirements(std::unordered_set<Variable*>& out) const {
        (void)out;
    }

    /// Recursively substitute all occurrences of `replacee` with `replacement`
    /// in this object's children. For leaf nodes (Constant, Variable), this is
    /// a no-op. For composite nodes (Operation, Instructions), this replaces
    /// direct children that match `replacee` and delegates recursively.
    virtual void substitute(Expression* replacee, Expression* replacement) {
        (void)replacee; (void)replacement;
    }

    /// Size of the object in bytes (register width, constant width, etc.)
    std::size_t size_bytes = 0;

    /// The type of this IR node (may be null if not yet resolved).
    TypePtr ir_type() const { return type_; }
    void set_ir_type(TypePtr t) { type_ = std::move(t); }

protected:
    /// Subclasses set this in their constructor.
    NodeKind node_kind_;

private:
    TypePtr type_;
};

// =============================================================================
// isa<T> specializations -- define right after DataflowObject so that inline
// class method bodies can use isa<T>/dyn_cast<T> freely.
// =============================================================================
// Each specialization handles the inheritance hierarchy correctly:
//   isa<Variable> matches Variable OR GlobalVariable
//   isa<Operation> matches Operation OR Call OR Condition
//   isa<Assignment> matches Assignment OR Phi OR MemPhi
//   isa<Phi> matches Phi OR MemPhi

template <> inline bool isa<Constant>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Constant;
}
template <> inline bool isa<Variable>(const DataflowObject* obj) {
    return obj && (obj->node_kind() == NodeKind::Variable || obj->node_kind() == NodeKind::GlobalVariable);
}
template <> inline bool isa<GlobalVariable>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::GlobalVariable;
}
template <> inline bool isa<Operation>(const DataflowObject* obj) {
    return obj && (obj->node_kind() == NodeKind::Operation || obj->node_kind() == NodeKind::Call || obj->node_kind() == NodeKind::Condition);
}
template <> inline bool isa<Call>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Call;
}
template <> inline bool isa<ListOperation>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::ListOperation;
}
template <> inline bool isa<Condition>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Condition;
}
template <> inline bool isa<Assignment>(const DataflowObject* obj) {
    return obj && (obj->node_kind() == NodeKind::Assignment || obj->node_kind() == NodeKind::Phi || obj->node_kind() == NodeKind::MemPhi);
}
template <> inline bool isa<Relation>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Relation;
}
template <> inline bool isa<Branch>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Branch;
}
template <> inline bool isa<IndirectBranch>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::IndirectBranch;
}
template <> inline bool isa<Return>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Return;
}
template <> inline bool isa<Phi>(const DataflowObject* obj) {
    return obj && (obj->node_kind() == NodeKind::Phi || obj->node_kind() == NodeKind::MemPhi);
}
template <> inline bool isa<MemPhi>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::MemPhi;
}
template <> inline bool isa<BreakInstr>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::BreakInstr;
}
template <> inline bool isa<ContinueInstr>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::ContinueInstr;
}
template <> inline bool isa<Comment>(const DataflowObject* obj) {
    return obj && obj->node_kind() == NodeKind::Comment;
}

// =============================================================================
// Expression -- Base class for all value-producing IR nodes
// =============================================================================
// Expressions are pure values: constants, variables, arithmetic/logic
// operations, function calls, conditions. They do NOT have side effects
// (assignments, branches, returns are Instructions, not Expressions).

class Expression : public DataflowObject {
public:
    virtual ~Expression() = default;

    /// Deep-clone this expression into the given arena.
    virtual Expression* copy(DecompilerArena& arena) const = 0;
};

// =============================================================================
// Constant -- Literal integer/address value
// =============================================================================

class Constant : public Expression {
public:
    Constant(std::uint64_t value, std::size_t size) : value_(value) {
        this->node_kind_ = NodeKind::Constant;
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    Expression* copy(DecompilerArena& arena) const override {
        auto* c = arena.create<Constant>(value_, size_bytes);
        c->set_ir_type(ir_type());
        return c;
    }

    // Constant is a leaf: substitute() is a no-op (inherited default).

    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_;
};

// =============================================================================
// Variable -- Named storage location (register, stack slot, temp, etc.)
// =============================================================================

/// Storage classification for variables.
enum class VariableKind : std::uint8_t {
    Register,       ///< Stored in a CPU register (default).
    StackLocal,     ///< Local variable on the stack frame (negative FP offset).
    StackArgument,  ///< Incoming stack argument (positive FP offset / arg area).
    Parameter,      ///< Function parameter (register-passed or identified by prototype).
    Temporary,      ///< Compiler/decompiler-generated temporary.
};

class Variable : public Expression {
public:
    Variable(std::string name, std::size_t size) : name_(std::move(name)) {
        this->node_kind_ = NodeKind::Variable;
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

    Expression* copy(DecompilerArena& arena) const override {
        auto* v = arena.create<Variable>(name_, size_bytes);
        v->set_ssa_version(ssa_version_);
        v->set_aliased(is_aliased_);
        v->set_ir_type(ir_type());
        v->set_kind(kind_);
        v->set_stack_offset(stack_offset_);
        v->set_parameter_index(parameter_index_);
        return v;
    }

    // Variable is a leaf: substitute() is a no-op (inherited default).

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // SSA fields
    std::size_t ssa_version() const { return ssa_version_; }
    void set_ssa_version(std::size_t version) { ssa_version_ = version; }

    /// Whether this variable is aliased (may be modified through a pointer).
    bool is_aliased() const { return is_aliased_; }
    void set_aliased(bool aliased) { is_aliased_ = aliased; }

    /// Storage classification.
    VariableKind kind() const { return kind_; }
    void set_kind(VariableKind kind) { kind_ = kind; }

    /// Stack frame offset (meaningful for StackLocal / StackArgument kinds).
    /// For frame-pointer-based: negative = local, positive = argument.
    /// For SP-based: normalized to frame-base-relative via SP delta.
    std::int64_t stack_offset() const { return stack_offset_; }
    void set_stack_offset(std::int64_t offset) { stack_offset_ = offset; }

    /// Parameter index (0-based, meaningful for Parameter kind).
    /// -1 means not a parameter.
    int parameter_index() const { return parameter_index_; }
    void set_parameter_index(int index) { parameter_index_ = index; }

    /// Convenience predicates.
    bool is_parameter() const { return kind_ == VariableKind::Parameter; }
    bool is_stack_variable() const {
        return kind_ == VariableKind::StackLocal || kind_ == VariableKind::StackArgument;
    }

private:
    std::string name_;
    std::size_t ssa_version_ = 0;
    bool is_aliased_ = false;
    VariableKind kind_ = VariableKind::Register;
    std::int64_t stack_offset_ = 0;
    int parameter_index_ = -1;
};

// =============================================================================
// GlobalVariable -- Variable with global storage metadata
// =============================================================================

class GlobalVariable : public Variable {
public:
    GlobalVariable(std::string name,
                   std::size_t size,
                   Expression* initial_value = nullptr,
                   bool is_constant = false)
        : Variable(std::move(name), size),
          initial_value_(initial_value),
          is_constant_(is_constant) {
        this->node_kind_ = NodeKind::GlobalVariable;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    Expression* copy(DecompilerArena& arena) const override {
        auto* g = arena.create<GlobalVariable>(
            name(),
            size_bytes,
            initial_value_ ? initial_value_->copy(arena) : nullptr,
            is_constant_);
        g->set_ssa_version(ssa_version());
        g->set_aliased(is_aliased());
        g->set_ir_type(ir_type());
        return g;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        if (!initial_value_) return;
        initial_value_->substitute(replacee, replacement);
        if (initial_value_ == replacee) {
            initial_value_ = replacement;
        }
    }

    Expression* initial_value() const { return initial_value_; }
    void set_initial_value(Expression* value) { initial_value_ = value; }

    bool is_constant() const { return is_constant_; }
    void set_is_constant(bool value) { is_constant_ = value; }

private:
    Expression* initial_value_ = nullptr;
    bool is_constant_ = false;
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
    add,                    // a + b
    add_with_carry,         // a + b + carry
    sub,                    // a - b
    sub_with_carry,         // a - b - borrow
    mul,                    // a * b  (signed)
    mul_us,                 // a * b  (unsigned)
    div,                    // a / b  (signed)
    div_us,                 // a / b  (unsigned)
    mod,                    // a % b  (signed)
    mod_us,                 // a % b  (unsigned)
    negate,                 // -a     (unary arithmetic negation)
    power,                  // a ** b (exponentiation, idiom recovery)

    // Floating point arithmetic
    add_float,              // a + b  (float)
    sub_float,              // a - b  (float)
    mul_float,              // a * b  (float)
    div_float,              // a / b  (float)

    // Bitwise
    bit_and,                // a & b
    bit_or,                 // a | b
    bit_xor,                // a ^ b
    bit_not,                // ~a

    // Shifts
    shl,                    // a << b  (logical left shift)
    shr,                    // a >> b  (arithmetic right shift, signed)
    shr_us,                 // a >> b  (logical right shift, unsigned)
    sar,                    // a >> b  (alias: arithmetic shift right, same as shr)

    // Rotates
    left_rotate,            // rotate left
    right_rotate,           // rotate right
    left_rotate_carry,      // rotate left through carry
    right_rotate_carry,     // rotate right through carry

    // Logical
    logical_and,            // a && b
    logical_or,             // a || b
    logical_not,            // !a

    // Comparison (signed)
    eq,                     // a == b
    neq,                    // a != b
    lt,                     // a <  b  (signed)
    le,                     // a <= b  (signed)
    gt,                     // a >  b  (signed)
    ge,                     // a >= b  (signed)

    // Comparison (unsigned)
    lt_us,                  // a <  b  (unsigned)
    le_us,                  // a <= b  (unsigned)
    gt_us,                  // a >  b  (unsigned)
    ge_us,                  // a >= b  (unsigned)

    // Memory / addressing
    deref,                  // *a      (dereference / memory load)
    address_of,             // &a      (address-of)
    member_access,          // a.b or a->b (struct/union member access)

    // Type / cast operations
    cast,                   // (type)a (explicit type cast)
    pointer,                // pointer type wrapper (used in type annotations)
    low,                    // low bits extraction (e.g., low byte of a register)
    field,                  // field access (struct/union field by offset)

    // Ternary
    ternary,                // a ? b : c

    // Function calls (expression-level: the call itself produces a value)
    call,

    // List (multiple values, used for function args / return lists)
    list_op,

    // Carry-based addition (used for multi-precision arithmetic)
    adc,                    // add-with-carry (x86 ADC instruction)

    // Unknown / unmapped
    unknown
};

struct ArrayAccessInfo {
    Variable* base = nullptr;
    Expression* index = nullptr;
    std::size_t element_size = 0;
    bool confidence = false;
};

// =============================================================================
// Operation -- Expression node for arithmetic, bitwise, comparison, etc.
// =============================================================================

class Operation : public Expression {
public:
    Operation(OperationType type, SmallVector<Expression*, 4> operands, std::size_t size)
        : type_(type), operands_(std::move(operands)) {
        this->node_kind_ = NodeKind::Operation;
        this->size_bytes = size;
    }

    // Backward compat: accept std::vector rvalue (e.g. from lifter/copy code)
    Operation(OperationType type, std::vector<Expression*>&& operands, std::size_t size)
        : type_(type), operands_(std::move(operands)) {
        this->node_kind_ = NodeKind::Operation;
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

    Expression* copy(DecompilerArena& arena) const override {
        SmallVector<Expression*, 4> new_ops;
        new_ops.reserve(operands_.size());
        for (auto* op : operands_) {
            new_ops.push_back(op ? op->copy(arena) : nullptr);
        }
        auto* o = arena.create<Operation>(type_, std::move(new_ops), size_bytes);
        o->set_ir_type(ir_type());
        if (array_access_.has_value()) {
            o->set_array_access(*array_access_);
        }
        return o;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        // First, recursively substitute in children
        for (auto* op : operands_) {
            if (op) op->substitute(replacee, replacement);
        }
        // Then, replace direct children that match
        for (auto& op : operands_) {
            if (op == replacee) op = replacement;
        }
        if (array_access_.has_value()) {
            if (array_access_->base == replacee) {
                if (auto* replacement_var = dyn_cast<Variable>(replacement)) {
                    array_access_->base = replacement_var;
                }
            }
            if (array_access_->index == replacee) {
                array_access_->index = replacement;
            }
        }
    }

    OperationType type() const { return type_; }
    void set_type(OperationType t) { type_ = t; }
    const SmallVector<Expression*, 4>& operands() const { return operands_; }
    SmallVector<Expression*, 4>& mutable_operands() { return operands_; }
    const std::optional<ArrayAccessInfo>& array_access() const { return array_access_; }
    void set_array_access(ArrayAccessInfo info) { array_access_ = std::move(info); }
    void clear_array_access() { array_access_.reset(); }

private:
    OperationType type_;
    SmallVector<Expression*, 4> operands_;
    std::optional<ArrayAccessInfo> array_access_;
};

// =============================================================================
// ListOperation -- Ordered list of expressions (phi args, call params, etc.)
// =============================================================================

class ListOperation : public Expression {
public:
    explicit ListOperation(SmallVector<Expression*, 4> operands, std::size_t size = 0)
        : operands_(std::move(operands)) {
        this->node_kind_ = NodeKind::ListOperation;
        this->size_bytes = size;
    }

    // Backward compat: accept std::vector rvalue
    explicit ListOperation(std::vector<Expression*>&& operands, std::size_t size = 0)
        : operands_(std::move(operands)) {
        this->node_kind_ = NodeKind::ListOperation;
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

    Expression* copy(DecompilerArena& arena) const override {
        SmallVector<Expression*, 4> new_ops;
        new_ops.reserve(operands_.size());
        for (auto* op : operands_) {
            new_ops.push_back(op ? op->copy(arena) : nullptr);
        }
        auto* lo = arena.create<ListOperation>(std::move(new_ops), size_bytes);
        lo->set_ir_type(ir_type());
        return lo;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        for (auto* op : operands_) {
            if (op) op->substitute(replacee, replacement);
        }
        for (auto& op : operands_) {
            if (op == replacee) op = replacement;
        }
    }

    const SmallVector<Expression*, 4>& operands() const { return operands_; }
    SmallVector<Expression*, 4>& mutable_operands() { return operands_; }
    bool empty() const { return operands_.empty(); }
    std::size_t size() const { return operands_.size(); }
    Expression* operator[](std::size_t i) const { return operands_[i]; }

private:
    SmallVector<Expression*, 4> operands_;
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
        : Operation(cmp_type, SmallVector<Expression*, 4>{lhs, rhs}, size) {
        this->node_kind_ = NodeKind::Condition;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    Expression* copy(DecompilerArena& arena) const override {
        auto* c = arena.create<Condition>(type(),
            lhs()->copy(arena), rhs()->copy(arena), size_bytes);
        c->set_ir_type(ir_type());
        return c;
    }

    // substitute() inherited from Operation is correct for Condition.

    Expression* lhs() const { return operands()[0]; }
    Expression* rhs() const { return operands()[1]; }

    /// Return the negated comparison type (e.g., eq -> neq, lt -> ge).
    static OperationType negate_comparison(OperationType op) {
        switch (op) {
            case OperationType::eq:    return OperationType::neq;
            case OperationType::neq:   return OperationType::eq;
            case OperationType::lt:    return OperationType::ge;
            case OperationType::ge:    return OperationType::lt;
            case OperationType::le:    return OperationType::gt;
            case OperationType::gt:    return OperationType::le;
            case OperationType::lt_us: return OperationType::ge_us;
            case OperationType::ge_us: return OperationType::lt_us;
            case OperationType::le_us: return OperationType::gt_us;
            case OperationType::gt_us: return OperationType::le_us;
            default: return op;
        }
    }

    /// Is this an equality check with a constant on one side?
    bool is_equality_with_constant() const {
        return type() == OperationType::eq &&
               (isa<Constant>(operands()[0]) ||
                isa<Constant>(operands()[1]));
    }
};

// =============================================================================
// Call -- Function call expression (subclass of Operation)
// =============================================================================
// A Call has operands[0] = function target, operands[1..N] = arguments.
// It dispatches to visit_call() instead of the generic visit(Operation*).

class Call : public Operation {
public:
    Call(Expression* target, std::vector<Expression*> args, std::size_t size)
        : Operation(OperationType::call, build_operands_(target, std::move(args)), size) {
        this->node_kind_ = NodeKind::Call;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    Expression* copy(DecompilerArena& arena) const override {
        Expression* new_target = target()->copy(arena);
        std::vector<Expression*> new_args;
        new_args.reserve(arg_count());
        for (std::size_t i = 0; i < arg_count(); ++i) {
            new_args.push_back(arg(i)->copy(arena));
        }
        auto* c = arena.create<Call>(new_target, std::move(new_args), size_bytes);
        c->set_ir_type(ir_type());
        return c;
    }

    // substitute() inherited from Operation is correct for Call.

    /// The function target expression (typically a Constant with the address or a Variable).
    Expression* target() const { return operands()[0]; }

    /// The argument expressions (operands[1..N]).
    std::size_t arg_count() const { return operands().size() > 0 ? operands().size() - 1 : 0; }
    Expression* arg(std::size_t i) const { return operands()[i + 1]; }

private:
    static SmallVector<Expression*, 4> build_operands_(Expression* target, std::vector<Expression*> args) {
        SmallVector<Expression*, 4> ops;
        ops.reserve(1 + args.size());
        ops.push_back(target);
        for (auto* a : args) ops.push_back(a);
        return ops;
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

    /// Deep-clone this instruction into the given arena.
    virtual Instruction* copy(DecompilerArena& arena) const = 0;

    /// Variables defined (written) by this instruction.
    /// Default: empty (branches, break, continue, comments define nothing).
    virtual void collect_definitions(std::unordered_set<Variable*>& out) const {
        (void)out;
    }

    // Convenience: get definitions as a vector
    std::vector<Variable*> definitions() const {
        std::unordered_set<Variable*> s;
        collect_definitions(s);
        std::vector<Variable*> out(s.begin(), s.end());
        std::stable_sort(out.begin(), out.end(), [](const Variable* lhs, const Variable* rhs) {
            if (lhs == rhs) {
                return false;
            }
            if (!lhs) {
                return false;
            }
            if (!rhs) {
                return true;
            }
            if (lhs->name() != rhs->name()) {
                return lhs->name() < rhs->name();
            }
            if (lhs->ssa_version() != rhs->ssa_version()) {
                return lhs->ssa_version() < rhs->ssa_version();
            }
            if (lhs->size_bytes != rhs->size_bytes) {
                return lhs->size_bytes < rhs->size_bytes;
            }
            if (lhs->is_aliased() != rhs->is_aliased()) {
                return !lhs->is_aliased() && rhs->is_aliased();
            }
            return false;
        });
        return out;
    }

    // Convenience: get requirements as a vector
    std::vector<Variable*> requirements() const {
        std::unordered_set<Variable*> s;
        collect_requirements(s);
        std::vector<Variable*> out(s.begin(), s.end());
        std::stable_sort(out.begin(), out.end(), [](const Variable* lhs, const Variable* rhs) {
            if (lhs == rhs) {
                return false;
            }
            if (!lhs) {
                return false;
            }
            if (!rhs) {
                return true;
            }
            if (lhs->name() != rhs->name()) {
                return lhs->name() < rhs->name();
            }
            if (lhs->ssa_version() != rhs->ssa_version()) {
                return lhs->ssa_version() < rhs->ssa_version();
            }
            if (lhs->size_bytes != rhs->size_bytes) {
                return lhs->size_bytes < rhs->size_bytes;
            }
            if (lhs->is_aliased() != rhs->is_aliased()) {
                return !lhs->is_aliased() && rhs->is_aliased();
            }
            return false;
        });
        return out;
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
        : destination_(destination), value_(value) {
        this->node_kind_ = NodeKind::Assignment;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_assignment(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        // The value (RHS) is always a requirement.
        if (value_) value_->collect_requirements(out);
        // If the destination is NOT a plain Variable (e.g., *(ptr+off) = val),
        // the destination's sub-expressions are also requirements (the pointer
        // address must be computed).
        if (destination_ && !isa<Variable>(destination_)) {
            destination_->collect_requirements(out);
        }
    }

    void collect_definitions(std::unordered_set<Variable*>& out) const override {
        if (auto* var = dyn_cast<Variable>(destination_)) {
            out.insert(var);
        }
        // ListOperation destination (e.g., [a,b] = f()) -- each element is a def
        if (auto* list = dyn_cast<ListOperation>(destination_)) {
            list->collect_requirements(out); // variables in the list are defs
        }
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* a = arena.create<Assignment>(
            destination_ ? destination_->copy(arena) : nullptr,
            value_ ? value_->copy(arena) : nullptr);
        a->set_address(address());
        a->set_ir_type(ir_type());
        return a;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        // Recursively substitute in children first
        if (value_) value_->substitute(replacee, replacement);
        if (destination_) destination_->substitute(replacee, replacement);
        // Then replace direct children
        if (value_ == replacee) value_ = replacement;
        if (destination_ == replacee) destination_ = replacement;
    }

    Expression* destination() const { return destination_; }
    Expression* value() const { return value_; }

    void set_destination(Expression* dest) { destination_ = dest; }
    void set_value(Expression* val) { value_ = val; }

    /// Rename the destination variable (for SSA renaming).
    /// Only works when destination is a Variable.
    void rename_destination(Variable* old_var, Variable* new_var) {
        if (auto* var = dyn_cast<Variable>(destination_)) {
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
        : destination_(destination), value_(value) {
        this->node_kind_ = NodeKind::Relation;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_relation(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        if (value_) out.insert(value_);
    }

    void collect_definitions(std::unordered_set<Variable*>& out) const override {
        if (destination_) out.insert(destination_);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* r = arena.create<Relation>(
            destination_ ? static_cast<Variable*>(destination_->copy(arena)) : nullptr,
            value_ ? static_cast<Variable*>(value_->copy(arena)) : nullptr);
        r->set_address(address());
        r->set_ir_type(ir_type());
        return r;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        if (value_ == replacee) {
            if (auto* v = dyn_cast<Variable>(replacement)) value_ = v;
        }
        if (destination_ == replacee) {
            if (auto* v = dyn_cast<Variable>(replacement)) destination_ = v;
        }
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
    explicit Branch(Condition* condition) : condition_(condition) {
        this->node_kind_ = NodeKind::Branch;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_branch(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        if (condition_) condition_->collect_requirements(out);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* b = arena.create<Branch>(
            condition_ ? static_cast<Condition*>(condition_->copy(arena)) : nullptr);
        b->set_address(address());
        b->set_ir_type(ir_type());
        return b;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        if (condition_) condition_->substitute(replacee, replacement);
        if (condition_ == replacee) {
            if (auto* c = dyn_cast<Condition>(replacement)) condition_ = c;
        }
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
    explicit IndirectBranch(Expression* expression) : expression_(expression) {
        this->node_kind_ = NodeKind::IndirectBranch;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_indirect_branch(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        if (expression_) expression_->collect_requirements(out);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* ib = arena.create<IndirectBranch>(
            expression_ ? expression_->copy(arena) : nullptr);
        ib->set_address(address());
        ib->set_ir_type(ir_type());
        return ib;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        if (expression_) expression_->substitute(replacee, replacement);
        if (expression_ == replacee) expression_ = replacement;
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
    explicit Return(SmallVector<Expression*, 2> values = {})
        : values_(std::move(values)) {
        this->node_kind_ = NodeKind::Return;
    }

    // Backward compat: accept std::vector rvalue
    explicit Return(std::vector<Expression*>&& values)
        : values_(std::move(values)) {
        this->node_kind_ = NodeKind::Return;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_return(this);
    }

    void collect_requirements(std::unordered_set<Variable*>& out) const override {
        for (auto* val : values_) {
            if (val) val->collect_requirements(out);
        }
    }

    Instruction* copy(DecompilerArena& arena) const override {
        SmallVector<Expression*, 2> new_vals;
        new_vals.reserve(values_.size());
        for (auto* v : values_) {
            new_vals.push_back(v ? v->copy(arena) : nullptr);
        }
        auto* r = arena.create<Return>(std::move(new_vals));
        r->set_address(address());
        r->set_ir_type(ir_type());
        return r;
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        for (auto* v : values_) {
            if (v) v->substitute(replacee, replacement);
        }
        for (auto& v : values_) {
            if (v == replacee) v = replacement;
        }
    }

    const SmallVector<Expression*, 2>& values() const { return values_; }
    SmallVector<Expression*, 2>& mutable_values() { return values_; }
    bool has_value() const { return !values_.empty(); }

private:
    SmallVector<Expression*, 2> values_;
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
        : Assignment(destination, operands) {
        this->node_kind_ = NodeKind::Phi;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_phi(this);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* new_dest = destination_ ? static_cast<Variable*>(destination_->copy(arena)) : nullptr;
        auto* new_ops = value_ ? static_cast<ListOperation*>(value_->copy(arena)) : nullptr;
        auto* p = arena.create<Phi>(new_dest, new_ops);
        p->set_address(address());
        p->set_ir_type(ir_type());
        // Note: origin_block_ is NOT deep-copied -- it maps BasicBlock* -> Expression*
        // and the BasicBlock pointers are shared references, not owned.
        // The caller must update origin_block after copying if needed.
        return p;
    }

    // substitute() inherited from Assignment handles destination_ and value_.

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
// MemPhi -- Memory phi wrapper used before preprocessing conversion
// =============================================================================

class MemPhi : public Phi {
public:
    MemPhi(Variable* destination, ListOperation* operands)
        : Phi(destination, operands) {
        this->node_kind_ = NodeKind::MemPhi;
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* new_dest = dest_var() ? static_cast<Variable*>(dest_var()->copy(arena)) : nullptr;
        auto* new_ops = operand_list() ? static_cast<ListOperation*>(operand_list()->copy(arena)) : nullptr;
        auto* p = arena.create<MemPhi>(new_dest, new_ops);
        p->set_address(address());
        p->set_ir_type(ir_type());
        p->update_phi_function(origin_block());
        return p;
    }

    // MemPhi is an intermediate pseudo-instruction; preprocessing converts it
    // to regular Phi instructions and therefore substitution is intentionally
    // disabled.
    void substitute(Expression* replacee, Expression* replacement) override {
        (void)replacee;
        (void)replacement;
    }

    std::vector<Phi*> create_phi_functions_for_variables(
        const std::vector<Variable*>& variables,
        DecompilerArena& arena) const {
        std::vector<Phi*> phis;
        auto* mem_dest = dest_var();
        auto* mem_sources = operand_list();
        if (!mem_dest || !mem_sources) {
            return phis;
        }

        for (Variable* base : variables) {
            if (!base) {
                continue;
            }

            auto* target = static_cast<Variable*>(base->copy(arena));
            target->set_ssa_version(mem_dest->ssa_version());
            target->set_aliased(true);

            std::vector<Expression*> phi_args;
            phi_args.reserve(mem_sources->size());
            for (Expression* src_expr : mem_sources->operands()) {
                if (auto* src_var = dyn_cast<Variable>(src_expr)) {
                    auto* arg = static_cast<Variable*>(base->copy(arena));
                    arg->set_ssa_version(src_var->ssa_version());
                    arg->set_aliased(true);
                    phi_args.push_back(arg);
                }
            }

            auto* value = arena.create<ListOperation>(std::move(phi_args), base->size_bytes);
            auto* phi = arena.create<Phi>(target, value);
            phi->set_address(address());
            phi->set_ir_type(base->ir_type());

            std::unordered_map<BasicBlock*, Expression*> origins;
            for (const auto& [pred, mem_expr] : origin_block()) {
                auto* mem_var = dyn_cast<Variable>(mem_expr);
                if (!pred || !mem_var) {
                    continue;
                }
                auto* arg = static_cast<Variable*>(base->copy(arena));
                arg->set_ssa_version(mem_var->ssa_version());
                arg->set_aliased(true);
                origins[pred] = arg;
            }
            if (!origins.empty()) {
                phi->update_phi_function(origins);
            }

            phis.push_back(phi);
        }

        return phis;
    }
};

// =============================================================================
// BreakInstr -- break statement (exits a loop)
// =============================================================================
// Named BreakInstr to avoid conflict with C++ `break` keyword and the AST
// BreakNode class.

class BreakInstr : public Instruction {
public:
    BreakInstr() { this->node_kind_ = NodeKind::BreakInstr; }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_break(this);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* b = arena.create<BreakInstr>();
        b->set_address(address());
        return b;
    }
};

// =============================================================================
// ContinueInstr -- continue statement (jumps to loop head)
// =============================================================================

class ContinueInstr : public Instruction {
public:
    ContinueInstr() { this->node_kind_ = NodeKind::ContinueInstr; }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_continue(this);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* c = arena.create<ContinueInstr>();
        c->set_address(address());
        return c;
    }
};

// =============================================================================
// Comment -- annotation / debug note (no semantic effect)
// =============================================================================

class Comment : public Instruction {
public:
    explicit Comment(std::string message) : message_(std::move(message)) {
        this->node_kind_ = NodeKind::Comment;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit_comment(this);
    }

    Instruction* copy(DecompilerArena& arena) const override {
        auto* c = arena.create<Comment>(message_);
        c->set_address(address());
        return c;
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

inline void DataflowObjectVisitorInterface::visit(Call* o) {
    // Fall back to generic Operation visitor since Call IS-A Operation.
    visit(static_cast<Operation*>(o));
}

inline void DataflowObjectVisitorInterface::visit(GlobalVariable* v) {
    // Fall back to Variable visitor since GlobalVariable IS-A Variable.
    visit(static_cast<Variable*>(v));
}

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

// =============================================================================
// Expression* / Instruction* overloads for isa/cast/dyn_cast
// =============================================================================
// Placed here (after all class definitions) so static_cast knows the
// inheritance relationship between Expression/Instruction and DataflowObject.

template <typename T>
inline bool isa(const Expression* obj) { return isa<T>(static_cast<const DataflowObject*>(obj)); }
template <typename T>
inline T* dyn_cast(Expression* obj) { return dyn_cast<T>(static_cast<DataflowObject*>(obj)); }
template <typename T>
inline const T* dyn_cast(const Expression* obj) { return dyn_cast<T>(static_cast<const DataflowObject*>(obj)); }
template <typename T>
inline T* cast(Expression* obj) { return cast<T>(static_cast<DataflowObject*>(obj)); }

template <typename T>
inline bool isa(const Instruction* obj) { return isa<T>(static_cast<const DataflowObject*>(obj)); }
template <typename T>
inline T* dyn_cast(Instruction* obj) { return dyn_cast<T>(static_cast<DataflowObject*>(obj)); }
template <typename T>
inline const T* dyn_cast(const Instruction* obj) { return dyn_cast<T>(static_cast<const DataflowObject*>(obj)); }
template <typename T>
inline T* cast(Instruction* obj) { return cast<T>(static_cast<DataflowObject*>(obj)); }

// =============================================================================
// Hash-based expression fingerprint -- O(1) per node, no string allocation
// =============================================================================
// Use this instead of string-based expression_fingerprint() in hot paths.
// Returns a uint64_t hash suitable for use in unordered_map keys.

inline std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) {
    // boost::hash_combine equivalent
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 12) + (seed >> 4);
    return seed;
}

inline std::uint64_t expression_fingerprint_hash(const Expression* expr) {
    if (!expr) return 0;

    switch (expr->node_kind()) {
    case NodeKind::Constant: {
        auto* c = static_cast<const Constant*>(expr);
        std::uint64_t h = hash_combine(0x100, c->value());
        return hash_combine(h, c->size_bytes);
    }
    case NodeKind::Variable: {
        auto* v = static_cast<const Variable*>(expr);
        // Hash the name using std::hash (typically FNV or similar) + ssa version
        std::uint64_t h = hash_combine(0x200, std::hash<std::string>{}(v->name()));
        return hash_combine(h, v->ssa_version());
    }
    case NodeKind::GlobalVariable: {
        auto* v = static_cast<const GlobalVariable*>(expr);
        std::uint64_t h = hash_combine(0x300, std::hash<std::string>{}(v->name()));
        return hash_combine(h, v->ssa_version());
    }
    case NodeKind::ListOperation: {
        auto* list = static_cast<const ListOperation*>(expr);
        std::uint64_t h = 0x400;
        for (const Expression* child : list->operands()) {
            h = hash_combine(h, expression_fingerprint_hash(child));
        }
        return h;
    }
    case NodeKind::Operation:
    case NodeKind::Call:
    case NodeKind::Condition: {
        auto* op = static_cast<const Operation*>(expr);
        std::uint64_t h = hash_combine(0x500, static_cast<std::uint64_t>(op->type()));
        for (const Expression* child : op->operands()) {
            h = hash_combine(h, expression_fingerprint_hash(child));
        }
        return h;
    }
    default:
        return 0x999;
    }
}

} // namespace aletheia
