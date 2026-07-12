#pragma once

#include <ida/idax.hpp>

#include "../pipeline/pipeline.hpp"
#include "../structures/dataflow.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aletheia::frontend {

struct FrameLayout {
    std::unordered_map<std::int64_t, ida::function::FrameVariable> offset_to_var;
    std::size_t local_size = 0;
    std::size_t regs_size = 0;
    std::size_t args_size = 0;
    std::size_t total_size = 0;
    bool valid = false;
};

struct SignatureState {
    std::unordered_map<std::string, int> param_register_map;
    std::unordered_map<std::string, std::string> regvar_alias_map;
    std::size_t current_function_param_count_hint = 0;
    bool current_function_prefers_w_args = false;
    bool current_function_prefers_w_return = false;
};

std::string sanitize_identifier(std::string text);
std::string strip_ida_address_prefix(const std::string& name);
std::string to_lower_ascii(std::string text);
std::string detect_arch();
std::string canonical_function_name(std::string name);
inline std::string sanitize_c_block_comment_text(std::string_view text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';
        if (current == '/' && next == '*') {
            sanitized += "/ *";
            ++index;
        } else if (current == '*' && next == '/') {
            sanitized += "* /";
            ++index;
        } else if (current == '\n' || current == '\r' || current == '\t') {
            sanitized.push_back(' ');
        } else if (static_cast<unsigned char>(current) < 0x20) {
            sanitized.push_back('?');
        } else {
            sanitized.push_back(current);
        }
    }
    return sanitized;
}

std::vector<std::string> x86_64_sub_register_aliases(std::string_view reg64);
std::pair<const std::string_view*, std::size_t> param_register_table_for_arch(std::string_view arch);
bool first_parameter_prefers_w_reg(const TypePtr& function_type);
bool return_prefers_w_reg(const TypePtr& function_type);
std::optional<std::size_t> known_call_min_arity(const std::string& canon_name);
std::optional<bool> known_call_arg_prefers_w(const std::string& canon_name, std::size_t arg_index);
TypePtr known_call_return_type(const std::string& canon_name);

SignatureState populate_task_signature(DecompilerTask& task);

FrameLayout build_frame_layout(ida::Address function_address);
std::optional<std::string> resolve_frame_variable(const FrameLayout& frame_layout,
                                                  std::int64_t frame_offset,
                                                  std::size_t access_size);
Variable* resolve_sp_relative_slot(DecompilerArena& arena,
                                   const FrameLayout& frame_layout,
                                   ida::Address insn_addr,
                                   std::int64_t sp_adjust_bytes,
                                   std::size_t access_size);

} // namespace aletheia::frontend
