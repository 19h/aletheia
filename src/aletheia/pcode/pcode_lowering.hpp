#pragma once

#include <ida/idax.hpp>

#include "../pipeline/pipeline.hpp"
#include "../structures/dataflow.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aletheia {

struct RawPcodeVarnode {
    std::string space_name;
    std::uint64_t offset = 0;
    std::size_t size = 0;
};

struct RawPcodeOp {
    std::string opcode;
    std::optional<RawPcodeVarnode> output;
    std::vector<RawPcodeVarnode> inputs;
    std::optional<std::string> memory_space_name;
    std::size_t memory_word_size = 1;
    ida::Address instruction_ea = 0;
    std::uint32_t op_ordinal = 0;
};

struct RawPcodeInstruction {
    ida::Address address = 0;
    std::size_t decoded_length = 0;
    std::vector<RawPcodeOp> ops;
};

struct PcodeRegisterView {
    std::string canonical_name;
    std::size_t canonical_size = 0;
    bool low_bits_view = false;
    bool zero_extend_on_write = false;
    bool zero_register = false;
};

struct PcodeRegisterSlice {
    std::uint64_t offset = 0;
    std::size_t size = 0;
};

enum class PcodeStackBase : std::uint8_t {
    StackPointer,
    FramePointer,
};

struct PcodeFunctionInfo {
    std::string name;
    std::vector<TypePtr> parameter_types;
    TypePtr return_type;
    bool prototype_known = false;
};

struct PcodeSignatureContext {
    std::unordered_map<std::string, int> parameter_register_map;
    std::size_t parameter_count_hint = 0;
    bool prefers_w_arguments = false;
    bool prefers_w_return = false;
};

struct PcodeArchitectureContext {
    std::string arch_name;
    std::size_t pointer_size = 8;
    bool big_endian = false;
    std::string register_space_name = "register";
    std::string const_space_name = "const";
    std::string unique_space_name = "unique";
    std::string data_space_name = "ram";
    std::string code_space_name = "code";
    std::function<std::optional<PcodeRegisterView>(std::uint64_t, std::size_t)> register_mapper;
    /// Optional low-to-high decomposition for aggregate register reads whose
    /// Sleigh writes are exposed through independently named component
    /// varnodes. Slice widths must sum exactly to the requested read width.
    std::function<std::optional<std::vector<PcodeRegisterSlice>>(
        std::uint64_t, std::size_t)> register_read_slices;
    std::function<std::optional<std::string>(ida::Address)> symbol_resolver;
    std::function<std::optional<PcodeFunctionInfo>(ida::Address)> function_resolver;
    std::function<Variable*(DecompilerArena&,
                            PcodeStackBase,
                            ida::Address,
                            std::int64_t,
                            std::size_t)> stack_variable_resolver;
};

class PcodeLowerer {
public:
    struct Options {
        ida::Address function_address;
        TypePtr function_type;
    };

    PcodeLowerer(DecompilerArena& arena,
                 DecompilerTask& task,
                 PcodeArchitectureContext architecture,
                 PcodeSignatureContext signature_context,
                 Options options = Options{});

    ida::Result<std::vector<Instruction*>> lower_instruction(const RawPcodeInstruction& instruction);

    /// Reset call-site write evidence at an architectural basic-block
    /// boundary. Unknown-prototype inference never crosses this boundary.
    void begin_basic_block();

private:
    struct LocalDefinition {
        Expression* storage = nullptr;
        Expression* provenance = nullptr;
    };

    using LocalDefinitionMap = std::unordered_map<std::string, LocalDefinition>;

    struct WriteTarget {
        Expression* destination = nullptr;
        Expression* value = nullptr;
        bool emit_assignment = true;
    };

    struct StackAddress {
        PcodeStackBase base = PcodeStackBase::StackPointer;
        std::int64_t offset = 0;
    };

    Expression* lower_varnode_read(
        const RawPcodeVarnode& varnode,
        const LocalDefinitionMap& local_defs);
    WriteTarget lower_output_write(const RawPcodeVarnode& output, Expression* value);
    Expression* lower_call_target(const RawPcodeVarnode& target,
                                  const LocalDefinitionMap& local_defs);
    Expression* lower_pointer_expression(
        const RawPcodeVarnode& ptr,
        const LocalDefinitionMap& local_defs);
    Expression* maybe_stack_or_memory_read(Expression* address_expr,
                                           const LocalDefinitionMap& local_defs,
                                           ida::Address instruction_ea,
                                           std::size_t width);
    Expression* make_cast(Expression* expr, std::size_t size, bool is_signed = false);
    Expression* make_float_cast(Expression* expr, std::size_t size);
    Expression* make_float_operand(Expression* expr, std::size_t size);
    Expression* make_abi_argument(std::size_t index, const TypePtr& type);
    Expression* make_abi_return_value(const TypePtr& type);
    Variable* make_abi_return_storage(const TypePtr& type);
    Condition* make_nonzero_condition(Expression* expr);
    std::string unique_key_for(const RawPcodeVarnode& varnode) const;
    std::string unique_name_for(const RawPcodeVarnode& varnode, ida::Address ea, std::uint32_t ordinal) const;
    std::string fallback_register_name(std::uint64_t offset, std::size_t size) const;
    std::optional<StackAddress> stack_offset_for_address(
        Expression* expr,
        const LocalDefinitionMap& local_defs) const;
    void record_unsupported(const RawPcodeOp& op, const std::string& message);
    void record_fallback(const RawPcodeOp& op, const std::string& message);
    void observe_abi_register_write(const RawPcodeVarnode& output);
    std::size_t inferred_gp_argument_count() const;
    void reset_call_argument_evidence(bool preserve_return_register);

    DecompilerArena& arena_;
    DecompilerTask& task_;
    PcodeArchitectureContext architecture_;
    PcodeSignatureContext signature_context_;
    Options options_;
    ida::Address current_instruction_ea_ = 0;
    std::uint32_t current_op_ordinal_ = 0;
    std::array<bool, 8> gp_argument_write_evidence_{};
};

} // namespace aletheia
