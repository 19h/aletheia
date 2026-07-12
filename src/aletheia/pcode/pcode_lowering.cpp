#include "pcode_lowering.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
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
    if (name.starts_with("j_")) {
        name.erase(0, 2);
        while (!name.empty() && name.front() == '_') {
            name.erase(name.begin());
        }
    }
    return name;
}

const Float* as_float_type(const TypePtr& type) {
    return type ? type_dyn_cast<Float>(type.get()) : nullptr;
}

const FunctionTypeDef* as_function_type(const TypePtr& type) {
    return type ? type_dyn_cast<FunctionTypeDef>(type.get()) : nullptr;
}

bool is_two_register_integer_type(const TypePtr& type) {
    return type && type->type_kind() == TypeKind::Integer
        && type->size_bytes() == 16;
}

std::optional<std::size_t> numbered_register_index(
    const std::string& name,
    char prefix) {
    if (name.size() < 2 || name.front() != prefix) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (std::size_t index = 1; index < name.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(name[index]);
        if (!std::isdigit(character)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::size_t>(character - '0');
    }
    return value;
}

std::optional<std::size_t> xmm_register_index(const std::string& name) {
    constexpr std::string_view prefix = "xmm";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (std::size_t index = prefix.size(); index < name.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(name[index]);
        if (!std::isdigit(character)) return std::nullopt;
        value = value * 10 + static_cast<std::size_t>(character - '0');
    }
    return value;
}

std::optional<std::size_t> abi_floating_register_index(
    const PcodeArchitectureContext* architecture,
    const std::string& name) {
    if (architecture && architecture->arch_name.starts_with("x86_64")) {
        return xmm_register_index(name);
    }
    for (char prefix : {'s', 'd', 'q', 'v'}) {
        if (auto index = numbered_register_index(name, prefix)) return index;
    }
    return std::nullopt;
}

std::string abi_floating_register_name(
    const PcodeArchitectureContext& architecture,
    std::size_t index,
    std::size_t width) {
    if (architecture.arch_name.starts_with("x86_64")) {
        return "xmm" + std::to_string(index);
    }
    const char prefix = width == 4 ? 's' : (width == 8 ? 'd' : 'q');
    return std::string(1, prefix) + std::to_string(index);
}

bool is_aapcs64_call_preserved_register(
    const std::string& name,
    std::size_t size_bytes) {
    if (name == "sp") {
        return true;
    }
    if (const auto index = numbered_register_index(name, 'x')) {
        return *index >= 19 && *index <= 29;
    }
    // AAPCS64 preserves only the low 64 bits of v8-v15. Sleigh may expose
    // those lanes as sN or dN; wider qN/vN semantics cannot be retained.
    for (char prefix : {'s', 'd'}) {
        if (const auto index = numbered_register_index(name, prefix)) {
            return *index >= 8 && *index <= 15 && size_bytes <= 8;
        }
    }
    return false;
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

void PcodeLowerer::begin_basic_block(bool preserve_linear_fallthrough) {
    if (!preserve_linear_fallthrough) {
        reset_call_argument_evidence(false);
        register_semantics_.clear();
        stack_stored_types_.clear();
    }
}

void PcodeLowerer::begin_basic_block(
    const std::vector<CallArgumentEvidence>& predecessor_evidence) {
    reset_call_argument_evidence(false);
    register_semantics_.clear();
    stack_stored_types_.clear();
    if (predecessor_evidence.empty()) {
        return;
    }

    gp_argument_write_evidence_.fill(true);
    fp_argument_write_evidence_.fill(true);
    for (const CallArgumentEvidence& evidence : predecessor_evidence) {
        for (std::size_t index = 0; index < gp_argument_write_evidence_.size(); ++index) {
            gp_argument_write_evidence_[index] = gp_argument_write_evidence_[index]
                && evidence.gp_register_writes[index];
            fp_argument_write_evidence_[index] = fp_argument_write_evidence_[index]
                && evidence.fp_register_writes[index];
        }

    }

    // Outgoing stack arguments are a must property. At a join, retain only
    // offsets defined with the same width on every predecessor. Prefer the
    // common stack slot over a branch-local value expression so the later SSA
    // pass can construct the memory value selected by control flow.
    const auto same_stack_slot = [](const Variable* lhs, const Variable* rhs) {
        if (!lhs || !rhs || lhs->size_bytes != rhs->size_bytes) return false;
        if (lhs->is_stack_variable() && rhs->is_stack_variable()) {
            return lhs->stack_offset() == rhs->stack_offset();
        }
        return lhs->name() == rhs->name();
    };
    const auto same_storage_leaf = [&](const Expression* lhs, const Expression* rhs) {
        if (!lhs || !rhs || lhs->size_bytes != rhs->size_bytes) return false;
        if (const auto* lhs_var = dyn_cast<Variable>(lhs)) {
            const auto* rhs_var = dyn_cast<Variable>(rhs);
            return rhs_var && lhs_var->name() == rhs_var->name()
                && lhs_var->ssa_version() == rhs_var->ssa_version();
        }
        if (const auto* lhs_constant = dyn_cast<Constant>(lhs)) {
            const auto* rhs_constant = dyn_cast<Constant>(rhs);
            return rhs_constant && lhs_constant->value() == rhs_constant->value();
        }
        return false;
    };
    for (const StackArgumentEvidence& candidate :
         predecessor_evidence.front().stack_writes) {
        if (candidate.width == 0 || !candidate.storage) continue;
        std::vector<const StackArgumentEvidence*> matches;
        matches.reserve(predecessor_evidence.size());
        bool complete = true;
        for (const CallArgumentEvidence& evidence : predecessor_evidence) {
            const auto match = std::find_if(
                evidence.stack_writes.begin(), evidence.stack_writes.end(),
                [&](const StackArgumentEvidence& other) {
                    return other.offset == candidate.offset
                        && other.width == candidate.width
                        && other.storage != nullptr;
                });
            if (match == evidence.stack_writes.end()) {
                complete = false;
                break;
            }
            matches.push_back(&*match);
        }
        if (!complete) continue;

        const bool common_slot = candidate.stack_slot
            && std::all_of(matches.begin(), matches.end(),
                [&](const StackArgumentEvidence* match) {
                    return match && same_stack_slot(
                        candidate.stack_slot, match->stack_slot);
                });
        const bool common_leaf = std::all_of(matches.begin(), matches.end(),
            [&](const StackArgumentEvidence* match) {
                return match && same_storage_leaf(
                    candidate.storage, match->storage);
            });
        if (!common_slot && !common_leaf) continue;

        Expression* merged_storage = common_slot
            ? candidate.stack_slot->copy(arena_)
            : candidate.storage->copy(arena_);
        auto* merged_slot = common_slot
            ? dyn_cast<Variable>(candidate.stack_slot->copy(arena_)) : nullptr;
        ida::Address latest_ea = candidate.instruction_ea;
        for (const StackArgumentEvidence* match : matches) {
            latest_ea = std::max(latest_ea, match->instruction_ea);
        }
        stack_argument_write_evidence_.push_back(StackArgumentEvidence{
            candidate.offset,
            candidate.width,
            merged_storage,
            merged_slot,
            latest_ea,
        });
    }

    // Stack type provenance is a must property: retain a slot only when every
    // predecessor agrees on base, offset, width, and semantic type. This
    // prevents a single floating path from retyping an integer path at a join.
    for (const StackTypeEvidence& candidate :
         predecessor_evidence.front().stack_types) {
        if (!candidate.type || candidate.width == 0) continue;
        const bool agreed = std::all_of(
            predecessor_evidence.begin() + 1,
            predecessor_evidence.end(),
            [&](const CallArgumentEvidence& evidence) {
                return std::any_of(
                    evidence.stack_types.begin(),
                    evidence.stack_types.end(),
                    [&](const StackTypeEvidence& other) {
                        return other.base == candidate.base
                            && other.offset == candidate.offset
                            && other.width == candidate.width
                            && other.type && *other.type == *candidate.type;
                    });
            });
        if (agreed) {
            stack_stored_types_.push_back(StackStoredType{
                StackAddress{candidate.base, candidate.offset},
                candidate.width,
                candidate.type,
            });
        }
    }
}

PcodeLowerer::CallArgumentEvidence PcodeLowerer::call_argument_evidence() const {
    CallArgumentEvidence result;
    result.gp_register_writes = gp_argument_write_evidence_;
    result.fp_register_writes = fp_argument_write_evidence_;
    result.stack_writes.reserve(stack_argument_write_evidence_.size());
    for (const StackArgumentEvidence& slot : stack_argument_write_evidence_) {
        result.stack_writes.push_back(StackArgumentEvidence{
            slot.offset,
            slot.width,
            slot.storage ? slot.storage->copy(arena_) : nullptr,
            slot.stack_slot
                ? dyn_cast<Variable>(slot.stack_slot->copy(arena_)) : nullptr,
            slot.instruction_ea,
        });
    }
    result.stack_types.reserve(stack_stored_types_.size());
    for (const StackStoredType& stored : stack_stored_types_) {
        if (!stored.type || stored.width == 0) continue;
        result.stack_types.push_back(StackTypeEvidence{
            stored.address.base,
            stored.address.offset,
            stored.width,
            stored.type,
        });
    }
    return result;
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
    if (name == architecture_.stack_pointer_register) {
        // Stack-relative evidence belongs to the current stack-pointer epoch.
        if (!architecture_.arch_name.starts_with("x86_64")) {
            stack_argument_write_evidence_.clear();
        }
        stack_stored_types_.clear();
        return;
    }
    if (const auto floating_index = abi_floating_register_index(
            &architecture_, name);
        floating_index.has_value()
        && *floating_index < fp_argument_write_evidence_.size()) {
        fp_argument_write_evidence_[*floating_index] = true;
        return;
    }
    const auto argument = std::find(
        architecture_.gp_argument_registers.begin(),
        architecture_.gp_argument_registers.end(),
        name);
    if (argument == architecture_.gp_argument_registers.end()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(architecture_.gp_argument_registers.begin(), argument));
    if (index < gp_argument_write_evidence_.size()) {
        gp_argument_write_evidence_[index] = true;
    }
}

std::size_t PcodeLowerer::inferred_gp_argument_count() const {
    std::size_t count = 0;
    while (count < gp_argument_write_evidence_.size()
           && gp_argument_write_evidence_[count]) {
        ++count;
    }
    return count;
}

std::size_t PcodeLowerer::inferred_fp_argument_count() const {
    std::size_t count = 0;
    while (count < fp_argument_write_evidence_.size()
           && fp_argument_write_evidence_[count]) {
        ++count;
    }
    return count;
}

void PcodeLowerer::reset_call_argument_evidence(bool preserve_return_register) {
    gp_argument_write_evidence_.fill(false);
    fp_argument_write_evidence_.fill(false);
    stack_argument_write_evidence_.clear();
    if (preserve_return_register) {
        // AAPCS64 aliases the scalar return register with argument register 0;
        // other ABIs (for example SysV x86-64) do not.
        if (!architecture_.gp_argument_registers.empty()
            && !architecture_.integer_return_registers.empty()
            && architecture_.gp_argument_registers.front()
                == architecture_.integer_return_registers.front()) {
            gp_argument_write_evidence_[0] = true;
        }
    }
}

void PcodeLowerer::invalidate_call_clobbered_register_semantics() {
    std::erase_if(register_semantics_, [&](const auto& entry) {
        const RegisterSemantic& semantic = entry.second;
        if (architecture_.arch_name.starts_with("x86_64")) {
            static const std::unordered_set<std::string> preserved = {
                "rbx", "rbp", "rsp", "r12", "r13", "r14", "r15"};
            return !preserved.contains(semantic.canonical_register_name);
        }
        return !is_aapcs64_call_preserved_register(
            semantic.canonical_register_name, semantic.size_bytes);
    });
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

    const TypePtr floating_type = float_for_size(size);
    if (isa<Constant>(expr)
        || (isa<Operation>(expr) && !isa<Variable>(expr))) {
        // Constants and freshly constructed memory expressions are owned by
        // this use and can carry the Sleigh interpretation directly.
        expr->set_ir_type(floating_type);
        return expr;
    }

    // Register/temporary definitions remain bitvectors. Represent the
    // floating interpretation explicitly so SSA/type propagation cannot
    // overwrite a per-use annotation with the integer definition type.
    auto* bitcast = arena_.create<Operation>(
        OperationType::bitcast,
        std::vector<Expression*>{expr},
        size);
    bitcast->set_ir_type(floating_type);
    return bitcast;
}

Expression* PcodeLowerer::make_abi_argument(std::size_t index, const TypePtr& type) {
    if (const auto* floating = as_float_type(type)) {
        const std::size_t width = floating->size_bytes();
        if (index >= 8 || (width != 4 && width != 8 && width != 16)) {
            return nullptr;
        }
        auto* reg = arena_.create<Variable>(
            abi_floating_register_name(architecture_, index, width), width);
        reg->set_ir_type(type);
        return reg;
    }

    if (index >= architecture_.gp_argument_registers.size()) {
        return nullptr;
    }

    auto* storage = arena_.create<Variable>(
        architecture_.gp_argument_registers[index], architecture_.pointer_size);
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
            auto* reg = arena_.create<Variable>(
                abi_floating_register_name(architecture_, 0, width), width);
            reg->set_ir_type(type);
            return reg;
        }
    }

    if (is_two_register_integer_type(type)) {
        if (architecture_.integer_return_registers.size() < 2) return nullptr;
        auto* low = arena_.create<Variable>(
            architecture_.integer_return_registers[0], architecture_.pointer_size);
        low->set_ir_type(Integer::uint64_t());
        auto* high = arena_.create<Variable>(
            architecture_.integer_return_registers[1], architecture_.pointer_size);
        high->set_ir_type(Integer::uint64_t());
        Expression* wide_low = make_cast(low, 16, false);
        Expression* wide_high = make_cast(high, 16, false);
        auto* shifted_high = arena_.create<Operation>(
            OperationType::shl,
            std::vector<Expression*>{
                wide_high,
                arena_.create<Constant>(64, 16),
            },
            16);
        shifted_high->set_ir_type(Integer::uint128_t());
        auto* combined = arena_.create<Operation>(
            OperationType::bit_or,
            std::vector<Expression*>{wide_low, shifted_high},
            16);
        combined->set_ir_type(type);
        return combined;
    }

    if (architecture_.integer_return_registers.empty()) return nullptr;
    auto* storage = arena_.create<Variable>(
        architecture_.integer_return_registers.front(), architecture_.pointer_size);
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
            auto* reg = arena_.create<Variable>(
                abi_floating_register_name(architecture_, 0, width), width);
            reg->set_ir_type(type);
            return reg;
        }
    }

    // Integer subregister writes are canonicalized onto x0 throughout this
    // frontend. Keeping call definitions on the same storage preserves SSA
    // connectivity with subsequent w0 reads, which are represented as casts.
    if (architecture_.integer_return_registers.empty()) return nullptr;
    auto* storage = arena_.create<Variable>(
        architecture_.integer_return_registers.front(), architecture_.pointer_size);
    storage->set_ir_type(type && type->size_bytes() >= 8 ? type : Integer::uint64_t());
    return storage;
}

Expression* PcodeLowerer::make_indirect_result_argument(
    std::size_t result_size,
    ida::Address instruction_ea) {
    if (result_size == 0 || !architecture_.stack_variable_resolver) {
        return nullptr;
    }
    const RegisterSemantic* result_semantic = nullptr;
    for (const auto& [_, semantic] : register_semantics_) {
        if (semantic.canonical_register_name == "x8"
            && semantic.stack_address.has_value()) {
            result_semantic = &semantic;
            break;
        }
    }
    if (!result_semantic) {
        return nullptr;
    }
    const StackAddress address = *result_semantic->stack_address;
    Variable* storage = architecture_.stack_variable_resolver(
        arena_, address.base, instruction_ea, address.offset, result_size);
    if (!storage) {
        return nullptr;
    }
    storage->size_bytes = result_size;
    storage->set_ir_type(std::make_shared<const ArrayType>(
        Integer::uint8_t(), result_size));
    task_.set_local_byte_array_extent(storage->name(), result_size);

    auto existing = std::find_if(
        indirect_result_regions_.begin(), indirect_result_regions_.end(),
        [&](const IndirectResultRegion& region) {
            return region.address.base == address.base
                && region.address.offset == address.offset;
        });
    if (existing == indirect_result_regions_.end()) {
        indirect_result_regions_.push_back(
            IndirectResultRegion{address, result_size, storage});
    } else {
        existing->size_bytes = std::max(existing->size_bytes, result_size);
        existing->storage = storage;
    }

    auto* address_of = arena_.create<Operation>(
        OperationType::address_of,
        std::vector<Expression*>{storage->copy(arena_)},
        architecture_.pointer_size);
    address_of->set_ir_type(std::make_shared<Pointer>(
        storage->ir_type(), architecture_.pointer_size * 8));
    auto* pointer_cast = arena_.create<Operation>(
        OperationType::cast,
        std::vector<Expression*>{address_of},
        architecture_.pointer_size);
    pointer_cast->set_ir_type(std::make_shared<Pointer>(
        Integer::uint8_t(), architecture_.pointer_size * 8));
    return pointer_cast;
}

Expression* PcodeLowerer::make_hfa_result_argument(
    std::size_t element_size,
    std::size_t element_count,
    ida::Address instruction_ea,
    Variable** storage_out) {
    if (storage_out) *storage_out = nullptr;
    if ((element_size != 4 && element_size != 8)
        || element_count < 2 || element_count > 4
        || element_size > std::numeric_limits<std::size_t>::max() / element_count) {
        return nullptr;
    }
    const TypePtr element_type = float_for_size(element_size);
    const std::size_t result_size = element_size * element_count;
    auto* storage = arena_.create<Variable>(
        "hfa_result_" + hex_u64(instruction_ea), result_size);
    storage->set_kind(VariableKind::Temporary);
    storage->set_aliased(true);
    storage->set_ir_type(std::make_shared<const ArrayType>(
        element_type, element_count));
    if (storage_out) *storage_out = storage;

    auto* address_of = arena_.create<Operation>(
        OperationType::address_of,
        std::vector<Expression*>{storage->copy(arena_)},
        architecture_.pointer_size);
    address_of->set_ir_type(std::make_shared<Pointer>(
        element_type, architecture_.pointer_size * 8));
    return address_of;
}

Expression* PcodeLowerer::access_indirect_result_region(
    const StackAddress& address,
    std::size_t width) {
    for (const IndirectResultRegion& region : indirect_result_regions_) {
        if (!region.storage || region.address.base != address.base
            || address.offset < region.address.offset) {
            continue;
        }
        const std::uint64_t delta = static_cast<std::uint64_t>(
            address.offset - region.address.offset);
        if (delta > region.size_bytes || width > region.size_bytes - delta) {
            continue;
        }
        auto* base = arena_.create<Operation>(
            OperationType::address_of,
            std::vector<Expression*>{region.storage->copy(arena_)},
            architecture_.pointer_size);
        base->set_ir_type(std::make_shared<Pointer>(
            Integer::uint8_t(), architecture_.pointer_size * 8));
        Expression* pointer = base;
        if (delta != 0) {
            pointer = arena_.create<Operation>(
                OperationType::add,
                std::vector<Expression*>{
                    base,
                    arena_.create<Constant>(delta, architecture_.pointer_size),
                },
                architecture_.pointer_size);
            pointer->set_ir_type(base->ir_type());
        }
        auto* dereference = arena_.create<Operation>(
            OperationType::deref,
            std::vector<Expression*>{pointer},
            width);
        dereference->set_ir_type(unsigned_integer_for_size(width));
        return dereference;
    }
    return nullptr;
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
        if (auto semantic = register_semantics_.find(unique_key_for(varnode));
            semantic != register_semantics_.end()) {
            if (semantic->second.stack_address.has_value()) {
                const StackAddress& address = *semantic->second.stack_address;
                auto* base = arena_.create<Variable>(
                    address.base == PcodeStackBase::StackPointer
                        ? architecture_.stack_pointer_register
                        : architecture_.frame_pointer_register,
                    architecture_.pointer_size);
                base->set_ir_type(std::make_shared<Pointer>(
                    Integer::uint8_t(), architecture_.pointer_size * 8));
                if (address.offset == 0) return base;
                auto* offset = arena_.create<Constant>(
                    static_cast<std::uint64_t>(address.offset),
                    architecture_.pointer_size);
                auto* adjusted = arena_.create<Operation>(
                    OperationType::add,
                    std::vector<Expression*>{base, offset},
                    architecture_.pointer_size);
                adjusted->set_ir_type(base->ir_type());
                return adjusted;
            }
            auto* remembered = arena_.create<Variable>(
                semantic->second.name,
                semantic->second.size_bytes > 0
                    ? semantic->second.size_bytes : varnode.size);
            remembered->set_ir_type(semantic->second.type
                ? semantic->second.type : unsigned_integer_for_size(varnode.size));
            if (semantic->second.bit_carried_type
                && semantic->second.bit_carried_width == varnode.size) {
                Expression* low_bits = remembered;
                if (remembered->size_bytes != varnode.size) {
                    low_bits = make_cast(remembered, varnode.size, false);
                }
                auto* bitcast = arena_.create<Operation>(
                    OperationType::bitcast,
                    std::vector<Expression*>{low_bits},
                    varnode.size);
                bitcast->set_ir_type(semantic->second.bit_carried_type);
                return bitcast;
            }
            return remembered;
        }
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
                        if (value->ir_type()
                            && value->ir_type()->type_kind() == TypeKind::Float) {
                            auto* bitcast = arena_.create<Operation>(
                                OperationType::bitcast,
                                std::vector<Expression*>{value},
                                slice.size);
                            bitcast->set_ir_type(unsigned_integer_for_size(slice.size));
                            value = bitcast;
                        }
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

                // Register views such as EDI/RDI or W0/X0 have distinct
                // Sleigh varnode keys but share one architectural storage.
                // Reuse the most recent canonical semantic before treating a
                // wider view as a fresh incoming parameter.
                const RegisterSemantic* canonical_semantic = nullptr;
                for (const auto& [_, candidate] : register_semantics_) {
                    if (candidate.canonical_register_name
                        == mapped->canonical_name) {
                        canonical_semantic = &candidate;
                    }
                }
                if (canonical_semantic) {
                    auto* remembered = arena_.create<Variable>(
                        canonical_semantic->name,
                        canonical_semantic->size_bytes > 0
                            ? canonical_semantic->size_bytes
                            : mapped->canonical_size);
                    remembered->set_ir_type(canonical_semantic->type
                        ? canonical_semantic->type
                        : unsigned_integer_for_size(remembered->size_bytes));
                    if (mapped->low_bits_view
                        && remembered->size_bytes > varnode.size) {
                        return make_cast(remembered, varnode.size, false);
                    }
                    return remembered;
                }

                auto* base = arena_.create<Variable>(mapped->canonical_name,
                    mapped->canonical_size > 0 ? mapped->canonical_size : varnode.size);
                const bool floating_register = abi_floating_register_index(
                    &architecture_, mapped->canonical_name).has_value();
                base->set_ir_type(floating_register
                    ? float_for_size(varnode.size)
                    : unsigned_integer_for_size(base->size_bytes));
                auto parameter = signature_context_.parameter_register_map.find(mapped->canonical_name);
                if (parameter == signature_context_.parameter_register_map.end()
                    && mapped->canonical_name.size() >= 2
                    && mapped->canonical_name.front() == 'x' && varnode.size <= 4) {
                    std::string narrow_name = mapped->canonical_name;
                    narrow_name.front() = 'w';
                    parameter = signature_context_.parameter_register_map.find(narrow_name);
                }
                if (parameter == signature_context_.parameter_register_map.end()) {
                    const auto abi_register = std::find(
                        architecture_.gp_argument_registers.begin(),
                        architecture_.gp_argument_registers.end(),
                        mapped->canonical_name);
                    if (abi_register != architecture_.gp_argument_registers.end()) {
                        const int parameter_index = static_cast<int>(
                            std::distance(
                                architecture_.gp_argument_registers.begin(),
                                abi_register));
                        signature_context_.parameter_register_map[
                            mapped->canonical_name] = parameter_index;
                        task_.set_parameter_register(
                            mapped->canonical_name,
                            DecompilerTask::ParameterInfo{
                                "a" + std::to_string(parameter_index + 1),
                                parameter_index,
                                unsigned_integer_for_size(
                                    mapped->canonical_size > 0
                                        ? mapped->canonical_size
                                        : varnode.size),
                            });
                        parameter = signature_context_.parameter_register_map.find(
                            mapped->canonical_name);
                    }
                }
                if (parameter == signature_context_.parameter_register_map.end()
                    && !suppress_current_parameter_inference_
                    && architecture_.arch_name.starts_with("x86_64")) {
                    const auto register_index = abi_floating_register_index(
                        &architecture_, mapped->canonical_name);
                    if (register_index.has_value() && *register_index < 8
                        && (varnode.size == 4 || varnode.size == 8)) {
                        const int parameter_index = static_cast<int>(*register_index);
                        const TypePtr parameter_type = float_for_size(varnode.size);
                        signature_context_.parameter_register_map[
                            mapped->canonical_name] = parameter_index;
                        task_.set_parameter_register(
                            mapped->canonical_name,
                            DecompilerTask::ParameterInfo{
                                "a" + std::to_string(parameter_index + 1),
                                parameter_index,
                                parameter_type,
                            });
                        parameter = signature_context_.parameter_register_map.find(
                            mapped->canonical_name);
                    }
                }
                if (parameter != signature_context_.parameter_register_map.end()) {
                    base->set_kind(VariableKind::Parameter);
                    base->set_parameter_index(parameter->second);
                    if (signature_context_.abi_indirect_result_size > 0
                        && mapped->canonical_name == "x8"
                        && parameter->second == 0) {
                        base->set_ir_type(std::make_shared<Pointer>(
                            Integer::uint8_t(), architecture_.pointer_size * 8));
                    }
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

    if (varnode.space_name == architecture_.data_space_name) {
        auto* address = arena_.create<Constant>(
            varnode.offset, architecture_.pointer_size);
        address->set_ir_type(std::make_shared<Pointer>(
            unsigned_integer_for_size(varnode.size),
            architecture_.pointer_size * 8));
        auto* loaded = arena_.create<Operation>(
            OperationType::deref,
            std::vector<Expression*>{address},
            varnode.size);
        loaded->set_ir_type(unsigned_integer_for_size(varnode.size));
        return loaded;
    }

    if (varnode.space_name == architecture_.code_space_name) {
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

PcodeLowerer::WriteTarget PcodeLowerer::lower_output_write(
    const RawPcodeVarnode& output,
    Expression* value,
    const LocalDefinitionMap& local_defs,
    std::optional<StackAddress> stack_address) {
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
                if (mapped->canonical_name
                    == architecture_.stack_pointer_register) {
                    // IDA's per-instruction SP delta is the authoritative
                    // frame coordinate used by stack_variable_resolver.
                    // Re-emitting architectural SP updates creates an
                    // undefined source-level entry value and duplicates the
                    // frame adjustment in portable C.
                    out.emit_assignment = false;
                    register_semantics_.erase(unique_key_for(output));
                    return out;
                }
                const auto fp_name = [&](const std::string& name) {
                    return abi_floating_register_index(
                        &architecture_, name).has_value();
                };
                const bool floating_result = value && value->ir_type()
                    && value->ir_type()->type_kind() == TypeKind::Float;
                const std::string register_name = floating_result
                    && !fp_name(mapped->canonical_name)
                    ? "fpreg_" + hex_u64(output.offset) + "_" + std::to_string(output.size)
                    : mapped->canonical_name;
                // A GP register can carry an HFA lane bit-for-bit during an
                // ABI stack transfer. Keep that per-view carrier at the lane
                // width; widening it to canonical xN storage would discard
                // the floating semantic before the subsequent store/load.
                const std::size_t dst_size = floating_result
                        && !fp_name(mapped->canonical_name)
                    ? output.size
                    : (mapped->canonical_size > 0
                        ? mapped->canonical_size : output.size);
                auto* reg = arena_.create<Variable>(register_name, dst_size);
                reg->set_ir_type(value && value->ir_type() && dst_size == output.size
                    ? value->ir_type() : unsigned_integer_for_size(dst_size));
                std::optional<StackAddress> semantic_stack_address =
                    stack_address.has_value()
                    ? stack_address : stack_offset_for_address(value, local_defs);
                if (mapped->canonical_name
                    == architecture_.frame_pointer_register) {
                    // Treat FP as an architectural coordinate, not as the
                    // current SP plus its prologue delta. This remains valid
                    // across CFG boundaries and when IDA has no SP delta for
                    // an individual instruction.
                    semantic_stack_address = StackAddress{
                        PcodeStackBase::FramePointer, 0};
                }
                RegisterSemantic output_semantic{
                    register_name,
                    mapped->canonical_name,
                    dst_size,
                    reg->ir_type(),
                    0,
                    nullptr,
                    semantic_stack_address};
                if (mapped->zero_extend_on_write
                    || output.size >= mapped->canonical_size) {
                    std::erase_if(register_semantics_, [&](const auto& entry) {
                        return entry.second.canonical_register_name
                            == mapped->canonical_name;
                    });
                }
                register_semantics_[unique_key_for(output)] = output_semantic;
                if (architecture_.register_read_slices) {
                    if (auto slices = architecture_.register_read_slices(
                            output.offset, output.size)) {
                        for (const PcodeRegisterSlice& slice : *slices) {
                            // RegisterSemantic currently represents a low
                            // subview of the written aggregate. Do not publish
                            // a false alias for non-zero-offset padding/high
                            // slices until the semantic records a byte offset.
                            if (slice.size == 0 || slice.offset != output.offset) {
                                continue;
                            }
                            RawPcodeVarnode component{
                                architecture_.register_space_name,
                                slice.offset,
                                slice.size,
                            };
                            RegisterSemantic component_semantic = output_semantic;
                            if (architecture_.register_mapper) {
                                if (auto component_view = architecture_.register_mapper(
                                        slice.offset, slice.size)) {
                                    const std::string& component_name =
                                        component_view->canonical_name;
                                    const bool floating_component =
                                        component_name.size() >= 2
                                        && (component_name.front() == 's'
                                            || component_name.front() == 'd')
                                        && std::all_of(
                                            component_name.begin() + 1,
                                            component_name.end(),
                                            [](unsigned char c) {
                                                return std::isdigit(c) != 0;
                                            });
                                    if (floating_component
                                        && (slice.size == 4 || slice.size == 8)) {
                                        component_semantic.bit_carried_width =
                                            slice.size;
                                        component_semantic.bit_carried_type =
                                            float_for_size(slice.size);
                                    }
                                }
                            }
                            register_semantics_[unique_key_for(component)] =
                                std::move(component_semantic);
                        }
                    }
                }
                out.destination = reg;
                if (mapped->zero_extend_on_write && dst_size > output.size) {
                    out.value = make_cast(value, dst_size, false);
                }
                return out;
            }
        }

        auto* reg = arena_.create<Variable>(fallback_register_name(output.offset, output.size), output.size);
        reg->set_ir_type(unsigned_integer_for_size(output.size));
        register_semantics_[unique_key_for(output)] = RegisterSemantic{
            reg->name(),
            reg->name(),
            output.size,
            reg->ir_type(),
            0,
            nullptr,
            stack_address.has_value()
                ? stack_address : stack_offset_for_address(value, local_defs)};
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
            if (reg->name() == architecture_.stack_pointer_register) {
                return finish(StackAddress{PcodeStackBase::StackPointer, 0});
            }
            if (reg->name() == architecture_.frame_pointer_register) {
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

        const auto constant_value = [&](Expression* candidate) -> std::optional<std::uint64_t> {
            if (auto* constant = dyn_cast<Constant>(candidate)) {
                return constant->value();
            }
            auto* variable = dyn_cast<Variable>(candidate);
            if (!variable) {
                return std::nullopt;
            }
            for (const auto& [_, definition] : local_defs) {
                auto* storage = dyn_cast<Variable>(definition.storage);
                if (!storage || storage->name() != variable->name()) {
                    continue;
                }
                if (auto* constant = dyn_cast<Constant>(definition.provenance)) {
                    return constant->value();
                }
                return std::nullopt;
            }
            return std::nullopt;
        };

        Expression* base_expr = op->operands()[0];
        auto delta_value = constant_value(op->operands()[1]);
        if (!delta_value.has_value() && op->type() == OperationType::add) {
            delta_value = constant_value(op->operands()[0]);
            base_expr = op->operands()[1];
        }
        if (!delta_value.has_value()) {
            return finish(std::nullopt);
        }

        auto base = self(self, base_expr);
        if (!base.has_value()) {
            return finish(std::nullopt);
        }

        const std::int64_t delta = static_cast<std::int64_t>(*delta_value);
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
                                                     std::size_t width,
                                                     bool bind_stack_parameter) {
    if (auto stack_offset = stack_offset_for_address(address_expr, local_defs)) {
        if (Expression* region_access = access_indirect_result_region(
                *stack_offset, width)) {
            return region_access;
        }
        if (architecture_.stack_variable_resolver) {
            if (auto* slot = architecture_.stack_variable_resolver(
                    arena_, stack_offset->base, instruction_ea, stack_offset->offset, width)) {
                if (!bind_stack_parameter) {
                    slot->set_kind(VariableKind::StackLocal);
                    slot->set_parameter_index(-1);
                }
                const auto* function = options_.function_type
                    ? type_dyn_cast<FunctionTypeDef>(options_.function_type.get())
                    : nullptr;
                // ABI binding describes an incoming stack read. A STORE may
                // reuse the same numeric offset for an outgoing argument or
                // local spill; classifying its destination as an incoming
                // parameter conflates two distinct source-level objects.
                if (bind_stack_parameter && function && slot->is_stack_variable()) {
                    const auto location = std::find_if(
                        function->abi_parameter_locations().begin(),
                        function->abi_parameter_locations().end(),
                        [&](const AbiParameterLocation& candidate) {
                            return candidate.storage == AbiParameterStorage::Stack
                                && candidate.stack_offset == slot->stack_offset()
                                && candidate.parameter_index
                                    < function->parameters().size();
                        });
                    if (location != function->abi_parameter_locations().end()) {
                        const TypePtr& type =
                            function->parameters()[location->parameter_index];
                        slot->set_kind(VariableKind::Parameter);
                        slot->set_parameter_index(
                            static_cast<int>(location->parameter_index));
                        slot->set_ir_type(type);
                        task_.set_parameter_register(slot->name(),
                            DecompilerTask::ParameterInfo{
                                "a" + std::to_string(location->parameter_index + 1),
                                static_cast<int>(location->parameter_index),
                                type,
                            });
                    }
                }
                const auto stored_type = std::find_if(
                    stack_stored_types_.rbegin(),
                    stack_stored_types_.rend(),
                    [&](const StackStoredType& stored) {
                        return stored.address.base == stack_offset->base
                            && stored.address.offset == stack_offset->offset
                            && stored.width == width && stored.type;
                    });
                if (stored_type != stack_stored_types_.rend()) {
                    slot->set_ir_type(stored_type->type);
                }
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
    Expression* expr = nullptr;
    if (target.space_name == architecture_.code_space_name
        || target.space_name == architecture_.data_space_name) {
        auto* address = arena_.create<Constant>(
            target.offset, architecture_.pointer_size);
        address->set_ir_type(std::make_shared<Pointer>(
            Integer::uint8_t(), architecture_.pointer_size * 8));
        expr = address;
    } else {
        expr = lower_varnode_read(target, local_defs);
    }
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
        suppress_current_parameter_inference_ = op.opcode == "INT_XOR"
            && op.inputs.size() == 2
            && op.inputs[0].space_name == op.inputs[1].space_name
            && op.inputs[0].offset == op.inputs[1].offset
            && op.inputs[0].size == op.inputs[1].size;
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

        std::optional<std::string> register_fallback_reason;
        const auto inspect_register = [&](const RawPcodeVarnode& varnode) {
            if (varnode.space_name != architecture_.register_space_name) {
                return;
            }
            if (!architecture_.register_mapper
                || !architecture_.register_mapper(varnode.offset, varnode.size).has_value()) {
                if (!register_fallback_reason.has_value()) {
                    register_fallback_reason = "stable offset/width register name used"
                        " (offset=0x" + hex_u64(varnode.offset)
                        + ", size=" + std::to_string(varnode.size) + ")";
                }
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
            } else if (register_fallback_reason.has_value()) {
                record_fallback(op, *register_fallback_reason);
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
            std::optional<StackAddress> output_stack_address =
                stack_offset_for_address(value, local_defs);
            if (op.output->space_name == architecture_.register_space_name
                && architecture_.register_mapper
                && architecture_.stack_variable_resolver) {
                auto mapped_output = architecture_.register_mapper(
                    op.output->offset, op.output->size);
                const bool preserves_stack_base = mapped_output.has_value()
                    && (mapped_output->canonical_name
                            == architecture_.stack_pointer_register
                        || mapped_output->canonical_name
                            == architecture_.frame_pointer_register);
                if (mapped_output.has_value() && !preserves_stack_base) {
                    if (output_stack_address.has_value()) {
                        Variable* slot = architecture_.stack_variable_resolver(
                            arena_,
                            output_stack_address->base,
                            op.instruction_ea,
                            output_stack_address->offset,
                            op.output->size);
                        if (!slot) {
                            const std::int64_t offset = output_stack_address->offset;
                            const std::uint64_t magnitude = offset < 0
                                ? static_cast<std::uint64_t>(-(offset + 1)) + 1
                                : static_cast<std::uint64_t>(offset);
                            const std::string name = offset >= 0
                                ? "arg_" + std::to_string(offset)
                                : "local_m" + std::to_string(magnitude);
                            slot = arena_.create<Variable>(name, op.output->size);
                            slot->set_kind(VariableKind::StackLocal);
                            slot->set_stack_offset(offset);
                            slot->set_ir_type(unsigned_integer_for_size(op.output->size));
                        }
                        if (slot) {
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

            WriteTarget out = lower_output_write(
                *op.output, value, local_defs, output_stack_address);
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
            Expression* copied = lower_varnode_read(op.inputs[0], local_defs);
            if (isa<Constant>(copied)
                && copied->ir_type()
                && copied->ir_type()->type_kind() != TypeKind::Float
                && op.output->space_name == architecture_.register_space_name
                && architecture_.register_mapper) {
                auto output_view = architecture_.register_mapper(
                    op.output->offset, op.output->size);
                const bool scalar_float_register = output_view.has_value()
                    && output_view->canonical_name.size() >= 2
                    && (output_view->canonical_name.front() == 's'
                        || output_view->canonical_name.front() == 'd')
                    && std::all_of(
                        output_view->canonical_name.begin() + 1,
                        output_view->canonical_name.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })
                    && (op.output->size == 4 || op.output->size == 8);
                if (scalar_float_register) {
                    auto* bitcast = arena_.create<Operation>(
                        OperationType::bitcast,
                        std::vector<Expression*>{copied},
                        op.output->size);
                    bitcast->set_ir_type(float_for_size(op.output->size));
                    copied = bitcast;
                }
            }
            assign_output(copied);
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
            TypePtr bit_carried_type;
            if (op.opcode == "INT_ZEXT" && input->ir_type()
                && input->ir_type()->type_kind() == TypeKind::Float
                && input->size_bytes == op.inputs[0].size) {
                bit_carried_type = input->ir_type();
                auto* bitcast = arena_.create<Operation>(
                    OperationType::bitcast,
                    std::vector<Expression*>{input},
                    op.inputs[0].size);
                bitcast->set_ir_type(unsigned_integer_for_size(op.inputs[0].size));
                input = bitcast;
            }
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
            if (bit_carried_type
                && op.output->space_name == architecture_.register_space_name) {
                auto full = register_semantics_.find(unique_key_for(*op.output));
                if (full != register_semantics_.end()) {
                    full->second.bit_carried_width = op.inputs[0].size;
                    full->second.bit_carried_type = bit_carried_type;
                    RawPcodeVarnode narrow = *op.output;
                    narrow.size = op.inputs[0].size;
                    register_semantics_[unique_key_for(narrow)] = full->second;
                }
            }
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
            complete_op();
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
            auto* count = arena_.create<Operation>(
                op.opcode == "POPCOUNT" ? OperationType::popcount
                                         : OperationType::lzcount,
                std::vector<Expression*>{
                    lower_varnode_read(op.inputs[0], local_defs),
                    arena_.create<Constant>(op.inputs[0].size * 8, architecture_.pointer_size),
                },
                op.output->size);
            count->set_ir_type(unsigned_integer_for_size(op.output->size));
            assign_output(count);
            complete_op();
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
                address_expr, local_defs, op.instruction_ea, op.inputs[2].size,
                false);
            emit_assignment(destination, value);
            if (architecture_.arch_name.starts_with("arm64")) {
                auto stack_address = stack_offset_for_address(address_expr, local_defs);
                if (stack_address.has_value()) {
                    std::erase_if(stack_stored_types_,
                        [&](const StackStoredType& stored) {
                            return stored.address.base == stack_address->base
                                && stored.address.offset == stack_address->offset
                                && stored.width == op.inputs[2].size;
                        });
                    if (value->ir_type()
                        && value->ir_type()->type_kind() == TypeKind::Float) {
                        stack_stored_types_.push_back(StackStoredType{
                            *stack_address,
                            op.inputs[2].size,
                            value->ir_type(),
                        });
                    }
                }
                if (stack_address.has_value()
                    && stack_address->base == PcodeStackBase::StackPointer
                    && stack_address->offset >= 0) {
                    if (stack_address->offset == 0) {
                        // Offset zero usually starts a new outgoing argument
                        // epoch, but Clang may emit higher slots first (for
                        // example STR d0,[sp,#8] followed by STR x19,[sp]).
                        // Retain stores from the immediately preceding machine
                        // instruction while discarding older prologue/frame
                        // writes that happen to occupy positive SP offsets.
                        const ida::Address stride = instruction.decoded_length > 0
                            ? instruction.decoded_length : 1;
                        std::erase_if(stack_argument_write_evidence_,
                            [&](const StackArgumentEvidence& evidence) {
                                if (evidence.offset <= 0) return true;
                                if (evidence.instruction_ea > op.instruction_ea) return true;
                                return op.instruction_ea - evidence.instruction_ea > stride;
                            });
                    }
                    const auto same_offset = [&](const StackArgumentEvidence& evidence) {
                        return evidence.offset == stack_address->offset;
                    };
                    std::erase_if(stack_argument_write_evidence_, same_offset);
                    auto* stored_variable = dyn_cast<Variable>(value);
                    const auto is_fp_register_name = [](const std::string& name) {
                        if (name.size() < 2
                            || (name.front() != 's' && name.front() != 'd'
                                && name.front() != 'q' && name.front() != 'v')) {
                            return false;
                        }
                        return std::all_of(name.begin() + 1, name.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; });
                    };
                    const bool floating_value = (value->ir_type()
                            && value->ir_type()->type_kind() == TypeKind::Float)
                        || (stored_variable
                            && is_fp_register_name(stored_variable->name()));
                    if (floating_value && stored_variable) {
                        stored_variable->set_ir_type(float_for_size(op.inputs[2].size));
                    }
                    if (floating_value) {
                        if (auto* stack_destination = dyn_cast<Variable>(destination)) {
                            stack_destination->set_ir_type(
                                float_for_size(op.inputs[2].size));
                        }
                    }
                    stack_argument_write_evidence_.push_back(StackArgumentEvidence{
                        stack_address->offset,
                        op.inputs[2].size,
                        // Integer stack slots participate in Aletheia's
                        // established stack/SSA normalization. Floating
                        // register stores must retain the value expression:
                        // re-reading the slot can select an older prologue
                        // definition instead of the just-stored d-register.
                        (floating_value ? value : destination)->copy(arena_),
                        dyn_cast<Variable>(destination)
                            ? dyn_cast<Variable>(destination->copy(arena_)) : nullptr,
                        op.instruction_ea,
                    });
                }
            } else if (architecture_.arch_name.starts_with("x86_64")
                       && std::none_of(
                           instruction.ops.begin(), instruction.ops.end(),
                           [](const RawPcodeOp& instruction_op) {
                               return instruction_op.opcode == "CALL"
                                   || instruction_op.opcode == "CALLIND";
                           })) {
                auto* stack_destination = dyn_cast<Variable>(destination);
                if (stack_destination && stack_destination->is_stack_variable()) {
                    ida::Address latest_ea = 0;
                    for (const StackArgumentEvidence& evidence :
                         stack_argument_write_evidence_) {
                        latest_ea = std::max(latest_ea, evidence.instruction_ea);
                    }
                    // Consecutive PUSH instructions form one outgoing stack
                    // argument epoch. A gap larger than x86's maximum machine
                    // instruction length separates prologue spills from the
                    // later argument sequence.
                    if (latest_ea != 0 && op.instruction_ea > latest_ea
                        && op.instruction_ea - latest_ea > 15) {
                        stack_argument_write_evidence_.clear();
                    }
                    for (StackArgumentEvidence& evidence :
                         stack_argument_write_evidence_) {
                        if (evidence.offset
                            > std::numeric_limits<std::int64_t>::max()
                                - static_cast<std::int64_t>(op.inputs[2].size)) {
                            return unsupported(
                                "x86 outgoing stack-argument offset overflow");
                        }
                        evidence.offset += static_cast<std::int64_t>(
                            op.inputs[2].size);
                    }
                    stack_argument_write_evidence_.push_back(StackArgumentEvidence{
                        0,
                        op.inputs[2].size,
                        value->copy(arena_),
                        dyn_cast<Variable>(destination->copy(arena_)),
                        op.instruction_ea,
                    });
                }
            }
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
            bool variadic = false;
            std::size_t abi_indirect_result_size = 0;
            std::size_t abi_hfa_result_element_size = 0;
            std::size_t abi_hfa_result_count = 0;
            std::vector<AbiParameterLocation> abi_parameter_locations;
            if (function_info.has_value()) {
                parameter_types = function_info->parameter_types;
                return_type = function_info->return_type;
                prototype_known = function_info->prototype_known;
                variadic = function_info->variadic;
                abi_indirect_result_size = function_info->abi_indirect_result_size;
                abi_hfa_result_element_size =
                    function_info->abi_hfa_result_element_size;
                abi_hfa_result_count = function_info->abi_hfa_result_count;
                abi_parameter_locations = function_info->abi_parameter_locations;
            }
            if (target_name == "printf" || target_name == "fprintf") {
                // Symbol identity is authoritative for these standardized
                // variadic interfaces even when an import thunk's recovered
                // type omits the ellipsis metadata.
                variadic = true;
            }
            if (recursive_self_call) {
                if (const auto* self_type = as_function_type(options_.function_type)) {
                    parameter_types = self_type->parameters();
                    return_type = self_type->return_type();
                    prototype_known = true;
                    abi_indirect_result_size = self_type->abi_indirect_result_size();
                    abi_hfa_result_element_size =
                        self_type->abi_hfa_result_element_size();
                    abi_hfa_result_count = self_type->abi_hfa_result_count();
                    abi_parameter_locations.assign(
                        self_type->abi_parameter_locations().begin(),
                        self_type->abi_parameter_locations().end());
                } else if (parameter_types.empty()) {
                    const std::size_t count = signature_context_.parameter_count_hint > 0
                        ? signature_context_.parameter_count_hint : 1;
                    parameter_types.assign(count, std::make_shared<UnknownType>());
                }
            }

            std::size_t inferred_argument_count = 0;
            const std::size_t fixed_argument_count = parameter_types.size();
            const std::size_t fixed_gp_argument_count = static_cast<std::size_t>(
                std::count_if(
                    parameter_types.begin(), parameter_types.end(),
                    [](const TypePtr& type) { return as_float_type(type) == nullptr; }));
            if (!prototype_known
                || (variadic && !architecture_.variadic_arguments_on_stack)) {
                inferred_argument_count = inferred_gp_argument_count();
                while (parameter_types.size() < inferred_argument_count) {
                    parameter_types.push_back(std::make_shared<UnknownType>());
                }
            }
            std::size_t inferred_fp_variadic_count = 0;
            if (variadic && architecture_.arch_name.starts_with("x86_64")
                && (target_name == "printf" || target_name == "fprintf")) {
                const std::size_t fp_count = inferred_fp_argument_count();
                if (fp_count > 0) {
                    if (inferred_argument_count > fixed_gp_argument_count) {
                        return unsupported(
                            "mixed variadic GP/FP argument order requires format recovery");
                    }
                    parameter_types.insert(
                        parameter_types.end(), fp_count, Float::float64());
                    inferred_fp_variadic_count = fp_count;
                }
            }

            std::vector<Expression*> args;
            args.reserve(parameter_types.size());
            std::unordered_set<std::size_t> located_parameter_indices;
            std::set<std::int64_t> located_stack_offsets;
            for (const AbiParameterLocation& location : abi_parameter_locations) {
                if (location.parameter_index >= parameter_types.size()
                    || location.storage == AbiParameterStorage::Default
                    || !located_parameter_indices.insert(
                            location.parameter_index).second
                    || ((location.storage
                            == AbiParameterStorage::GeneralRegister
                         || location.storage
                            == AbiParameterStorage::FloatingRegister)
                        && location.register_index >= 8)
                    || (location.storage == AbiParameterStorage::Stack
                        && (location.stack_offset < 0
                            || !located_stack_offsets.insert(
                                    location.stack_offset).second))) {
                    return unsupported("invalid explicit ABI parameter metadata");
                }
            }
            std::size_t gp_index = 0;
            std::size_t fp_index = 0;
            bool has_stack_arguments = false;
            std::vector<StackArgumentEvidence> ordered_stack_arguments =
                stack_argument_write_evidence_;
            if (architecture_.arch_name.starts_with("x86_64")
                && !ordered_stack_arguments.empty()) {
                // SysV PUSH sequences are emitted in right-to-left argument
                // order. Sleigh intentionally hides architectural SP writes,
                // so instruction order is the stable coordinate: the last
                // PUSH occupies the lowest-addressed ABI stack slot.
                std::stable_sort(
                    ordered_stack_arguments.begin(),
                    ordered_stack_arguments.end(),
                    [](const StackArgumentEvidence& lhs,
                       const StackArgumentEvidence& rhs) {
                        return lhs.instruction_ea > rhs.instruction_ea;
                    });
                std::int64_t normalized_offset = 0;
                for (StackArgumentEvidence& evidence : ordered_stack_arguments) {
                    evidence.offset = normalized_offset;
                    const std::size_t slot_width = std::max(
                        architecture_.pointer_size, evidence.width);
                    if (slot_width > static_cast<std::size_t>(
                            std::numeric_limits<std::int64_t>::max()
                            - normalized_offset)) {
                        return unsupported(
                            "x86 outgoing stack-argument offset overflow");
                    }
                    normalized_offset += static_cast<std::int64_t>(slot_width);
                }
            } else {
                std::sort(
                    ordered_stack_arguments.begin(),
                    ordered_stack_arguments.end(),
                    [](const StackArgumentEvidence& lhs,
                       const StackArgumentEvidence& rhs) {
                        return lhs.offset < rhs.offset;
                    });
            }
            std::int64_t expected_stack_offset = 0;
            std::optional<std::int64_t> explicit_stack_base;
            for (const AbiParameterLocation& location : abi_parameter_locations) {
                if (location.storage == AbiParameterStorage::Stack
                    && (!explicit_stack_base.has_value()
                        || location.stack_offset < *explicit_stack_base)) {
                    explicit_stack_base = location.stack_offset;
                }
            }
            const auto consume_stack_argument = [&]() -> Expression* {
                auto found = std::find_if(
                    ordered_stack_arguments.begin(),
                    ordered_stack_arguments.end(),
                    [&](const StackArgumentEvidence& evidence) {
                        return evidence.offset == expected_stack_offset;
                    });
                if (found == ordered_stack_arguments.end()
                    || found->width == 0 || !found->storage) {
                    return nullptr;
                }
                Expression* argument = found->storage->copy(arena_);
                const std::size_t slot_width = std::max(
                    architecture_.pointer_size, found->width);
                if (slot_width > static_cast<std::size_t>(
                        std::numeric_limits<std::int64_t>::max()
                        - expected_stack_offset)) {
                    return nullptr;
                }
                expected_stack_offset += static_cast<std::int64_t>(slot_width);
                return argument;
            };
            const auto consume_explicit_stack_argument = [&, this](
                std::int64_t offset,
                const TypePtr& type) -> Expression* {
                auto found = std::find_if(
                    ordered_stack_arguments.begin(),
                    ordered_stack_arguments.end(),
                    [&](const StackArgumentEvidence& evidence) {
                        return evidence.offset == offset
                            && evidence.width == (type ? type->size_bytes() : 0);
                    });
                if (found == ordered_stack_arguments.end() || !found->storage) {
                    return nullptr;
                }
                Expression* argument = found->storage->copy(arena_);
                if (type) {
                    if (type->type_kind() == TypeKind::Float
                        && (!argument->ir_type()
                            || argument->ir_type()->type_kind() != TypeKind::Float)) {
                        auto* bitcast = arena_.create<Operation>(
                            OperationType::bitcast,
                            std::vector<Expression*>{argument},
                            type->size_bytes());
                        bitcast->set_ir_type(type);
                        argument = bitcast;
                    } else {
                        argument->set_ir_type(type);
                    }
                }
                return argument;
            };
            std::size_t parameter_start = 0;
            Variable* hfa_result_storage = nullptr;
            if (abi_indirect_result_size > 0) {
                Expression* result_argument = make_indirect_result_argument(
                    abi_indirect_result_size, op.instruction_ea);
                if (!result_argument || parameter_types.empty()) {
                    return unsupported(
                        "indirect result call has no reaching x8 stack destination");
                }
                args.push_back(result_argument);
                parameter_start = 1;
            } else if (abi_hfa_result_count > 0) {
                Expression* result_argument = make_hfa_result_argument(
                    abi_hfa_result_element_size,
                    abi_hfa_result_count,
                    op.instruction_ea,
                    &hfa_result_storage);
                if (!result_argument || !hfa_result_storage
                    || parameter_types.empty()) {
                    return unsupported("invalid homogeneous floating result metadata");
                }
                args.push_back(result_argument);
                parameter_start = 1;
            }
            for (std::size_t parameter_index = parameter_start;
                 parameter_index < parameter_types.size();
                 ++parameter_index) {
                const TypePtr& parameter_type = parameter_types[parameter_index];
                const auto explicit_location = std::find_if(
                    abi_parameter_locations.begin(),
                    abi_parameter_locations.end(),
                    [&](const AbiParameterLocation& location) {
                        return location.parameter_index == parameter_index;
                    });
                if (explicit_location != abi_parameter_locations.end()
                    && explicit_location->storage != AbiParameterStorage::Default) {
                    Expression* argument = nullptr;
                    if (explicit_location->storage
                        == AbiParameterStorage::Stack) {
                        argument = consume_explicit_stack_argument(
                            explicit_stack_base.has_value()
                                ? explicit_location->stack_offset
                                    - *explicit_stack_base
                                : explicit_location->stack_offset,
                            parameter_type);
                    } else if (explicit_location->storage
                               == AbiParameterStorage::FloatingRegister) {
                        argument = make_abi_argument(
                            explicit_location->register_index, parameter_type);
                    } else if (explicit_location->storage
                               == AbiParameterStorage::GeneralRegister) {
                        argument = make_abi_argument(
                            explicit_location->register_index, parameter_type);
                        if (parameter_type
                            && argument
                            && parameter_type->type_kind() == TypeKind::Float
                            && (!argument->ir_type()
                                || argument->ir_type()->type_kind()
                                    != TypeKind::Float)) {
                            auto* bitcast = arena_.create<Operation>(
                                OperationType::bitcast,
                                std::vector<Expression*>{argument},
                                parameter_type->size_bytes());
                            bitcast->set_ir_type(parameter_type);
                            argument = bitcast;
                        } else if (parameter_type) {
                            argument->set_ir_type(parameter_type);
                        }
                    }
                    if (!argument) {
                        std::string detail =
                            "explicit ABI parameter location has no reaching value"
                            " (parameter=" + std::to_string(parameter_index)
                            + ", storage="
                            + std::to_string(static_cast<int>(
                                explicit_location->storage))
                            + ", stack_offset="
                            + std::to_string(explicit_location->stack_offset)
                            + ", available_stack_offsets=";
                        if (ordered_stack_arguments.empty()) {
                            detail += "none";
                        } else {
                            for (std::size_t evidence_index = 0;
                                 evidence_index < ordered_stack_arguments.size();
                                 ++evidence_index) {
                                if (evidence_index != 0) detail += ",";
                                detail += std::to_string(
                                    ordered_stack_arguments[evidence_index].offset);
                            }
                        }
                        detail += ")";
                        return unsupported(detail);
                    }
                    args.push_back(argument);
                    continue;
                }
                const bool floating = as_float_type(parameter_type) != nullptr;
                std::size_t& register_index = floating ? fp_index : gp_index;
                Expression* argument = make_abi_argument(register_index, parameter_type);
                if (argument == nullptr) {
                    argument = consume_stack_argument();
                    has_stack_arguments = has_stack_arguments || argument == nullptr;
                } else {
                    ++register_index;
                }
                if (argument != nullptr) {
                    args.push_back(argument);
                }
            }

            std::size_t inferred_stack_variadic_count = 0;
            if (variadic && architecture_.variadic_arguments_on_stack) {
                while (Expression* argument = consume_stack_argument()) {
                    args.push_back(argument);
                    parameter_types.push_back(std::make_shared<UnknownType>());
                    ++inferred_stack_variadic_count;
                }
            }

            const bool returns_void = return_type && return_type->to_string() == "void";
            const std::size_t return_size = return_type && return_type->size_bytes() > 0
                ? return_type->size_bytes() : architecture_.pointer_size;
            const bool two_register_integer_return =
                is_two_register_integer_type(return_type);
            if (!returns_void && return_type && as_float_type(return_type) == nullptr
                && return_size > architecture_.pointer_size
                && !two_register_integer_return) {
                return unsupported("multi-register or indirect call return is not modeled");
            }
            auto* call = arena_.create<Call>(target, std::move(args), return_size);
            if (prototype_known || !parameter_types.empty() || return_type) {
                call->set_ir_type(std::make_shared<FunctionTypeDef>(
                    return_type ? return_type : std::make_shared<UnknownType>(),
                    parameter_types,
                    variadic,
                    abi_indirect_result_size,
                    abi_hfa_result_element_size,
                    abi_hfa_result_count,
                    abi_parameter_locations));
            }
            Variable* scalar_return_storage = nullptr;
            if (returns_void) {
                emit_assignment(nullptr, call);
            } else if (two_register_integer_return) {
                if (architecture_.integer_return_registers.size() < 2) {
                    return unsupported(
                        "two-register call return has no ABI register pair");
                }
                auto* aggregate = arena_.create<Variable>(
                    "call_result_" + hex_u64(op.instruction_ea)
                        + "_" + std::to_string(op.op_ordinal),
                    16);
                aggregate->set_kind(VariableKind::Temporary);
                aggregate->set_ir_type(return_type);
                emit_assignment(aggregate, call);

                auto* low = arena_.create<Variable>(
                    architecture_.integer_return_registers[0],
                    architecture_.pointer_size);
                low->set_ir_type(Integer::uint64_t());
                emit_assignment(low, make_cast(aggregate->copy(arena_), 8, false));

                auto* shifted = arena_.create<Operation>(
                    OperationType::shr_us,
                    std::vector<Expression*>{
                        aggregate->copy(arena_),
                        arena_.create<Constant>(64, 16),
                    },
                    16);
                shifted->set_ir_type(Integer::uint128_t());
                auto* high = arena_.create<Variable>(
                    architecture_.integer_return_registers[1],
                    architecture_.pointer_size);
                high->set_ir_type(Integer::uint64_t());
                emit_assignment(high, make_cast(shifted, 8, false));
            } else {
                scalar_return_storage = make_abi_return_storage(return_type);
                emit_assignment(scalar_return_storage, call);
            }
            if (hfa_result_storage) {
                const TypePtr element_type = float_for_size(
                    abi_hfa_result_element_size);
                for (std::size_t index = 0;
                     index < abi_hfa_result_count;
                     ++index) {
                    auto* base = arena_.create<Operation>(
                        OperationType::address_of,
                        std::vector<Expression*>{hfa_result_storage->copy(arena_)},
                        architecture_.pointer_size);
                    base->set_ir_type(std::make_shared<Pointer>(
                        element_type, architecture_.pointer_size * 8));
                    Expression* address = base;
                    if (index != 0) {
                        address = arena_.create<Operation>(
                            OperationType::add,
                            std::vector<Expression*>{
                                base,
                                arena_.create<Constant>(
                                    index * abi_hfa_result_element_size,
                                    architecture_.pointer_size),
                            },
                            architecture_.pointer_size);
                        address->set_ir_type(base->ir_type());
                    }
                    auto* loaded = arena_.create<Operation>(
                        OperationType::deref,
                        std::vector<Expression*>{address},
                        abi_hfa_result_element_size);
                    loaded->set_ir_type(element_type);
                    auto* destination = arena_.create<Variable>(
                        std::string(1, abi_hfa_result_element_size == 4 ? 's'
                            : (abi_hfa_result_element_size == 8 ? 'd' : 'q'))
                            + std::to_string(index),
                        abi_hfa_result_element_size);
                    destination->set_ir_type(element_type);
                    emit_assignment(destination, loaded);
                }
            }
            invalidate_call_clobbered_register_semantics();
            if (scalar_return_storage) {
                const std::string semantic_key = "abi-return:"
                    + scalar_return_storage->name();
                register_semantics_[semantic_key] = RegisterSemantic{
                    scalar_return_storage->name(),
                    scalar_return_storage->name(),
                    scalar_return_storage->size_bytes,
                    scalar_return_storage->ir_type(),
                    0,
                    nullptr,
                    std::nullopt,
                };
            }
            reset_call_argument_evidence(!returns_void);
            if (scalar_return_storage) {
                if (const auto floating_index = abi_floating_register_index(
                        &architecture_, scalar_return_storage->name());
                    floating_index.has_value()
                    && *floating_index < fp_argument_write_evidence_.size()) {
                    fp_argument_write_evidence_[*floating_index] = true;
                }
            }
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
            } else if (variadic && inferred_stack_variadic_count > 0) {
                complete_op("inferred " + std::to_string(inferred_stack_variadic_count)
                    + " variadic stack argument"
                    + (inferred_stack_variadic_count == 1 ? "" : "s")
                    + " from contiguous current-block SP-relative writes");
            } else if (variadic && inferred_argument_count > fixed_argument_count) {
                complete_op("inferred "
                    + std::to_string(inferred_argument_count - fixed_argument_count)
                    + " variadic general-purpose argument register"
                    + (inferred_argument_count - fixed_argument_count == 1 ? "" : "s")
                    + " from current basic-block writes");
            } else if (variadic && inferred_fp_variadic_count > 0) {
                complete_op("inferred "
                    + std::to_string(inferred_fp_variadic_count)
                    + " variadic floating argument register"
                    + (inferred_fp_variadic_count == 1 ? "" : "s")
                    + " from current basic-block writes");
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
            const bool abi_indirect_result =
                signature_context_.abi_indirect_result_size > 0;
            const bool abi_hfa_result =
                signature_context_.abi_hfa_result_count > 0;
            if (abi_hfa_result) {
                const std::size_t element_size =
                    signature_context_.abi_hfa_result_element_size;
                const std::size_t element_count =
                    signature_context_.abi_hfa_result_count;
                if ((element_size != 4 && element_size != 8)
                    || element_count < 2 || element_count > 4) {
                    return unsupported("invalid homogeneous floating function result metadata");
                }
                const TypePtr element_type = float_for_size(element_size);
                auto* result_pointer = arena_.create<Variable>(
                    "result", architecture_.pointer_size);
                result_pointer->set_kind(VariableKind::Parameter);
                result_pointer->set_parameter_index(0);
                result_pointer->set_ir_type(std::make_shared<Pointer>(
                    element_type, architecture_.pointer_size * 8));
                for (std::size_t index = 0; index < element_count; ++index) {
                    Expression* address = result_pointer->copy(arena_);
                    if (index != 0) {
                        address = arena_.create<Operation>(
                            OperationType::add,
                            std::vector<Expression*>{
                                result_pointer->copy(arena_),
                                arena_.create<Constant>(
                                    index * element_size,
                                    architecture_.pointer_size),
                            },
                            architecture_.pointer_size);
                        address->set_ir_type(result_pointer->ir_type());
                    }
                    auto* destination = arena_.create<Operation>(
                        OperationType::deref,
                        std::vector<Expression*>{address},
                        element_size);
                    destination->set_ir_type(element_type);
                    auto* source = arena_.create<Variable>(
                        std::string(1, element_size == 4 ? 's'
                            : (element_size == 8 ? 'd' : 'q'))
                            + std::to_string(index),
                        element_size);
                    source->set_ir_type(element_type);
                    emit_assignment(destination, source);
                }
            }
            if (!returns_void && !abi_indirect_result && !abi_hfa_result) {
                const TypePtr return_type = function_type
                    ? function_type->return_type() : nullptr;
                if (return_type && as_float_type(return_type) == nullptr
                    && return_type->size_bytes() > architecture_.pointer_size
                    && !is_two_register_integer_type(return_type)) {
                    return unsupported("multi-register or indirect function return is not modeled");
                }
                Expression* return_value = nullptr;
                if (!return_type) {
                    for (const auto& [_, semantic] : register_semantics_) {
                        const auto register_index = abi_floating_register_index(
                            &architecture_, semantic.name);
                        if (semantic.type
                            && semantic.type->type_kind() == TypeKind::Float
                            && register_index.has_value() && *register_index == 0) {
                            auto* floating = arena_.create<Variable>(
                                semantic.name, semantic.size_bytes);
                            floating->set_ir_type(semantic.type);
                            return_value = floating;
                            break;
                        }
                    }
                }
                values.push_back(return_value
                    ? return_value : make_abi_return_value(return_type));
            }
            auto* ret = arena_.create<Return>(std::move(values));
            ret->set_address(op.instruction_ea);
            emitted.push_back(ret);
            terminated = true;
            if (function_type == nullptr) {
                const bool inferred_float = !ret->values().empty()
                    && ret->values()[0]->ir_type()
                    && ret->values()[0]->ir_type()->type_kind() == TypeKind::Float;
                complete_op(inferred_float
                    ? "function return type unavailable; inferred floating ABI return register"
                    : "function return type unavailable; assumed scalar x0 return");
            } else {
                complete_op();
            }
            continue;
        }

        return unsupported("opcode not in the supported raw P-Code set");
    }

    return emitted;
}

PcodeSignatureRefinement refine_pcode_function_signature(
    DecompilerTask& task,
    ControlFlowGraph& cfg,
    std::size_t abi_indirect_result_size,
    std::size_t abi_hfa_result_element_size,
    std::size_t abi_hfa_result_count,
    const PcodeArchitectureContext* architecture) {
    PcodeSignatureRefinement result;
    const std::vector<std::string> default_gp_registers = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    const std::vector<std::string> default_return_registers = {"x0", "x1"};
    const auto& gp_registers = architecture
        ? architecture->gp_argument_registers : default_gp_registers;
    const auto& return_registers = architecture
        ? architecture->integer_return_registers : default_return_registers;
    const bool has_hidden_result = abi_indirect_result_size > 0
        || abi_hfa_result_count > 0;
    const int hidden_parameter_count = has_hidden_result ? 1 : 0;
    constexpr std::size_t floating_register_count = 8;
    const int register_parameter_limit = static_cast<int>(
        std::max(gp_registers.size(), floating_register_count))
        + hidden_parameter_count;
    const auto* existing_function = task.function_type()
        ? type_dyn_cast<FunctionTypeDef>(task.function_type().get()) : nullptr;
    if (task.function_type() && !existing_function) {
        return result;
    }

    struct SelfCallSite {
        Assignment* assignment = nullptr;
        Call* call = nullptr;
    };
    std::vector<SelfCallSite> self_calls;
    const auto direct_target = [](Call* call) -> std::optional<ida::Address> {
        if (!call || !call->target()) {
            return std::nullopt;
        }
        if (auto* constant = dyn_cast<Constant>(call->target())) {
            return static_cast<ida::Address>(constant->value());
        }
        if (auto* global = dyn_cast<GlobalVariable>(call->target())) {
            if (auto* constant = dyn_cast<Constant>(global->initial_value())) {
                return static_cast<ida::Address>(constant->value());
            }
        }
        return std::nullopt;
    };

    for (BasicBlock* block : cfg.blocks()) {
        if (!block) continue;
        for (Instruction* instruction : block->instructions()) {
            auto* assignment = dyn_cast<Assignment>(instruction);
            auto* call = assignment ? dyn_cast<Call>(assignment->value()) : nullptr;
            const auto target = direct_target(call);
            if (target.has_value() && *target == task.function_address()) {
                self_calls.push_back(SelfCallSite{assignment, call});
            }
        }
    }
    std::unordered_map<int, TypePtr> parameter_types_by_index;
    int maximum_parameter_index = -1;
    bool conflicting_parameter_evidence = false;
    using StackSlotKey = std::pair<std::int64_t, std::size_t>;
    struct IncomingStackParameter {
        std::vector<Variable*> variables;
        int index = -1;
        TypePtr type;
        std::int64_t abi_stack_offset = 0;
    };
    std::set<StackSlotKey> defined_stack_slots;
    std::map<StackSlotKey, std::vector<Variable*>> required_stack_slots;
    std::vector<IncomingStackParameter> inferred_stack_parameters;
    std::map<int, AbiParameterLocation> parameter_locations_by_index;
    for (BasicBlock* block : cfg.blocks()) {
        if (!block) continue;
        for (Instruction* instruction : block->instructions()) {
            for (Variable* variable : instruction->definitions()) {
                if (variable && variable->is_stack_variable()) {
                    defined_stack_slots.emplace(
                        variable->stack_offset(), variable->size_bytes);
                }
            }
            std::unordered_set<Variable*> requirements;
            instruction->collect_requirements(requirements);
            for (Variable* variable : requirements) {
                if (variable && variable->is_stack_variable()
                    && variable->stack_offset() >= 0
                    && variable->size_bytes > 0) {
                    required_stack_slots[StackSlotKey{
                        variable->stack_offset(), variable->size_bytes}]
                        .push_back(variable);
                }
                if (!variable || !variable->is_parameter()
                    || variable->parameter_index() < 0
                    || variable->parameter_index() >= register_parameter_limit) {
                    continue;
                }
                const int index = variable->parameter_index();
                maximum_parameter_index = std::max(maximum_parameter_index, index);
                std::optional<AbiParameterLocation> location;
                if (const auto register_index = abi_floating_register_index(
                        architecture, variable->name())) {
                    location = AbiParameterLocation{
                        static_cast<std::size_t>(index),
                        AbiParameterStorage::FloatingRegister,
                        *register_index,
                        0,
                    };
                }
                if (!location.has_value()) {
                    auto gp_register = std::find(
                        gp_registers.begin(), gp_registers.end(),
                        variable->name());
                    if (gp_register != gp_registers.end()) {
                        location = AbiParameterLocation{
                            static_cast<std::size_t>(index),
                            AbiParameterStorage::GeneralRegister,
                            static_cast<std::size_t>(std::distance(
                                gp_registers.begin(), gp_register)),
                            0,
                        };
                    }
                }
                if (location.has_value()) {
                    auto [position, inserted] = parameter_locations_by_index.emplace(
                        index, *location);
                    if (!inserted && position->second != *location) {
                        conflicting_parameter_evidence = true;
                    }
                }
                TypePtr candidate = variable->ir_type();
                if (!candidate || candidate->type_kind() == TypeKind::Unknown) {
                    candidate = unsigned_integer_for_size(variable->size_bytes);
                }
                auto [position, inserted] = parameter_types_by_index.emplace(index, candidate);
                if (!inserted && position->second && candidate
                    && *position->second != *candidate) {
                    const bool compatible_integer_views =
                        position->second->type_kind() == TypeKind::Integer
                        && candidate->type_kind() == TypeKind::Integer
                        && position->second->size_bytes() == candidate->size_bytes();
                    if (compatible_integer_views) {
                        position->second = unsigned_integer_for_size(
                            candidate->size_bytes());
                    } else {
                        conflicting_parameter_evidence = true;
                    }
                }
            }
        }
    }
    if (conflicting_parameter_evidence) {
        return result;
    }

    bool contiguous_floating_register_parameters =
        maximum_parameter_index >= hidden_parameter_count;
    if (contiguous_floating_register_parameters) {
        for (int index = hidden_parameter_count;
             index <= maximum_parameter_index;
             ++index) {
            auto found = parameter_types_by_index.find(index);
            if (found == parameter_types_by_index.end()
                || !found->second
                || found->second->type_kind() != TypeKind::Float) {
                contiguous_floating_register_parameters = false;
                break;
            }
        }
    }

    std::vector<std::pair<StackSlotKey, std::vector<Variable*>>> incoming_stack_slots;
    for (const auto& [slot, variables] : required_stack_slots) {
        const auto range_contains = [](const StackSlotKey& outer,
                                       const StackSlotKey& inner) {
            if (outer.first > inner.first) return false;
            const std::uint64_t outer_delta = static_cast<std::uint64_t>(
                inner.first - outer.first);
            return outer_delta <= outer.second
                && inner.second <= outer.second - static_cast<std::size_t>(outer_delta);
        };
        const bool covered_by_definition = std::any_of(
            defined_stack_slots.begin(), defined_stack_slots.end(),
            [&](const StackSlotKey& definition) {
                return range_contains(definition, slot);
            });
        if (!covered_by_definition) {
            incoming_stack_slots.emplace_back(slot, variables);
        }
    }
    if (!incoming_stack_slots.empty()) {
        if (maximum_parameter_index >= register_parameter_limit) {
            return result;
        }
        const std::int64_t first_stack_offset =
            incoming_stack_slots.front().first.first;
        std::int64_t expected_offset = first_stack_offset;
        int parameter_index = maximum_parameter_index + 1;
        for (std::size_t slot_index = 0;
             slot_index < incoming_stack_slots.size();
             ++slot_index) {
            const auto& [slot, variables] = incoming_stack_slots[slot_index];
            const std::int64_t offset = slot.first;
            const std::size_t width = slot.second;
            if (offset != expected_offset || width == 0 || width > 16) {
                return result;
            }
            const bool homogeneous_floating_stack_slot =
                contiguous_floating_register_parameters
                && (width == 4 || width == 8);
            TypePtr parameter_type = homogeneous_floating_stack_slot
                ? float_for_size(width)
                : unsigned_integer_for_size(width);
            bool type_from_evidence = homogeneous_floating_stack_slot;
            for (Variable* variable : variables) {
                if (!variable) continue;
                TypePtr candidate = variable->ir_type();
                if (!candidate || candidate->type_kind() == TypeKind::Unknown) {
                    continue;
                }
                if (candidate->size_bytes() != width) {
                    // Reject auto-guessed aggregate annotations that disagree
                    // with the scalar P-Code load width.
                    continue;
                }
                if (homogeneous_floating_stack_slot
                    && candidate->type_kind() == TypeKind::Integer
                    && candidate->size_bytes() == width) {
                    // AArch64 scalar FP stack arguments can be loaded through
                    // a general-purpose register before being moved into d/s/q.
                    // Homogeneous d0-d7 evidence supplies the semantic type.
                    continue;
                }
                if (!type_from_evidence) {
                    parameter_type = candidate;
                    type_from_evidence = true;
                    continue;
                }
                if (*parameter_type == *candidate) continue;
                const bool compatible_integer_views =
                    parameter_type->type_kind() == TypeKind::Integer
                    && candidate->type_kind() == TypeKind::Integer
                    && parameter_type->size_bytes() == candidate->size_bytes();
                if (!compatible_integer_views) {
                    return result;
                }
                parameter_type = unsigned_integer_for_size(width);
            }
            parameter_types_by_index[parameter_index] = parameter_type;
            const std::int64_t abi_stack_offset = offset;
            inferred_stack_parameters.push_back(IncomingStackParameter{
                variables, parameter_index, parameter_type, abi_stack_offset});
            parameter_locations_by_index[parameter_index] = AbiParameterLocation{
                static_cast<std::size_t>(parameter_index),
                AbiParameterStorage::Stack,
                0,
                abi_stack_offset,
            };
            ++parameter_index;
            maximum_parameter_index = parameter_index - 1;
            bool packed_subword_member = false;
            if (homogeneous_floating_stack_slot && width < 8) {
                if (slot_index > 0) {
                    const auto& previous = incoming_stack_slots[slot_index - 1].first;
                    packed_subword_member = previous.second == width
                        && previous.first <= offset
                        && static_cast<std::uint64_t>(offset - previous.first) == width;
                }
                if (slot_index + 1 < incoming_stack_slots.size()) {
                    const auto& next = incoming_stack_slots[slot_index + 1].first;
                    packed_subword_member = packed_subword_member
                        || (next.second == width && next.first >= offset
                            && static_cast<std::uint64_t>(next.first - offset) == width);
                }
            }
            const std::size_t slot_width = packed_subword_member
                ? width : std::max<std::size_t>(8, width);
            if (slot_width > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max() - expected_offset)) {
                return result;
            }
            expected_offset += static_cast<std::int64_t>(slot_width);
        }
    }

    const auto annotate_stack_parameters = [&]() {
        for (const IncomingStackParameter& parameter : inferred_stack_parameters) {
            for (Variable* variable : parameter.variables) {
                if (!variable) continue;
                variable->set_kind(VariableKind::Parameter);
                variable->set_parameter_index(parameter.index);
                variable->set_ir_type(parameter.type);
                task.set_parameter_register(variable->name(),
                    DecompilerTask::ParameterInfo{
                        "a" + std::to_string(parameter.index + 1),
                        parameter.index,
                        parameter.type,
                    });
            }
        }
    };

    if (existing_function) {
        std::vector<int> stack_parameter_indices;
        if (!existing_function->abi_parameter_locations().empty()) {
            std::vector<AbiParameterLocation> stack_locations;
            for (const AbiParameterLocation& location :
                 existing_function->abi_parameter_locations()) {
                if (location.storage == AbiParameterStorage::Stack) {
                    stack_locations.push_back(location);
                }
            }
            std::sort(stack_locations.begin(), stack_locations.end(),
                [](const AbiParameterLocation& lhs,
                   const AbiParameterLocation& rhs) {
                    return lhs.stack_offset < rhs.stack_offset;
                });
            for (const AbiParameterLocation& location : stack_locations) {
                stack_parameter_indices.push_back(
                    static_cast<int>(location.parameter_index));
            }
        } else {
            std::size_t gp_index = 0;
            std::size_t fp_index = 0;
            for (std::size_t index = 0;
                 index < existing_function->parameters().size();
                 ++index) {
                if ((existing_function->abi_indirect_result_size() > 0
                        || existing_function->abi_hfa_result_count() > 0)
                    && index == 0) {
                    continue;
                }
                const TypePtr& parameter = existing_function->parameters()[index];
                std::size_t& register_index = as_float_type(parameter)
                    ? fp_index : gp_index;
                if (register_index < gp_registers.size()) {
                    ++register_index;
                } else {
                    stack_parameter_indices.push_back(static_cast<int>(index));
                }
            }
        }
        if (stack_parameter_indices.size() != inferred_stack_parameters.size()) {
            return result;
        }
        for (std::size_t index = 0; index < stack_parameter_indices.size(); ++index) {
            const int parameter_index = stack_parameter_indices[index];
            inferred_stack_parameters[index].index = parameter_index;
            inferred_stack_parameters[index].type =
                existing_function->parameters()[static_cast<std::size_t>(parameter_index)];
        }
        annotate_stack_parameters();
        return result;
    }

    std::vector<TypePtr> parameter_types;
    parameter_types.reserve(static_cast<std::size_t>(maximum_parameter_index + 1));
    for (int index = 0; index <= maximum_parameter_index; ++index) {
        auto found = parameter_types_by_index.find(index);
        parameter_types.push_back(found != parameter_types_by_index.end()
            ? found->second : Integer::uint64_t());
    }

    if (has_hidden_result) {
        if (parameter_types.empty()
            || parameter_types.front()->type_kind() != TypeKind::Pointer) {
            return result;
        }
        for (BasicBlock* block : cfg.blocks()) {
            if (!block) continue;
            for (Instruction* instruction : block->instructions()) {
                if (auto* returned = dyn_cast<Return>(instruction)) {
                    returned->mutable_values().clear();
                }
            }
        }
        if (abi_indirect_result_size > 0) {
            task.add_frontend_diagnostic(FrontendDiagnostic{
                FrontendDiagnosticSeverity::Info,
                "pcode-inferred-indirect-result",
                "Incoming x8 reaches result-memory stores; lowered to an explicit "
                + std::to_string(abi_indirect_result_size)
                + "-byte output buffer parameter",
                task.function_address(),
                0,
            });
        } else {
            task.add_frontend_diagnostic(FrontendDiagnostic{
                FrontendDiagnosticSeverity::Info,
                "pcode-inferred-hfa-result",
                "Consecutive floating return-register definitions lowered to an explicit "
                + std::to_string(abi_hfa_result_count) + " x "
                + std::to_string(abi_hfa_result_element_size)
                + "-byte output buffer parameter",
                task.function_address(),
                0,
            });
        }
    }

    std::vector<Return*> two_register_returns;
    bool all_value_returns_are_register_pairs = true;
    bool observed_value_return = false;
    for (BasicBlock* block : cfg.blocks()) {
        if (!block) continue;
        const auto& instructions = block->instructions();
        for (std::size_t return_index = 0;
             return_index < instructions.size();
             ++return_index) {
            auto* returned = dyn_cast<Return>(instructions[return_index]);
            if (!returned || returned->values().empty()) continue;
            observed_value_return = true;
            auto* scalar = returned->values().size() == 1
                ? dyn_cast<Variable>(returned->values()[0]) : nullptr;
            if (return_registers.size() < 2 || !scalar
                || scalar->name() != return_registers[0]
                || returned->address() == 0) {
                all_value_returns_are_register_pairs = false;
                continue;
            }
            ida::Address x0_definition = 0;
            ida::Address x1_definition = 0;
            for (std::size_t cursor = return_index; cursor-- > 0; ) {
                auto* assignment = dyn_cast<Assignment>(instructions[cursor]);
                auto* destination = assignment
                    ? dyn_cast<Variable>(assignment->destination()) : nullptr;
                if (!destination) continue;
                if (destination->name() == return_registers[0]
                    && x0_definition == 0) {
                    x0_definition = assignment->address();
                } else if (destination->name() == return_registers[1]
                           && x1_definition == 0) {
                    x1_definition = assignment->address();
                }
                if (x0_definition != 0 && x1_definition != 0) break;
            }
            const auto recent = [&](ida::Address definition) {
                return definition != 0 && definition <= returned->address()
                    && returned->address() - definition <= 16;
            };
            if (!recent(x0_definition) || !recent(x1_definition)) {
                all_value_returns_are_register_pairs = false;
                continue;
            }
            two_register_returns.push_back(returned);
        }
    }
    if (!has_hidden_result
        && (!architecture || architecture->arch_name == "arm64")
        && observed_value_return && all_value_returns_are_register_pairs
        && !two_register_returns.empty()) {
        for (Return* returned : two_register_returns) {
            auto* low = task.arena().create<Variable>(
                return_registers[0], 8);
            low->set_ir_type(Integer::uint64_t());
            auto* high = task.arena().create<Variable>(
                return_registers[1], 8);
            high->set_ir_type(Integer::uint64_t());
            auto* wide_low = task.arena().create<Operation>(
                OperationType::cast, std::vector<Expression*>{low}, 16);
            wide_low->set_ir_type(Integer::uint128_t());
            auto* wide_high = task.arena().create<Operation>(
                OperationType::cast, std::vector<Expression*>{high}, 16);
            wide_high->set_ir_type(Integer::uint128_t());
            auto* shifted_high = task.arena().create<Operation>(
                OperationType::shl,
                std::vector<Expression*>{
                    wide_high,
                    task.arena().create<Constant>(64, 16),
                },
                16);
            shifted_high->set_ir_type(Integer::uint128_t());
            auto* combined = task.arena().create<Operation>(
                OperationType::bit_or,
                std::vector<Expression*>{wide_low, shifted_high},
                16);
            combined->set_ir_type(Integer::uint128_t());
            returned->mutable_values() = {combined};
        }
        task.add_frontend_diagnostic(FrontendDiagnostic{
            FrontendDiagnosticSeverity::Info,
            "pcode-inferred-two-register-return",
            "Recent x0/x1 definitions combined into a 128-bit ABI return value",
            task.function_address(),
            0,
        });
    }

    bool saw_return = false;
    bool saw_void_return = false;
    bool saw_value_return = false;
    TypePtr return_type;
    for (BasicBlock* block : cfg.blocks()) {
        if (!block) continue;
        for (Instruction* instruction : block->instructions()) {
            auto* returned = dyn_cast<Return>(instruction);
            if (!returned) continue;
            saw_return = true;
            if (returned->values().empty()) {
                saw_void_return = true;
                continue;
            }
            if (returned->values().size() != 1 || !returned->values()[0]) {
                return result;
            }
            saw_value_return = true;
            Expression* value = returned->values()[0];
            TypePtr candidate = value->ir_type();
            if (!candidate || candidate->type_kind() == TypeKind::Unknown) {
                candidate = unsigned_integer_for_size(value->size_bytes);
            }
            if (!return_type) {
                return_type = candidate;
            } else if (!candidate || *return_type != *candidate) {
                return result;
            }
        }
    }
    if (!saw_return || (saw_void_return && saw_value_return)) {
        return result;
    }
    if (saw_void_return || has_hidden_result) {
        return_type = CustomType::void_type();
    }
    if (!return_type) {
        return result;
    }

    const auto is_float_return_register = [&](const Variable* variable) {
        if (!variable) return false;
        const auto register_index = abi_floating_register_index(
            architecture, variable->name());
        return register_index.has_value() && *register_index == 0;
    };
    const bool returns_void = return_type->to_string() == "void";
    const bool returns_float = return_type->type_kind() == TypeKind::Float;
    for (const SelfCallSite& site : self_calls) {
        if (!site.assignment || !site.call
            || site.call->arg_count() != parameter_types.size()) {
            return result;
        }
        auto* destination = dyn_cast<Variable>(site.assignment->destination());
        if (returns_void) {
            if (site.assignment->destination() != nullptr) {
                return result;
            }
        } else if (!destination) {
            return result;
        } else if (returns_float) {
            if (!is_float_return_register(destination)
                || destination->size_bytes != return_type->size_bytes()) {
                return result;
            }
        } else if (return_registers.empty()
                   || destination->name() != return_registers[0]
                   || destination->size_bytes < return_type->size_bytes()) {
            return result;
        }
    }

    std::vector<AbiParameterLocation> inferred_parameter_locations;
    inferred_parameter_locations.reserve(parameter_locations_by_index.size());
    for (const auto& [_, location] : parameter_locations_by_index) {
        if (has_hidden_result && location.parameter_index == 0) {
            continue;
        }
        inferred_parameter_locations.push_back(location);
    }
    TypePtr inferred = std::make_shared<const FunctionTypeDef>(
        return_type,
        parameter_types,
        false,
        abi_indirect_result_size,
        abi_hfa_result_element_size,
        abi_hfa_result_count,
        std::move(inferred_parameter_locations));
    task.set_function_type(inferred);
    result.signature_inferred = true;
    annotate_stack_parameters();

    std::unordered_map<ida::Address, std::size_t> resolved_by_address;
    for (const SelfCallSite& site : self_calls) {
        site.call->set_ir_type(inferred);
        if (!returns_void && return_type->size_bytes() > 0) {
            site.call->size_bytes = return_type->size_bytes();
            auto* destination = dyn_cast<Variable>(site.assignment->destination());
            if (destination) {
                destination->set_ir_type(return_type);
            }
        }
        ++resolved_by_address[site.assignment->address()];
        ++result.resolved_self_calls;
    }

    std::size_t removed_fallbacks = 0;
    std::erase_if(task.mutable_frontend_diagnostics(),
        [&](const FrontendDiagnostic& diagnostic) {
            if (diagnostic.code != "pcode-fallback"
                || !diagnostic.message.starts_with(
                    "CALL: callee prototype unavailable")) {
                return false;
            }
            auto found = resolved_by_address.find(diagnostic.address);
            if (found == resolved_by_address.end() || found->second == 0) {
                return false;
            }
            --found->second;
            ++removed_fallbacks;
            return true;
        });
    auto& support = task.mutable_frontend_support_report();
    const std::size_t reclassified = std::min(removed_fallbacks, support.fallback_ops);
    support.fallback_ops -= reclassified;
    support.implemented_ops += reclassified;
    if (result.resolved_self_calls > 0) {
        task.add_frontend_diagnostic(FrontendDiagnostic{
            FrontendDiagnosticSeverity::Info,
            "pcode-refined-self-signature",
            "Body-derived ABI signature resolved "
                + std::to_string(result.resolved_self_calls)
                + " direct recursive call"
                + (result.resolved_self_calls == 1 ? "" : "s"),
            task.function_address(),
            0,
        });
    } else {
        task.add_frontend_diagnostic(FrontendDiagnostic{
            FrontendDiagnosticSeverity::Info,
            "pcode-inferred-body-signature",
            "Function signature inferred from ABI parameter uses and return storage",
            task.function_address(),
            0,
        });
    }
    return result;
}

} // namespace aletheia
