#pragma once
#include "../../common/arena_allocated.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dewolf {

class Constant;
class Variable;
class Operation;

class DataflowObjectVisitorInterface {
public:
    virtual ~DataflowObjectVisitorInterface() = default;

    virtual void visit(Constant* c) = 0;
    virtual void visit(Variable* v) = 0;
    virtual void visit(Operation* o) = 0;
};

class DataflowObject : public ArenaAllocated {
public:
    virtual ~DataflowObject() = default;
    virtual void accept(DataflowObjectVisitorInterface& visitor) = 0;

    // Size of the object in bytes
    std::size_t size_bytes = 0;
};

class Expression : public DataflowObject {
public:
    virtual ~Expression() = default;
};

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

class Variable : public Expression {
public:
    Variable(std::string name, std::size_t size) : name_(std::move(name)) {
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
};

enum class OperationType {
    // Arithmetic
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

    // Comparison
    eq,
    neq,
    lt,
    le,
    gt,
    ge,

    // Memory
    deref,
    address_of,

    // Assignments
    assign,

    // Other
    call,
    phi,
    unknown
};

class Operation : public Expression {
public:
    Operation(OperationType type, std::vector<Expression*> operands, std::size_t size)
        : type_(type), operands_(std::move(operands)) {
        this->size_bytes = size;
    }

    void accept(DataflowObjectVisitorInterface& visitor) override {
        visitor.visit(this);
    }

    OperationType type() const { return type_; }
    const std::vector<Expression*>& operands() const { return operands_; }

private:
    OperationType type_;
    std::vector<Expression*> operands_;
};

} // namespace dewolf
