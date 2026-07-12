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
#include <stdexcept>

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

/// Stack-safe deep clone preserving the historical tree-copy semantics.
inline Expression* clone_expression_iterative(
    const Expression* root,
    DecompilerArena& arena);

/// Stack-safe, cycle-safe requirement collection for an expression graph.
/// Defined after the concrete expression classes.
inline void collect_expression_requirements_iterative(
    const Expression* root,
    std::unordered_set<Variable*>& out);

/// Stack-safe, cycle-safe substitution in the descendants of an expression.
inline void substitute_expression_iterative(
    Expression* root,
    Expression* replacee,
    Expression* replacement);

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
        return clone_expression_iterative(this, arena);
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
        return clone_expression_iterative(this, arena);
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
        return clone_expression_iterative(this, arena);
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        substitute_expression_iterative(this, replacee, replacement);
    }

    Expression* initial_value() const { return initial_value_; }
    void set_initial_value(Expression* value) { initial_value_ = value; }

    bool is_constant() const { return is_constant_; }
    void set_is_constant(bool value) { is_constant_ = value; }

    /// True when this node denotes the address of the named storage object,
    /// rather than the stored value itself.  Raw binary address constants and
    /// source-level global lvalues must remain distinguishable in portable C.
    bool represents_address() const { return represents_address_; }
    void set_represents_address(bool value) { represents_address_ = value; }

private:
    Expression* initial_value_ = nullptr;
    bool is_constant_ = false;
    bool represents_address_ = false;
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
    bitcast,                // reinterpret bits without numeric conversion
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

    // Pure bit-count intrinsics. Appended to preserve legacy enum ordinals.
    popcount,               // population count; operands are value, bit width
    lzcount,                // leading-zero count; operands are value, bit width

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
        collect_expression_requirements_iterative(this, out);
    }

    Expression* copy(DecompilerArena& arena) const override {
        return clone_expression_iterative(this, arena);
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        substitute_expression_iterative(this, replacee, replacement);
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
        collect_expression_requirements_iterative(this, out);
    }

    Expression* copy(DecompilerArena& arena) const override {
        return clone_expression_iterative(this, arena);
    }

    void substitute(Expression* replacee, Expression* replacement) override {
        substitute_expression_iterative(this, replacee, replacement);
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
        return clone_expression_iterative(this, arena);
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
        return clone_expression_iterative(this, arena);
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
// Cycle-safe structural expression-graph fingerprint. Resolved type metadata
// contributes its canonical spelling, so fingerprinting may allocate while
// formatting a type.
// =============================================================================
// Use this instead of string-based expression_fingerprint() in hot paths.
// Returns a uint64_t hash suitable for use in unordered_map keys.

inline std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) {
    // boost::hash_combine equivalent
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 12) + (seed >> 4);
    return seed;
}

inline std::uint64_t expression_type_fingerprint(const TypePtr& type) {
    if (!type) return 0;
    std::uint64_t h = hash_combine(
        0x7a11,
        static_cast<std::uint64_t>(type->type_kind()));
    h = hash_combine(h, type->size());
    // Kind and width do not distinguish signed integers, pointer pointees,
    // custom types, or function signatures. The canonical type spelling is
    // stable value metadata and must participate in semantic equivalence.
    return hash_combine(h, std::hash<std::string>{}(type->to_string()));
}

namespace expression_graph_detail {

struct FingerprintState {
    std::unordered_map<const Expression*, std::uint64_t> completed_hashes;
    std::unordered_set<const Expression*> active;
    bool cycle_detected = false;
};

inline std::uint64_t fingerprint_variable(
    std::uint64_t hash,
    const Variable* variable) {
    hash = hash_combine(
        hash, std::hash<std::string>{}(variable->name()));
    hash = hash_combine(hash, variable->ssa_version());
    hash = hash_combine(
        hash, static_cast<std::uint64_t>(variable->kind()));
    hash = hash_combine(
        hash, static_cast<std::uint64_t>(variable->stack_offset()));
    hash = hash_combine(
        hash, static_cast<std::uint64_t>(variable->parameter_index()));
    return hash_combine(hash, variable->is_aliased() ? 1 : 0);
}

inline std::uint64_t fingerprint_expression_graph(
    const Expression* expression,
    FingerprintState& state) {
    if (!expression) return 0x6e756c6cULL;

    if (auto completed = state.completed_hashes.find(expression);
        completed != state.completed_hashes.end()) {
        // Reusing the completed subtree hash makes DAG sharing semantically
        // equivalent to duplicating the same pure expression tree.
        return completed->second;
    }
    if (!state.active.insert(expression).second) {
        state.cycle_detected = true;
        return 0x6379636c65ULL;
    }

    std::uint64_t hash = 0x6e6f6465ULL;
    hash = hash_combine(
        hash, static_cast<std::uint64_t>(expression->node_kind()));
    hash = hash_combine(
        hash, expression_type_fingerprint(expression->ir_type()));

    switch (expression->node_kind()) {
    case NodeKind::Constant:
        hash = hash_combine(
            hash, static_cast<const Constant*>(expression)->value());
        hash = hash_combine(hash, expression->size_bytes);
        break;
    case NodeKind::Variable:
        hash = fingerprint_variable(
            hash, static_cast<const Variable*>(expression));
        break;
    case NodeKind::GlobalVariable: {
        const auto* global =
            static_cast<const GlobalVariable*>(expression);
        hash = fingerprint_variable(hash, global);
        hash = hash_combine(
            hash,
            fingerprint_expression_graph(global->initial_value(), state));
        break;
    }
    case NodeKind::ListOperation: {
        const auto& operands =
            static_cast<const ListOperation*>(expression)->operands();
        hash = hash_combine(hash, operands.size());
        for (const Expression* child : operands) {
            hash = hash_combine(
                hash, fingerprint_expression_graph(child, state));
        }
        break;
    }
    case NodeKind::Operation:
    case NodeKind::Call:
    case NodeKind::Condition: {
        const auto* operation =
            static_cast<const Operation*>(expression);
        hash = hash_combine(
            hash, static_cast<std::uint64_t>(operation->type()));
        hash = hash_combine(hash, operation->operands().size());
        for (const Expression* child : operation->operands()) {
            hash = hash_combine(
                hash, fingerprint_expression_graph(child, state));
        }

        const auto& access = operation->array_access();
        hash = hash_combine(hash, access.has_value() ? 1 : 0);
        if (access) {
            hash = hash_combine(hash, access->element_size);
            hash = hash_combine(hash, access->confidence ? 1 : 0);
            hash = hash_combine(
                hash, fingerprint_expression_graph(access->base, state));
            hash = hash_combine(
                hash, fingerprint_expression_graph(access->index, state));
        }
        break;
    }
    default:
        hash = hash_combine(hash, 0x756e6b6e6f776eULL);
        break;
    }

    state.active.erase(expression);
    state.completed_hashes.emplace(expression, hash);
    return hash;
}

struct ExpressionPair {
    const Expression* lhs = nullptr;
    const Expression* rhs = nullptr;

    bool operator==(const ExpressionPair&) const = default;
};

struct ExpressionPairHash {
    std::size_t operator()(const ExpressionPair& pair) const {
        std::size_t hash = std::hash<const Expression*>{}(pair.lhs);
        return hash ^ (
            std::hash<const Expression*>{}(pair.rhs)
            + 0x9e3779b9U + (hash << 6) + (hash >> 2));
    }
};

struct EqualityState {
    std::unordered_map<const Expression*, const Expression*> active_lhs_to_rhs;
    std::unordered_map<const Expression*, const Expression*> active_rhs_to_lhs;
    std::unordered_set<ExpressionPair, ExpressionPairHash> completed_pairs;
    std::size_t cycle_references = 0;
};

inline bool variables_equal(
    const Variable* lhs,
    const Variable* rhs) {
    return lhs->name() == rhs->name()
        && lhs->ssa_version() == rhs->ssa_version()
        && lhs->kind() == rhs->kind()
        && lhs->stack_offset() == rhs->stack_offset()
        && lhs->parameter_index() == rhs->parameter_index()
        && lhs->is_aliased() == rhs->is_aliased();
}

inline bool expressions_equal(
    const Expression* lhs,
    const Expression* rhs,
    EqualityState& state);

template <typename LeftOperands, typename RightOperands>
inline bool operand_lists_equal(
    const LeftOperands& lhs_operands,
    const RightOperands& rhs_operands,
    EqualityState& state) {
    if (lhs_operands.size() != rhs_operands.size()) return false;
    for (std::size_t index = 0; index < lhs_operands.size(); ++index) {
        if (!expressions_equal(
                lhs_operands[index], rhs_operands[index], state)) {
            return false;
        }
    }
    return true;
}

inline bool expressions_equal(
    const Expression* lhs,
    const Expression* rhs,
    EqualityState& state) {
    if (!lhs || !rhs) return lhs == rhs;

    auto lhs_active = state.active_lhs_to_rhs.find(lhs);
    auto rhs_active = state.active_rhs_to_lhs.find(rhs);
    if (lhs_active != state.active_lhs_to_rhs.end()
        || rhs_active != state.active_rhs_to_lhs.end()) {
        const bool corresponding =
            lhs_active != state.active_lhs_to_rhs.end()
            && rhs_active != state.active_rhs_to_lhs.end()
            && lhs_active->second == rhs
            && rhs_active->second == lhs;
        if (corresponding) ++state.cycle_references;
        return corresponding;
    }
    const ExpressionPair pair{lhs, rhs};
    if (state.completed_pairs.contains(pair)) {
        return true;
    }
    const std::size_t cycle_references_before = state.cycle_references;

    if (lhs->node_kind() != rhs->node_kind()
        || lhs->size_bytes != rhs->size_bytes) {
        return false;
    }
    const TypePtr lhs_type = lhs->ir_type();
    const TypePtr rhs_type = rhs->ir_type();
    if (static_cast<bool>(lhs_type) != static_cast<bool>(rhs_type)
        || (lhs_type && *lhs_type != *rhs_type)) {
        return false;
    }

    // Active-path correspondence terminates cycles and distinguishes different
    // cycle topology. Completed pairs may be compared again elsewhere, making
    // shared DAG nodes equivalent to duplicated pure subexpressions.
    state.active_lhs_to_rhs.emplace(lhs, rhs);
    state.active_rhs_to_lhs.emplace(rhs, lhs);

    bool equal = false;
    switch (lhs->node_kind()) {
    case NodeKind::Constant:
        equal = static_cast<const Constant*>(lhs)->value()
            == static_cast<const Constant*>(rhs)->value();
        break;
    case NodeKind::Variable:
        equal = variables_equal(
            static_cast<const Variable*>(lhs),
            static_cast<const Variable*>(rhs));
        break;
    case NodeKind::GlobalVariable: {
        const auto* lhs_global =
            static_cast<const GlobalVariable*>(lhs);
        const auto* rhs_global =
            static_cast<const GlobalVariable*>(rhs);
        equal = variables_equal(lhs_global, rhs_global)
            && lhs_global->is_constant() == rhs_global->is_constant()
            && lhs_global->represents_address()
                == rhs_global->represents_address()
            && expressions_equal(
                lhs_global->initial_value(),
                rhs_global->initial_value(),
                state);
        break;
    }
    case NodeKind::ListOperation:
        equal = operand_lists_equal(
            static_cast<const ListOperation*>(lhs)->operands(),
            static_cast<const ListOperation*>(rhs)->operands(),
            state);
        break;
    case NodeKind::Operation:
    case NodeKind::Call:
    case NodeKind::Condition: {
        const auto* lhs_operation =
            static_cast<const Operation*>(lhs);
        const auto* rhs_operation =
            static_cast<const Operation*>(rhs);
        equal = lhs_operation->type() == rhs_operation->type()
            && operand_lists_equal(
                lhs_operation->operands(),
                rhs_operation->operands(),
                state);
        if (equal) {
            const auto& lhs_access = lhs_operation->array_access();
            const auto& rhs_access = rhs_operation->array_access();
            equal = lhs_access.has_value() == rhs_access.has_value();
            if (equal && lhs_access) {
                equal =
                    lhs_access->element_size == rhs_access->element_size
                    && lhs_access->confidence == rhs_access->confidence
                    && expressions_equal(
                        lhs_access->base, rhs_access->base, state)
                    && expressions_equal(
                        lhs_access->index, rhs_access->index, state);
            }
        }
        break;
    }
    default:
        equal = false;
        break;
    }

    state.active_lhs_to_rhs.erase(lhs);
    state.active_rhs_to_lhs.erase(rhs);
    if (equal
        && state.cycle_references == cycle_references_before) {
        state.completed_pairs.insert(pair);
    }
    return equal;
}

} // namespace expression_graph_detail

// Rooted ordered graph hashing is O(V + E) time and O(V + H) auxiliary
// storage for V expression nodes, E child edges, and recursion height H.
inline std::uint64_t expression_fingerprint_hash(
    const Expression* expression) {
    expression_graph_detail::FingerprintState state;
    const std::uint64_t fingerprint =
        expression_graph_detail::fingerprint_expression_graph(
        expression, state);
    // Cyclic expressions are malformed for evaluation, but hashing them must
    // terminate. A shared sentinel preserves the equality/hash contract; exact
    // graph comparison remains the authority within this collision bucket.
    return state.cycle_detected ? 0x6379636c6963ULL : fingerprint;
}

// Exact comparison is O(V + E) expected time for acyclic graphs. A cyclic pair
// that depends on active-path correspondence is not memoized across contexts;
// the conservative cyclic worst case is O(VE). Auxiliary storage is O(V + H).
// Hashes may reject unequal graphs quickly, but a matching hash never
// establishes equality on its own.
inline bool expression_structurally_equal_exact(
    const Expression* lhs,
    const Expression* rhs) {
    if (lhs == rhs) return true;
    expression_graph_detail::EqualityState state;
    return expression_graph_detail::expressions_equal(
        lhs, rhs, state);
}

inline bool expressions_structurally_equal(
    const Expression* lhs,
    const Expression* rhs) {
    if (lhs == rhs) return true;
    if (!lhs || !rhs) return false;
    return expression_fingerprint_hash(lhs) == expression_fingerprint_hash(rhs)
        && expression_structurally_equal_exact(lhs, rhs);
}

inline std::vector<const Expression*> expression_graph_children(
    const Expression* expression) {
    std::vector<const Expression*> children;
    if (!expression) return children;
    if (const auto* global = dyn_cast<GlobalVariable>(expression)) {
        if (global->initial_value()) {
            children.push_back(global->initial_value());
        }
        return children;
    }
    if (const auto* list = dyn_cast<ListOperation>(expression)) {
        children.reserve(list->operands().size());
        for (const Expression* child : list->operands()) {
            if (child) children.push_back(child);
        }
        return children;
    }
    if (const auto* operation = dyn_cast<Operation>(expression)) {
        children.reserve(
            operation->operands().size()
            + (operation->array_access() ? 2 : 0));
        for (const Expression* child : operation->operands()) {
            if (child) children.push_back(child);
        }
        if (const auto& access = operation->array_access()) {
            if (access->base) children.push_back(access->base);
            if (access->index) children.push_back(access->index);
        }
    }
    return children;
}

namespace expression_clone_detail {

inline constexpr std::size_t kMaxCloneDepth = 65'536;
inline constexpr std::size_t kMaxCloneExpansions = 1'000'000;

inline std::vector<const Expression*> children(const Expression* expression) {
    std::vector<const Expression*> result;
    if (!expression) return result;
    if (const auto* global = dyn_cast<GlobalVariable>(expression)) {
        if (global->initial_value()) result.push_back(global->initial_value());
    } else if (const auto* list = dyn_cast<ListOperation>(expression)) {
        result.reserve(list->operands().size());
        for (const Expression* child : list->operands()) {
            result.push_back(child);
        }
    } else if (const auto* operation = dyn_cast<Operation>(expression)) {
        result.reserve(operation->operands().size());
        for (const Expression* child : operation->operands()) {
            result.push_back(child);
        }
    }
    return result;
}

struct AnnotationPaths {
    std::optional<std::vector<std::size_t>> base;
    std::optional<std::vector<std::size_t>> index;
};

using AnnotationPathMap =
    std::unordered_map<const Operation*, AnnotationPaths>;

inline const Expression* child_at(
    const Expression* expression,
    std::size_t index) {
    if (const auto* global = dyn_cast<GlobalVariable>(expression)) {
        return index == 0 ? global->initial_value() : nullptr;
    }
    if (const auto* list = dyn_cast<ListOperation>(expression)) {
        return index < list->operands().size()
            ? list->operands()[index] : nullptr;
    }
    if (const auto* operation = dyn_cast<Operation>(expression)) {
        return index < operation->operands().size()
            ? operation->operands()[index] : nullptr;
    }
    return nullptr;
}

inline Expression* child_at(Expression* expression, std::size_t index) {
    return const_cast<Expression*>(child_at(
        static_cast<const Expression*>(expression), index));
}

inline std::optional<std::vector<std::size_t>> find_path(
    const Expression* root,
    const Expression* target) {
    if (!root || !target) return std::nullopt;
    if (root == target) return std::vector<std::size_t>{};
    struct ParentEdge {
        const Expression* parent = nullptr;
        std::size_t index = 0;
    };
    std::unordered_map<const Expression*, ParentEdge> parents;
    std::vector<const Expression*> work{root};
    parents.emplace(root, ParentEdge{});
    while (!work.empty()) {
        const Expression* expression = work.back();
        work.pop_back();
        const auto semantic_children = children(expression);
        for (std::size_t index = 0;
             index < semantic_children.size();
             ++index) {
            const Expression* child = semantic_children[index];
            if (!child || parents.contains(child)) continue;
            parents.emplace(child, ParentEdge{expression, index});
            if (child == target) {
                std::vector<std::size_t> reversed;
                const Expression* cursor = target;
                while (cursor != root) {
                    const ParentEdge& edge = parents.at(cursor);
                    reversed.push_back(edge.index);
                    cursor = edge.parent;
                }
                return std::vector<std::size_t>(
                    reversed.rbegin(), reversed.rend());
            }
            work.push_back(child);
        }
    }
    return std::nullopt;
}

inline Expression* follow_path(
    Expression* root,
    const std::vector<std::size_t>& path) {
    Expression* cursor = root;
    for (std::size_t index : path) {
        cursor = child_at(cursor, index);
        if (!cursor) return nullptr;
    }
    return cursor;
}

inline void validate_annotation(
    const Expression* expression,
    AnnotationPathMap& paths) {
    const auto* operation = dyn_cast<Operation>(expression);
    if (!operation || !operation->array_access()
        || paths.contains(operation)) {
        return;
    }
    const ArrayAccessInfo& access = *operation->array_access();
    AnnotationPaths annotation;
    if (access.base) {
        annotation.base = find_path(operation, access.base);
        if (!annotation.base) {
            throw std::runtime_error(
                "expression clone rejected detached array-access base");
        }
    }
    if (access.index) {
        if (access.index == operation) {
            throw std::runtime_error(
                "expression clone rejected self-referential array-access index");
        }
        annotation.index = find_path(operation, access.index);
        if (!annotation.index) {
            throw std::runtime_error(
                "expression clone rejected detached array-access index");
        }
    }
    paths.emplace(operation, std::move(annotation));
}

inline AnnotationPathMap validate(const Expression* root) {
    AnnotationPathMap annotation_paths;
    if (!root) return annotation_paths;
    struct Frame {
        const Expression* source = nullptr;
        std::vector<const Expression*> children;
        std::size_t next_child = 0;
    };
    std::unordered_set<const Expression*> active;
    std::vector<Frame> stack;
    active.insert(root);
    validate_annotation(root, annotation_paths);
    stack.push_back({root, children(root), 0});
    std::size_t expansions = 1;
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            active.erase(frame.source);
            stack.pop_back();
            continue;
        }
        const Expression* child = frame.children[frame.next_child++];
        if (!child) continue;
        if (active.contains(child)) {
            throw std::runtime_error(
                "expression clone rejected cyclic source graph");
        }
        if (++expansions > kMaxCloneExpansions) {
            throw std::runtime_error(
                "expression clone exceeded expansion limit "
                + std::to_string(kMaxCloneExpansions));
        }
        if (stack.size() >= kMaxCloneDepth) {
            throw std::runtime_error(
                "expression clone exceeded depth limit "
                + std::to_string(kMaxCloneDepth));
        }
        active.insert(child);
        validate_annotation(child, annotation_paths);
        stack.push_back({child, children(child), 0});
    }
    return annotation_paths;
}

inline Expression* shallow_clone(
    const Expression* source,
    DecompilerArena& arena) {
    if (!source) return nullptr;
    Expression* clone = nullptr;
    if (const auto* global = dyn_cast<GlobalVariable>(source)) {
        auto* result = arena.create<GlobalVariable>(
            global->name(), global->size_bytes, nullptr, global->is_constant());
        result->set_ssa_version(global->ssa_version());
        result->set_aliased(global->is_aliased());
        result->set_represents_address(global->represents_address());
        clone = result;
    } else if (const auto* variable = dyn_cast<Variable>(source)) {
        auto* result = arena.create<Variable>(
            variable->name(), variable->size_bytes);
        result->set_ssa_version(variable->ssa_version());
        result->set_aliased(variable->is_aliased());
        result->set_kind(variable->kind());
        result->set_stack_offset(variable->stack_offset());
        result->set_parameter_index(variable->parameter_index());
        clone = result;
    } else if (const auto* constant = dyn_cast<Constant>(source)) {
        clone = arena.create<Constant>(constant->value(), constant->size_bytes);
    } else if (const auto* condition = dyn_cast<Condition>(source)) {
        clone = arena.create<Condition>(
            condition->type(), nullptr, nullptr, condition->size_bytes);
    } else if (const auto* call = dyn_cast<Call>(source)) {
        clone = arena.create<Call>(
            nullptr,
            std::vector<Expression*>(call->arg_count(), nullptr),
            call->size_bytes);
    } else if (const auto* list = dyn_cast<ListOperation>(source)) {
        clone = arena.create<ListOperation>(
            std::vector<Expression*>(list->operands().size(), nullptr),
            list->size_bytes);
    } else if (const auto* operation = dyn_cast<Operation>(source)) {
        clone = arena.create<Operation>(
            operation->type(),
            std::vector<Expression*>(operation->operands().size(), nullptr),
            operation->size_bytes);
    } else {
        throw std::runtime_error(
            "expression clone encountered unknown node kind "
            + std::to_string(static_cast<int>(source->node_kind())));
    }
    clone->set_ir_type(source->ir_type());
    return clone;
}

inline void attach_child(
    Expression* parent,
    std::size_t index,
    Expression* child) {
    if (auto* global = dyn_cast<GlobalVariable>(parent)) {
        if (index != 0) {
            throw std::runtime_error("invalid global clone child index");
        }
        global->set_initial_value(child);
    } else if (auto* list = dyn_cast<ListOperation>(parent)) {
        if (index >= list->mutable_operands().size()) {
            throw std::runtime_error("invalid list clone child index");
        }
        list->mutable_operands()[index] = child;
    } else if (auto* operation = dyn_cast<Operation>(parent)) {
        if (index >= operation->mutable_operands().size()) {
            throw std::runtime_error("invalid operation clone child index");
        }
        operation->mutable_operands()[index] = child;
    } else {
        throw std::runtime_error("invalid expression clone parent");
    }
}

inline void remap_annotation(
    const Expression* source,
    Expression* clone,
    const AnnotationPathMap& paths) {
    const auto* source_operation = dyn_cast<Operation>(source);
    auto* clone_operation = dyn_cast<Operation>(clone);
    if (!source_operation || !clone_operation
        || !source_operation->array_access()) {
        return;
    }
    const auto path = paths.find(source_operation);
    if (path == paths.end()) {
        throw std::runtime_error(
            "missing preflighted array-access clone paths");
    }
    const ArrayAccessInfo& source_access = *source_operation->array_access();
    ArrayAccessInfo cloned_access{
        .base = nullptr,
        .index = nullptr,
        .element_size = source_access.element_size,
        .confidence = source_access.confidence,
    };
    if (path->second.base) {
        cloned_access.base = dyn_cast<Variable>(
            follow_path(clone_operation, *path->second.base));
        if (!cloned_access.base) {
            throw std::runtime_error(
                "array-access base clone did not preserve variable kind");
        }
    }
    if (path->second.index) {
        cloned_access.index = follow_path(
            clone_operation, *path->second.index);
        if (!cloned_access.index) {
            throw std::runtime_error(
                "array-access index clone path is invalid");
        }
    }
    clone_operation->set_array_access(std::move(cloned_access));
}

} // namespace expression_clone_detail

inline Expression* clone_expression_iterative(
    const Expression* root,
    DecompilerArena& arena) {
    if (!root) return nullptr;
    const auto annotation_paths = expression_clone_detail::validate(root);

    struct Frame {
        const Expression* source = nullptr;
        Expression* clone = nullptr;
        std::vector<const Expression*> children;
        std::size_t next_child = 0;
    };
    Expression* root_clone =
        expression_clone_detail::shallow_clone(root, arena);
    std::vector<Frame> stack;
    stack.push_back({
        root,
        root_clone,
        expression_clone_detail::children(root),
        0,
    });
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            expression_clone_detail::remap_annotation(
                frame.source, frame.clone, annotation_paths);
            stack.pop_back();
            continue;
        }
        const std::size_t child_index = frame.next_child++;
        const Expression* source_child = frame.children[child_index];
        if (!source_child) continue;
        Expression* child_clone =
            expression_clone_detail::shallow_clone(source_child, arena);
        expression_clone_detail::attach_child(
            frame.clone, child_index, child_clone);
        stack.push_back({
            source_child,
            child_clone,
            expression_clone_detail::children(source_child),
            0,
        });
    }
    return root_clone;
}

// Enumerates requirement variables without recursion. This preserves the
// virtual collect_requirements semantics: GlobalVariable is a leaf, Call
// targets are requirements, and member/field labels are metadata rather than
// runtime dependencies. O(V + E) expected time and O(V) auxiliary storage.
inline void collect_expression_requirements_iterative(
    const Expression* root,
    std::unordered_set<Variable*>& out) {
    if (!root) return;
    std::unordered_set<const Expression*> visited;
    std::vector<const Expression*> work{root};
    while (!work.empty()) {
        const Expression* expression = work.back();
        work.pop_back();
        if (!expression || !visited.insert(expression).second) continue;
        if (const auto* variable = dyn_cast<Variable>(expression)) {
            out.insert(const_cast<Variable*>(variable));
            continue;
        }
        if (const auto* list = dyn_cast<ListOperation>(expression)) {
            for (const Expression* child : list->operands()) {
                if (child) work.push_back(child);
            }
            continue;
        }
        if (const auto* operation = dyn_cast<Operation>(expression)) {
            for (std::size_t index = 0;
                 index < operation->operands().size();
                 ++index) {
                if (index == 1
                    && (operation->type() == OperationType::member_access
                        || operation->type() == OperationType::field)) {
                    continue;
                }
                if (const Expression* child = operation->operands()[index]) {
                    work.push_back(child);
                }
            }
        }
    }
}

// Mutates existing descendants without traversing into the replacement graph,
// matching the former recursive post-order semantics. Each existing node is
// visited once, so cyclic or shared IR cannot recurse indefinitely or expand
// exponentially. O(V + E) expected time and O(V) auxiliary storage.
inline void substitute_expression_iterative(
    Expression* root,
    Expression* replacee,
    Expression* replacement) {
    if (!root || !replacee || replacee == replacement) return;
    std::unordered_set<Expression*> visited;
    std::vector<Expression*> work{root};
    while (!work.empty()) {
        Expression* expression = work.back();
        work.pop_back();
        if (!expression || !visited.insert(expression).second) continue;

        if (auto* global = dyn_cast<GlobalVariable>(expression)) {
            Expression* child = global->initial_value();
            if (child == replacee) {
                global->set_initial_value(replacement);
            } else if (child) {
                work.push_back(child);
            }
            continue;
        }
        if (auto* list = dyn_cast<ListOperation>(expression)) {
            for (Expression*& child : list->mutable_operands()) {
                if (child == replacee) {
                    child = replacement;
                } else if (child) {
                    work.push_back(child);
                }
            }
            continue;
        }
        if (auto* operation = dyn_cast<Operation>(expression)) {
            for (Expression*& child : operation->mutable_operands()) {
                if (child == replacee) {
                    child = replacement;
                } else if (child) {
                    work.push_back(child);
                }
            }
            if (operation->array_access()) {
                ArrayAccessInfo access = *operation->array_access();
                if (access.base == replacee) {
                    if (auto* variable = dyn_cast<Variable>(replacement)) {
                        access.base = variable;
                    }
                } else if (access.base) {
                    work.push_back(access.base);
                }
                if (access.index == replacee) {
                    access.index = replacement;
                } else if (access.index) {
                    work.push_back(access.index);
                }
                operation->set_array_access(std::move(access));
            }
        }
    }
}

// Enumerates every distinct variable node reachable from an expression root.
// The explicit visited set makes this O(V + E), preserves shared DAG nodes,
// tolerates malformed cycles, and cannot exhaust the native call stack.
inline std::vector<const Variable*> expression_graph_variables(
    const Expression* root) {
    std::vector<const Variable*> variables;
    if (!root) return variables;

    std::unordered_set<const Expression*> visited;
    std::vector<const Expression*> work{root};
    while (!work.empty()) {
        const Expression* expression = work.back();
        work.pop_back();
        if (!expression || !visited.insert(expression).second) continue;
        if (const auto* variable = dyn_cast<Variable>(expression)) {
            variables.push_back(variable);
        }
        auto children = expression_graph_children(expression);
        work.insert(work.end(), children.begin(), children.end());
    }
    return variables;
}

inline std::optional<std::string> variable_parameter_metadata_error(
    const Variable* variable,
    std::optional<std::size_t> declared_parameter_count = std::nullopt) {
    if (!variable || isa<GlobalVariable>(variable)) return std::nullopt;
    if (variable->is_parameter()) {
        if (variable->parameter_index() < 0) {
            return "parameter variable has no non-negative parameter index";
        }
        if (declared_parameter_count.has_value()
            && static_cast<std::size_t>(variable->parameter_index())
                >= *declared_parameter_count) {
            return "parameter index "
                + std::to_string(variable->parameter_index())
                + " is outside declared parameter count "
                + std::to_string(*declared_parameter_count);
        }
    } else if (variable->parameter_index() >= 0) {
        return "non-parameter variable carries parameter index "
            + std::to_string(variable->parameter_index());
    }
    return std::nullopt;
}

// Returns one closed cycle path (the first and last pointers are identical),
// or an empty vector for an acyclic expression DAG. The iterative three-color
// DFS is O(V + E) time and O(V + E) auxiliary storage and cannot exhaust the
// native call stack on a deeply nested expression.
inline std::vector<const Expression*> expression_graph_cycle_trace(
    const Expression* root) {
    if (!root) return {};

    enum class VisitState : std::uint8_t {
        Active,
        Complete,
    };
    struct Frame {
        const Expression* expression = nullptr;
        std::vector<const Expression*> children;
        std::size_t next_child = 0;
    };

    std::unordered_map<const Expression*, VisitState> states;
    std::unordered_map<const Expression*, std::size_t> active_indices;
    std::vector<Frame> stack;
    states.emplace(root, VisitState::Active);
    active_indices.emplace(root, 0);
    stack.push_back(Frame{root, expression_graph_children(root), 0});

    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next_child >= frame.children.size()) {
            states[frame.expression] = VisitState::Complete;
            active_indices.erase(frame.expression);
            stack.pop_back();
            continue;
        }

        const Expression* child =
            frame.children[frame.next_child++];
        auto state = states.find(child);
        if (state != states.end()
            && state->second == VisitState::Active) {
            const std::size_t cycle_start = active_indices.at(child);
            std::vector<const Expression*> trace;
            trace.reserve(stack.size() - cycle_start + 1);
            for (std::size_t index = cycle_start;
                 index < stack.size();
                 ++index) {
                trace.push_back(stack[index].expression);
            }
            trace.push_back(child);
            return trace;
        }
        if (state != states.end()) continue;

        states.emplace(child, VisitState::Active);
        active_indices.emplace(child, stack.size());
        stack.push_back(Frame{child, expression_graph_children(child), 0});
    }

    return {};
}

} // namespace aletheia
