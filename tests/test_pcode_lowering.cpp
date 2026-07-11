#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../src/aletheia/pcode/pcode_lowering.hpp"
#include "../src/aletheia/pipeline/optimization_stages.hpp"
#include "../src/aletheia/codegen/codegen.hpp"

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAIL: " << #condition << " at " << __FILE__ << ':' << __LINE__ << '\n'; \
        std::exit(1); \
    } \
} while (false)

#define ASSERT_EQ(lhs, rhs) ASSERT_TRUE((lhs) == (rhs))

using namespace aletheia;

namespace {

RawPcodeVarnode varnode(std::string space, std::uint64_t offset, std::size_t size) {
    return RawPcodeVarnode{std::move(space), offset, size};
}

RawPcodeOp operation(ida::Address ea,
                     std::uint32_t ordinal,
                     std::string opcode,
                     std::optional<RawPcodeVarnode> output,
                     std::vector<RawPcodeVarnode> inputs) {
    RawPcodeOp result;
    result.opcode = std::move(opcode);
    result.output = std::move(output);
    result.inputs = std::move(inputs);
    result.instruction_ea = ea;
    result.op_ordinal = ordinal;
    return result;
}

RawPcodeOp memory_operation(ida::Address ea,
                            std::uint32_t ordinal,
                            std::string opcode,
                            std::optional<RawPcodeVarnode> output,
                            std::vector<RawPcodeVarnode> inputs,
                            std::string memory_space = "ram",
                            std::size_t word_size = 1) {
    RawPcodeOp result = operation(
        ea, ordinal, std::move(opcode), std::move(output), std::move(inputs));
    result.memory_space_name = std::move(memory_space);
    result.memory_word_size = word_size;
    return result;
}

RawPcodeInstruction instruction(ida::Address ea, std::vector<RawPcodeOp> ops) {
    return RawPcodeInstruction{ea, 4, std::move(ops)};
}

PcodeArchitectureContext test_architecture(bool big_endian = false) {
    PcodeArchitectureContext architecture;
    architecture.arch_name = "arm64-test";
    architecture.pointer_size = 8;
    architecture.big_endian = big_endian;
    architecture.register_space_name = "register";
    architecture.const_space_name = "const";
    architecture.unique_space_name = "unique";
    architecture.data_space_name = "ram";
    architecture.code_space_name = "code";
    architecture.register_mapper = [](std::uint64_t offset,
                                      std::size_t size) -> std::optional<PcodeRegisterView> {
        if (offset == 0x200 && (size == 4 || size == 8)) {
            return PcodeRegisterView{"xzr", 8, size == 4, size == 4, true};
        }
        if (offset == 0x100 && (size == 4 || size == 8)) {
            return PcodeRegisterView{"sp", 8, size == 4, size == 4, false};
        }
        if (offset == 0x1e0 && size == 8) {
            return PcodeRegisterView{"x30", 8, false, false, false};
        }
        if (offset <= 0x10 && offset % 8 == 0 && (size == 4 || size == 8)) {
            const std::size_t index = static_cast<std::size_t>(offset / 8);
            return PcodeRegisterView{
                "x" + std::to_string(index), 8, size == 4, size == 4, false};
        }
        return std::nullopt;
    };
    return architecture;
}

struct Harness {
    explicit Harness(TypePtr function_type = nullptr,
                     PcodeArchitectureContext architecture = test_architecture(),
                     PcodeSignatureContext signature = {})
        : task(0x1000),
          lowerer(task.arena(),
                  task,
                  std::move(architecture),
                  std::move(signature),
                  PcodeLowerer::Options{0x1000, function_type}) {
        task.set_function_type(std::move(function_type));
        task.set_frontend_kind(FrontendKind::Pcode);
    }

    DecompilerTask task;
    PcodeLowerer lowerer;
};

Assignment* assignment_at(const std::vector<Instruction*>& instructions, std::size_t index) {
    ASSERT_TRUE(index < instructions.size());
    auto* assignment = dyn_cast<Assignment>(instructions[index]);
    ASSERT_TRUE(assignment != nullptr);
    return assignment;
}

void test_full_micro_op_sequence_and_temp_dependencies() {
    Harness harness;
    const ida::Address ea = 0x1000;
    auto raw = instruction(ea, {
        operation(ea, 0, "COPY",
                  varnode("unique", 0x10, 8),
                  {varnode("register", 0x08, 8)}),
        operation(ea, 1, "INT_ADD",
                  varnode("unique", 0x20, 8),
                  {varnode("unique", 0x10, 8), varnode("const", 1, 8)}),
        operation(ea, 2, "COPY",
                  varnode("register", 0x00, 8),
                  {varnode("unique", 0x20, 8)}),
    });

    auto lowered = harness.lowerer.lower_instruction(raw);
    ASSERT_TRUE(lowered.has_value());
    ASSERT_EQ(lowered->size(), 3U);
    ASSERT_EQ(harness.task.frontend_support_report().implemented_ops, 3U);
    ASSERT_EQ(harness.task.frontend_support_report().fallback_ops, 0U);

    auto* first = assignment_at(*lowered, 0);
    auto* second = assignment_at(*lowered, 1);
    auto* third = assignment_at(*lowered, 2);
    ASSERT_EQ(first->address(), ea);
    ASSERT_EQ(second->address(), ea);
    ASSERT_EQ(third->address(), ea);

    auto* first_temp = dyn_cast<Variable>(first->destination());
    auto* second_temp = dyn_cast<Variable>(second->destination());
    ASSERT_TRUE(first_temp != nullptr && second_temp != nullptr);
    ASSERT_TRUE(first_temp->name().find("u_1000_") == 0);
    ASSERT_TRUE(second_temp->name().find("u_1000_") == 0);
    ASSERT_TRUE(first_temp->name() != second_temp->name());

    auto* add = dyn_cast<Operation>(second->value());
    ASSERT_TRUE(add != nullptr);
    ASSERT_EQ(add->type(), OperationType::add);
    auto* add_lhs = dyn_cast<Variable>(add->operands()[0]);
    ASSERT_TRUE(add_lhs != nullptr);
    ASSERT_EQ(add_lhs->name(), first_temp->name());

    auto* final_source = dyn_cast<Variable>(third->value());
    ASSERT_TRUE(final_source != nullptr);
    ASSERT_EQ(final_source->name(), second_temp->name());
}

void test_unique_names_are_scoped_to_machine_address() {
    Harness harness;
    auto first = harness.lowerer.lower_instruction(instruction(0x1000, {
        operation(0x1000, 0, "COPY", varnode("unique", 0x10, 8), {varnode("const", 1, 8)}),
    }));
    auto second = harness.lowerer.lower_instruction(instruction(0x1004, {
        operation(0x1004, 0, "COPY", varnode("unique", 0x10, 8), {varnode("const", 2, 8)}),
    }));
    ASSERT_TRUE(first.has_value() && second.has_value());
    auto* first_var = dyn_cast<Variable>(assignment_at(*first, 0)->destination());
    auto* second_var = dyn_cast<Variable>(assignment_at(*second, 0)->destination());
    ASSERT_TRUE(first_var != nullptr && second_var != nullptr);
    ASSERT_TRUE(first_var->name() != second_var->name());
    ASSERT_TRUE(second_var->name().find("u_1004_") == 0);
}

void test_redefined_unique_varnode_uses_defining_ordinal() {
    Harness harness;
    const ida::Address ea = 0x1008;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "COPY",
                  varnode("unique", 0x10, 8),
                  {varnode("const", 1, 8)}),
        operation(ea, 1, "COPY",
                  varnode("unique", 0x10, 8),
                  {varnode("const", 2, 8)}),
        operation(ea, 2, "COPY",
                  varnode("register", 0, 8),
                  {varnode("unique", 0x10, 8)}),
    }));

    ASSERT_TRUE(lowered.has_value());
    ASSERT_EQ(lowered->size(), 3U);
    auto* first = dyn_cast<Variable>(assignment_at(*lowered, 0)->destination());
    auto* second = dyn_cast<Variable>(assignment_at(*lowered, 1)->destination());
    auto* readback = dyn_cast<Variable>(assignment_at(*lowered, 2)->value());
    ASSERT_TRUE(first != nullptr && second != nullptr && readback != nullptr);
    ASSERT_TRUE(first->name() != second->name());
    ASSERT_TRUE(first->name().find("u_1008_0_") == 0);
    ASSERT_TRUE(second->name().find("u_1008_1_") == 0);
    ASSERT_EQ(readback->name(), second->name());
}

void test_comparison_and_architectural_branch() {
    Harness harness;
    const ida::Address ea = 0x1100;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "INT_EQUAL",
                  varnode("unique", 0x30, 1),
                  {varnode("register", 0, 8), varnode("const", 0, 8)}),
        operation(ea, 1, "CBRANCH",
                  std::nullopt,
                  {varnode("code", 0x1200, 8), varnode("unique", 0x30, 1)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    ASSERT_EQ(lowered->size(), 2U);
    ASSERT_TRUE(isa<Condition>(assignment_at(*lowered, 0)->value()));
    auto* branch = dyn_cast<Branch>((*lowered)[1]);
    ASSERT_TRUE(branch != nullptr && branch->condition() != nullptr);
    ASSERT_EQ(branch->address(), ea);
    ASSERT_EQ(branch->condition()->type(), OperationType::neq);
}

void test_piece_subpiece_and_sign_extension() {
    Harness big_endian_harness(nullptr, test_architecture(true));
    auto piece = big_endian_harness.lowerer.lower_instruction(instruction(0x1200, {
        operation(0x1200, 0, "PIECE",
                  varnode("unique", 0x40, 8),
                  {varnode("const", 0x11223344, 4), varnode("const", 0x55667788, 4)}),
    }));
    ASSERT_TRUE(piece.has_value());
    auto* combined = dyn_cast<Operation>(assignment_at(*piece, 0)->value());
    ASSERT_TRUE(combined != nullptr);
    ASSERT_EQ(combined->type(), OperationType::bit_or);

    Harness harness;
    auto subpiece = harness.lowerer.lower_instruction(instruction(0x1204, {
        operation(0x1204, 0, "SUBPIECE",
                  varnode("unique", 0x50, 4),
                  {varnode("const", 0x1122334455667788ULL, 8), varnode("const", 2, 4)}),
    }));
    ASSERT_TRUE(subpiece.has_value());
    auto* truncate = dyn_cast<Operation>(assignment_at(*subpiece, 0)->value());
    ASSERT_TRUE(truncate != nullptr && truncate->type() == OperationType::cast);
    auto* shift = dyn_cast<Operation>(truncate->operands()[0]);
    ASSERT_TRUE(shift != nullptr && shift->type() == OperationType::shr_us);
    auto* shift_amount = dyn_cast<Constant>(shift->operands()[1]);
    ASSERT_TRUE(shift_amount != nullptr);
    ASSERT_EQ(shift_amount->value(), 16U);

    auto sign_extend = harness.lowerer.lower_instruction(instruction(0x1208, {
        operation(0x1208, 0, "INT_SEXT",
                  varnode("unique", 0x60, 8),
                  {varnode("const", 0xffffffffU, 4)}),
    }));
    ASSERT_TRUE(sign_extend.has_value());
    auto* outer = dyn_cast<Operation>(assignment_at(*sign_extend, 0)->value());
    ASSERT_TRUE(outer != nullptr && outer->type() == OperationType::cast);
    ASSERT_EQ(outer->size_bytes, 8U);
    auto* source_reinterpret = dyn_cast<Operation>(outer->operands()[0]);
    ASSERT_TRUE(source_reinterpret != nullptr && source_reinterpret->type() == OperationType::cast);
    ASSERT_EQ(source_reinterpret->size_bytes, 4U);
    auto* signed_source_type = source_reinterpret->ir_type()
        ? type_dyn_cast<Integer>(source_reinterpret->ir_type().get()) : nullptr;
    ASSERT_TRUE(signed_source_type != nullptr && signed_source_type->is_signed());
    ASSERT_EQ(signed_source_type->size(), 32U);
}

void test_composite_register_read_uses_component_varnodes() {
    auto architecture = test_architecture();
    architecture.register_mapper = [](std::uint64_t offset,
                                      std::size_t size) -> std::optional<PcodeRegisterView> {
        if (offset == 0x5000 && size == 16) {
            return PcodeRegisterView{"q0", 16, false, false, false};
        }
        if (offset == 0x5000 && size == 8) {
            return PcodeRegisterView{"d0", 8, false, false, false};
        }
        return std::nullopt;
    };
    architecture.register_read_slices = [](
        std::uint64_t offset,
        std::size_t size) -> std::optional<std::vector<PcodeRegisterSlice>> {
        if (offset == 0x5000 && size == 16) {
            return std::vector<PcodeRegisterSlice>{{0x5000, 8}, {0x5008, 8}};
        }
        return std::nullopt;
    };

    Harness harness(nullptr, std::move(architecture));
    auto lowered = harness.lowerer.lower_instruction(instruction(0x1210, {
        operation(0x1210, 0, "COPY",
                  varnode("unique", 0x70, 16),
                  {varnode("register", 0x5000, 16)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    auto* combined = dyn_cast<Operation>(assignment_at(*lowered, 0)->value());
    ASSERT_TRUE(combined != nullptr);
    ASSERT_EQ(combined->type(), OperationType::bit_or);

    auto* shifted_high = dyn_cast<Operation>(combined->operands()[0]);
    ASSERT_TRUE(shifted_high != nullptr);
    ASSERT_EQ(shifted_high->type(), OperationType::shl);
    auto* high_cast = dyn_cast<Operation>(shifted_high->operands()[0]);
    ASSERT_TRUE(high_cast != nullptr && high_cast->type() == OperationType::cast);
    auto* high = dyn_cast<Variable>(high_cast->operands()[0]);
    ASSERT_TRUE(high != nullptr);
    ASSERT_EQ(high->name(), std::string("reg_5008_8"));

    auto* low_cast = dyn_cast<Operation>(combined->operands()[1]);
    ASSERT_TRUE(low_cast != nullptr && low_cast->type() == OperationType::cast);
    auto* low = dyn_cast<Variable>(low_cast->operands()[0]);
    ASSERT_TRUE(low != nullptr);
    ASSERT_EQ(low->name(), std::string("d0"));
}

void test_carry_borrow_and_integer_op_coverage() {
    Harness harness;
    auto carry = harness.lowerer.lower_instruction(instruction(0x1300, {
        operation(0x1300, 0, "INT_CARRY",
                  varnode("unique", 0x70, 1),
                  {varnode("const", 0xff, 1), varnode("const", 1, 1)}),
        operation(0x1300, 1, "INT_SCARRY",
                  varnode("unique", 0x71, 1),
                  {varnode("const", 0x7f, 1), varnode("const", 1, 1)}),
        operation(0x1300, 2, "INT_SBORROW",
                  varnode("unique", 0x72, 1),
                  {varnode("const", 0x80, 1), varnode("const", 1, 1)}),
        operation(0x1300, 3, "INT_SDIV",
                  varnode("unique", 0x80, 8),
                  {varnode("const", 10, 8), varnode("const", 3, 8)}),
        operation(0x1300, 4, "INT_SREM",
                  varnode("unique", 0x88, 8),
                  {varnode("const", 10, 8), varnode("const", 3, 8)}),
        operation(0x1300, 5, "INT_2COMP",
                  varnode("unique", 0x90, 8),
                  {varnode("const", 1, 8)}),
        operation(0x1300, 6, "INT_NEGATE",
                  varnode("unique", 0x98, 8),
                  {varnode("const", 0, 8)}),
    }));
    ASSERT_TRUE(carry.has_value());
    ASSERT_EQ(carry->size(), 7U);
    ASSERT_EQ(harness.task.frontend_support_report().implemented_ops, 7U);
    ASSERT_TRUE(isa<Condition>(assignment_at(*carry, 0)->value()));
    ASSERT_TRUE(isa<Condition>(assignment_at(*carry, 1)->value()));
    ASSERT_TRUE(isa<Condition>(assignment_at(*carry, 2)->value()));
}

void test_memory_is_single_evaluation_and_space_checked() {
    Harness harness;
    const ida::Address ea = 0x1400;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        memory_operation(ea, 0, "LOAD",
                         varnode("unique", 0xa0, 8),
                         {varnode("const", 0, 8), varnode("register", 8, 8)}),
        operation(ea, 1, "COPY",
                  varnode("register", 0, 8),
                  {varnode("unique", 0xa0, 8)}),
        memory_operation(ea, 2, "STORE",
                         std::nullopt,
                         {varnode("const", 0, 8), varnode("register", 8, 8), varnode("register", 0, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    ASSERT_EQ(lowered->size(), 3U);
    ASSERT_TRUE(isa<Operation>(assignment_at(*lowered, 0)->value()));
    ASSERT_TRUE(isa<Variable>(assignment_at(*lowered, 1)->value()));
    auto* store_destination = dyn_cast<Operation>(assignment_at(*lowered, 2)->destination());
    ASSERT_TRUE(store_destination != nullptr);
    ASSERT_EQ(store_destination->type(), OperationType::deref);

    Harness wrong_space;
    auto rejected = wrong_space.lowerer.lower_instruction(instruction(0x1404, {
        memory_operation(0x1404, 0, "LOAD",
                         varnode("unique", 0xb0, 8),
                         {varnode("const", 0, 8), varnode("register", 8, 8)},
                         "io"),
    }));
    ASSERT_TRUE(!rejected.has_value());
    ASSERT_EQ(wrong_space.task.frontend_support_report().unsupported_ops, 1U);
}

void test_injected_metadata_calls_and_parameters() {
    auto architecture = test_architecture();
    architecture.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        return address == 0x4000 ? std::optional<std::string>("callee") : std::nullopt;
    };
    architecture.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4000) return std::nullopt;
        return PcodeFunctionInfo{
            "callee",
            {Integer::int32_t(), Integer::int64_t()},
            Integer::int32_t(),
            true,
        };
    };
    PcodeSignatureContext signature;
    signature.parameter_register_map["x0"] = 0;
    auto function_type = std::make_shared<const FunctionTypeDef>(
        Integer::int64_t(), std::vector<TypePtr>{Integer::int32_t()});
    Harness harness(function_type, std::move(architecture), std::move(signature));

    auto parameter_copy = harness.lowerer.lower_instruction(instruction(0x1500, {
        operation(0x1500, 0, "COPY",
                  varnode("unique", 0xc0, 4),
                  {varnode("register", 0, 4)}),
    }));
    ASSERT_TRUE(parameter_copy.has_value());
    auto* low_view = dyn_cast<Operation>(assignment_at(*parameter_copy, 0)->value());
    ASSERT_TRUE(low_view != nullptr && low_view->type() == OperationType::cast);
    auto* parameter = dyn_cast<Variable>(low_view->operands()[0]);
    ASSERT_TRUE(parameter != nullptr);
    ASSERT_EQ(parameter->kind(), VariableKind::Parameter);
    ASSERT_EQ(parameter->parameter_index(), 0);

    auto call = harness.lowerer.lower_instruction(instruction(0x1504, {
        operation(0x1504, 0, "CALL", std::nullopt, {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(call.has_value());
    auto* call_assignment = assignment_at(*call, 0);
    auto* result = dyn_cast<Variable>(call_assignment->destination());
    auto* call_expression = dyn_cast<Call>(call_assignment->value());
    ASSERT_TRUE(result != nullptr && call_expression != nullptr);
    ASSERT_EQ(result->name(), std::string("x0"));
    ASSERT_EQ(result->size_bytes, 8U);
    ASSERT_EQ(call_expression->arg_count(), 2U);
    auto* first_view = dyn_cast<Operation>(call_expression->arg(0));
    ASSERT_TRUE(first_view != nullptr && first_view->type() == OperationType::cast);
    auto* first_storage = dyn_cast<Variable>(first_view->operands()[0]);
    ASSERT_TRUE(first_storage != nullptr);
    ASSERT_EQ(first_storage->name(), std::string("x0"));
    ASSERT_TRUE(first_view->ir_type() != nullptr);
    ASSERT_EQ(first_view->ir_type()->to_string(), std::string("int"));
    ASSERT_EQ(dyn_cast<Variable>(call_expression->arg(1))->name(), std::string("x1"));
}

void test_returns_intrinsics_and_fallback_reporting() {
    Harness unknown_return_harness;
    auto unknown_return = unknown_return_harness.lowerer.lower_instruction(instruction(0x15fc, {
        operation(0x15fc, 0, "RETURN", std::nullopt, {varnode("register", 0x1e0, 8)}),
    }));
    ASSERT_TRUE(unknown_return.has_value());
    ASSERT_EQ(unknown_return_harness.task.frontend_support_report().fallback_ops, 1U);

    auto void_type = std::make_shared<const FunctionTypeDef>(
        CustomType::void_type(), std::vector<TypePtr>{});
    Harness void_harness(void_type);
    auto void_return = void_harness.lowerer.lower_instruction(instruction(0x1600, {
        operation(0x1600, 0, "RETURN", std::nullopt, {varnode("register", 0x1e0, 8)}),
    }));
    ASSERT_TRUE(void_return.has_value());
    auto* void_ret = dyn_cast<Return>((*void_return)[0]);
    ASSERT_TRUE(void_ret != nullptr && !void_ret->has_value());

    auto int_type = std::make_shared<const FunctionTypeDef>(
        Integer::int32_t(), std::vector<TypePtr>{});
    Harness int_harness(int_type);
    auto int_return = int_harness.lowerer.lower_instruction(instruction(0x1604, {
        operation(0x1604, 0, "RETURN", std::nullopt, {varnode("register", 0x1e0, 8)}),
    }));
    ASSERT_TRUE(int_return.has_value());
    auto* int_ret = dyn_cast<Return>((*int_return)[0]);
    ASSERT_TRUE(int_ret != nullptr && int_ret->has_value());
    auto* low_view = dyn_cast<Operation>(int_ret->values()[0]);
    ASSERT_TRUE(low_view != nullptr && low_view->type() == OperationType::cast);
    auto* x0 = dyn_cast<Variable>(low_view->operands()[0]);
    ASSERT_TRUE(x0 != nullptr);
    ASSERT_EQ(x0->name(), std::string("x0"));
    ASSERT_EQ(low_view->size_bytes, 4U);

    Harness intrinsic_harness;
    auto intrinsic = intrinsic_harness.lowerer.lower_instruction(instruction(0x1608, {
        operation(0x1608, 0, "POPCOUNT",
                  varnode("unique", 0xd0, 8),
                  {varnode("const", 0xff, 8)}),
        operation(0x1608, 1, "CALLOTHER",
                  varnode("unique", 0xd8, 8),
                  {varnode("const", 7, 8), varnode("unique", 0xd0, 8)}),
    }));
    ASSERT_TRUE(intrinsic.has_value());
    ASSERT_EQ(intrinsic_harness.task.frontend_support_report().fallback_ops, 2U);
    ASSERT_EQ(intrinsic_harness.task.frontend_diagnostics().size(), 2U);
    auto* intrinsic_call = dyn_cast<Call>(assignment_at(*intrinsic, 0)->value());
    ASSERT_TRUE(intrinsic_call != nullptr);
    ASSERT_TRUE(isa<GlobalVariable>(intrinsic_call->target()));
    ASSERT_EQ(intrinsic_call->arg_count(), 2U);
    auto* intrinsic_width = dyn_cast<Constant>(intrinsic_call->arg(1));
    ASSERT_TRUE(intrinsic_width != nullptr);
    ASSERT_EQ(intrinsic_width->value(), 64U);
}

void test_unknown_prototype_call_argument_write_inference() {
    auto architecture = test_architecture();
    architecture.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        return address == 0x4000
            ? std::optional<std::string>{"unknown_callee"} : std::nullopt;
    };
    architecture.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4000) {
            return std::nullopt;
        }
        PcodeFunctionInfo info;
        info.name = "unknown_callee";
        info.return_type = Integer::uint64_t();
        info.prototype_known = false;
        return info;
    };
    Harness harness(nullptr, std::move(architecture));
    harness.lowerer.begin_basic_block();

    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1610, {
        operation(0x1610, 0, "COPY", varnode("register", 0, 4),
                  {varnode("const", 7, 4)}),
    })).has_value());
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1614, {
        operation(0x1614, 0, "COPY", varnode("register", 8, 8),
                  {varnode("const", 9, 8)}),
    })).has_value());
    auto inferred = harness.lowerer.lower_instruction(instruction(0x1618, {
        operation(0x1618, 0, "CALL", std::nullopt, {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(inferred.has_value());
    auto* inferred_call = dyn_cast<Call>(assignment_at(*inferred, 0)->value());
    ASSERT_TRUE(inferred_call != nullptr);
    ASSERT_EQ(inferred_call->arg_count(), 2U);
    ASSERT_TRUE(harness.task.frontend_diagnostics().back().message.find(
        "inferred 2 contiguous general-purpose argument registers") != std::string::npos);

    // Architectural block boundaries invalidate predecessor-dependent evidence.
    harness.lowerer.begin_basic_block();
    auto after_boundary = harness.lowerer.lower_instruction(instruction(0x161c, {
        operation(0x161c, 0, "CALL", std::nullopt, {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(after_boundary.has_value());
    auto* boundary_call = dyn_cast<Call>(assignment_at(*after_boundary, 0)->value());
    ASSERT_TRUE(boundary_call != nullptr);
    ASSERT_EQ(boundary_call->arg_count(), 0U);

    // Sparse writes stop at the first unwritten ABI register.
    harness.lowerer.begin_basic_block();
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1620, {
        operation(0x1620, 0, "COPY", varnode("register", 0, 8),
                  {varnode("const", 1, 8)}),
        operation(0x1620, 1, "COPY", varnode("register", 16, 8),
                  {varnode("const", 3, 8)}),
    })).has_value());
    auto sparse = harness.lowerer.lower_instruction(instruction(0x1624, {
        operation(0x1624, 0, "CALL", std::nullopt, {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(sparse.has_value());
    auto* sparse_call = dyn_cast<Call>(assignment_at(*sparse, 0)->value());
    ASSERT_TRUE(sparse_call != nullptr);
    ASSERT_EQ(sparse_call->arg_count(), 1U);

    // Authoritative zero-argument prototypes suppress heuristic evidence.
    auto known_architecture = test_architecture();
    known_architecture.symbol_resolver = [](ida::Address) -> std::optional<std::string> {
        return "known_zero";
    };
    known_architecture.function_resolver = [](ida::Address) -> std::optional<PcodeFunctionInfo> {
        PcodeFunctionInfo info;
        info.name = "known_zero";
        info.return_type = Integer::uint64_t();
        info.prototype_known = true;
        return info;
    };
    Harness known_harness(nullptr, std::move(known_architecture));
    known_harness.lowerer.begin_basic_block();
    ASSERT_TRUE(known_harness.lowerer.lower_instruction(instruction(0x1628, {
        operation(0x1628, 0, "COPY", varnode("register", 0, 8),
                  {varnode("const", 1, 8)}),
    })).has_value());
    auto known = known_harness.lowerer.lower_instruction(instruction(0x162c, {
        operation(0x162c, 0, "CALL", std::nullopt, {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(known.has_value());
    auto* known_call = dyn_cast<Call>(assignment_at(*known, 0)->value());
    ASSERT_TRUE(known_call != nullptr);
    ASSERT_EQ(known_call->arg_count(), 0U);
}

void test_stack_address_provenance_through_unique_temp() {
    std::optional<std::tuple<PcodeStackBase, ida::Address, std::int64_t, std::size_t>> observed;
    auto architecture = test_architecture();
    architecture.stack_variable_resolver =
        [&](DecompilerArena& arena,
            PcodeStackBase base,
            ida::Address ea,
            std::int64_t offset,
            std::size_t width) -> Variable* {
            observed = {base, ea, offset, width};
            auto* slot = arena.create<Variable>("local_20", width);
            slot->set_kind(VariableKind::StackLocal);
            slot->set_stack_offset(offset);
            return slot;
        };
    Harness harness(nullptr, std::move(architecture));
    const ida::Address ea = 0x1800;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "INT_ADD",
                  varnode("unique", 0x110, 8),
                  {varnode("register", 0x100, 8), varnode("const", 0x20, 8)}),
        memory_operation(ea, 1, "LOAD",
                         varnode("unique", 0x120, 4),
                         {varnode("const", 0, 8), varnode("unique", 0x110, 8)}),
    }));

    ASSERT_TRUE(lowered.has_value());
    ASSERT_TRUE(observed.has_value());
    ASSERT_EQ(std::get<0>(*observed), PcodeStackBase::StackPointer);
    ASSERT_EQ(std::get<1>(*observed), ea);
    ASSERT_EQ(std::get<2>(*observed), 0x20);
    ASSERT_EQ(std::get<3>(*observed), 4U);
    auto* load_value = assignment_at(*lowered, 1)->value();
    ASSERT_TRUE(isa<Variable>(load_value));
    ASSERT_EQ(dyn_cast<Variable>(load_value)->name(), std::string("local_20"));

    observed.reset();
    auto escaped = harness.lowerer.lower_instruction(instruction(ea + 4, {
        operation(ea + 4, 0, "INT_ADD",
                  varnode("unique", 0x130, 8),
                  {varnode("register", 0x100, 8), varnode("const", 0x28, 8)}),
        operation(ea + 4, 1, "COPY",
                  varnode("register", 0x08, 8),
                  {varnode("unique", 0x130, 8)}),
    }));
    ASSERT_TRUE(escaped.has_value());
    ASSERT_TRUE(observed.has_value());
    ASSERT_EQ(std::get<2>(*observed), 0x28);
    ASSERT_EQ(std::get<3>(*observed), 8U);
    auto* escaped_value = assignment_at(*escaped, 1)->value();
    auto* address_of = dyn_cast<Operation>(escaped_value);
    ASSERT_TRUE(address_of != nullptr);
    ASSERT_EQ(address_of->type(), OperationType::address_of);
    auto* escaped_slot = dyn_cast<Variable>(address_of->operands()[0]);
    ASSERT_TRUE(escaped_slot != nullptr);
    ASSERT_EQ(escaped_slot->name(), std::string("local_20"));
}

void test_architectural_terminator_must_be_last() {
    Harness harness;
    const ida::Address ea = 0x1810;
    auto result = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "CBRANCH",
                  std::nullopt,
                  {varnode("code", 0x1900, 8), varnode("const", 1, 1)}),
        operation(ea, 1, "COPY",
                  varnode("register", 8, 8),
                  {varnode("const", 7, 8)}),
    }));
    ASSERT_TRUE(!result.has_value());
    ASSERT_TRUE(harness.task.frontend_diagnostics().back().message.find("terminator")
        != std::string::npos);
}

void test_intra_instruction_conditional_exit_is_predicated() {
    Harness harness;
    const ida::Address ea = 0x1814;
    auto result = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "INT2FLOAT",
                  varnode("unique", 0x1200, 8),
                  {varnode("const", 0, 8)}),
        operation(ea, 1, "COPY", varnode("register", 0x100, 1), {varnode("const", 0, 1)}),
        operation(ea, 2, "COPY", varnode("register", 0x101, 1), {varnode("const", 0, 1)}),
        operation(ea, 3, "COPY", varnode("register", 0x102, 1), {varnode("const", 1, 1)}),
        operation(ea, 4, "COPY", varnode("register", 0x103, 1), {varnode("const", 1, 1)}),
        operation(ea, 5, "FLOAT_NAN",
                  varnode("unique", 0x1b500, 1),
                  {varnode("register", 0x5100, 8)}),
        operation(ea, 6, "FLOAT_NAN",
                  varnode("unique", 0x1b600, 1),
                  {varnode("unique", 0x1200, 8)}),
        operation(ea, 7, "BOOL_OR",
                  varnode("unique", 0x1b800, 1),
                  {varnode("unique", 0x1b500, 1), varnode("unique", 0x1b600, 1)}),
        operation(ea, 8, "CBRANCH",
                  std::nullopt,
                  {varnode("code", ea + 4, 8), varnode("unique", 0x1b800, 1)}),
        operation(ea, 9, "FLOAT_LESS",
                  varnode("register", 0x100, 1),
                  {varnode("register", 0x5100, 8), varnode("unique", 0x1200, 8)}),
        operation(ea, 10, "FLOAT_EQUAL",
                  varnode("register", 0x101, 1),
                  {varnode("register", 0x5100, 8), varnode("unique", 0x1200, 8)}),
        operation(ea, 11, "FLOAT_LESSEQUAL",
                  varnode("register", 0x102, 1),
                  {varnode("unique", 0x1200, 8), varnode("register", 0x5100, 8)}),
        operation(ea, 12, "COPY",
                  varnode("register", 0x103, 1),
                  {varnode("const", 0, 1)}),
    }));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 12U);
    ASSERT_EQ(harness.task.frontend_support_report().implemented_ops
        + harness.task.frontend_support_report().fallback_ops, 13U);
    ASSERT_TRUE(harness.task.frontend_support_report().fallback_ops >= 2U);
    for (Instruction* emitted : *result) {
        ASSERT_TRUE(!isa<Branch>(emitted));
    }
    for (std::size_t index = 8; index < result->size(); ++index) {
        auto* selected = dyn_cast<Operation>(assignment_at(*result, index)->value());
        ASSERT_TRUE(selected != nullptr && selected->type() == OperationType::ternary);
        ASSERT_EQ(selected->operands().size(), 3U);
        ASSERT_TRUE(isa<Condition>(selected->operands()[0]));
        ASSERT_TRUE(isa<Variable>(selected->operands()[1]));
    }

    Harness side_effect_harness;
    auto rejected = side_effect_harness.lowerer.lower_instruction(instruction(0x1818, {
        operation(0x1818, 0, "CBRANCH",
                  std::nullopt,
                  {varnode("code", 0x181c, 8), varnode("const", 1, 1)}),
        memory_operation(0x1818, 1, "STORE",
                         std::nullopt,
                         {varnode("const", 0, 8),
                          varnode("register", 0, 8),
                          varnode("register", 8, 8)}),
    }));
    ASSERT_TRUE(!rejected.has_value());
    ASSERT_TRUE(side_effect_harness.task.frontend_diagnostics().back().message.find("predicable")
        != std::string::npos);
}

void test_recursive_and_float_call_abi() {
    PcodeSignatureContext signature;
    signature.parameter_count_hint = 1;
    signature.prefers_w_arguments = true;
    signature.prefers_w_return = true;
    auto self_type = std::make_shared<const FunctionTypeDef>(
        Integer::int32_t(), std::vector<TypePtr>{Integer::int32_t()});
    Harness recursive(self_type, test_architecture(), signature);
    auto self_call = recursive.lowerer.lower_instruction(instruction(0x1820, {
        operation(0x1820, 0, "CALL", std::nullopt, {varnode("code", 0x1000, 8)}),
    }));
    ASSERT_TRUE(self_call.has_value());
    auto* self_assignment = assignment_at(*self_call, 0);
    auto* self_result = dyn_cast<Variable>(self_assignment->destination());
    auto* self_expression = dyn_cast<Call>(self_assignment->value());
    ASSERT_TRUE(self_result != nullptr && self_expression != nullptr);
    ASSERT_EQ(self_result->name(), std::string("x0"));
    ASSERT_EQ(self_expression->size_bytes, 4U);
    ASSERT_EQ(self_expression->arg_count(), 1U);
    auto* self_arg_view = dyn_cast<Operation>(self_expression->arg(0));
    ASSERT_TRUE(self_arg_view != nullptr && self_arg_view->type() == OperationType::cast);
    ASSERT_EQ(dyn_cast<Variable>(self_arg_view->operands()[0])->name(), std::string("x0"));
    ASSERT_TRUE(self_expression->ir_type() != nullptr);
    ASSERT_TRUE(type_dyn_cast<FunctionTypeDef>(self_expression->ir_type().get()) != nullptr);

    auto architecture = test_architecture();
    architecture.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4000) return std::nullopt;
        return PcodeFunctionInfo{
            "floating_callee",
            {Float::float64(), Integer::int32_t()},
            Float::float64(),
            true,
        };
    };
    Harness floating(nullptr, std::move(architecture));
    auto float_call = floating.lowerer.lower_instruction(instruction(0x1824, {
        operation(0x1824, 0, "CALL", std::nullopt, {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(float_call.has_value());
    auto* float_assignment = assignment_at(*float_call, 0);
    auto* float_result = dyn_cast<Variable>(float_assignment->destination());
    auto* float_expression = dyn_cast<Call>(float_assignment->value());
    ASSERT_TRUE(float_result != nullptr && float_expression != nullptr);
    ASSERT_EQ(float_result->name(), std::string("d0"));
    ASSERT_EQ(float_expression->arg_count(), 2U);
    ASSERT_EQ(dyn_cast<Variable>(float_expression->arg(0))->name(), std::string("d0"));
    auto* integer_arg = dyn_cast<Operation>(float_expression->arg(1));
    ASSERT_TRUE(integer_arg != nullptr && integer_arg->type() == OperationType::cast);
    ASSERT_EQ(dyn_cast<Variable>(integer_arg->operands()[0])->name(), std::string("x0"));
}

void test_float_lowering_and_exact_constant_rendering() {
    Harness harness;
    const ida::Address ea = 0x1830;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "INT2FLOAT",
                  varnode("unique", 0x200, 8),
                  {varnode("const", 7, 4)}),
        operation(ea, 1, "FLOAT_ADD",
                  varnode("unique", 0x208, 8),
                  {varnode("unique", 0x200, 8),
                   varnode("const", 0x3ff8000000000000ULL, 8)}),
        operation(ea, 2, "FLOAT_EQUAL",
                  varnode("unique", 0x210, 1),
                  {varnode("unique", 0x208, 8),
                   varnode("const", 0x4021000000000000ULL, 8)}),
        operation(ea, 3, "FLOAT_NEG",
                  varnode("unique", 0x218, 8),
                  {varnode("unique", 0x208, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    ASSERT_EQ(lowered->size(), 4U);
    auto* conversion = dyn_cast<Operation>(assignment_at(*lowered, 0)->value());
    auto* addition = dyn_cast<Operation>(assignment_at(*lowered, 1)->value());
    auto* comparison = dyn_cast<Condition>(assignment_at(*lowered, 2)->value());
    auto* negation = dyn_cast<Operation>(assignment_at(*lowered, 3)->value());
    ASSERT_TRUE(conversion != nullptr && conversion->type() == OperationType::cast);
    ASSERT_TRUE(conversion->ir_type() != nullptr
        && type_dyn_cast<Float>(conversion->ir_type().get()) != nullptr);
    ASSERT_TRUE(addition != nullptr && addition->type() == OperationType::add_float);
    ASSERT_TRUE(comparison != nullptr && comparison->type() == OperationType::eq);
    ASSERT_TRUE(negation != nullptr && negation->type() == OperationType::negate);

    auto* bits = dyn_cast<Constant>(addition->operands()[1]);
    ASSERT_TRUE(bits != nullptr && bits->ir_type() != nullptr);
    CExpressionGenerator generator;
    const std::string rendered = generator.generate(bits);
    ASSERT_TRUE(rendered.find("union { uint64_t u; double f; }") != std::string::npos);
    ASSERT_TRUE(rendered.find("3ff8000000000000") != std::string::npos);
}

void test_shift_boundary_matches_sleigh_bitvector_semantics() {
    Harness harness;
    const ida::Address ea = 0x1840;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "INT_LEFT",
                  varnode("unique", 0x220, 8),
                  {varnode("const", 1, 8), varnode("const", 64, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* block = harness.task.arena().create<BasicBlock>(0);
    block->add_instruction((*lowered)[0]);
    cfg->add_block(block);
    cfg->set_entry_block(block);
    harness.task.set_cfg(std::move(cfg));

    ExpressionSimplificationStage stage;
    stage.execute(harness.task);
    auto* folded = dyn_cast<Constant>(assignment_at(
        harness.task.cfg()->blocks()[0]->instructions(), 0)->value());
    ASSERT_TRUE(folded != nullptr);
    ASSERT_EQ(folded->value(), 0U);
    ASSERT_EQ(folded->size_bytes, 8U);
}

void test_fail_closed_validation_and_diagnostics() {
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x1700, {
            operation(0x1700, 9, "COPY",
                      varnode("unique", 0xe0, 8),
                      {varnode("mystery", 0, 8)}),
        }));
        ASSERT_TRUE(!result.has_value());
        ASSERT_EQ(harness.task.frontend_support_report().unsupported_ops, 1U);
        ASSERT_EQ(harness.task.frontend_diagnostics().size(), 1U);
        ASSERT_EQ(harness.task.frontend_diagnostics()[0].address, 0x1700U);
        ASSERT_EQ(harness.task.frontend_diagnostics()[0].op_ordinal, 9U);
    }
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x1704, {
            operation(0x1704, 0, "COPY",
                      varnode("register", 0, 8),
                      {varnode("unique", 0xf0, 8)}),
        }));
        ASSERT_TRUE(!result.has_value());
        ASSERT_TRUE(harness.task.frontend_diagnostics()[0].message.find("before definition") != std::string::npos);
    }
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x1708, {
            operation(0x1708, 0, "CBRANCH",
                      std::nullopt,
                      {varnode("const", 2, 8), varnode("const", 1, 1)}),
        }));
        ASSERT_TRUE(!result.has_value());
        ASSERT_TRUE(harness.task.frontend_diagnostics()[0].message.find("micro-CFG") != std::string::npos);
    }
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x170c, {
            operation(0x1710, 0, "COPY",
                      varnode("register", 0, 8),
                      {varnode("const", 0, 8)}),
        }));
        ASSERT_TRUE(!result.has_value());
        ASSERT_TRUE(harness.task.frontend_diagnostics()[0].message.find("address") != std::string::npos);
    }
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x1714, {
            operation(0x1714, 0, "COPY",
                      varnode("register", 0x999, 8),
                      {varnode("const", 1, 8)}),
        }));
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(harness.task.frontend_support_report().fallback_ops, 1U);
        auto* destination = dyn_cast<Variable>(assignment_at(*result, 0)->destination());
        ASSERT_TRUE(destination != nullptr);
        ASSERT_EQ(destination->name(), std::string("reg_999_8"));
    }
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x1718, {
            memory_operation(0x1718, 0, "LOAD",
                             varnode("ram", 0x2000, 8),
                             {varnode("const", 0, 8), varnode("register", 8, 8)}),
        }));
        ASSERT_TRUE(!result.has_value());
        ASSERT_TRUE(harness.task.frontend_diagnostics()[0].message.find("output")
            != std::string::npos);
    }
    {
        Harness harness;
        auto result = harness.lowerer.lower_instruction(instruction(0x171c, {
            operation(0x171c, 0, "INT_ZEXT",
                      varnode("unique", 0x300, 4),
                      {varnode("register", 0, 8)}),
        }));
        ASSERT_TRUE(!result.has_value());
        ASSERT_TRUE(harness.task.frontend_diagnostics()[0].message.find("narrows")
            != std::string::npos);
    }
}

} // namespace

int main() {
    test_full_micro_op_sequence_and_temp_dependencies();
    test_unique_names_are_scoped_to_machine_address();
    test_redefined_unique_varnode_uses_defining_ordinal();
    test_comparison_and_architectural_branch();
    test_piece_subpiece_and_sign_extension();
    test_composite_register_read_uses_component_varnodes();
    test_carry_borrow_and_integer_op_coverage();
    test_memory_is_single_evaluation_and_space_checked();
    test_injected_metadata_calls_and_parameters();
    test_returns_intrinsics_and_fallback_reporting();
    test_unknown_prototype_call_argument_write_inference();
    test_stack_address_provenance_through_unique_temp();
    test_architectural_terminator_must_be_last();
    test_intra_instruction_conditional_exit_is_predicated();
    test_recursive_and_float_call_abi();
    test_float_lowering_and_exact_constant_rendering();
    test_shift_boundary_matches_sleigh_bitvector_semantics();
    test_fail_closed_validation_and_diagnostics();
    std::cout << "All P-Code lowering tests passed.\n";
    return 0;
}
