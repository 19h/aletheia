#pragma once

#include <ida/idax.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aletheia {

enum class FrontendKind : std::uint8_t {
    Native,
    Pcode,
};

enum class FrontendDiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error,
};

struct FrontendDiagnostic {
    FrontendDiagnosticSeverity severity = FrontendDiagnosticSeverity::Info;
    std::string code;
    std::string message;
    ida::Address address = 0;
    std::uint32_t op_ordinal = 0;
};

struct FrontendSupportReport {
    std::size_t implemented_ops = 0;
    std::size_t fallback_ops = 0;
    std::size_t unsupported_ops = 0;
};

inline std::string_view frontend_kind_name(FrontendKind kind) {
    switch (kind) {
        case FrontendKind::Native:
            return "native";
        case FrontendKind::Pcode:
            return "pcode";
    }
    return "unknown";
}

inline std::string frontend_kind_name_string(FrontendKind kind) {
    return std::string(frontend_kind_name(kind));
}

inline bool parse_frontend_kind(std::string_view text, FrontendKind& out) {
    if (text == "native") {
        out = FrontendKind::Native;
        return true;
    }
    if (text == "pcode") {
        out = FrontendKind::Pcode;
        return true;
    }
    return false;
}

} // namespace aletheia
