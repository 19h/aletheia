#include "pcode_lowering.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace aletheia {

namespace {

TypePtr unsigned_integer_for_size(std::size_t size) {
    switch (size) {
        case 1: return Integer::uint8_t();
        case 2: return Integer::uint16_t();
        case 4: return Integer::uint32_t();
        case 8: return Integer::uint64_t();
        default: return std::make_shared<const Integer>(size * 8, false);
    }
}

TypePtr signed_integer_for_size(std::size_t size) {
    switch (size) {
        case 1: return Integer::int8_t();
        case 2: return Integer::int16_t();
        case 4: return Integer::int32_t();
        case 8: return Integer::int64_t();
        default: return std::make_shared<const Integer>(size * 8, true);
    }
}

TypePtr float_for_size(std::size_t size) {
    switch (size) {
        case 4: return Float::float32();
        case 8: return Float::float64();
        default: return std::make_shared<const Float>(size * 8);
    }
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << value;
    return out.str();
}

std::string canonical_function_name(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    while (!name.empty() && name.front() == '_') {
        name.erase(name.begin());
    }
    return name;
}

const Float* as_float_type(const TypePtr& type) {
    return type ? type_dyn_cast<Float>(type.get()) : nullptr;
}

const FunctionTypeDef* as_function_type(const TypePtr& type) {
    return type ? type_dyn_cast<FunctionTypeDef>(type.get()) : nullptr;
}

} // namespace

PcodeLowerer::PcodeLowerer(DecompilerArena& arena,
                           DecompilerTask& task,
                           PcodeArchitectureContext architecture,
                           PcodeSignatureContext signature_context,
                           Options options)
    : arena_(arena),
      task_(task),
      architecture_(std::move(architecture)),
      signature_context_(std::move(signature_context)),
      options_(std::move(options)) {}

void PcodeLowerer::begin_basic_block() {
    reset_call_argument_evidence(false);
}

void PcodeLowerer::observe_abi_register_write(const RawPcodeVarnode& output) {
    if (output.space_name != architecture_.register_space_name
        || !architecture_.register_mapper) {
        return;
    }
    const auto mapped = architecture_.register_mapper(output.offset, output.size);
    if (!mapped.has_value() || mapped->zero_register) {
        return;
    }
    const std::string& name = mapped->canonical_name;
    if (name.size() != 2 || name[0] != 'x' || name[1] < '0' || name[1] > '7') {
        return;
    }
    gp_argument_write_evidence_[static_cast<std::size_t>(name[1] - '0')] = true;
}

std::size_t PcodeLowerer::inferred_gp_argument_count() const {
    std::size_t count = 0;
    while (count < gp_argument_write_evidence_.size()
           && gp_argument_write_evidence_[count]) {
        ++count;
    }
    return count;
}

void PcodeLowerer::reset_call_argument_evidence(bool preserve_return_register) {
    gp_argument_write_evidence_.fill(false);
    if (preserve_return_register) {
        // AAPCS64 returns scalar integer and pointer values in x0. Retain that
        // reaching definition as evidence for an immediately chained call.
        gp_argument_write_evidence_[0] = true;
    }
}

std::string PcodeLowerer::unique_name_for(const RawPcodeVarnode& varnode,
                                          ida::Address ea,
                                          std::uint32_t ordinal) const {
    return "u_" + hex_u64(static_cast<std::uint64_t>(ea))
        + "_" + std::to_string(ordinal)
        + "_" + hex_u64(varnode.offset)
        + "_" + std::to_string(varnode.size);
}

std::string PcodeLowerer::unique_key_for(const RawPcodeVarnode& varnode) const {
    return hex_u64(varnode.offset) + "_" + std::to_string(varnode.size);
}

std::string PcodeLowerer::fallback_register_name(std::uint64_t offset, std::size_t size) const {
    return "reg_" + hex_u64(offset) + "_" + std::to_string(size);
}

Expression* PcodeLowerer::make_cast(Expression* expr, std::size_t size, bool is_signed) {
    auto* cast = arena_.create<Operation>(OperationType::cast, std::vector<Expression*>{expr}, size);
    cast->set_ir_type(is_signed ? signed_integer_for_size(size) : unsigned_integer_for_size(size));
    return cast;
}

Expression* PcodeLowerer::make_float_cast(Expression* expr, std::size_t size) {
    auto* cast = arena_.create<Operation>(OperationType::cast, std::vector<Expression*>{expr}, size);
    cast->set_ir_type(float_for_size(size));
    return cast;
}

Expression* PcodeLowerer::make_float_operand(Expression* expr, std::size_t size) {
    if (expr == nullptr) {
        return nullptr;
    }
    if (expr->ir_type() && as_float_type(expr->ir_type()) != nullptr
        && expr->size_bytes == size) {
        return expr;
    }

    // Raw P-Code varnodes are bitvectors. The current IR has no distinct
    // bitcast node, so preserve the storage expression and attach the Sleigh
    // floating interpretation to this per-use copy. Constants are left as raw
    // bits and handled conservatively by downstream code rather than converted
    // numerically here.
    expr->set_ir_type(float_for_size(size));
    return expr;
}

Expression* PcodeLowerer::make_abi_argument(std::size_t index, const TypePtr& type) {
    if (index >= 8) {
        return nullptr;
    }

    if (const auto* floating = as_float_type(type)) {
        const std::size_t width = floating->size_bytes();
        if (width != 4 && width != 8 && width != 16) {
            return nullptr;
        }
        const char prefix = width == 4 ? 's' : (width == 8 ? 'd' : 'q');
        auto* reg = arena_.create<Variable>(
            std::string(1, prefix) + std::to_string(index), width);
        reg->set_ir_type(type);
        return reg;
    }

    auto* storage = arena_.create<Variable>("x" + std::to_string(index), 8);
    storage->set_ir_type(Integer::uint64_t());
    if (type && type->size_bytes() > 0 && type->size_bytes() < 8) {
        auto* view = make_cast(storage, type->size_bytes(), false);
        view->set_ir_type(type);
        return view;
    }
    if (type) {
        storage->set_ir_type(type);
    }
    return storage;
}

Expression* PcodeLowerer::make_abi_return_value(const TypePtr& type) {
    if (const auto* floating = as_float_type(type)) {
        const std::size_t width = floating->size_bytes();
        if (width == 4 || width == 8 || width == 16) {
            const char prefix = width == 4 ? 's' : (width == 8 ? 'd' : 'q');
            auto* reg = arena_.create<Variable>(std::string(1, prefix) + "0", width);
            reg->set_ir_type(type);
            return reg;
        }
    }

    auto* storage = arena_.create<Variable>("x0", 8);
    storage->set_ir_type(Integer::uint64_t());
    if (type && type->size_bytes() > 0 && type->size_bytes() < 8) {
        auto* view = make_cast(storage, type->size_bytes(), false);
        view->set_ir_type(type);
        return view;
    }
    if (type) {
        storage->set_ir_type(type);
    }
    return storage;
}

Variable* PcodeLowerer::make_abi_return_storage(const TypePtr& type) {
    if (const auto* floating = as_float_type(type)) {
        const std::size_t width = floating->size_bytes();
        if (width == 4 || width == 8 || width == 16) {
            const char prefix = width == 4 ? 's' : (width == 8 ? 'd' : 'q');
            auto* reg = arena_.create<Variable>(std::string(1, prefix) + "0", width);
            reg->set_ir_type(type);
            return reg;
        }
    }

    // Integer subregister writes are canonicalized onto x0 throughout this
    // frontend. Keeping call definitions on the same storage preserves SSA
    // connectivity with subsequent w0 reads, which are represented as casts.
    auto* storage = arena_.create<Variable>("x0", 8);
    storage->set_ir_type(type && type->size_bytes() >= 8 ? type : Integer::uint64_t());
    return storage;
}

Condition* PcodeLowerer::make_nonzero_condition(Expression* expr) {
    if (auto* cond = dyn_cast<Condition>(expr)) {
        return cond;
    }
    const std::size_t width = expr && expr->size_bytes > 0 ? expr->size_bytes : static_cast<std::size_t>(1);
    return arena_.create<Condition>(
        OperationType::neq,
        expr,
        arena_.create<Constant>(0, width),
        1);
}

void PcodeLowerer::record_unsupported(const RawPcodeOp& op, const std::string& message) {
    task_.mutable_frontend_support_report().unsupported_ops += 1;
    task_.add_frontend_diagnostic(FrontendDiagnostic{
        FrontendDiagnosticSeverity::Error,
        "pcode-unsupported",
        op.opcode + ": " + message,
        op.instruction_ea,
        op.op_ordinal,
    });
}

void PcodeLowerer::record_fallback(const RawPcodeOp& op, const std::string& message) {
    task_.mutable_frontend_support_report().fallback_ops += 1;
    task_.add_frontend_diagnostic(FrontendDiagnostic{
        FrontendDiagnosticSeverity::Warning,
        "pcode-fallback",
        op.opcode + ": " + message,
        op.instruction_ea,
        op.op_ordinal,
    });
}

Expression* PcodeLowerer::lower_varnode_read(
    const RawPcodeVarnode& varnode,
    const LocalDefinitionMap& local_defs) {
    if (varnode.space_name == architecture_.const_space_name) {
        auto* constant = arena_.create<Constant>(varnode.offset, varnode.size);
        constant->set_ir_type(unsigned_integer_for_size(varnode.size));
        return constant;
    }

    if (varnode.space_name == architecture_.unique_space_name) {
        const std::string key = unique_key_for(varnode);
        auto it = local_defs.find(key);
        if (it != local_defs.end() && it->second.storage != nullptr) {
            return it->second.storage->copy(arena_);
        }
        auto* temp = arena_.create<Variable>(
            unique_name_for(varnode, current_instruction_ea_, current_op_ordinal_), varnode.size);
        temp->set_kind(VariableKind::Temporary);
        temp->set_ir_type(unsigned_integer_for_size(varnode.size));
        return temp;
    }

    if (varnode.space_name == architecture_.register_space_name) {
        if (architecture_.register_read_slices) {
            auto slices = architecture_.register_read_slices(varnode.offset, varnode.size);
            if (slices.has_value() && slices->size() >= 2) {
                std::size_t total_size = 0;
                bool valid = true;
                for (const PcodeRegisterSlice& slice : *slices) {
                    if (slice.size == 0
                        || slice.offset > std::numeric_limits<std::uint64_t>::max() - slice.size
                        || total_size > varnode.size
                        || slice.size > varnode.size - total_size) {
                        valid = false;
                        break;
                    }
                    total_size += slice.size;
                }
                valid = valid
                    && total_size == varnode.size
                    && varnode.size <= std::numeric_limits<std::size_t>::max() / 8;

                if (valid) {
                    Expression* combined = nullptr;
                    std::size_t low_size = 0;
                    for (const PcodeRegisterSlice& slice : *slices) {
                        RawPcodeVarnode component{
                            architecture_.register_space_name,
                            slice.offset,
                            slice.size};
                        Expression* value = lower_varnode_read(component, local_defs);
                        if (!combined) {
                            combined = make_cast(value, varnode.size, false);
                            low_size = slice.size;
                            continue;
                        }

                        auto* shift = arena_.create<Constant>(low_size * 8, varnode.size);
                        auto* shifted = arena_.create<Operation>(
                            OperationType::shl,
                            std::vector<Expression*>{
                                make_cast(value, varnode.size, false), shift},
                            varnode.size);
                        combined = arena_.create<Operation>(
                            OperationType::bit_or,
                            std::vector<Expression*>{shifted, combined},
                            varnode.size);
                        combined->set_ir_type(unsigned_integer_for_size(varnode.size));
                        low_size += slice.size;
                    }
                    return combined;
                }
            }
        }

        if (architecture_.register_mapper) {
            if (auto mapped = architecture_.register_mapper(varnode.offset, varnode.size)) {
                if (mapped->zero_register) {
                    auto* zero = arena_.create<Constant>(0, varnode.size);
                    zero->set_ir_type(unsigned_integer_for_size(varnode.size));
                    return zero;
                }

                auto* base = arena_.create<Variable>(mapped->canonical_name,
                    mapped->canonical_size > 0 ? mapped->canonical_size : varnode.size);
                base->set_ir_type(unsigned_integer_for_size(base->size_bytes));
                auto parameter = signature_context_.parameter_register_map.find(mapped->canonical_name);
                if (parameter == signature_context_.parameter_register_map.end()
                    && mapped->canonical_name.size() >= 2
                    && mapped->canonical_name.front() == 'x' && varnode.size <= 4) {
                    std::string narrow_name = mapped->canonical_name;
                    narrow_name.front() = 'w';
                    parameter = signature_context_.parameter_register_map.find(narrow_name);
                }
                if (parameter != signature_context_.parameter_register_map.end()) {
                    base->set_kind(VariableKind::Parameter);
                    base->set_parameter_index(parameter->second);
                    const auto* function = options_.function_type
                        ? type_dyn_cast<FunctionTypeDef>(options_.function_type.get()) : nullptr;
                    if (function != nullptr && parameter->second >= 0
                        && static_cast<std::size_t>(parameter->second) < function->parameters().size()) {
                        base->set_ir_type(function->parameters()[static_cast<std::size_t>(parameter->second)]);
                    }
                }
                if (mapped->low_bits_view && mapped->canonical_size > varnode.size) {
                    return make_cast(base, varnode.size, false);
                }
                return base;
            }
        }

        auto* reg = arena_.create<Variable>(fallback_register_name(varnode.offset, varnode.size), varnode.size);
        reg->set_ir_type(unsigned_integer_for_size(varnode.size));
        return reg;
    }

    if (varnode.space_name == architecture_.code_space_name || varnode.space_name == architecture_.data_space_name) {
        auto* address = arena_.create<Constant>(varnode.offset, architecture_.pointer_size);
        address->set_ir_type(std::make_shared<Pointer>(Integer::uint8_t(), architecture_.pointer_size * 8));
        return address;
    }

    auto* fallback = arena_.create<Variable>(
        varnode.space_name + "_" + hex_u64(varnode.offset) + "_" + std::to_string(varnode.size),
        varnode.size);
    fallback->set_ir_type(unsigned_integer_for_size(varnode.size));
    return fallback;
}

PcodeLowerer::WriteTarget PcodeLowerer::lower_output_write(const RawPcodeVarnode& output, Expression* value) {
    WriteTarget out;
    out.value = value;

    if (output.space_name == architecture_.unique_space_name) {
        auto* temp = arena_.create<Variable>(
            unique_name_for(output, current_instruction_ea_, current_op_ordinal_), output.size);
        temp->set_kind(VariableKind::Temporary);
        temp->set_ir_type(value && value->ir_type()
            ? value->ir_type() : unsigned_integer_for_size(output.size));
        out.destination = temp;
        return out;
    }

    if (output.space_name == architecture_.register_space_name) {
        if (architecture_.register_mapper) {
            if (auto mapped = architecture_.register_mapper(output.offset, output.size)) {
                if (mapped->zero_register) {
                    out.emit_assignment = false;
                    return out;
                }
                const std::size_t dst_size = mapped->canonical_size > 0 ? mapped->canonical_size : output.size;
                auto* reg = arena_.create<Variable>(mapped->canonical_name, dst_size);
                reg->set_ir_type(value && value->ir_type() && dst_size == output.size
                    ? value->ir_type() : unsigned_integer_for_size(dst_size));
                out.destination = reg;
                if (mapped->zero_extend_on_write && dst_size > output.size) {
                    out.value = make_cast(value, dst_size, false);
                }
                return out;
            }
        }

        auto* reg = arena_.create<Variable>(fallback_register_name(output.offset, output.size), output.size);
        reg->set_ir_type(unsigned_integer_for_size(output.size));
        out.destination = reg;
        return out;
    }

    if (output.space_name == architecture_.data_space_name || output.space_name == architecture_.code_space_name) {
        auto* ptr = arena_.create<Constant>(output.offset, architecture_.pointer_size);
        out.destination = arena_.create<Operation>(OperationType::deref, std::vector<Expression*>{ptr}, output.size);
        return out;
    }

    auto* dest = arena_.create<Variable>(
        output.space_name + "_" + hex_u64(output.offset) + "_" + std::to_string(output.size),
        output.size);
    dest->set_ir_type(unsigned_integer_for_size(output.size));
    out.destination = dest;
    return out;
}

std::optional<PcodeLowerer::StackAddress> PcodeLowerer::stack_offset_for_address(
    Expression* expr,
    const LocalDefinitionMap& local_defs) const {
    std::unordered_set<const Expression*> active;
    const auto resolve = [&](const auto& self, Expression* current) -> std::optional<StackAddress> {
        if (current == nullptr || !active.insert(current).second) {
            return std::nullopt;
        }

        auto finish = [&](std::optional<StackAddress> result) {
            active.erase(current);
            return result;
        };

        if (auto* reg = dyn_cast<Variable>(current)) {
            if (reg->name() == "sp") {
                return finish(StackAddress{PcodeStackBase::StackPointer, 0});
            }
            if (reg->name() == "x29") {
                return finish(StackAddress{PcodeStackBase::FramePointer, 0});
            }

            for (const auto& [_, definition] : local_defs) {
                auto* storage = dyn_cast<Variable>(definition.storage);
                if (storage != nullptr && definition.provenance != nullptr
                    && storage->name() == reg->name()) {
                    return finish(self(self, definition.provenance));
                }
            }
            return finish(std::nullopt);
        }

        auto* op = dyn_cast<Operation>(current);
        if (!op || op->operands().size() != 2
            || (op->type() != OperationType::add && op->type() != OperationType::sub)) {
            return finish(std::nullopt);
        }

        Expression* base_expr = op->operands()[0];
        auto* delta_const = dyn_cast<Constant>(op->operands()[1]);
        if (delta_const == nullptr && op->type() == OperationType::add) {
            delta_const = dyn_cast<Constant>(op->operands()[0]);
            base_expr = op->operands()[1];
        }
        if (delta_const == nullptr) {
            return finish(std::nullopt);
        }

        auto base = self(self, base_expr);
        if (!base.has_value()) {
            return finish(std::nullopt);
        }

        const std::int64_t delta = static_cast<std::int64_t>(delta_const->value());
        if ((op->type() == OperationType::add && delta > 0
                && base->offset > std::numeric_limits<std::int64_t>::max() - delta)
            || (op->type() == OperationType::add && delta < 0
                && base->offset < std::numeric_limits<std::int64_t>::min() - delta)
            || (op->type() == OperationType::sub && delta < 0
                && base->offset > std::numeric_limits<std::int64_t>::max() + delta)
            || (op->type() == OperationType::sub && delta > 0
                && base->offset < std::numeric_limits<std::int64_t>::min() + delta)) {
            return finish(std::nullopt);
        }
        base->offset += op->type() == OperationType::add ? delta : -delta;
        return finish(base);
    };

    return resolve(resolve, expr);
}

Expression* PcodeLowerer::maybe_stack_or_memory_read(Expression* address_expr,
                                                     const LocalDefinitionMap& local_defs,
                                                     ida::Address instruction_ea,
                                                     std::size_t width) {
    if (auto stack_offset = stack_offset_for_address(address_expr, local_defs)) {
        if (architecture_.stack_variable_resolver) {
            if (auto* slot = architecture_.stack_variable_resolver(
                    arena_, stack_offset->base, instruction_ea, stack_offset->offset, width)) {
                return slot;
            }
        }
    }

    auto* deref = arena_.create<Operation>(OperationType::deref, std::vector<Expression*>{address_expr}, width);
    deref->set_ir_type(unsigned_integer_for_size(width));
    return deref;
}

Expression* PcodeLowerer::lower_pointer_expression(
    const RawPcodeVarnode& ptr,
    const LocalDefinitionMap& local_defs) {
    Expression* expr = lower_varnode_read(ptr, local_defs);
    if (expr && !expr->ir_type()) {
        expr->set_ir_type(std::make_shared<Pointer>(Integer::uint8_t(), architecture_.pointer_size * 8));
    }
    return expr;
}

Expression* PcodeLowerer::lower_call_target(const RawPcodeVarnode& target,
                                            const LocalDefinitionMap& local_defs) {
    Expression* expr = lower_varnode_read(target, local_defs);
    if (auto* c = dyn_cast<Constant>(expr); c && architecture_.symbol_resolver) {
        const ida::Address target_addr = static_cast<ida::Address>(c->value());
        if (auto name = architecture_.symbol_resolver(target_addr); name && !name->empty()) {
            return arena_.create<GlobalVariable>(
                *name,
                architecture_.pointer_size,
                arena_.create<Constant>(c->value(), architecture_.pointer_size),
                false);
        }
    }
    return expr;
}

ida::Result<std::vector<Instruction*>> PcodeLowerer::lower_instruction(const RawPcodeInstruction& instruction) {
    current_instruction_ea_ = instruction.address;
    std::vector<Instruction*> emitted;
    LocalDefinitionMap local_defs;
    bool terminated = false;
    Expression* micro_exit_condition = nullptr;

    for (std::size_t op_index = 0; op_index < instruction.ops.size(); ++op_index) {
        const RawPcodeOp& op = instruction.ops[op_index];
        current_op_ordinal_ = op.op_ordinal;
        const auto unsupported = [&](const std::string& why) -> ida::Result<std::vector<Instruction*>> {
            record_unsupported(op, why);
            return std::unexpected(ida::Error::unsupported(
                "Unsupported P-Code semantics",
                op.opcode + " at 0x" + hex_u64(static_cast<std::uint64_t>(op.instruction_ea)) + ": " + why));
        };

        if (op.instruction_ea != instruction.address) {
            return unsupported("operation address does not match its containing instruction");
        }
        if (op.op_ordinal != op_index) {
            return unsupported("operation ordinal is not contiguous");
        }
        if (op.opcode.empty()) {
            return unsupported("empty opcode");
        }
        if (terminated) {
            return unsupported("operation follows an architectural control-flow terminator");
        }
        if (micro_exit_condition != nullptr
            && (!op.output.has_value()
                || op.output->space_name != architecture_.register_space_name)) {
            return unsupported(
                "operation after an intra-instruction conditional exit is not a predicable register write");
        }

        const auto known_space = [&](const RawPcodeVarnode& varnode) {
            return varnode.space_name == architecture_.const_space_name
                || varnode.space_name == architecture_.unique_space_name
                || varnode.space_name == architecture_.register_space_name
                || varnode.space_name == architecture_.data_space_name
                || varnode.space_name == architecture_.code_space_name;
        };
        const auto validate_varnode = [&](const RawPcodeVarnode& varnode,
                                          std::string_view role) -> std::optional<std::string> {
            if (varnode.size == 0) {
                return std::string(role) + " varnode has zero width";
            }
            if (varnode.size > std::numeric_limits<std::size_t>::max() / 8) {
                return std::string(role) + " varnode width overflows bit-size representation";
            }
            if (varnode.space_name == architecture_.const_space_name && varnode.size > 8) {
                return std::string(role) + " constant exceeds the IR's 64-bit literal capacity";
            }
            if (!known_space(varnode)) {
                return std::string(role) + " varnode uses unsupported address space '"
                    + varnode.space_name + "'";
            }
            return std::nullopt;
        };

        if (op.output.has_value()) {
            if (auto error = validate_varnode(*op.output, "output")) {
                return unsupported(*error);
            }
            if (op.output->space_name == architecture_.const_space_name) {
                return unsupported("constant-space varnode cannot be an output");
            }
            if (op.output->space_name != architecture_.unique_space_name
                && op.output->space_name != architecture_.register_space_name) {
                return unsupported("output varnode is not register or unique storage");
            }
            observe_abi_register_write(*op.output);
        }
        for (const auto& input : op.inputs) {
            if (auto error = validate_varnode(input, "input")) {
                return unsupported(*error);
            }
            if (input.space_name == architecture_.unique_space_name
                && !local_defs.contains(unique_key_for(input))) {
                return unsupported("unique-space varnode is read before definition");
            }
        }

        bool uses_register_fallback = false;
        const auto inspect_register = [&](const RawPcodeVarnode& varnode) {
            if (varnode.space_name != architecture_.register_space_name) {
                return;
            }
            if (!architecture_.register_mapper
                || !architecture_.register_mapper(varnode.offset, varnode.size).has_value()) {
                uses_register_fallback = true;
            }
        };
        if (op.output.has_value()) {
            inspect_register(*op.output);
        }
        for (const auto& input : op.inputs) {
            inspect_register(input);
        }

        bool support_counted = false;
        const auto complete_op = [&](std::optional<std::string> fallback_reason = std::nullopt) {
            if (support_counted) {
                return;
            }
            support_counted = true;
            if (fallback_reason.has_value()) {
                record_fallback(op, *fallback_reason);
            } else if (uses_register_fallback) {
                record_fallback(op, "stable offset/width register name used");
            } else {
                task_.mutable_frontend_support_report().implemented_ops += 1;
            }
        };

        auto emit_assignment = [&](Expression* destination, Expression* value) {
            auto* assign = arena_.create<Assignment>(destination, value);
            assign->set_address(op.instruction_ea);
            emitted.push_back(assign);
        };

        auto assign_output = [&](Expression* value) {
            if (op.output->space_name == architecture_.register_space_name
                && architecture_.register_mapper
                && architecture_.stack_variable_resolver) {
                auto mapped_output = architecture_.register_mapper(
                    op.output->offset, op.output->size);
                const bool preserves_stack_base = mapped_output.has_value()
                    && (mapped_output->canonical_name == "sp"
                        || mapped_output->canonical_name == "x29");
                if (mapped_output.has_value() && !preserves_stack_base) {
                    if (auto stack_address = stack_offset_for_address(value, local_defs)) {
                        if (auto* slot = architecture_.stack_variable_resolver(
                                arena_,
                                stack_address->base,
                                op.instruction_ea,
                                stack_address->offset,
                                op.output->size)) {
                            auto* address = arena_.create<Operation>(
                                OperationType::address_of,
                                std::vector<Expression*>{slot},
                                architecture_.pointer_size);
                            address->set_ir_type(std::make_shared<Pointer>(
                                slot->ir_type() ? slot->ir_type() : Integer::uint8_t(),
                                architecture_.pointer_size * 8));
                            value = address;
                        }
                    }
                }
            }

            WriteTarget out = lower_output_write(*op.output, value);
            if (out.emit_assignment && out.destination) {
                if (micro_exit_condition != nullptr) {
                    auto* selected = arena_.create<Operation>(
                        OperationType::ternary,
                        std::vector<Expression*>{
                            micro_exit_condition->copy(arena_),
                            out.destination->copy(arena_),
                            out.value,
                        },
                        out.destination->size_bytes);
                    selected->set_ir_type(out.destination->ir_type());
                    out.value = selected;
                }
                emit_assignment(out.destination, out.value);
            }
            if (op.output->space_name == architecture_.unique_space_name
                && out.destination != nullptr) {
                local_defs[unique_key_for(*op.output)] = LocalDefinition{
                    out.destination,
                    out.value,
                };
            }
        };

        if (op.opcode == "COPY") {
            if (!op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("COPY shape mismatch");
            }
            assign_output(lower_varnode_read(op.inputs[0], local_defs));
            complete_op();
            continue;
        }

        std::optional<OperationType> binary_type;
        bool comparison = false;
        bool signed_operands = false;
        if (op.opcode == "INT_ADD") binary_type = OperationType::add;
        else if (op.opcode == "INT_SUB") binary_type = OperationType::sub;
        else if (op.opcode == "INT_MULT") binary_type = OperationType::mul;
        else if (op.opcode == "INT_DIV") binary_type = OperationType::div_us;
        else if (op.opcode == "INT_SDIV") { binary_type = OperationType::div; signed_operands = true; }
        else if (op.opcode == "INT_REM") binary_type = OperationType::mod_us;
        else if (op.opcode == "INT_SREM") { binary_type = OperationType::mod; signed_operands = true; }
        else if (op.opcode == "INT_AND") binary_type = OperationType::bit_and;
        else if (op.opcode == "INT_OR") binary_type = OperationType::bit_or;
        else if (op.opcode == "INT_XOR") binary_type = OperationType::bit_xor;
        else if (op.opcode == "INT_LEFT") binary_type = OperationType::shl;
        else if (op.opcode == "INT_RIGHT") binary_type = OperationType::shr_us;
        else if (op.opcode == "INT_SRIGHT") { binary_type = OperationType::shr; signed_operands = true; }
        else if (op.opcode == "INT_EQUAL") { binary_type = OperationType::eq; comparison = true; }
        else if (op.opcode == "INT_NOTEQUAL") { binary_type = OperationType::neq; comparison = true; }
        else if (op.opcode == "INT_LESS") { binary_type = OperationType::lt_us; comparison = true; }
        else if (op.opcode == "INT_LESSEQUAL") { binary_type = OperationType::le_us; comparison = true; }
        else if (op.opcode == "INT_SLESS") { binary_type = OperationType::lt; comparison = true; signed_operands = true; }
        else if (op.opcode == "INT_SLESSEQUAL") { binary_type = OperationType::le; comparison = true; signed_operands = true; }
        else if (op.opcode == "BOOL_AND") binary_type = OperationType::logical_and;
        else if (op.opcode == "BOOL_OR") binary_type = OperationType::logical_or;
        else if (op.opcode == "BOOL_XOR") binary_type = OperationType::bit_xor;

        if (binary_type.has_value()) {
            if (!op.output.has_value() || op.inputs.size() != 2) {
                return unsupported("binary op shape mismatch");
            }
            const bool shift_op = op.opcode == "INT_LEFT"
                || op.opcode == "INT_RIGHT" || op.opcode == "INT_SRIGHT";
            const bool boolean_op = op.opcode == "BOOL_AND"
                || op.opcode == "BOOL_OR" || op.opcode == "BOOL_XOR";
            if (!shift_op && op.inputs[0].size != op.inputs[1].size) {
                return unsupported("binary operands have different widths");
            }
            if (comparison || boolean_op) {
                if (op.output->size != 1) {
                    return unsupported("boolean result is not one byte wide");
                }
            } else if (op.output->size != op.inputs[0].size) {
                return unsupported("binary output width differs from its value operand");
            }
            Expression* lhs = lower_varnode_read(op.inputs[0], local_defs);
            Expression* rhs = lower_varnode_read(op.inputs[1], local_defs);
            if (signed_operands) {
                lhs = make_cast(lhs, op.inputs[0].size, true);
                if (op.opcode != "INT_SRIGHT") {
                    rhs = make_cast(rhs, op.inputs[1].size, true);
                }
            }
            Expression* expr = nullptr;
            if (comparison) {
                auto* condition = arena_.create<Condition>(
                    *binary_type, lhs, rhs, op.output->size);
                condition->set_ir_type(CustomType::bool_type());
                expr = condition;
            } else {
                auto* operation = arena_.create<Operation>(
                    *binary_type, std::vector<Expression*>{lhs, rhs}, op.output->size);
                operation->set_ir_type(signed_operands
                    ? signed_integer_for_size(op.output->size)
                    : unsigned_integer_for_size(op.output->size));
                expr = operation;
            }
            assign_output(expr);
            complete_op();
            continue;
        }

        std::optional<OperationType> unary_type;
        if (op.opcode == "INT_2COMP") unary_type = OperationType::negate;
        else if (op.opcode == "INT_NEGATE") unary_type = OperationType::bit_not;
        else if (op.opcode == "BOOL_NEGATE") unary_type = OperationType::logical_not;

        if (unary_type.has_value()) {
            if (!op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("unary op shape mismatch");
            }
            Expression* input = lower_varnode_read(op.inputs[0], local_defs);
            auto* expr = arena_.create<Operation>(
                *unary_type, std::vector<Expression*>{input}, op.output->size);
            expr->set_ir_type(unsigned_integer_for_size(op.output->size));
            assign_output(expr);
            complete_op();
            continue;
        }

        if (op.opcode == "INT_ZEXT" || op.opcode == "INT_SEXT" || op.opcode == "SUBPIECE") {
            if (!op.output.has_value()) {
                return unsupported("cast/truncation op shape mismatch");
            }
            const std::size_t expected_inputs = op.opcode == "SUBPIECE" ? 2 : 1;
            if (op.inputs.size() != expected_inputs) {
                return unsupported("cast/truncation op shape mismatch");
            }
            if (op.opcode != "SUBPIECE" && op.output->size < op.inputs[0].size) {
                return unsupported("integer extension narrows its input");
            }

            Expression* input = lower_varnode_read(op.inputs[0], local_defs);
            Expression* expr = nullptr;
            if (op.opcode == "SUBPIECE") {
                const RawPcodeVarnode& offset = op.inputs[1];
                if (offset.space_name != architecture_.const_space_name) {
                    return unsupported("SUBPIECE byte offset is not constant");
                }
                if (offset.offset > op.inputs[0].size
                    || op.output->size > op.inputs[0].size - static_cast<std::size_t>(offset.offset)) {
                    return unsupported("SUBPIECE range exceeds source width");
                }
                if (offset.offset > std::numeric_limits<std::size_t>::max() / 8) {
                    return unsupported("SUBPIECE byte offset overflows host size");
                }
                const std::size_t shift_bits = static_cast<std::size_t>(offset.offset) * 8;
                if (shift_bits != 0) {
                    input = arena_.create<Operation>(
                        OperationType::shr_us,
                        std::vector<Expression*>{
                            input,
                            arena_.create<Constant>(shift_bits, op.inputs[0].size)},
                        op.inputs[0].size);
                }
                expr = make_cast(input, op.output->size, false);
            } else {
                if (op.opcode == "INT_SEXT") {
                    input = make_cast(input, op.inputs[0].size, true);
                }
                expr = make_cast(input, op.output->size, op.opcode == "INT_SEXT");
            }
            assign_output(expr);
            complete_op();
            continue;
        }

        std::optional<OperationType> float_binary_type;
        bool float_comparison = false;
        if (op.opcode == "FLOAT_ADD") float_binary_type = OperationType::add_float;
        else if (op.opcode == "FLOAT_SUB") float_binary_type = OperationType::sub_float;
        else if (op.opcode == "FLOAT_MULT") float_binary_type = OperationType::mul_float;
        else if (op.opcode == "FLOAT_DIV") float_binary_type = OperationType::div_float;
        else if (op.opcode == "FLOAT_EQUAL") { float_binary_type = OperationType::eq; float_comparison = true; }
        else if (op.opcode == "FLOAT_NOTEQUAL") { float_binary_type = OperationType::neq; float_comparison = true; }
        else if (op.opcode == "FLOAT_LESS") { float_binary_type = OperationType::lt; float_comparison = true; }
        else if (op.opcode == "FLOAT_LESSEQUAL") { float_binary_type = OperationType::le; float_comparison = true; }

        if (float_binary_type.has_value()) {
            if (!op.output.has_value() || op.inputs.size() != 2) {
                return unsupported("floating binary op shape mismatch");
            }
            if (op.inputs[0].size != op.inputs[1].size) {
                return unsupported("floating operands have different widths");
            }
            if ((op.inputs[0].size != 2 && op.inputs[0].size != 4
                    && op.inputs[0].size != 8 && op.inputs[0].size != 10
                    && op.inputs[0].size != 16)
                || (float_comparison ? op.output->size != 1
                                     : op.output->size != op.inputs[0].size)) {
                return unsupported("unsupported floating-point width relationship");
            }

            Expression* lhs = make_float_operand(
                lower_varnode_read(op.inputs[0], local_defs), op.inputs[0].size);
            Expression* rhs = make_float_operand(
                lower_varnode_read(op.inputs[1], local_defs), op.inputs[1].size);
            Expression* result = nullptr;
            if (float_comparison) {
                auto* condition = arena_.create<Condition>(
                    *float_binary_type, lhs, rhs, op.output->size);
                condition->set_ir_type(CustomType::bool_type());
                result = condition;
            } else {
                auto* operation = arena_.create<Operation>(
                    *float_binary_type,
                    std::vector<Expression*>{lhs, rhs},
                    op.output->size);
                operation->set_ir_type(float_for_size(op.output->size));
                result = operation;
            }
            assign_output(result);
            complete_op();
            continue;
        }

        if (op.opcode == "FLOAT_NEG" || op.opcode == "INT2FLOAT"
            || op.opcode == "FLOAT2FLOAT" || op.opcode == "TRUNC") {
            if (!op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("floating conversion/unary op shape mismatch");
            }

            Expression* input = lower_varnode_read(op.inputs[0], local_defs);
            Expression* result = nullptr;
            if (op.opcode == "FLOAT_NEG") {
                if (op.output->size != op.inputs[0].size) {
                    return unsupported("FLOAT_NEG changes width");
                }
                input = make_float_operand(input, op.inputs[0].size);
                auto* negated = arena_.create<Operation>(
                    OperationType::negate,
                    std::vector<Expression*>{input},
                    op.output->size);
                negated->set_ir_type(float_for_size(op.output->size));
                result = negated;
            } else if (op.opcode == "INT2FLOAT") {
                input = make_cast(input, op.inputs[0].size, true);
                result = make_float_cast(input, op.output->size);
            } else if (op.opcode == "FLOAT2FLOAT") {
                input = make_float_operand(input, op.inputs[0].size);
                result = make_float_cast(input, op.output->size);
            } else {
                input = make_float_operand(input, op.inputs[0].size);
                result = make_cast(input, op.output->size, true);
            }
            assign_output(result);
            complete_op();
            continue;
        }

        if (op.opcode == "FLOAT_NAN" || op.opcode == "FLOAT_ABS"
            || op.opcode == "FLOAT_SQRT" || op.opcode == "CEIL"
            || op.opcode == "FLOOR" || op.opcode == "ROUND") {
            if (!op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("floating intrinsic op shape mismatch");
            }
            const bool boolean_result = op.opcode == "FLOAT_NAN";
            if ((boolean_result && op.output->size != 1)
                || (!boolean_result && op.output->size != op.inputs[0].size)) {
                return unsupported("floating intrinsic width mismatch");
            }
            auto* target = arena_.create<GlobalVariable>(
                "__pcode_" + canonical_function_name(op.opcode),
                architecture_.pointer_size,
                nullptr,
                true);
            auto* call = arena_.create<Call>(
                target,
                std::vector<Expression*>{make_float_operand(
                    lower_varnode_read(op.inputs[0], local_defs), op.inputs[0].size)},
                op.output->size);
            call->set_ir_type(boolean_result
                ? CustomType::bool_type() : float_for_size(op.output->size));
            assign_output(call);
            complete_op("lowered to explicit floating-point intrinsic");
            continue;
        }

        if (op.opcode == "INT_CARRY" || op.opcode == "INT_SCARRY" || op.opcode == "INT_SBORROW") {
            if (!op.output.has_value() || op.inputs.size() != 2) {
                return unsupported("carry/borrow op shape mismatch");
            }
            if (op.inputs[0].size != op.inputs[1].size) {
                return unsupported("carry/borrow operands have different widths");
            }
            const std::size_t width = op.inputs[0].size;
            if (width == 0 || width > 8) {
                return unsupported("carry/borrow lowering supports widths from 1 to 8 bytes");
            }
            Expression* lhs = lower_varnode_read(op.inputs[0], local_defs);
            Expression* rhs = lower_varnode_read(op.inputs[1], local_defs);
            Condition* condition = nullptr;
            if (op.opcode == "INT_CARRY") {
                auto* raw_sum = arena_.create<Operation>(
                    OperationType::add,
                    std::vector<Expression*>{lhs->copy(arena_), rhs},
                    width);
                Expression* sum = make_cast(raw_sum, width, false);
                condition = arena_.create<Condition>(OperationType::lt_us, sum, lhs, op.output->size);
            } else {
                const OperationType arithmetic = op.opcode == "INT_SCARRY"
                    ? OperationType::add : OperationType::sub;
                auto* raw_result = arena_.create<Operation>(
                    arithmetic,
                    std::vector<Expression*>{lhs->copy(arena_), rhs->copy(arena_)},
                    width);
                Expression* result = make_cast(raw_result, width, false);
                auto* lhs_xor_rhs = arena_.create<Operation>(
                    OperationType::bit_xor,
                    std::vector<Expression*>{lhs->copy(arena_), rhs},
                    width);
                Expression* first_term = lhs_xor_rhs;
                if (op.opcode == "INT_SCARRY") {
                    first_term = arena_.create<Operation>(
                        OperationType::bit_not,
                        std::vector<Expression*>{first_term},
                        width);
                }
                auto* lhs_xor_result = arena_.create<Operation>(
                    OperationType::bit_xor,
                    std::vector<Expression*>{lhs, result},
                    width);
                auto* overflow_bits = arena_.create<Operation>(
                    OperationType::bit_and,
                    std::vector<Expression*>{first_term, lhs_xor_result},
                    width);
                const std::uint64_t sign_mask = std::uint64_t{1} << (width * 8 - 1);
                auto* masked = arena_.create<Operation>(
                    OperationType::bit_and,
                    std::vector<Expression*>{overflow_bits, arena_.create<Constant>(sign_mask, width)},
                    width);
                condition = arena_.create<Condition>(
                    OperationType::neq,
                    masked,
                    arena_.create<Constant>(0, width),
                    op.output->size);
            }
            condition->set_ir_type(CustomType::bool_type());
            assign_output(condition);
            complete_op();
            continue;
        }

        if (op.opcode == "SUBPIECE") {
            // Handled in the cast/truncation family above.
            return unsupported("internal SUBPIECE dispatch failure");
        }

        if (op.opcode == "PIECE") {
            if (!op.output.has_value() || op.inputs.size() != 2) {
                return unsupported("PIECE shape mismatch");
            }
            if (op.inputs[0].size > std::numeric_limits<std::size_t>::max() - op.inputs[1].size
                || op.output->size != op.inputs[0].size + op.inputs[1].size) {
                return unsupported("PIECE output width is not the sum of its inputs");
            }
            if (op.inputs[1].size > std::numeric_limits<std::size_t>::max() / 8) {
                return unsupported("PIECE shift amount overflows host size");
            }
            Expression* high = lower_varnode_read(op.inputs[0], local_defs);
            Expression* low = lower_varnode_read(op.inputs[1], local_defs);
            auto* shift_amount = arena_.create<Constant>(op.inputs[1].size * 8, op.output->size);
            auto* shifted_high = arena_.create<Operation>(
                OperationType::shl,
                std::vector<Expression*>{make_cast(high, op.output->size, false), shift_amount},
                op.output->size);
            auto* combined = arena_.create<Operation>(
                OperationType::bit_or,
                std::vector<Expression*>{shifted_high, make_cast(low, op.output->size, false)},
                op.output->size);
            combined->set_ir_type(unsigned_integer_for_size(op.output->size));
            assign_output(combined);
            complete_op();
            continue;
        }

        if (op.opcode == "POPCOUNT" || op.opcode == "LZCOUNT") {
            if (!op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("count op shape mismatch");
            }
            auto* target = arena_.create<GlobalVariable>(
                op.opcode == "POPCOUNT" ? "__pcode_popcount" : "__pcode_lzcount",
                architecture_.pointer_size,
                nullptr,
                true);
            auto* call = arena_.create<Call>(
                target,
                std::vector<Expression*>{
                    lower_varnode_read(op.inputs[0], local_defs),
                    arena_.create<Constant>(op.inputs[0].size * 8, architecture_.pointer_size),
                },
                op.output->size);
            assign_output(call);
            complete_op("lowered to explicit intrinsic");
            continue;
        }

        if (op.opcode == "LOAD") {
            if (!op.output.has_value() || op.inputs.size() != 2) {
                return unsupported("LOAD shape mismatch");
            }
            if (op.inputs[0].space_name != architecture_.const_space_name) {
                return unsupported("LOAD address-space selector is not constant");
            }
            if (!op.memory_space_name.has_value()
                || *op.memory_space_name != architecture_.data_space_name) {
                return unsupported("LOAD selects an unsupported memory space");
            }
            if (op.memory_word_size != 1) {
                return unsupported("LOAD from word-addressed memory is not supported");
            }
            Expression* address_expr = lower_pointer_expression(op.inputs[1], local_defs);
            Expression* value = maybe_stack_or_memory_read(
                address_expr, local_defs, op.instruction_ea, op.output->size);
            assign_output(value);
            complete_op();
            continue;
        }

        if (op.opcode == "STORE") {
            if (op.output.has_value() || op.inputs.size() != 3) {
                return unsupported("STORE shape mismatch");
            }
            if (op.inputs[0].space_name != architecture_.const_space_name) {
                return unsupported("STORE address-space selector is not constant");
            }
            if (!op.memory_space_name.has_value()
                || *op.memory_space_name != architecture_.data_space_name) {
                return unsupported("STORE selects an unsupported memory space");
            }
            if (op.memory_word_size != 1) {
                return unsupported("STORE to word-addressed memory is not supported");
            }
            Expression* address_expr = lower_pointer_expression(op.inputs[1], local_defs);
            Expression* value = lower_varnode_read(op.inputs[2], local_defs);
            Expression* destination = maybe_stack_or_memory_read(
                address_expr, local_defs, op.instruction_ea, op.inputs[2].size);
            emit_assignment(destination, value);
            complete_op();
            continue;
        }

        if (op.opcode == "CBRANCH") {
            if (op.output.has_value() || op.inputs.size() != 2) {
                return unsupported("CBRANCH shape mismatch");
            }
            if (op.inputs[0].space_name == architecture_.const_space_name) {
                return unsupported("intra-instruction CBRANCH requires a P-Code micro-CFG");
            }
            if (op.inputs[0].space_name != architecture_.code_space_name) {
                return unsupported("architectural CBRANCH target is not in code space");
            }
            Expression* condition_expr = lower_varnode_read(op.inputs[1], local_defs);
            const bool has_representable_fallthrough = instruction.decoded_length > 0
                && instruction.decoded_length
                    <= std::numeric_limits<ida::Address>::max() - instruction.address;
            const ida::Address fallthrough = has_representable_fallthrough
                ? instruction.address + instruction.decoded_length : 0;
            if (has_representable_fallthrough
                && op.inputs[0].offset == static_cast<std::uint64_t>(fallthrough)) {
                // Sleigh uses a branch to the next machine address as an early
                // exit from an instruction's P-Code micro-CFG. AArch64 FCMP,
                // for example, initializes NZCV and skips later flag writes
                // when either operand is NaN. Preserve that behavior by
                // predicating the remaining register definitions: when the
                // branch condition is true, retain the value defined earlier
                // in this instruction; otherwise commit the new value.
                micro_exit_condition = make_nonzero_condition(condition_expr);
                complete_op();
                continue;
            }
            auto* branch = arena_.create<Branch>(make_nonzero_condition(condition_expr));
            branch->set_address(op.instruction_ea);
            emitted.push_back(branch);
            terminated = true;
            complete_op();
            continue;
        }

        if (op.opcode == "BRANCH") {
            if (op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("BRANCH shape mismatch");
            }
            if (op.inputs[0].space_name == architecture_.const_space_name) {
                return unsupported("intra-instruction BRANCH requires a P-Code micro-CFG");
            }
            if (op.inputs[0].space_name != architecture_.code_space_name) {
                return unsupported("architectural BRANCH target is not in code space");
            }
            terminated = true;
            complete_op();
            continue;
        }

        if (op.opcode == "BRANCHIND") {
            if (op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("BRANCHIND shape mismatch");
            }
            auto* branch = arena_.create<IndirectBranch>(lower_varnode_read(op.inputs[0], local_defs));
            branch->set_address(op.instruction_ea);
            emitted.push_back(branch);
            terminated = true;
            complete_op();
            continue;
        }

        if (op.opcode == "CALL" || op.opcode == "CALLIND") {
            if (op.output.has_value() || op.inputs.size() != 1) {
                return unsupported("CALL shape mismatch");
            }
            Expression* target = lower_call_target(op.inputs[0], local_defs);
            const std::string target_name = dyn_cast<GlobalVariable>(target)
                ? canonical_function_name(dyn_cast<GlobalVariable>(target)->name())
                : std::string();
            std::optional<ida::Address> direct_target;
            if (auto* c = dyn_cast<Constant>(target)) {
                direct_target = static_cast<ida::Address>(c->value());
            } else if (auto* gv = dyn_cast<GlobalVariable>(target)) {
                if (auto* init = dyn_cast<Constant>(gv->initial_value())) {
                    direct_target = static_cast<ida::Address>(init->value());
                }
            }
            std::optional<PcodeFunctionInfo> function_info;
            if (direct_target.has_value() && architecture_.function_resolver) {
                function_info = architecture_.function_resolver(*direct_target);
            }
            const bool recursive_self_call = direct_target.has_value()
                && *direct_target == options_.function_address;

            std::vector<TypePtr> parameter_types;
            TypePtr return_type;
            bool prototype_known = false;
            if (function_info.has_value()) {
                parameter_types = function_info->parameter_types;
                return_type = function_info->return_type;
                prototype_known = function_info->prototype_known;
            }
            if (recursive_self_call) {
                if (const auto* self_type = as_function_type(options_.function_type)) {
                    parameter_types = self_type->parameters();
                    return_type = self_type->return_type();
                    prototype_known = true;
                } else if (parameter_types.empty()) {
                    const std::size_t count = signature_context_.parameter_count_hint > 0
                        ? signature_context_.parameter_count_hint : 1;
                    parameter_types.assign(count, std::make_shared<UnknownType>());
                }
            }

            std::size_t inferred_argument_count = 0;
            if (!prototype_known) {
                inferred_argument_count = inferred_gp_argument_count();
                while (parameter_types.size() < inferred_argument_count) {
                    parameter_types.push_back(std::make_shared<UnknownType>());
                }
            }

            std::vector<Expression*> args;
            args.reserve(parameter_types.size());
            std::size_t gp_index = 0;
            std::size_t fp_index = 0;
            bool has_stack_arguments = false;
            for (const TypePtr& parameter_type : parameter_types) {
                const bool floating = as_float_type(parameter_type) != nullptr;
                std::size_t& register_index = floating ? fp_index : gp_index;
                Expression* argument = make_abi_argument(register_index, parameter_type);
                if (argument == nullptr) {
                    has_stack_arguments = true;
                } else {
                    args.push_back(argument);
                    ++register_index;
                }
            }

            const bool returns_void = return_type && return_type->to_string() == "void";
            const std::size_t return_size = return_type && return_type->size_bytes() > 0
                ? return_type->size_bytes() : architecture_.pointer_size;
            if (!returns_void && return_type && as_float_type(return_type) == nullptr
                && return_size > architecture_.pointer_size) {
                return unsupported("multi-register or indirect call return is not modeled");
            }
            auto* call = arena_.create<Call>(target, std::move(args), return_size);
            if (prototype_known || !parameter_types.empty() || return_type) {
                call->set_ir_type(std::make_shared<FunctionTypeDef>(
                    return_type ? return_type : std::make_shared<UnknownType>(),
                    parameter_types));
            }
            if (returns_void) {
                emit_assignment(nullptr, call);
            } else {
                emit_assignment(make_abi_return_storage(return_type), call);
            }
            reset_call_argument_evidence(!returns_void);
            if (!prototype_known) {
                std::string message = target_name.empty()
                    ? "callee prototype unavailable"
                    : "callee prototype unavailable for " + target_name;
                if (inferred_argument_count > 0) {
                    message += "; inferred " + std::to_string(inferred_argument_count)
                        + " contiguous general-purpose argument register"
                        + (inferred_argument_count == 1 ? "" : "s")
                        + " from current basic-block writes";
                } else {
                    message += "; no contiguous general-purpose argument-register writes observed";
                }
                complete_op(message);
            } else if (has_stack_arguments) {
                complete_op("stack-passed call arguments are not modeled");
            } else {
                complete_op();
            }
            continue;
        }

        if (op.opcode == "CALLOTHER") {
            if (op.inputs.empty()) {
                return unsupported("CALLOTHER is missing its userop identifier");
            }
            std::string userop_name = "unknown";
            if (op.inputs[0].space_name == architecture_.const_space_name) {
                userop_name = hex_u64(op.inputs[0].offset);
            }
            auto* target = arena_.create<GlobalVariable>(
                "__pcode_userop_" + userop_name,
                architecture_.pointer_size,
                nullptr,
                true);
            std::vector<Expression*> args;
            args.reserve(op.inputs.size() - 1);
            for (std::size_t index = 1; index < op.inputs.size(); ++index) {
                args.push_back(lower_varnode_read(op.inputs[index], local_defs));
            }
            const std::size_t result_size = op.output.has_value()
                ? op.output->size : static_cast<std::size_t>(1);
            auto* call = arena_.create<Call>(target, std::move(args), result_size);
            if (op.output.has_value()) {
                assign_output(call);
            } else {
                emit_assignment(nullptr, call);
            }
            complete_op("lowered to explicit userop intrinsic");
            continue;
        }

        if (op.opcode == "RETURN") {
            if (op.output.has_value() || op.inputs.empty()) {
                return unsupported("RETURN shape mismatch");
            }
            std::vector<Expression*> values;
            const auto* function_type = options_.function_type
                ? type_dyn_cast<FunctionTypeDef>(options_.function_type.get()) : nullptr;
            const bool returns_void = function_type != nullptr
                && function_type->return_type()
                && function_type->return_type()->to_string() == "void";
            if (!returns_void) {
                const TypePtr return_type = function_type
                    ? function_type->return_type() : nullptr;
                if (return_type && as_float_type(return_type) == nullptr
                    && return_type->size_bytes() > architecture_.pointer_size) {
                    return unsupported("multi-register or indirect function return is not modeled");
                }
                values.push_back(make_abi_return_value(return_type));
            }
            auto* ret = arena_.create<Return>(std::move(values));
            ret->set_address(op.instruction_ea);
            emitted.push_back(ret);
            terminated = true;
            if (function_type == nullptr) {
                complete_op("function return type unavailable; assumed scalar x0 return");
            } else {
                complete_op();
            }
            continue;
        }

        return unsupported("opcode not in the supported raw P-Code set");
    }

    return emitted;
}

} // namespace aletheia
