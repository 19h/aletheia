#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aletheia {

// =============================================================================
// TypeKind -- LLVM-style RTTI tag for Type hierarchy
// =============================================================================

enum class TypeKind : std::uint8_t {
    Unknown,
    Integer,
    Float,
    Pointer,
    Array,
    Custom,
    FunctionTypeDef,
    Struct,
    Union,
    Enum,
};

// =============================================================================
// Type -- Abstract base class for the Aletheia type system
// =============================================================================
// Types are immutable value objects. Unlike IR nodes (DataflowObject), types
// are NOT arena-allocated -- they are either static singletons (for common
// types like int32, void, bool) or heap-allocated via shared_ptr for
// composite types (Pointer, ArrayType, Struct, etc.).
//
// The Python reference uses frozen dataclasses. In C++ we achieve immutability
// by making all fields private/const and providing only const accessors.

class Type {
public:
    virtual ~Type() = default;

    /// Size of the type in bits (matching the Python reference convention).
    std::size_t size() const { return size_bits_; }

    /// Size in bytes (convenience, rounds up).
    std::size_t size_bytes() const { return (size_bits_ + 7) / 8; }

    /// C-like string representation of this type.
    virtual std::string to_string() const = 0;

    /// Check whether this type represents a boolean.
    bool is_boolean() const { return to_string() == "bool"; }

    /// Equality comparison (by value, not identity).
    virtual bool operator==(const Type& other) const = 0;
    bool operator!=(const Type& other) const { return !(*this == other); }

    /// LLVM-style RTTI tag.
    TypeKind type_kind() const { return type_kind_; }

protected:
    explicit Type(std::size_t size_bits) : size_bits_(size_bits) {}
    std::size_t size_bits_;
    TypeKind type_kind_;
};

// Convenience alias for shared ownership of immutable types
using TypePtr = std::shared_ptr<const Type>;

// =============================================================================
// UnknownType -- Placeholder for unresolved or testing types
// =============================================================================

class UnknownType final : public Type {
public:
    explicit UnknownType(std::size_t size_bits = 0) : Type(size_bits) { type_kind_ = TypeKind::Unknown; }

    std::string to_string() const override { return "unknown type"; }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Unknown) return false;
        auto* o = static_cast<const UnknownType*>(&other);
        return size_bits_ == o->size_bits_;
    }

    /// Singleton for zero-sized unknown type.
    static TypePtr instance() {
        static auto inst = std::make_shared<const UnknownType>(0);
        return inst;
    }
};

// =============================================================================
// Integer -- Signed or unsigned integer type
// =============================================================================

class Integer final : public Type {
public:
    Integer(std::size_t size_bits, bool is_signed)
        : Type(size_bits), signed_(is_signed) { type_kind_ = TypeKind::Integer; }

    bool is_signed() const { return signed_; }

    std::string to_string() const override {
        static const std::unordered_map<std::size_t, const char*> size_types = {
            {8, "char"}, {16, "short"}, {32, "int"}, {64, "long"}
        };
        if (auto it = size_types.find(size_bits_); it != size_types.end()) {
            return signed_ ? std::string(it->second)
                           : "unsigned " + std::string(it->second);
        }
        // Fallback: e.g., int128_t or uint128_t
        return (signed_ ? "" : "u") + std::string("int") +
               std::to_string(size_bits_) + "_t";
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Integer) return false;
        auto* o = static_cast<const Integer*>(&other);
        return size_bits_ == o->size_bits_ && signed_ == o->signed_;
    }

    // ---- Factory methods matching the Python reference ----
    static TypePtr char_type()    { static auto t = std::make_shared<const Integer>(8, true);    return t; }
    static TypePtr int8_t()       { static auto t = std::make_shared<const Integer>(8, true);    return t; }
    static TypePtr int16_t()      { static auto t = std::make_shared<const Integer>(16, true);   return t; }
    static TypePtr int32_t()      { static auto t = std::make_shared<const Integer>(32, true);   return t; }
    static TypePtr int64_t()      { static auto t = std::make_shared<const Integer>(64, true);   return t; }
    static TypePtr int128_t()     { static auto t = std::make_shared<const Integer>(128, true);  return t; }
    static TypePtr uint8_t()      { static auto t = std::make_shared<const Integer>(8, false);   return t; }
    static TypePtr uint16_t()     { static auto t = std::make_shared<const Integer>(16, false);  return t; }
    static TypePtr uint32_t()     { static auto t = std::make_shared<const Integer>(32, false);  return t; }
    static TypePtr uint64_t()     { static auto t = std::make_shared<const Integer>(64, false);  return t; }
    static TypePtr uint128_t()    { static auto t = std::make_shared<const Integer>(128, false); return t; }

private:
    bool signed_;
};

// =============================================================================
// Float -- IEEE 754 floating-point type
// =============================================================================

class Float final : public Type {
public:
    explicit Float(std::size_t size_bits) : Type(size_bits) { type_kind_ = TypeKind::Float; }

    std::string to_string() const override {
        static const std::unordered_map<std::size_t, const char*> size_types = {
            {8, "quarter"}, {16, "half"}, {32, "float"}, {64, "double"},
            {80, "long double"}, {128, "quadruple"}, {256, "octuple"}
        };
        if (auto it = size_types.find(size_bits_); it != size_types.end())
            return it->second;
        return "float" + std::to_string(size_bits_);
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Float) return false;
        return size_bits_ == static_cast<const Float*>(&other)->size_bits_;
    }

    // ---- Factory methods ----
    static TypePtr float32() { static auto t = std::make_shared<const Float>(32); return t; }
    static TypePtr float64() { static auto t = std::make_shared<const Float>(64); return t; }

private:
};

// =============================================================================
// Pointer -- Pointer to another type
// =============================================================================

class Pointer final : public Type {
public:
    /// @param pointee The type being pointed to.
    /// @param ptr_size_bits Width of the pointer itself (32 or 64 typically).
    Pointer(TypePtr pointee, std::size_t ptr_size_bits = 64)
        : Type(ptr_size_bits), pointee_(std::move(pointee)) { type_kind_ = TypeKind::Pointer; }

    const TypePtr& pointee() const { return pointee_; }

    std::string to_string() const override {
        // Nested pointers: "int **" vs "int *"
        if (pointee_ && pointee_->type_kind() == TypeKind::Pointer)
            return pointee_->to_string() + "*";
        return pointee_->to_string() + " *";
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Pointer) return false;
        auto* o = static_cast<const Pointer*>(&other);
        return size_bits_ == o->size_bits_ && *pointee_ == *o->pointee_;
    }

private:
    TypePtr pointee_;
};

// =============================================================================
// ArrayType -- Fixed-size array of a base element type
// =============================================================================

class ArrayType final : public Type {
public:
    /// @param element The type of each element.
    /// @param count Number of elements.
    ArrayType(TypePtr element, std::size_t count)
        : Type(element->size() * count), element_(std::move(element)), count_(count) { type_kind_ = TypeKind::Array; }

    const TypePtr& element() const { return element_; }
    std::size_t count() const { return count_; }

    std::string to_string() const override {
        return element_->to_string() + " [" + std::to_string(count_) + "]";
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Array) return false;
        auto* o = static_cast<const ArrayType*>(&other);
        return count_ == o->count_ && *element_ == *o->element_;
    }

private:
    TypePtr element_;
    std::size_t count_;
};

// =============================================================================
// CustomType -- Non-basic type with a string name (void, bool, wchar, etc.)
// =============================================================================

class CustomType final : public Type {
public:
    CustomType(std::string text, std::size_t size_bits)
        : Type(size_bits), text_(std::move(text)) { type_kind_ = TypeKind::Custom; }

    const std::string& text() const { return text_; }

    std::string to_string() const override { return text_; }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Custom) return false;
        auto* o = static_cast<const CustomType*>(&other);
        return text_ == o->text_ && size_bits_ == o->size_bits_;
    }

    // ---- Factory methods ----
    static TypePtr void_type()   { static auto t = std::make_shared<const CustomType>("void", 0);       return t; }
    static TypePtr bool_type()   { static auto t = std::make_shared<const CustomType>("bool", 8);       return t; }
    static TypePtr wchar16()     { static auto t = std::make_shared<const CustomType>("wchar16", 16);   return t; }
    static TypePtr wchar32()     { static auto t = std::make_shared<const CustomType>("wchar32", 32);   return t; }

private:
    std::string text_;
};

// =============================================================================
// FunctionTypeDef -- Function type: return_type(param1, param2, ...)
// =============================================================================

class FunctionTypeDef final : public Type {
public:
    FunctionTypeDef(TypePtr return_type, std::vector<TypePtr> parameters)
        : Type(0), return_type_(std::move(return_type)),
          parameters_(std::move(parameters)) { type_kind_ = TypeKind::FunctionTypeDef; }

    const TypePtr& return_type() const { return return_type_; }
    const std::vector<TypePtr>& parameters() const { return parameters_; }

    std::string to_string() const override {
        std::string result = return_type_->to_string() + "(";
        for (std::size_t i = 0; i < parameters_.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters_[i]->to_string();
        }
        result += ")";
        return result;
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::FunctionTypeDef) return false;
        auto* o = static_cast<const FunctionTypeDef*>(&other);
        if (*return_type_ != *o->return_type_) return false;
        if (parameters_.size() != o->parameters_.size()) return false;
        for (std::size_t i = 0; i < parameters_.size(); ++i)
            if (*parameters_[i] != *o->parameters_[i]) return false;
        return true;
    }

private:
    TypePtr return_type_;
    std::vector<TypePtr> parameters_;
};

// =============================================================================
// ComplexType hierarchy -- Struct, Union, Enum
// =============================================================================

/// A single member of a struct, union, or enum.
struct ComplexTypeMember {
    std::string name;
    std::size_t offset = 0;      // Byte offset within the parent (struct/class)
    TypePtr type;                 // Data type of the member
    std::optional<int64_t> value; // Initial value (enums only)

    /// C declaration string for this member.
    std::string declaration() const {
        return type->to_string() + " " + name;
    }
};

/// Abstract base for Struct, Union, Enum, Class.
class ComplexType : public Type {
public:
    const std::string& name() const { return name_; }

    std::string to_string() const override { return name_; }

    /// C declaration string for the full type (with members).
    virtual std::string declaration() const = 0;

protected:
    ComplexType(std::string name, std::size_t size_bits)
        : Type(size_bits), name_(std::move(name)) {}

    std::string name_;
};

/// Struct or Class type (members accessed by offset).
class Struct final : public ComplexType {
public:
    Struct(std::string name, std::size_t size_bits,
           std::unordered_map<std::size_t, ComplexTypeMember> members,
           bool is_class = false)
        : ComplexType(std::move(name), size_bits),
          members_(std::move(members)), is_class_(is_class) { type_kind_ = TypeKind::Struct; }

    bool is_class() const { return is_class_; }
    const std::unordered_map<std::size_t, ComplexTypeMember>& members() const {
        return members_;
    }

    const ComplexTypeMember* get_member_by_offset(std::size_t offset) const {
        if (auto it = members_.find(offset); it != members_.end())
            return &it->second;
        return nullptr;
    }

    std::string get_member_name_by_offset(std::size_t offset) const {
        if (auto* m = get_member_by_offset(offset))
            return m->name;
        return "field_0x" + std::to_string(offset);
    }

    std::string declaration() const override {
        std::string specifier = is_class_ ? "class" : "struct";
        std::string result = specifier + " " + name_ + " {\n";
        // Sort members by offset for declaration
        std::vector<std::size_t> offsets;
        offsets.reserve(members_.size());
        for (auto& [off, _] : members_) offsets.push_back(off);
        std::sort(offsets.begin(), offsets.end());
        for (auto off : offsets) {
            result += "\t" + members_.at(off).declaration() + ";\n";
        }
        result += "}";
        return result;
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Struct) return false;
        auto* o = static_cast<const Struct*>(&other);
        return name_ == o->name_ && size_bits_ == o->size_bits_;
    }

private:
    std::unordered_map<std::size_t, ComplexTypeMember> members_;
    bool is_class_;
};

/// Union type (members accessed by type).
class Union final : public ComplexType {
public:
    Union(std::string name, std::size_t size_bits,
          std::vector<ComplexTypeMember> members)
        : ComplexType(std::move(name), size_bits),
          members_(std::move(members)) { type_kind_ = TypeKind::Union; }

    const std::vector<ComplexTypeMember>& members() const { return members_; }

    const ComplexTypeMember* get_member_by_type(const Type& type) const {
        for (auto& m : members_)
            if (m.type && *m.type == type) return &m;
        return nullptr;
    }

    std::string declaration() const override {
        std::string result = "union " + name_ + " {\n";
        for (auto& m : members_) {
            result += "\t" + m.declaration() + ";\n";
        }
        result += "}";
        return result;
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Union) return false;
        auto* o = static_cast<const Union*>(&other);
        return name_ == o->name_ && size_bits_ == o->size_bits_;
    }

private:
    std::vector<ComplexTypeMember> members_;
};

/// Enum type (members have integer values).
class Enum final : public ComplexType {
public:
    Enum(std::string name, std::size_t size_bits,
         std::unordered_map<int64_t, ComplexTypeMember> members)
        : ComplexType(std::move(name), size_bits),
          members_(std::move(members)) { type_kind_ = TypeKind::Enum; }

    const std::unordered_map<int64_t, ComplexTypeMember>& members() const {
        return members_;
    }

    std::optional<std::string> get_name_by_value(int64_t value) const {
        if (auto it = members_.find(value); it != members_.end())
            return it->second.name;
        return std::nullopt;
    }

    std::string declaration() const override {
        std::string result = "enum " + name_ + " {\n";
        bool first = true;
        for (auto& [val, m] : members_) {
            if (!first) result += ",\n";
            result += "\t" + m.name + " = " + std::to_string(val);
            first = false;
        }
        result += "\n}";
        return result;
    }

    bool operator==(const Type& other) const override {
        if (other.type_kind() != TypeKind::Enum) return false;
        auto* o = static_cast<const Enum*>(&other);
        return name_ == o->name_ && size_bits_ == o->size_bits_;
    }

private:
    std::unordered_map<int64_t, ComplexTypeMember> members_;
};

// =============================================================================
// TypeParser -- Converts type name strings into Type objects
// =============================================================================

class TypeParser {
public:
    /// @param bitness Pointer width in bits (32 or 64).
    explicit TypeParser(std::size_t bitness = 64) : bitness_(bitness) {
        init_known_types();
    }

    /// Parse a type string (e.g., "int", "char *", "unsigned long") into a Type.
    TypePtr parse(const std::string& text) const {
        std::string trimmed = trim(text);

        // Pointer types: strip trailing '*' and recurse
        if (!trimmed.empty() && trimmed.back() == '*') {
            std::string base = trimmed.substr(0, trimmed.size() - 1);
            return std::make_shared<const Pointer>(parse(base), bitness_);
        }

        // Look up in known types
        std::string lower = to_lower(trimmed);
        if (auto it = known_types_.find(lower); it != known_types_.end())
            return it->second;

        // Fallback: CustomType with the platform word size
        return std::make_shared<const CustomType>(trimmed, bitness_);
    }

private:
    std::size_t bitness_;
    std::unordered_map<std::string, TypePtr> known_types_;

    void init_known_types() {
        known_types_ = {
            {"char",               Integer::char_type()},
            {"signed char",        Integer::int8_t()},
            {"unsigned char",      Integer::uint8_t()},
            {"short",              Integer::int16_t()},
            {"unsigned short",     Integer::uint16_t()},
            {"word",               Integer::int16_t()},
            {"unsigned word",      Integer::uint16_t()},
            {"int",                Integer::int32_t()},
            {"unsigned int",       Integer::uint32_t()},
            {"dword",              Integer::int32_t()},
            {"unsigned dword",     Integer::uint32_t()},
            {"long",               Integer::int64_t()},
            {"unsigned long",      Integer::uint64_t()},
            {"long int",           Integer::int64_t()},
            {"unsigned long int",  Integer::uint64_t()},
            {"long long",          Integer::int128_t()},
            {"unsigned long long", Integer::uint128_t()},
            {"void",               CustomType::void_type()},
            {"bool",               CustomType::bool_type()},
            {"float",              Float::float32()},
            {"double",             Float::float64()},
        };
    }

    static std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }

    static std::string to_lower(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
};

// =============================================================================
// LLVM-style RTTI helpers for Type hierarchy
// =============================================================================

// Forward declarations needed for specializations
class ComplexType;

template <typename T> bool type_isa(const Type* t);

template <> inline bool type_isa<UnknownType>(const Type* t)      { return t && t->type_kind() == TypeKind::Unknown; }
template <> inline bool type_isa<Integer>(const Type* t)           { return t && t->type_kind() == TypeKind::Integer; }
template <> inline bool type_isa<Float>(const Type* t)             { return t && t->type_kind() == TypeKind::Float; }
template <> inline bool type_isa<Pointer>(const Type* t)           { return t && t->type_kind() == TypeKind::Pointer; }
template <> inline bool type_isa<ArrayType>(const Type* t)         { return t && t->type_kind() == TypeKind::Array; }
template <> inline bool type_isa<CustomType>(const Type* t)        { return t && t->type_kind() == TypeKind::Custom; }
template <> inline bool type_isa<FunctionTypeDef>(const Type* t)   { return t && t->type_kind() == TypeKind::FunctionTypeDef; }
template <> inline bool type_isa<Struct>(const Type* t)            { return t && t->type_kind() == TypeKind::Struct; }
template <> inline bool type_isa<Union>(const Type* t)             { return t && t->type_kind() == TypeKind::Union; }
template <> inline bool type_isa<Enum>(const Type* t)              { return t && t->type_kind() == TypeKind::Enum; }

// ComplexType matches Struct, Union, or Enum
template <> inline bool type_isa<ComplexType>(const Type* t) {
    return t && (t->type_kind() == TypeKind::Struct ||
                 t->type_kind() == TypeKind::Union ||
                 t->type_kind() == TypeKind::Enum);
}

template <typename T>
T* type_dyn_cast(Type* t) {
    return type_isa<T>(t) ? static_cast<T*>(t) : nullptr;
}

template <typename T>
const T* type_dyn_cast(const Type* t) {
    return type_isa<T>(t) ? static_cast<const T*>(t) : nullptr;
}

template <typename T>
T* type_cast(Type* t) {
    return static_cast<T*>(t);
}

template <typename T>
const T* type_cast(const Type* t) {
    return static_cast<const T*>(t);
}

} // namespace aletheia
