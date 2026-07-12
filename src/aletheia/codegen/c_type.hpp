#pragma once

#include "c_identifier.hpp"
#include "../structures/types.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aletheia {

inline std::string trim_c_type_text(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\n\r");
    return std::string(text.substr(first, last - first + 1));
}

inline std::optional<std::string> builtin_custom_c_type(
    std::string_view raw) {
    const std::string text = trim_c_type_text(raw);
    if (text == "void" || text == "bool" || text == "FILE"
        || text == "timespec" || text == "clockid_t"
        || text == "size_t" || text == "ptrdiff_t"
        || text == "intptr_t" || text == "uintptr_t"
        || text == "int8_t" || text == "uint8_t"
        || text == "int16_t" || text == "uint16_t"
        || text == "int32_t" || text == "uint32_t"
        || text == "int64_t" || text == "uint64_t"
        || text == "int128_t" || text == "uint128_t") {
        return text;
    }
    if (text == "wchar16") return "uint16_t";
    if (text == "wchar32") return "uint32_t";
    return std::nullopt;
}

inline std::optional<std::pair<std::string, std::string>>
split_tagged_c_type(std::string_view raw) {
    const std::string text = trim_c_type_text(raw);
    for (std::string_view prefix : {"struct ", "union ", "enum "}) {
        if (text.starts_with(prefix)) {
            const std::string tag = trim_c_type_text(
                std::string_view(text).substr(prefix.size()));
            if (!tag.empty()) {
                return std::pair{
                    std::string(prefix.substr(0, prefix.size() - 1)),
                    normalize_distinct_c_identifier(tag, "tag")};
            }
        }
    }
    return std::nullopt;
}

inline std::string render_portable_c_type(const TypePtr& type);

inline std::string portable_complex_type_name(const ComplexType& type) {
    if (type.type_kind() == TypeKind::Enum) {
        return encode_c_identifier_bytes("aletheia_enum_type_", type.name());
    }
    const bool is_union = type.type_kind() == TypeKind::Union;
    return std::string(is_union ? "union " : "struct ")
        + encode_c_identifier_bytes(
            is_union ? "aletheia_union_tag_" : "aletheia_struct_tag_",
            type.name());
}

inline std::optional<std::string> portable_integer_storage(
    std::size_t bits,
    bool signed_storage) {
    const char* prefix = signed_storage ? "int" : "uint";
    switch (bits) {
        case 8: case 16: case 32: case 64: case 128:
            return std::string(prefix) + std::to_string(bits) + "_t";
        default:
            return std::nullopt;
    }
}

inline std::string render_portable_c_function_parameters(
    const FunctionTypeDef& function) {
    std::string result;
    for (std::size_t index = 0; index < function.parameters().size(); ++index) {
        if (index > 0) result += ", ";
        result += render_portable_c_type(function.parameters()[index]);
    }
    if (function.variadic()) {
        if (!function.parameters().empty()) result += ", ";
        result += "...";
    } else if (function.parameters().empty()) {
        result = "void";
    }
    return result;
}

inline std::string render_portable_c_type(const TypePtr& type) {
    if (!type || type->type_kind() == TypeKind::Unknown) return "uintptr_t";
    switch (type->type_kind()) {
        case TypeKind::Integer: {
            const auto* integer = type_dyn_cast<Integer>(type.get());
            if (portable_integer_storage(
                    integer->size(), integer->is_signed())) {
                return integer->to_string();
            }
            return "ALETHEIA_UNSUPPORTED_PORTABLE_TYPE_INTEGER_WIDTH_"
                + std::to_string(integer->size());
        }
        case TypeKind::Float:
            switch (type->size()) {
                case 16: return "_Float16";
                case 32: return "float";
                case 64: return "double";
                case 80: return "long double";
                case 128: return "_Float128";
                default:
                    return "ALETHEIA_UNSUPPORTED_PORTABLE_TYPE_FLOAT_WIDTH_"
                        + std::to_string(type->size());
            }
        case TypeKind::Pointer: {
            const auto* pointer = type_dyn_cast<Pointer>(type.get());
            return render_portable_c_type(pointer->pointee()) + " *";
        }
        case TypeKind::Array: {
            const auto* array = type_dyn_cast<ArrayType>(type.get());
            return render_portable_c_type(array->element()) + " ["
                + std::to_string(array->count()) + "]";
        }
        case TypeKind::Custom: {
            const auto* custom = type_dyn_cast<CustomType>(type.get());
            if (auto builtin = builtin_custom_c_type(custom->text())) {
                return *builtin;
            }
            if (auto tagged = split_tagged_c_type(custom->text())) {
                return tagged->first + " " + tagged->second;
            }
            return encode_c_identifier_bytes(
                "aletheia_type_", custom->text());
        }
        case TypeKind::FunctionTypeDef: {
            const auto* function =
                type_dyn_cast<FunctionTypeDef>(type.get());
            return render_portable_c_type(function->return_type()) + " ("
                + render_portable_c_function_parameters(*function) + ")";
        }
        case TypeKind::Struct: {
            const auto* structure = type_dyn_cast<Struct>(type.get());
            return portable_complex_type_name(*structure);
        }
        case TypeKind::Union: {
            const auto* union_type = type_dyn_cast<Union>(type.get());
            return portable_complex_type_name(*union_type);
        }
        case TypeKind::Enum: {
            const auto* enumeration = type_dyn_cast<Enum>(type.get());
            return portable_complex_type_name(*enumeration);
        }
        case TypeKind::Unknown:
            break;
    }
    return "uintptr_t";
}

inline std::string render_portable_c_declaration(
    const TypePtr& type,
    std::string identifier) {
    if (!type) return "uintptr_t " + identifier;
    if (const auto* array = type_dyn_cast<ArrayType>(type.get())) {
        return render_portable_c_declaration(
            array->element(),
            identifier + "[" + std::to_string(array->count()) + "]");
    }
    if (const auto* pointer = type_dyn_cast<Pointer>(type.get())) {
        if (pointer->pointee()
            && (pointer->pointee()->type_kind() == TypeKind::Array
                || pointer->pointee()->type_kind()
                    == TypeKind::FunctionTypeDef)) {
            identifier = "(*" + identifier + ")";
        } else {
            identifier = "* " + identifier;
        }
        return render_portable_c_declaration(pointer->pointee(), identifier);
    }
    if (const auto* function =
            type_dyn_cast<FunctionTypeDef>(type.get())) {
        return render_portable_c_type(function->return_type()) + " "
            + identifier + "("
            + render_portable_c_function_parameters(*function) + ")";
    }
    return render_portable_c_type(type) + " " + identifier;
}

inline std::optional<std::string> portable_custom_type_alias(
    const CustomType& custom) {
    if (builtin_custom_c_type(custom.text())
        || split_tagged_c_type(custom.text())) {
        return std::nullopt;
    }
    const std::string name = encode_c_identifier_bytes(
        "aletheia_type_", custom.text());
    std::string storage;
    switch (custom.size()) {
        case 8: storage = "uint8_t"; break;
        case 16: storage = "uint16_t"; break;
        case 32: storage = "uint32_t"; break;
        case 64: storage = "uint64_t"; break;
        case 128: storage = "uint128_t"; break;
        default:
            return "/* ALETHEIA_UNSUPPORTED_CUSTOM_TYPE_WIDTH_"
                + std::to_string(custom.size()) + ": " + name + " */";
    }
    if (storage == name) return std::nullopt;
    return "typedef " + storage + " " + name + ";";
}

struct PortableComplexDeclaration {
    std::string text;
    bool unresolved = false;
};

inline std::optional<std::string> portable_aggregate_member_issue(
    TypePtr type) {
    while (type) {
        if (const auto* pointer = type_dyn_cast<Pointer>(type.get())) {
            return pointer->size() == 64
                ? std::nullopt
                : std::optional<std::string>(
                    "pointer member does not match the LP64 output model");
        }
        if (const auto* array = type_dyn_cast<ArrayType>(type.get())) {
            if (array->count() == 0) return "zero-length array member";
            type = array->element();
            continue;
        }
        if (const auto* integer = type_dyn_cast<Integer>(type.get())) {
            return portable_integer_storage(integer->size(), integer->is_signed())
                ? std::nullopt
                : std::optional<std::string>(
                    "integer member has unsupported storage width");
        }
        if (const auto* floating = type_dyn_cast<Float>(type.get())) {
            switch (floating->size()) {
                case 16: case 32: case 64: case 80: case 128:
                    return std::nullopt;
                default:
                    return "floating member has unsupported storage width";
            }
        }
        if (const auto* custom = type_dyn_cast<CustomType>(type.get())) {
            if (split_tagged_c_type(custom->text())) {
                return "tagged custom member has no recovered definition";
            }
            const std::string text = trim_c_type_text(custom->text());
            if (text == "void" || text == "FILE" || text == "timespec") {
                return "incomplete or void custom member";
            }
            if (text == "bool" && custom->size() != 8) {
                return "boolean member width disagrees with the C model";
            }
            static const std::map<std::string, std::size_t> builtin_widths = {
                {"int8_t", 8}, {"uint8_t", 8},
                {"int16_t", 16}, {"uint16_t", 16},
                {"wchar16", 16},
                {"int32_t", 32}, {"uint32_t", 32},
                {"wchar32", 32}, {"clockid_t", 32},
                {"int64_t", 64}, {"uint64_t", 64},
                {"size_t", 64}, {"ptrdiff_t", 64},
                {"intptr_t", 64}, {"uintptr_t", 64},
                {"int128_t", 128}, {"uint128_t", 128},
            };
            if (const auto it = builtin_widths.find(text);
                it != builtin_widths.end() && custom->size() != it->second) {
                return "custom member width disagrees with the C model";
            }
            return portable_integer_storage(custom->size(), false)
                    || builtin_custom_c_type(text)
                ? std::nullopt
                : std::optional<std::string>(
                    "custom member has unsupported storage width");
        }
        if (type->type_kind() == TypeKind::Struct
            || type->type_kind() == TypeKind::Union
            || type->type_kind() == TypeKind::Enum) {
            return std::nullopt;
        }
        if (type->type_kind() == TypeKind::FunctionTypeDef) {
            return "function-valued member is not representable in C";
        }
        return "unknown member type is not layout-recoverable";
    }
    return "member has null type";
}

inline PortableComplexDeclaration portable_complex_type_declaration(
    const ComplexType& type) {
    const std::string encoded_key = encode_c_identifier_bytes(
        "ALETHEIA_COMPLEX_", type.name());
    const std::string guard = encoded_key + "_"
        + std::to_string(static_cast<int>(type.type_kind()));
    const auto unresolved = [&](std::string reason) {
        return PortableComplexDeclaration{
            "/* ALETHEIA_UNSUPPORTED_COMPLEX_TYPE_LAYOUT: "
                + normalize_distinct_c_identifier(type.name(), "tag")
                + ": " + std::move(reason) + " */",
            true,
        };
    };

    std::string declaration;
    if (const auto* structure = type_dyn_cast<Struct>(&type)) {
        if (structure->size() == 0 || structure->size() % 8 != 0) {
            return unresolved("aggregate size is zero or not byte-addressable");
        }
        std::vector<std::size_t> offsets;
        offsets.reserve(structure->members().size());
        for (const auto& [offset, _] : structure->members()) {
            offsets.push_back(offset);
        }
        std::sort(offsets.begin(), offsets.end());

        std::set<std::string> member_names;
        std::map<std::size_t, std::string> normalized_member_names;
        for (std::size_t offset : offsets) {
            const ComplexTypeMember& member = structure->members().at(offset);
            const std::string member_name = normalize_distinct_c_identifier(
                member.name.empty()
                    ? "field_" + std::to_string(offset) : member.name,
                "member");
            if (!member_names.insert(member_name).second) {
                return unresolved("duplicate normalized member name");
            }
            normalized_member_names.emplace(offset, member_name);
        }
        std::size_t cursor = 0;
        const std::size_t total_size = structure->size_bytes();
        declaration = portable_complex_type_name(*structure) + " {\n";
        const auto internal_member_name = [&](std::string base) {
            std::size_t discriminator = 2;
            while (!member_names.insert(base).second) {
                base = base + "_" + std::to_string(discriminator++);
            }
            return base;
        };
        for (std::size_t offset : offsets) {
            const ComplexTypeMember& member = structure->members().at(offset);
            if (member.offset != offset) {
                return unresolved("member offset disagrees with struct index");
            }
            if (!member.type || member.type->size() == 0
                || member.type->size() % 8 != 0) {
                return unresolved(
                    "member has null, zero-width, or non-byte-addressable type");
            }
            if (auto issue = portable_aggregate_member_issue(member.type)) {
                return unresolved(*issue);
            }
            if (offset < cursor) {
                return unresolved("overlapping members require union/bit-field metadata");
            }
            if (offset > cursor) {
                declaration += "    unsigned char "
                    + internal_member_name(
                        "__aletheia_padding_" + std::to_string(cursor)) + "["
                    + std::to_string(offset - cursor) + "];\n";
            }
            declaration += "    "
                + render_portable_c_declaration(
                    member.type, normalized_member_names.at(offset))
                + ";\n";
            cursor = offset + member.type->size_bytes();
            if (cursor > total_size) {
                return unresolved("member extends beyond declared aggregate size");
            }
        }
        if (cursor < total_size) {
            declaration += "    unsigned char "
                + internal_member_name("__aletheia_tail_padding") + "["
                + std::to_string(total_size - cursor) + "];\n";
        }
        declaration += "} __attribute__((packed));\n";
        declaration += "_Static_assert(sizeof("
            + portable_complex_type_name(*structure) + ") == "
            + std::to_string(total_size)
            + ", \"Aletheia struct layout mismatch\");";
    } else if (const auto* union_type = type_dyn_cast<Union>(&type)) {
        if (union_type->size() == 0 || union_type->size() % 8 != 0) {
            return unresolved("aggregate size is zero or not byte-addressable");
        }
        const std::size_t total_size = union_type->size_bytes();
        std::set<std::string> member_names;
        declaration = portable_complex_type_name(*union_type) + " {\n";
        for (std::size_t index = 0;
             index < union_type->members().size();
             ++index) {
            const ComplexTypeMember& member = union_type->members()[index];
            if (member.offset != 0) {
                return unresolved("union member has nonzero offset");
            }
            if (!member.type || member.type->size() == 0
                || member.type->size() % 8 != 0
                || member.type->size_bytes() > total_size) {
                return unresolved("union member has invalid width");
            }
            if (auto issue = portable_aggregate_member_issue(member.type)) {
                return unresolved(*issue);
            }
            const std::string member_name = normalize_distinct_c_identifier(
                member.name.empty()
                    ? "field_" + std::to_string(index) : member.name,
                "member");
            if (!member_names.insert(member_name).second) {
                return unresolved("duplicate normalized member name");
            }
            declaration += "    "
                + render_portable_c_declaration(member.type, member_name)
                + ";\n";
        }
        if (total_size > 0) {
            std::string storage_name = "__aletheia_storage";
            std::size_t discriminator = 2;
            while (!member_names.insert(storage_name).second) {
                storage_name = "__aletheia_storage_"
                    + std::to_string(discriminator++);
            }
            declaration += "    unsigned char " + storage_name + "["
                + std::to_string(total_size) + "];\n";
        }
        declaration += "} __attribute__((packed));\n";
        declaration += "_Static_assert(sizeof("
            + portable_complex_type_name(*union_type) + ") == "
            + std::to_string(total_size)
            + ", \"Aletheia union layout mismatch\");";
    } else if (const auto* enumeration = type_dyn_cast<Enum>(&type)) {
        bool signed_storage = false;
        for (const auto& [value, _] : enumeration->members()) {
            signed_storage = signed_storage || value < 0;
        }
        auto storage = portable_integer_storage(
            enumeration->size(), signed_storage);
        if (!storage) return unresolved("unsupported enum storage width");
        const std::string type_name = portable_complex_type_name(*enumeration);
        declaration = "typedef " + *storage + " " + type_name + ";\n";
        std::vector<std::int64_t> values;
        values.reserve(enumeration->members().size());
        for (const auto& [value, _] : enumeration->members()) {
            values.push_back(value);
        }
        std::sort(values.begin(), values.end());
        std::set<std::string> constant_names;
        for (std::int64_t value : values) {
            const auto& member = enumeration->members().at(value);
            if (member.value.has_value() && *member.value != value) {
                return unresolved("enumerator value disagrees with enum index");
            }
            const std::size_t bits = enumeration->size();
            if (bits < 64) {
                const std::int64_t minimum = signed_storage
                    ? -(INT64_C(1) << (bits - 1)) : 0;
                const std::uint64_t maximum = signed_storage
                    ? (UINT64_C(1) << (bits - 1)) - 1
                    : (UINT64_C(1) << bits) - 1;
                if (value < minimum
                    || (value >= 0
                        && static_cast<std::uint64_t>(value) > maximum)) {
                    return unresolved("enumerator is outside declared storage width");
                }
            }
            const std::string constant_name = encode_c_identifier_bytes(
                "aletheia_enum_" + encode_c_identifier_bytes(
                    "", enumeration->name()) + "_",
                member.name.empty()
                    ? "value_" + std::to_string(value) : member.name);
            if (!constant_names.insert(constant_name).second) {
                return unresolved("duplicate encoded enumerator name");
            }
            const std::string value_literal = value == INT64_MIN
                ? "(-INT64_C(9223372036854775807) - INT64_C(1))"
                : std::to_string(value);
            declaration += "#define " + constant_name + " (("
                + type_name + ")(" + value_literal + "))\n";
        }
        if (!values.empty()) declaration.pop_back();
    } else {
        return unresolved("unknown complex type kind");
    }

    return {
        "#ifndef " + guard + "\n#define " + guard + " 1\n"
            + declaration + "\n#endif",
        false,
    };
}

inline std::string render_codegen_type(
    const TypePtr& type,
    bool portable_c) {
    if (portable_c) return render_portable_c_type(type);
    return type ? type->to_string() : "unknown type";
}

inline std::string render_codegen_declaration(
    const TypePtr& type,
    std::string identifier,
    bool portable_c) {
    if (portable_c) {
        return render_portable_c_declaration(type, std::move(identifier));
    }
    return (type ? type->to_string() : std::string("unknown type"))
        + " " + identifier;
}

} // namespace aletheia
