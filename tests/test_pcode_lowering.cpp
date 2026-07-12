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
#include "../src/aletheia/ssa/ssa_constructor.hpp"
#include "../src/aletheia/ssa/ssa_destructor.hpp"

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
        if (offset == 0x300 && size == 8) {
            return PcodeRegisterView{"d0", 8, false, false, false};
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
        if (offset == 0x6000 && size == 16) {
            return PcodeRegisterView{"q6", 16, false, false, false};
        }
        if (offset == 0x6000 && size == 8) {
            return PcodeRegisterView{"d6", 8, false, false, false};
        }
        return std::nullopt;
    };
    architecture.register_read_slices = [](
        std::uint64_t offset,
        std::size_t size) -> std::optional<std::vector<PcodeRegisterSlice>> {
        if (offset == 0x5000 && size == 16) {
            return std::vector<PcodeRegisterSlice>{{0x5000, 8}, {0x5008, 8}};
        }
        if (offset == 0x6000 && size == 16) {
            return std::vector<PcodeRegisterSlice>{{0x6000, 8}, {0x6008, 8}};
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
    auto* low_bits = dyn_cast<Operation>(low_cast->operands()[0]);
    ASSERT_TRUE(low_bits != nullptr && low_bits->type() == OperationType::bitcast);
    auto* low = dyn_cast<Variable>(low_bits->operands()[0]);
    ASSERT_TRUE(low != nullptr);
    ASSERT_EQ(low->name(), std::string("d0"));

    auto copied = harness.lowerer.lower_instruction(instruction(0x1214, {
        operation(0x1214, 0, "COPY", varnode("register", 0x6000, 16),
                  {varnode("register", 0x5000, 16)}),
    }));
    ASSERT_TRUE(copied.has_value());
    auto low_lane = harness.lowerer.lower_instruction(instruction(0x1218, {
        operation(0x1218, 0, "COPY", varnode("unique", 0x78, 8),
                  {varnode("register", 0x6000, 8)}),
    }));
    ASSERT_TRUE(low_lane.has_value());
    auto* lane_bitcast = dyn_cast<Operation>(assignment_at(*low_lane, 0)->value());
    ASSERT_TRUE(lane_bitcast != nullptr
        && lane_bitcast->type() == OperationType::bitcast
        && lane_bitcast->ir_type() != nullptr
        && lane_bitcast->ir_type()->type_kind() == TypeKind::Float);
    auto* lane_low = dyn_cast<Operation>(lane_bitcast->operands()[0]);
    ASSERT_TRUE(lane_low != nullptr && lane_low->type() == OperationType::cast);
    auto* wide_storage = dyn_cast<Variable>(lane_low->operands()[0]);
    ASSERT_TRUE(wide_storage != nullptr);
    ASSERT_EQ(wide_storage->name(), std::string("q6"));
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

    Harness floating_return_harness;
    auto floating_return = floating_return_harness.lowerer.lower_instruction(
        instruction(0x15fe, {
            operation(0x15fe, 0, "INT2FLOAT", varnode("register", 0x300, 8),
                      {varnode("const", 7, 8)}),
            operation(0x15fe, 1, "RETURN", std::nullopt,
                      {varnode("register", 0x1e0, 8)}),
        }));
    ASSERT_TRUE(floating_return.has_value());
    auto* floating_ret = dyn_cast<Return>((*floating_return)[1]);
    ASSERT_TRUE(floating_ret != nullptr && floating_ret->has_value());
    auto* d0 = dyn_cast<Variable>(floating_ret->values()[0]);
    ASSERT_TRUE(d0 != nullptr);
    ASSERT_EQ(d0->name(), std::string("d0"));
    ASSERT_TRUE(d0->ir_type() != nullptr);
    ASSERT_TRUE(d0->ir_type()->type_kind() == TypeKind::Float);
    ASSERT_EQ(floating_return_harness.task.frontend_support_report().fallback_ops, 1U);
    ASSERT_TRUE(floating_return_harness.task.frontend_diagnostics().back().message.find(
        "inferred floating ABI return register") != std::string::npos);

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
    ASSERT_EQ(intrinsic_harness.task.frontend_support_report().implemented_ops, 1U);
    ASSERT_EQ(intrinsic_harness.task.frontend_support_report().fallback_ops, 1U);
    ASSERT_EQ(intrinsic_harness.task.frontend_diagnostics().size(), 1U);
    auto* intrinsic_count = dyn_cast<Operation>(assignment_at(*intrinsic, 0)->value());
    ASSERT_TRUE(intrinsic_count != nullptr);
    ASSERT_EQ(intrinsic_count->type(), OperationType::popcount);
    ASSERT_EQ(intrinsic_count->operands().size(), 2U);
    auto* intrinsic_width = dyn_cast<Constant>(intrinsic_count->operands()[1]);
    ASSERT_TRUE(intrinsic_width != nullptr);
    ASSERT_EQ(intrinsic_width->value(), 64U);
}

void test_x86_push_arguments_exclude_call_return_address_store() {
    PcodeArchitectureContext architecture;
    architecture.arch_name = "x86_64-test";
    architecture.gp_argument_registers = {
        "rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    architecture.integer_return_registers = {"rax", "rdx"};
    architecture.stack_pointer_register = "rsp";
    architecture.frame_pointer_register = "rbp";
    architecture.register_mapper = [](std::uint64_t offset,
                                      std::size_t size)
        -> std::optional<PcodeRegisterView> {
        if (offset == 0x100 && size == 8) {
            return PcodeRegisterView{"rsp", 8, false, false, false};
        }
        return std::nullopt;
    };
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address ea,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>(
            "stack_" + std::to_string(ea), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        slot->set_ir_type(Integer::uint64_t());
        return slot;
    };
    architecture.function_resolver = [](ida::Address address)
        -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4000) return std::nullopt;
        PcodeFunctionInfo info;
        info.name = "callee10";
        info.parameter_types.assign(10, Integer::uint64_t());
        info.return_type = Integer::uint64_t();
        info.prototype_known = true;
        for (std::size_t index = 6; index < 10; ++index) {
            info.abi_parameter_locations.push_back(AbiParameterLocation{
                index,
                AbiParameterStorage::Stack,
                0,
                static_cast<std::int64_t>(16 + (index - 6) * 8),
            });
        }
        return info;
    };
    Harness harness(nullptr, std::move(architecture));

    const auto push = [&](ida::Address ea, std::uint64_t value) {
        auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
            memory_operation(
                ea,
                0,
                "STORE",
                std::nullopt,
                {varnode("const", 0, 8),
                 varnode("register", 0x100, 8),
                 varnode("const", value, 8)}),
        }));
        ASSERT_TRUE(lowered.has_value());
    };
    push(0x2000, 29);
    push(0x2002, 23);
    push(0x2004, 19);
    push(0x2006, 17);

    auto call = harness.lowerer.lower_instruction(instruction(0x2008, {
        memory_operation(
            0x2008,
            0,
            "STORE",
            std::nullopt,
            {varnode("const", 0, 8),
             varnode("register", 0x100, 8),
             varnode("const", 0x200c, 8)}),
        operation(
            0x2008, 1, "CALL", std::nullopt,
            {varnode("code", 0x4000, 8)}),
    }));
    ASSERT_TRUE(call.has_value());
    ASSERT_EQ(call->size(), 2U);
    auto* call_expression = dyn_cast<Call>(assignment_at(*call, 1)->value());
    ASSERT_TRUE(call_expression != nullptr);
    ASSERT_EQ(call_expression->arg_count(), 10U);
    for (std::size_t index = 0; index < 4; ++index) {
        auto* argument = dyn_cast<Constant>(call_expression->arg(index + 6));
        ASSERT_TRUE(argument != nullptr);
        constexpr std::uint64_t expected[] = {17, 19, 23, 29};
        ASSERT_EQ(argument->value(), expected[index]);
    }
}

void test_x86_float_abi_memory_and_variadic_evidence() {
    PcodeArchitectureContext architecture;
    architecture.arch_name = "x86_64-test";
    architecture.gp_argument_registers = {
        "rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    architecture.integer_return_registers = {"rax", "rdx"};
    architecture.stack_pointer_register = "rsp";
    architecture.frame_pointer_register = "rbp";
    architecture.register_mapper = [](std::uint64_t offset,
                                      std::size_t size)
        -> std::optional<PcodeRegisterView> {
        if (offset == 0 && size == 8) {
            return PcodeRegisterView{"rdi", 8, false, false, false};
        }
        if (offset == 0x200 && (size == 4 || size == 8)) {
            return PcodeRegisterView{"xmm0", size, false, false, false};
        }
        if (offset == 0x208 && (size == 4 || size == 8)) {
            return PcodeRegisterView{"xmm1", size, false, false, false};
        }
        return std::nullopt;
    };
    architecture.symbol_resolver = [](ida::Address address)
        -> std::optional<std::string> {
        if (address == 0x5000) return "float_pair";
        if (address == 0x6000) return "printf";
        return std::nullopt;
    };
    architecture.function_resolver = [](ida::Address address)
        -> std::optional<PcodeFunctionInfo> {
        PcodeFunctionInfo info;
        if (address == 0x5000) {
            info.name = "float_pair";
            info.parameter_types = {Float::float64(), Float::float64()};
            info.return_type = Float::float64();
            info.prototype_known = true;
            return info;
        }
        if (address == 0x6000) {
            info.name = "printf";
            info.parameter_types = {
                std::make_shared<Pointer>(Integer::int8_t(), 64)};
            info.return_type = Integer::int32_t();
            info.prototype_known = true;
            info.variadic = true;
            return info;
        }
        return std::nullopt;
    };
    Harness harness(nullptr, architecture);

    auto first_write = harness.lowerer.lower_instruction(instruction(0x2100, {
        operation(0x2100, 0, "COPY", varnode("register", 0x200, 8),
                  {varnode("const", 0x3ff0000000000000ULL, 8)}),
    }));
    auto second_write = harness.lowerer.lower_instruction(instruction(0x2104, {
        operation(0x2104, 0, "COPY", varnode("register", 0x208, 8),
                  {varnode("const", 0x4000000000000000ULL, 8)}),
    }));
    ASSERT_TRUE(first_write.has_value() && second_write.has_value());

    auto pair_call = harness.lowerer.lower_instruction(instruction(0x2108, {
        operation(0x2108, 0, "CALL", std::nullopt,
                  {varnode("code", 0x5000, 8)}),
    }));
    ASSERT_TRUE(pair_call.has_value());
    auto* pair_assignment = assignment_at(*pair_call, 0);
    auto* pair_result = dyn_cast<Variable>(pair_assignment->destination());
    auto* pair_expression = dyn_cast<Call>(pair_assignment->value());
    ASSERT_TRUE(pair_result != nullptr && pair_expression != nullptr);
    ASSERT_EQ(pair_result->name(), std::string("xmm0"));
    ASSERT_TRUE(pair_result->ir_type() != nullptr);
    ASSERT_EQ(pair_result->ir_type()->type_kind(), TypeKind::Float);
    ASSERT_EQ(pair_expression->arg_count(), 2U);
    ASSERT_EQ(dyn_cast<Variable>(pair_expression->arg(0))->name(),
              std::string("xmm0"));
    ASSERT_EQ(dyn_cast<Variable>(pair_expression->arg(1))->name(),
              std::string("xmm1"));

    auto unknown_return = harness.lowerer.lower_instruction(instruction(0x210c, {
        operation(0x210c, 0, "RETURN", std::nullopt,
                  {varnode("register", 0x100, 8)}),
    }));
    ASSERT_TRUE(unknown_return.has_value());
    auto* returned = dyn_cast<Return>((*unknown_return)[0]);
    ASSERT_TRUE(returned != nullptr && returned->values().size() == 1);
    auto* returned_xmm = dyn_cast<Variable>(returned->values()[0]);
    ASSERT_TRUE(returned_xmm != nullptr);
    ASSERT_EQ(returned_xmm->name(), std::string("xmm0"));
    ASSERT_EQ(returned_xmm->ir_type()->type_kind(), TypeKind::Float);

    auto format_write = harness.lowerer.lower_instruction(instruction(0x2110, {
        operation(0x2110, 0, "COPY", varnode("register", 0, 8),
                  {varnode("const", 0x7000, 8)}),
    }));
    ASSERT_TRUE(format_write.has_value());
    auto printf_call = harness.lowerer.lower_instruction(instruction(0x2114, {
        operation(0x2114, 0, "CALL", std::nullopt,
                  {varnode("code", 0x6000, 8)}),
    }));
    ASSERT_TRUE(printf_call.has_value());
    auto* printf_expression = dyn_cast<Call>(assignment_at(*printf_call, 0)->value());
    ASSERT_TRUE(printf_expression != nullptr);
    ASSERT_EQ(printf_expression->arg_count(), 2U);
    auto* printf_float = dyn_cast<Variable>(printf_expression->arg(1));
    ASSERT_TRUE(printf_float != nullptr);
    ASSERT_EQ(printf_float->name(), std::string("xmm0"));
    ASSERT_EQ(printf_float->ir_type()->type_kind(), TypeKind::Float);

    Harness memory_harness(nullptr, architecture);
    auto memory_float = memory_harness.lowerer.lower_instruction(instruction(0x2120, {
        operation(0x2120, 0, "FLOAT_MULT", varnode("register", 0x200, 8),
                  {varnode("register", 0x208, 8), varnode("ram", 0x8000, 8)}),
    }));
    ASSERT_TRUE(memory_float.has_value());
    auto* product = dyn_cast<Operation>(assignment_at(*memory_float, 0)->value());
    ASSERT_TRUE(product != nullptr && product->type() == OperationType::mul_float);
    auto* loaded = dyn_cast<Operation>(product->operands()[1]);
    ASSERT_TRUE(loaded != nullptr && loaded->type() == OperationType::deref);
    ASSERT_EQ(loaded->ir_type()->type_kind(), TypeKind::Float);

    Harness zero_harness(nullptr, architecture);
    auto zeroed = zero_harness.lowerer.lower_instruction(instruction(0x2124, {
        operation(0x2124, 0, "INT_XOR", varnode("register", 0x208, 4),
                  {varnode("register", 0x208, 4),
                   varnode("register", 0x208, 4)}),
    }));
    ASSERT_TRUE(zeroed.has_value());
    ASSERT_TRUE(!zero_harness.task.parameter_registers().contains("xmm1"));
}

void test_structural_recursive_signature_refinement() {
    PcodeSignatureContext signature;
    signature.parameter_register_map["x0"] = 0;
    signature.parameter_register_map["w0"] = 0;
    Harness harness(nullptr, test_architecture(), std::move(signature));
    ControlFlowGraph cfg;
    auto* block = harness.task.arena().create<BasicBlock>(0);
    cfg.set_entry_block(block);
    cfg.add_block(block);

    const std::vector<RawPcodeInstruction> raw = {
        instruction(0x1000, {
            operation(0x1000, 0, "INT_SUB", varnode("register", 0, 8),
                      {varnode("register", 0, 8), varnode("const", 1, 8)}),
        }),
        instruction(0x1004, {
            operation(0x1004, 0, "CALL", std::nullopt,
                      {varnode("code", 0x1000, 8)}),
        }),
        instruction(0x1008, {
            operation(0x1008, 0, "RETURN", std::nullopt,
                      {varnode("register", 0x1e0, 8)}),
        }),
    };
    for (const RawPcodeInstruction& machine_instruction : raw) {
        auto lowered = harness.lowerer.lower_instruction(machine_instruction);
        ASSERT_TRUE(lowered.has_value());
        for (Instruction* lowered_instruction : *lowered) {
            block->add_instruction(lowered_instruction);
        }
    }
    ASSERT_EQ(harness.task.frontend_support_report().fallback_ops, 2U);

    const PcodeSignatureRefinement refinement =
        refine_pcode_function_signature(harness.task, cfg);
    ASSERT_TRUE(refinement.signature_inferred);
    ASSERT_EQ(refinement.resolved_self_calls, 1U);
    auto* function = type_dyn_cast<FunctionTypeDef>(harness.task.function_type().get());
    ASSERT_TRUE(function != nullptr);
    ASSERT_EQ(function->parameters().size(), 1U);
    ASSERT_EQ(function->parameters()[0]->to_string(), std::string("unsigned long"));
    ASSERT_EQ(function->return_type()->to_string(), std::string("unsigned long"));
    auto* recursive_assignment = dyn_cast<Assignment>(block->instructions()[1]);
    auto* recursive_call = recursive_assignment
        ? dyn_cast<Call>(recursive_assignment->value()) : nullptr;
    ASSERT_TRUE(recursive_call != nullptr && recursive_call->ir_type() != nullptr);
    ASSERT_TRUE(*recursive_call->ir_type() == *harness.task.function_type());
    ASSERT_EQ(harness.task.frontend_support_report().fallback_ops, 1U);
    ASSERT_EQ(harness.task.frontend_support_report().implemented_ops, 2U);
    ASSERT_TRUE(std::none_of(
        harness.task.frontend_diagnostics().begin(),
        harness.task.frontend_diagnostics().end(),
        [](const FrontendDiagnostic& diagnostic) {
            return diagnostic.message.starts_with(
                "CALL: callee prototype unavailable");
        }));
}

void test_recursive_signature_refinement_return_classes_and_fail_closed_arity() {
    {
        DecompilerTask task(0x2000);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto* target = task.arena().create<GlobalVariable>(
            "self_float", 8, task.arena().create<Constant>(0x2000, 8), true);
        auto* call = task.arena().create<Call>(target, std::vector<Expression*>{}, 8);
        auto* destination = task.arena().create<Variable>("d0", 8);
        destination->set_ir_type(Float::float64());
        auto* assignment = task.arena().create<Assignment>(destination, call);
        assignment->set_address(0x2010);
        block->add_instruction(assignment);
        auto* returned_value = task.arena().create<Variable>("d0", 8);
        returned_value->set_ir_type(Float::float64());
        block->add_instruction(task.arena().create<Return>(
            std::vector<Expression*>{returned_value}));

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(refinement.signature_inferred);
        ASSERT_EQ(refinement.resolved_self_calls, 1U);
        auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
        ASSERT_TRUE(function != nullptr && function->parameters().empty());
        ASSERT_EQ(function->return_type()->to_string(), std::string("double"));
        ASSERT_TRUE(call->ir_type() != nullptr);
        ASSERT_EQ(call->ir_type()->to_string(), task.function_type()->to_string());
    }
    {
        DecompilerTask task(0x3000);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto* target = task.arena().create<GlobalVariable>(
            "self_void", 8, task.arena().create<Constant>(0x3000, 8), true);
        auto* call = task.arena().create<Call>(target, std::vector<Expression*>{}, 1);
        auto* assignment = task.arena().create<Assignment>(nullptr, call);
        assignment->set_address(0x3010);
        block->add_instruction(assignment);
        block->add_instruction(task.arena().create<Return>());

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(refinement.signature_inferred);
        auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
        ASSERT_TRUE(function != nullptr);
        ASSERT_EQ(function->return_type()->to_string(), std::string("void"));
    }
    {
        DecompilerTask task(0x4000);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto* parameter = task.arena().create<Variable>("x0", 8);
        parameter->set_kind(VariableKind::Parameter);
        parameter->set_parameter_index(0);
        parameter->set_ir_type(Integer::uint64_t());
        auto* temporary = task.arena().create<Variable>("tmp", 8);
        block->add_instruction(task.arena().create<Assignment>(temporary, parameter));
        auto* target = task.arena().create<GlobalVariable>(
            "self_mismatch", 8, task.arena().create<Constant>(0x4000, 8), true);
        auto* call = task.arena().create<Call>(target, std::vector<Expression*>{}, 8);
        auto* destination = task.arena().create<Variable>("x0", 8);
        auto* assignment = task.arena().create<Assignment>(destination, call);
        assignment->set_address(0x4010);
        block->add_instruction(assignment);
        auto* returned_value = task.arena().create<Variable>("x0", 8);
        returned_value->set_ir_type(Integer::uint64_t());
        block->add_instruction(task.arena().create<Return>(
            std::vector<Expression*>{returned_value}));
        task.mutable_frontend_support_report().fallback_ops = 1;
        task.add_frontend_diagnostic(FrontendDiagnostic{
            FrontendDiagnosticSeverity::Warning,
            "pcode-fallback",
            "CALL: callee prototype unavailable for self_mismatch",
            0x4010,
            0,
        });

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(!refinement.signature_inferred);
        ASSERT_EQ(refinement.resolved_self_calls, 0U);
        ASSERT_TRUE(task.function_type() == nullptr);
        ASSERT_EQ(task.frontend_support_report().fallback_ops, 1U);
        ASSERT_EQ(task.frontend_diagnostics().size(), 1U);
    }
    {
        DecompilerTask task(0x5000);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto add_parameter_use = [&](const std::string& temporary_name, TypePtr type) {
            auto* parameter = task.arena().create<Variable>("x0", 8);
            parameter->set_kind(VariableKind::Parameter);
            parameter->set_parameter_index(0);
            parameter->set_ir_type(std::move(type));
            block->add_instruction(task.arena().create<Assignment>(
                task.arena().create<Variable>(temporary_name, 8), parameter));
            return parameter;
        };
        Variable* integer_argument = add_parameter_use("integer_view", Integer::uint64_t());
        add_parameter_use("floating_view", Float::float64());
        auto* target = task.arena().create<GlobalVariable>(
            "self_type_conflict", 8,
            task.arena().create<Constant>(0x5000, 8), true);
        auto* call = task.arena().create<Call>(
            target, std::vector<Expression*>{integer_argument->copy(task.arena())}, 8);
        auto* destination = task.arena().create<Variable>("x0", 8);
        block->add_instruction(task.arena().create<Assignment>(destination, call));
        auto* returned_value = task.arena().create<Variable>("x0", 8);
        returned_value->set_ir_type(Integer::uint64_t());
        block->add_instruction(task.arena().create<Return>(
            std::vector<Expression*>{returned_value}));

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(!refinement.signature_inferred);
        ASSERT_EQ(refinement.resolved_self_calls, 0U);
        ASSERT_TRUE(task.function_type() == nullptr);
        ASSERT_TRUE(call->ir_type() == nullptr);
    }
}

void test_incoming_stack_parameters_require_undefined_contiguous_slots() {
    DecompilerTask task(0x6000);
    ControlFlowGraph cfg;
    auto* block = task.arena().create<BasicBlock>(0);
    cfg.set_entry_block(block);
    cfg.add_block(block);

    for (int index = 0; index < 8; ++index) {
        auto* parameter = task.arena().create<Variable>(
            "x" + std::to_string(index), 8);
        parameter->set_kind(VariableKind::Parameter);
        parameter->set_parameter_index(index);
        parameter->set_ir_type(Integer::uint64_t());
        block->add_instruction(task.arena().create<Assignment>(
            task.arena().create<Variable>("register_use_" + std::to_string(index), 8),
            parameter));
    }

    auto* spill = task.arena().create<Variable>("arg_72", 8);
    spill->set_kind(VariableKind::StackArgument);
    spill->set_stack_offset(72);
    spill->set_ir_type(Integer::uint64_t());
    block->add_instruction(task.arena().create<Assignment>(
        spill, task.arena().create<Constant>(99, 8)));
    block->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("spill_use", 8), spill->copy(task.arena())));

    auto* wide_spill = task.arena().create<Variable>("arg_56", 16);
    wide_spill->set_kind(VariableKind::StackArgument);
    wide_spill->set_stack_offset(56);
    wide_spill->set_ir_type(Integer::uint128_t());
    block->add_instruction(task.arena().create<Assignment>(
        wide_spill, task.arena().create<Constant>(0, 16)));
    auto* covered_spill_view = task.arena().create<Variable>("arg_64", 8);
    covered_spill_view->set_kind(VariableKind::StackArgument);
    covered_spill_view->set_stack_offset(64);
    covered_spill_view->set_ir_type(Integer::uint64_t());
    block->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("covered_spill_use", 8),
        covered_spill_view));

    auto* stack_ninth = task.arena().create<Variable>("arg_80", 8);
    stack_ninth->set_kind(VariableKind::StackArgument);
    stack_ninth->set_stack_offset(80);
    stack_ninth->set_ir_type(Integer::uint64_t());
    auto* stack_tenth = task.arena().create<Variable>("arg_88", 8);
    stack_tenth->set_kind(VariableKind::StackArgument);
    stack_tenth->set_stack_offset(88);
    stack_tenth->set_ir_type(Integer::uint64_t());
    block->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("ninth_use", 8), stack_ninth));
    block->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("tenth_use", 8), stack_tenth));
    auto* returned_value = task.arena().create<Variable>("x0", 8);
    returned_value->set_ir_type(Integer::uint64_t());
    block->add_instruction(task.arena().create<Return>(
        std::vector<Expression*>{returned_value}));

    const auto refinement = refine_pcode_function_signature(task, cfg);
    ASSERT_TRUE(refinement.signature_inferred);
    auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
    ASSERT_TRUE(function != nullptr);
    ASSERT_EQ(function->parameters().size(), 10U);
    ASSERT_TRUE(stack_ninth->is_parameter());
    ASSERT_TRUE(stack_tenth->is_parameter());
    ASSERT_EQ(stack_ninth->parameter_index(), 8);
    ASSERT_EQ(stack_tenth->parameter_index(), 9);
    ASSERT_TRUE(!spill->is_parameter());
    ASSERT_TRUE(!covered_spill_view->is_parameter());
    ASSERT_TRUE(task.parameter_registers().contains("arg_80"));
    ASSERT_TRUE(task.parameter_registers().contains("arg_88"));
}

void test_atomic_hfa_stack_parameter_locations_and_call_recovery() {
    std::vector<AbiParameterLocation> inferred_locations;
    std::vector<TypePtr> inferred_parameters;
    {
        DecompilerTask task(0x9900);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);

        for (int index = 0; index < 6; ++index) {
            auto* parameter = task.arena().create<Variable>(
                "d" + std::to_string(index), 8);
            parameter->set_kind(VariableKind::Parameter);
            parameter->set_parameter_index(index);
            parameter->set_ir_type(Float::float64());
            block->add_instruction(task.arena().create<Assignment>(
                task.arena().create<Variable>(
                    "fp_use_" + std::to_string(index), 8),
                parameter));
        }
        for (int index = 0; index < 4; ++index) {
            const std::int64_t offset = 80 + index * 8;
            auto* parameter = task.arena().create<Variable>(
                "arg_" + std::to_string(offset), 8);
            parameter->set_kind(VariableKind::StackArgument);
            parameter->set_stack_offset(offset);
            parameter->set_ir_type(Integer::uint64_t());
            block->add_instruction(task.arena().create<Assignment>(
                task.arena().create<Variable>(
                    "stack_use_" + std::to_string(index), 8),
                parameter));
        }
        auto* returned = task.arena().create<Variable>("d0", 8);
        returned->set_ir_type(Float::float64());
        block->add_instruction(task.arena().create<Return>(
            std::vector<Expression*>{returned}));

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(refinement.signature_inferred);
        const auto* function = type_dyn_cast<FunctionTypeDef>(
            task.function_type().get());
        ASSERT_TRUE(function != nullptr);
        ASSERT_EQ(function->parameters().size(), 10U);
        ASSERT_EQ(function->abi_parameter_locations().size(), 10U);
        for (const TypePtr& parameter : function->parameters()) {
            ASSERT_TRUE(parameter != nullptr
                && parameter->type_kind() == TypeKind::Float
                && parameter->size_bytes() == 8);
        }
        for (std::size_t index = 0; index < 6; ++index) {
            const AbiParameterLocation& location =
                function->abi_parameter_locations()[index];
            ASSERT_EQ(location.parameter_index, index);
            ASSERT_EQ(location.storage, AbiParameterStorage::FloatingRegister);
            ASSERT_EQ(location.register_index, index);
        }
        for (std::size_t index = 6; index < 10; ++index) {
            const AbiParameterLocation& location =
                function->abi_parameter_locations()[index];
            ASSERT_EQ(location.parameter_index, index);
            ASSERT_EQ(location.storage, AbiParameterStorage::Stack);
            ASSERT_EQ(location.stack_offset,
                static_cast<std::int64_t>(80 + (index - 6) * 8));
        }
        inferred_parameters.assign(
            function->parameters().begin(), function->parameters().end());
        inferred_locations.assign(
            function->abi_parameter_locations().begin(),
            function->abi_parameter_locations().end());
    }

    auto architecture = test_architecture();
    architecture.function_resolver = [inferred_parameters, inferred_locations](
        ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x9900) return std::nullopt;
        PcodeFunctionInfo info;
        info.name = "atomic_hfa_stack";
        info.parameter_types = inferred_parameters;
        info.return_type = Float::float64();
        info.prototype_known = true;
        info.abi_parameter_locations = inferred_locations;
        return info;
    };
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>(
            "out_" + std::to_string(offset), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        slot->set_ir_type(Integer::uint64_t());
        return slot;
    };
    Harness harness(nullptr, std::move(architecture));
    std::vector<RawPcodeOp> stores;
    std::uint32_t ordinal = 0;
    for (std::int64_t offset : {24, 16, 8}) {
        const std::uint64_t unique = 0x900 + static_cast<std::uint64_t>(offset);
        stores.push_back(operation(0x1990, ordinal++, "INT_ADD",
            varnode("unique", unique, 8),
            {varnode("register", 0x100, 8),
             varnode("const", static_cast<std::uint64_t>(offset), 8)}));
        stores.push_back(memory_operation(0x1990, ordinal++, "STORE", std::nullopt,
            {varnode("const", 0, 8), varnode("unique", unique, 8),
             varnode("const", 0x3ff0000000000000ULL
                 + static_cast<std::uint64_t>(offset), 8)}));
    }
    stores.push_back(memory_operation(0x1990, ordinal++, "STORE", std::nullopt,
        {varnode("const", 0, 8), varnode("register", 0x100, 8),
         varnode("const", 0x3ff0000000000000ULL, 8)}));
    ASSERT_TRUE(harness.lowerer.lower_instruction(
        instruction(0x1990, std::move(stores))).has_value());

    auto lowered = harness.lowerer.lower_instruction(instruction(0x1994, {
        operation(0x1994, 0, "CALL", std::nullopt,
                  {varnode("code", 0x9900, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    auto* call = dyn_cast<Call>(assignment_at(*lowered, 0)->value());
    ASSERT_TRUE(call != nullptr);
    ASSERT_EQ(call->arg_count(), 10U);
    for (std::size_t index = 0; index < 6; ++index) {
        auto* argument = dyn_cast<Variable>(call->arg(index));
        ASSERT_TRUE(argument != nullptr);
        ASSERT_EQ(argument->name(), "d" + std::to_string(index));
    }
    for (std::size_t index = 6; index < 10; ++index) {
        auto* argument = dyn_cast<Operation>(call->arg(index));
        ASSERT_TRUE(argument != nullptr
            && argument->type() == OperationType::bitcast
            && argument->ir_type() != nullptr
            && argument->ir_type()->type_kind() == TypeKind::Float);
        auto* storage = dyn_cast<Variable>(argument->operands()[0]);
        ASSERT_TRUE(storage != nullptr);
        ASSERT_EQ(storage->name(),
            "out_" + std::to_string((index - 6) * 8));
    }

    auto missing_architecture = test_architecture();
    missing_architecture.function_resolver = [inferred_parameters, inferred_locations](
        ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x9900) return std::nullopt;
        PcodeFunctionInfo info;
        info.name = "atomic_hfa_stack";
        info.parameter_types = inferred_parameters;
        info.return_type = Float::float64();
        info.prototype_known = true;
        info.abi_parameter_locations = inferred_locations;
        return info;
    };
    Harness missing(nullptr, std::move(missing_architecture));
    auto rejected = missing.lowerer.lower_instruction(instruction(0x1998, {
        operation(0x1998, 0, "CALL", std::nullopt,
                  {varnode("code", 0x9900, 8)}),
    }));
    ASSERT_TRUE(!rejected.has_value());
    ASSERT_TRUE(!missing.task.frontend_diagnostics().empty());
    ASSERT_TRUE(missing.task.frontend_diagnostics().back().message.find(
        "explicit ABI parameter location has no reaching value")
        != std::string::npos);
}

void test_packed_hfa_stack_lane_type_survives_gp_carrier() {
    std::vector<TypePtr> parameters(6, Float::float64());
    parameters.insert(parameters.end(), 4, Float::float32());
    parameters.push_back(Float::float64());
    std::vector<AbiParameterLocation> locations;
    for (std::size_t index = 0; index < 6; ++index) {
        locations.push_back(AbiParameterLocation{
            index, AbiParameterStorage::FloatingRegister, index, 0});
    }
    for (std::size_t index = 6; index < 10; ++index) {
        locations.push_back(AbiParameterLocation{
            index,
            AbiParameterStorage::Stack,
            0,
            static_cast<std::int64_t>(80 + (index - 6) * 4),
        });
    }
    locations.push_back(AbiParameterLocation{
        10, AbiParameterStorage::Stack, 0, 96});
    TypePtr function_type = std::make_shared<const FunctionTypeDef>(
        Float::float64(), parameters, false, 0, 0, 0, locations);

    auto architecture = test_architecture();
    const auto base_mapper = architecture.register_mapper;
    architecture.register_mapper = [base_mapper](
        std::uint64_t offset,
        std::size_t size) -> std::optional<PcodeRegisterView> {
        if (offset == 0x400 && (size == 4 || size == 8)) {
            return PcodeRegisterView{"x12", 8, size == 4, size == 4, false};
        }
        if (offset == 0x500 && size == 4) {
            return PcodeRegisterView{"s4", 4, false, false, false};
        }
        return base_mapper(offset, size);
    };
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>(
            (offset >= 80 ? "arg_" : "local_") + std::to_string(offset), width);
        slot->set_kind(offset >= 80
            ? VariableKind::StackArgument : VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        slot->set_ir_type(width == 4 ? Integer::uint32_t() : Integer::uint64_t());
        return slot;
    };

    Harness harness(function_type, std::move(architecture));
    auto incoming = harness.lowerer.lower_instruction(instruction(0x19a0, {
        operation(0x19a0, 0, "INT_ADD", varnode("unique", 0xa00, 8),
            {varnode("register", 0x100, 8), varnode("const", 80, 8)}),
        memory_operation(0x19a0, 1, "LOAD", varnode("unique", 0xa08, 4),
            {varnode("const", 0, 8), varnode("unique", 0xa00, 8)}),
        operation(0x19a0, 2, "COPY", varnode("register", 0x400, 4),
            {varnode("unique", 0xa08, 4)}),
    }));
    ASSERT_TRUE(incoming.has_value());
    auto* carrier = dyn_cast<Variable>(assignment_at(*incoming, 2)->destination());
    ASSERT_TRUE(carrier != nullptr);
    ASSERT_TRUE(carrier->ir_type() != nullptr
        && carrier->ir_type()->type_kind() == TypeKind::Float
        && carrier->size_bytes == 4);

    auto stored = harness.lowerer.lower_instruction(instruction(0x19a4, {
        operation(0x19a4, 0, "INT_ADD", varnode("unique", 0xa10, 8),
            {varnode("register", 0x100, 8), varnode("const", 64, 8)}),
        memory_operation(0x19a4, 1, "STORE", std::nullopt,
            {varnode("const", 0, 8), varnode("unique", 0xa10, 8),
             varnode("register", 0x400, 4)}),
    }));
    ASSERT_TRUE(stored.has_value());

    const PcodeLowerer::CallArgumentEvidence floating_predecessor =
        harness.lowerer.call_argument_evidence();
    ASSERT_TRUE(!floating_predecessor.stack_types.empty());
    harness.lowerer.begin_basic_block({floating_predecessor});

    auto converted = harness.lowerer.lower_instruction(instruction(0x19a8, {
        operation(0x19a8, 0, "INT_ADD", varnode("unique", 0xa18, 8),
            {varnode("register", 0x100, 8), varnode("const", 64, 8)}),
        memory_operation(0x19a8, 1, "LOAD", varnode("unique", 0xa20, 4),
            {varnode("const", 0, 8), varnode("unique", 0xa18, 8)}),
        operation(0x19a8, 2, "COPY", varnode("register", 0x500, 4),
            {varnode("unique", 0xa20, 4)}),
        operation(0x19a8, 3, "FLOAT2FLOAT", varnode("unique", 0xa28, 8),
            {varnode("register", 0x500, 4)}),
    }));
    ASSERT_TRUE(converted.has_value());
    auto* conversion = dyn_cast<Operation>(assignment_at(*converted, 3)->value());
    ASSERT_TRUE(conversion != nullptr && conversion->type() == OperationType::cast);
    ASSERT_TRUE(conversion->operands()[0]->ir_type() != nullptr
        && conversion->operands()[0]->ir_type()->type_kind() == TypeKind::Float);

    auto conflicting_predecessor = floating_predecessor;
    conflicting_predecessor.stack_types.front().type = Integer::uint32_t();
    harness.lowerer.begin_basic_block(
        {floating_predecessor, conflicting_predecessor});
    auto ambiguous = harness.lowerer.lower_instruction(instruction(0x19ac, {
        operation(0x19ac, 0, "INT_ADD", varnode("unique", 0xa30, 8),
            {varnode("register", 0x100, 8), varnode("const", 64, 8)}),
        memory_operation(0x19ac, 1, "LOAD", varnode("unique", 0xa38, 4),
            {varnode("const", 0, 8), varnode("unique", 0xa30, 8)}),
    }));
    ASSERT_TRUE(ambiguous.has_value());
    Expression* ambiguous_value = assignment_at(*ambiguous, 1)->value();
    ASSERT_TRUE(ambiguous_value != nullptr && ambiguous_value->ir_type() != nullptr);
    ASSERT_EQ(ambiguous_value->ir_type()->type_kind(), TypeKind::Integer);
}

void test_two_register_integer_return_inference_and_call_split() {
    {
        DecompilerTask task(0x7000);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto* x0 = task.arena().create<Variable>("x0", 8);
        x0->set_ir_type(Integer::uint64_t());
        auto* low_definition = task.arena().create<Assignment>(
            x0, task.arena().create<Constant>(8, 8));
        low_definition->set_address(0x7010);
        block->add_instruction(low_definition);
        auto* x1 = task.arena().create<Variable>("x1", 8);
        x1->set_ir_type(Integer::uint64_t());
        auto* high_definition = task.arena().create<Assignment>(
            x1, task.arena().create<Constant>(21, 8));
        high_definition->set_address(0x7014);
        block->add_instruction(high_definition);
        auto* returned_x0 = task.arena().create<Variable>("x0", 8);
        returned_x0->set_ir_type(Integer::uint64_t());
        auto* returned = task.arena().create<Return>(
            std::vector<Expression*>{returned_x0});
        returned->set_address(0x7018);
        block->add_instruction(returned);

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(refinement.signature_inferred);
        auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
        ASSERT_TRUE(function != nullptr);
        ASSERT_EQ(function->return_type()->to_string(), std::string("uint128_t"));
        ASSERT_EQ(returned->values().size(), 1U);
        ASSERT_EQ(returned->values()[0]->size_bytes, 16U);
        ASSERT_TRUE(returned->values()[0]->ir_type() != nullptr);
        ASSERT_EQ(returned->values()[0]->ir_type()->to_string(),
                  std::string("uint128_t"));
    }
    {
        auto architecture = test_architecture();
        architecture.function_resolver = [](ida::Address address)
            -> std::optional<PcodeFunctionInfo> {
            if (address != 0x7000) return std::nullopt;
            PcodeFunctionInfo info;
            info.name = "wide_result";
            info.parameter_types = {Integer::uint64_t()};
            info.return_type = Integer::uint128_t();
            info.prototype_known = true;
            return info;
        };
        Harness harness(nullptr, std::move(architecture));
        ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1700, {
            operation(0x1700, 0, "COPY", varnode("register", 0, 8),
                      {varnode("const", 7, 8)}),
        })).has_value());
        auto lowered = harness.lowerer.lower_instruction(instruction(0x1704, {
            operation(0x1704, 0, "CALL", std::nullopt,
                      {varnode("code", 0x7000, 8)}),
        }));
        ASSERT_TRUE(lowered.has_value());
        ASSERT_EQ(lowered->size(), 3U);
        auto* aggregate_assignment = assignment_at(*lowered, 0);
        auto* aggregate = dyn_cast<Variable>(aggregate_assignment->destination());
        ASSERT_TRUE(aggregate != nullptr);
        ASSERT_EQ(aggregate->size_bytes, 16U);
        ASSERT_TRUE(aggregate->ir_type() != nullptr);
        ASSERT_EQ(aggregate->ir_type()->to_string(), std::string("uint128_t"));
        ASSERT_TRUE(dyn_cast<Call>(aggregate_assignment->value()) != nullptr);
        auto* low = dyn_cast<Variable>(assignment_at(*lowered, 1)->destination());
        auto* high = dyn_cast<Variable>(assignment_at(*lowered, 2)->destination());
        ASSERT_TRUE(low != nullptr && high != nullptr);
        ASSERT_EQ(low->name(), std::string("x0"));
        ASSERT_EQ(high->name(), std::string("x1"));
    }
    {
        // A nearby x1 scratch definition is required. An older definition
        // must not widen an otherwise scalar x0 return.
        DecompilerTask task(0x7100);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto* old_x1 = task.arena().create<Variable>("x1", 8);
        old_x1->set_ir_type(Integer::uint64_t());
        auto* old_definition = task.arena().create<Assignment>(
            old_x1, task.arena().create<Constant>(21, 8));
        old_definition->set_address(0x7110);
        block->add_instruction(old_definition);
        auto* x0 = task.arena().create<Variable>("x0", 8);
        x0->set_ir_type(Integer::uint64_t());
        auto* low_definition = task.arena().create<Assignment>(
            x0, task.arena().create<Constant>(8, 8));
        low_definition->set_address(0x7130);
        block->add_instruction(low_definition);
        auto* returned_x0 = task.arena().create<Variable>("x0", 8);
        returned_x0->set_ir_type(Integer::uint64_t());
        auto* returned = task.arena().create<Return>(
            std::vector<Expression*>{returned_x0});
        returned->set_address(0x7134);
        block->add_instruction(returned);

        const auto refinement = refine_pcode_function_signature(task, cfg);
        ASSERT_TRUE(refinement.signature_inferred);
        auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
        ASSERT_TRUE(function != nullptr);
        ASSERT_EQ(function->return_type()->type_kind(), TypeKind::Integer);
        ASSERT_EQ(function->return_type()->size_bytes(), 8U);
        ASSERT_EQ(returned->values()[0]->size_bytes, 8U);
    }
}

void test_call_clobber_preserves_callee_saved_stack_provenance() {
    auto architecture = test_architecture();
    const auto base_mapper = architecture.register_mapper;
    architecture.register_mapper = [base_mapper](
        std::uint64_t offset,
        std::size_t size) -> std::optional<PcodeRegisterView> {
        if (offset == 0xe8 && size == 8) {
            return PcodeRegisterView{"x29", 8, false, false, false};
        }
        return base_mapper(offset, size);
    };
    architecture.function_resolver = [](ida::Address address)
        -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4300) return std::nullopt;
        PcodeFunctionInfo info;
        info.name = "known_void";
        info.return_type = CustomType::void_type();
        info.prototype_known = true;
        return info;
    };
    std::vector<std::int64_t> resolved_offsets;
    architecture.stack_variable_resolver = [&resolved_offsets](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        resolved_offsets.push_back(offset);
        auto* slot = arena.create<Variable>(
            "slot_" + std::to_string(offset), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        return slot;
    };

    Harness harness(nullptr, std::move(architecture));
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1720, {
        operation(0x1720, 0, "INT_ADD", varnode("register", 0, 8),
                  {varnode("register", 0x100, 8), varnode("const", 0x20, 8)}),
        operation(0x1720, 1, "INT_ADD", varnode("register", 0xe8, 8),
                  {varnode("register", 0x100, 8), varnode("const", 0x28, 8)}),
    })).has_value());
    resolved_offsets.clear();
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1724, {
        operation(0x1724, 0, "CALL", std::nullopt,
                  {varnode("code", 0x4300, 8)}),
    })).has_value());

    auto clobbered_load = harness.lowerer.lower_instruction(instruction(0x1728, {
        memory_operation(0x1728, 0, "LOAD", varnode("unique", 0x500, 8),
                         {varnode("const", 0, 8), varnode("register", 0, 8)}),
    }));
    ASSERT_TRUE(clobbered_load.has_value());
    ASSERT_TRUE(resolved_offsets.empty());
    ASSERT_TRUE(isa<Operation>(assignment_at(*clobbered_load, 0)->value()));

    auto preserved_load = harness.lowerer.lower_instruction(instruction(0x172c, {
        memory_operation(0x172c, 0, "LOAD", varnode("unique", 0x508, 8),
                         {varnode("const", 0, 8), varnode("register", 0xe8, 8)}),
    }));
        ASSERT_TRUE(preserved_load.has_value());
        ASSERT_EQ(resolved_offsets.size(), 1U);
        ASSERT_EQ(resolved_offsets[0], 0);
    auto* preserved_value = dyn_cast<Variable>(
        assignment_at(*preserved_load, 0)->value());
    ASSERT_TRUE(preserved_value != nullptr);
    ASSERT_EQ(preserved_value->name(), std::string("slot_0"));
}

void test_indirect_result_refinement_and_call_region() {
    {
        DecompilerTask task(0x7200);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);

        auto* result = task.arena().create<Variable>("x8", 8);
        result->set_kind(VariableKind::Parameter);
        result->set_parameter_index(0);
        result->set_ir_type(std::make_shared<Pointer>(Integer::uint8_t(), 64));
        auto* value = task.arena().create<Variable>("x0", 8);
        value->set_kind(VariableKind::Parameter);
        value->set_parameter_index(1);
        value->set_ir_type(Integer::uint64_t());
        auto* destination = task.arena().create<Operation>(
            OperationType::deref,
            std::vector<Expression*>{result},
            8);
        destination->set_ir_type(Integer::uint64_t());
        block->add_instruction(task.arena().create<Assignment>(destination, value));
        auto* returned_x0 = task.arena().create<Variable>("x0", 8);
        returned_x0->set_ir_type(Integer::uint64_t());
        auto* returned = task.arena().create<Return>(
            std::vector<Expression*>{returned_x0});
        returned->set_address(0x7230);
        block->add_instruction(returned);

        const auto refinement = refine_pcode_function_signature(task, cfg, 24);
        ASSERT_TRUE(refinement.signature_inferred);
        auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
        ASSERT_TRUE(function != nullptr);
        ASSERT_EQ(function->return_type()->to_string(), std::string("void"));
        ASSERT_EQ(function->abi_indirect_result_size(), 24U);
        ASSERT_EQ(function->parameters().size(), 2U);
        ASSERT_EQ(function->parameters()[0]->type_kind(), TypeKind::Pointer);
        ASSERT_EQ(function->parameters()[1]->size_bytes(), 8U);
        ASSERT_TRUE(returned->values().empty());
    }
    {
        auto architecture = test_architecture();
        const auto base_mapper = architecture.register_mapper;
        architecture.register_mapper = [base_mapper](
            std::uint64_t offset,
            std::size_t size) -> std::optional<PcodeRegisterView> {
            if (offset == 0x40 && size == 8) {
                return PcodeRegisterView{"x8", 8, false, false, false};
            }
            return base_mapper(offset, size);
        };
        architecture.function_resolver = [](ida::Address address)
            -> std::optional<PcodeFunctionInfo> {
            if (address != 0x7200) return std::nullopt;
            PcodeFunctionInfo info;
            info.name = "indirect_result";
            info.parameter_types = {
                std::make_shared<Pointer>(Integer::uint8_t(), 64),
                Integer::uint64_t(),
            };
            info.return_type = CustomType::void_type();
            info.prototype_known = true;
            info.abi_indirect_result_size = 24;
            return info;
        };
        std::vector<std::size_t> resolved_widths;
        architecture.stack_variable_resolver = [&resolved_widths](
            DecompilerArena& arena,
            PcodeStackBase,
            ida::Address,
            std::int64_t offset,
            std::size_t width) -> Variable* {
            resolved_widths.push_back(width);
            auto* slot = arena.create<Variable>(
                "result_" + std::to_string(offset), width);
            slot->set_kind(VariableKind::StackLocal);
            slot->set_stack_offset(offset);
            slot->set_ir_type(std::make_shared<Integer>(width * 8, false));
            return slot;
        };
        auto missing_architecture = architecture;
        Harness harness(nullptr, std::move(architecture));
        ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1730, {
            operation(0x1730, 0, "INT_ADD", varnode("register", 0x40, 8),
                      {varnode("register", 0x100, 8), varnode("const", 0x20, 8)}),
            operation(0x1730, 1, "COPY", varnode("register", 0, 8),
                      {varnode("const", 7, 8)}),
        })).has_value());
        auto call_instructions = harness.lowerer.lower_instruction(instruction(0x1734, {
            operation(0x1734, 0, "CALL", std::nullopt,
                      {varnode("code", 0x7200, 8)}),
        }));
        ASSERT_TRUE(call_instructions.has_value());
        ASSERT_EQ(call_instructions->size(), 1U);
        auto* call_assignment = assignment_at(*call_instructions, 0);
        ASSERT_TRUE(call_assignment->destination() == nullptr);
        auto* call = dyn_cast<Call>(call_assignment->value());
        ASSERT_TRUE(call != nullptr);
        ASSERT_EQ(call->arg_count(), 2U);
        ASSERT_EQ(resolved_widths.size(), 2U);
        ASSERT_EQ(resolved_widths[0], 8U);
        ASSERT_EQ(resolved_widths[1], 24U);

        auto region_load = harness.lowerer.lower_instruction(instruction(0x1738, {
            memory_operation(0x1738, 0, "LOAD", varnode("unique", 0x520, 8),
                             {varnode("const", 0, 8),
                              varnode("register", 0x40, 8)}),
        }));
        // x8 is caller-clobbered, so a raw post-call x8 read cannot recover
        // the region. Real callers reload through SP/FP; exercise that path.
        ASSERT_TRUE(region_load.has_value());
        auto stack_region_load = harness.lowerer.lower_instruction(instruction(0x173c, {
            operation(0x173c, 0, "INT_ADD", varnode("unique", 0x528, 8),
                      {varnode("register", 0x100, 8), varnode("const", 0x28, 8)}),
            memory_operation(0x173c, 1, "LOAD", varnode("unique", 0x530, 8),
                             {varnode("const", 0, 8), varnode("unique", 0x528, 8)}),
        }));
        ASSERT_TRUE(stack_region_load.has_value());
        ASSERT_EQ(resolved_widths.size(), 2U);
        ASSERT_TRUE(isa<Operation>(assignment_at(*stack_region_load, 1)->value()));

        Harness missing_x8(nullptr, std::move(missing_architecture));
        auto missing = missing_x8.lowerer.lower_instruction(instruction(0x1740, {
            operation(0x1740, 0, "CALL", std::nullopt,
                      {varnode("code", 0x7200, 8)}),
        }));
        ASSERT_TRUE(!missing.has_value());
    }
}

void test_hfa_result_refinement_and_call_split() {
    {
        DecompilerTask task(0x7300);
        ControlFlowGraph cfg;
        auto* block = task.arena().create<BasicBlock>(0);
        cfg.set_entry_block(block);
        cfg.add_block(block);
        auto* result = task.arena().create<Variable>("result", 8);
        result->set_kind(VariableKind::Parameter);
        result->set_parameter_index(0);
        result->set_ir_type(std::make_shared<Pointer>(Float::float64(), 64));
        auto* argument = task.arena().create<Variable>("d0", 8);
        argument->set_kind(VariableKind::Parameter);
        argument->set_parameter_index(1);
        argument->set_ir_type(Float::float64());
        auto* sink = task.arena().create<Operation>(
            OperationType::deref,
            std::vector<Expression*>{result},
            8);
        sink->set_ir_type(Float::float64());
        block->add_instruction(task.arena().create<Assignment>(sink, argument));
        auto* returned_x0 = task.arena().create<Variable>("x0", 8);
        returned_x0->set_ir_type(Integer::uint64_t());
        auto* returned = task.arena().create<Return>(
            std::vector<Expression*>{returned_x0});
        returned->set_address(0x7330);
        block->add_instruction(returned);

        const auto refinement = refine_pcode_function_signature(
            task, cfg, 0, 8, 3);
        ASSERT_TRUE(refinement.signature_inferred);
        auto* function = type_dyn_cast<FunctionTypeDef>(task.function_type().get());
        ASSERT_TRUE(function != nullptr);
        ASSERT_EQ(function->return_type()->to_string(), std::string("void"));
        ASSERT_EQ(function->abi_indirect_result_size(), 0U);
        ASSERT_EQ(function->abi_hfa_result_element_size(), 8U);
        ASSERT_EQ(function->abi_hfa_result_count(), 3U);
        ASSERT_EQ(function->parameters().size(), 2U);
        ASSERT_EQ(function->parameters()[0]->type_kind(), TypeKind::Pointer);
        ASSERT_EQ(function->parameters()[1]->type_kind(), TypeKind::Float);
        ASSERT_TRUE(returned->values().empty());
    }
    {
        auto architecture = test_architecture();
        architecture.function_resolver = [](ida::Address address)
            -> std::optional<PcodeFunctionInfo> {
            if (address != 0x7300) return std::nullopt;
            PcodeFunctionInfo info;
            info.name = "hfa_result";
            info.parameter_types = {
                std::make_shared<Pointer>(Float::float64(), 64),
                Float::float64(),
            };
            info.return_type = CustomType::void_type();
            info.prototype_known = true;
            info.abi_hfa_result_element_size = 8;
            info.abi_hfa_result_count = 3;
            return info;
        };
        Harness harness(nullptr, std::move(architecture));
        ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1750, {
            operation(0x1750, 0, "COPY", varnode("register", 0x300, 8),
                      {varnode("const", 0x401c000000000000, 8)}),
        })).has_value());
        auto lowered = harness.lowerer.lower_instruction(instruction(0x1754, {
            operation(0x1754, 0, "CALL", std::nullopt,
                      {varnode("code", 0x7300, 8)}),
        }));
        ASSERT_TRUE(lowered.has_value());
        ASSERT_EQ(lowered->size(), 4U);
        auto* call_assignment = assignment_at(*lowered, 0);
        ASSERT_TRUE(call_assignment->destination() == nullptr);
        auto* call = dyn_cast<Call>(call_assignment->value());
        ASSERT_TRUE(call != nullptr);
        ASSERT_EQ(call->arg_count(), 2U);
        auto* result_address = dyn_cast<Operation>(call->arg(0));
        ASSERT_TRUE(result_address != nullptr);
        ASSERT_EQ(result_address->operands().size(), 1U);
        auto* result_array = dyn_cast<Variable>(
            result_address->operands()[0]);
        ASSERT_TRUE(result_array != nullptr);
        auto* array_type = type_dyn_cast<ArrayType>(result_array->ir_type().get());
        ASSERT_TRUE(array_type != nullptr);
        ASSERT_EQ(array_type->count(), 3U);
        for (std::size_t index = 0; index < 3; ++index) {
            auto* destination = dyn_cast<Variable>(
                assignment_at(*lowered, index + 1)->destination());
            ASSERT_TRUE(destination != nullptr);
            ASSERT_EQ(destination->name(), "d" + std::to_string(index));
            ASSERT_EQ(destination->ir_type()->type_kind(), TypeKind::Float);
        }
    }
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

void test_variadic_prototype_extends_fixed_arguments_from_register_evidence() {
    auto architecture = test_architecture();
    architecture.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        return address == 0x4100
            ? std::optional<std::string>{"printf"} : std::nullopt;
    };
    architecture.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4100) {
            return std::nullopt;
        }
        PcodeFunctionInfo info;
        info.name = "printf";
        info.parameter_types = {
            std::make_shared<Pointer>(Integer::char_type()),
        };
        info.return_type = Integer::int32_t();
        info.prototype_known = true;
        // Exercise canonical-name recovery when an import thunk omits the
        // variadic flag from its prototype metadata.
        info.variadic = false;
        return info;
    };

    Harness harness(nullptr, std::move(architecture));
    harness.lowerer.begin_basic_block();
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1630, {
        operation(0x1630, 0, "COPY", varnode("register", 0, 8),
                  {varnode("const", 0x5000, 8)}),
        operation(0x1630, 1, "COPY", varnode("register", 8, 8),
                  {varnode("const", 40, 8)}),
        operation(0x1630, 2, "COPY", varnode("register", 16, 8),
                  {varnode("const", 102334155, 8)}),
    })).has_value());

    auto lowered = harness.lowerer.lower_instruction(instruction(0x1634, {
        operation(0x1634, 0, "CALL", std::nullopt, {varnode("code", 0x4100, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    auto* call = dyn_cast<Call>(assignment_at(*lowered, 0)->value());
    ASSERT_TRUE(call != nullptr);
    ASSERT_EQ(call->arg_count(), 3U);
    ASSERT_TRUE(harness.task.frontend_diagnostics().back().message.find(
        "inferred 2 variadic general-purpose argument registers") != std::string::npos);
}

void test_darwin_variadic_prototype_uses_contiguous_stack_evidence() {
    auto architecture = test_architecture();
    architecture.variadic_arguments_on_stack = true;
    architecture.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        return address == 0x4200
            ? std::optional<std::string>{"printf"} : std::nullopt;
    };
    architecture.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4200) {
            return std::nullopt;
        }
        PcodeFunctionInfo info;
        info.name = "printf";
        info.parameter_types = {
            std::make_shared<Pointer>(Integer::char_type()),
        };
        info.return_type = Integer::int32_t();
        info.prototype_known = true;
        info.variadic = true;
        return info;
    };
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>("arg_" + std::to_string(offset), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        return slot;
    };

    auto floating_architecture = architecture;
    Harness harness(nullptr, std::move(architecture));
    harness.lowerer.begin_basic_block();
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1640, {
        operation(0x1640, 0, "COPY", varnode("register", 0, 8),
                  {varnode("const", 0x5000, 8)}),
        operation(0x1640, 1, "COPY", varnode("register", 8, 8),
                  {varnode("const", 0x1111, 8)}),
        memory_operation(0x1640, 2, "STORE", std::nullopt,
                         {varnode("const", 0, 8), varnode("register", 0x100, 8),
                          varnode("register", 8, 8)}),
    })).has_value());

    // IDA may split a block immediately before BL. A unique linear
    // fallthrough must retain the outgoing stack-argument evidence.
    harness.lowerer.begin_basic_block(true);

    auto lowered = harness.lowerer.lower_instruction(instruction(0x1644, {
        operation(0x1644, 0, "CALL", std::nullopt, {varnode("code", 0x4200, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    auto* call = dyn_cast<Call>(assignment_at(*lowered, 0)->value());
    ASSERT_TRUE(call != nullptr);
    ASSERT_EQ(call->arg_count(), 2U);
    auto* call_type = type_dyn_cast<FunctionTypeDef>(call->ir_type().get());
    ASSERT_TRUE(call_type != nullptr && call_type->variadic());
    auto* stack_arg = dyn_cast<Variable>(call->arg(1));
    ASSERT_TRUE(stack_arg != nullptr);
    ASSERT_EQ(stack_arg->name(), std::string("arg_0"));
    ASSERT_TRUE(harness.task.frontend_diagnostics().back().message.find(
        "inferred 1 variadic stack argument") != std::string::npos);

    Harness floating(nullptr, std::move(floating_architecture));
    floating.lowerer.begin_basic_block();
    auto floating_lowered = floating.lowerer.lower_instruction(instruction(0x1648, {
        operation(0x1648, 0, "INT2FLOAT", varnode("register", 0x300, 8),
                  {varnode("const", 7, 8)}),
        memory_operation(0x1648, 1, "STORE", std::nullopt,
                         {varnode("const", 0, 8), varnode("register", 0x100, 8),
                          varnode("register", 0x300, 8)}),
        operation(0x1648, 2, "CALL", std::nullopt, {varnode("code", 0x4200, 8)}),
    }));
    ASSERT_TRUE(floating_lowered.has_value());
    auto* floating_call = dyn_cast<Call>(assignment_at(*floating_lowered, 2)->value());
    ASSERT_TRUE(floating_call != nullptr);
    ASSERT_EQ(floating_call->arg_count(), 2U);
    auto* floating_argument = dyn_cast<Variable>(floating_call->arg(1));
    ASSERT_TRUE(floating_argument != nullptr);
    ASSERT_EQ(floating_argument->name(), std::string("d0"));
    ASSERT_TRUE(floating_argument->ir_type() != nullptr);
    ASSERT_TRUE(floating_argument->ir_type()->type_kind() == TypeKind::Float);
}

void test_darwin_variadic_stack_evidence_through_copied_sp_register() {
    auto architecture = test_architecture();
    architecture.variadic_arguments_on_stack = true;
    architecture.symbol_resolver = [](ida::Address address) -> std::optional<std::string> {
        return address == 0x4210
            ? std::optional<std::string>{"printf"} : std::nullopt;
    };
    architecture.function_resolver = [](ida::Address address) -> std::optional<PcodeFunctionInfo> {
        if (address != 0x4210) return std::nullopt;
        PcodeFunctionInfo info;
        info.name = "printf";
        info.parameter_types = {std::make_shared<Pointer>(Integer::char_type())};
        info.return_type = Integer::int32_t();
        info.prototype_known = true;
        info.variadic = true;
        return info;
    };
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>("arg_" + std::to_string(offset), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        return slot;
    };

    Harness harness(nullptr, std::move(architecture));
    harness.lowerer.begin_basic_block();
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1650, {
        operation(0x1650, 0, "COPY", varnode("register", 16, 8),
                  {varnode("register", 0x100, 8)}),
    })).has_value());
    ASSERT_TRUE(harness.lowerer.lower_instruction(instruction(0x1654, {
        memory_operation(0x1654, 0, "STORE", std::nullopt,
                         {varnode("const", 0, 8), varnode("register", 16, 8),
                          varnode("register", 8, 8)}),
    })).has_value());
    auto lowered = harness.lowerer.lower_instruction(instruction(0x1658, {
        operation(0x1658, 0, "CALL", std::nullopt, {varnode("code", 0x4210, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    auto* call = dyn_cast<Call>(assignment_at(*lowered, 0)->value());
    ASSERT_TRUE(call != nullptr);
    ASSERT_EQ(call->arg_count(), 2U);
    auto* stack_argument = dyn_cast<Variable>(call->arg(1));
    ASSERT_TRUE(stack_argument != nullptr);
    ASSERT_EQ(stack_argument->name(), std::string("arg_0"));
}

void test_joined_stack_argument_evidence_is_path_complete() {
    Harness harness;
    auto& arena = harness.task.arena();

    const auto make_slot = [&](std::int64_t offset) {
        auto* slot = arena.create<Variable>("out_" + std::to_string(offset), 8);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        slot->set_ir_type(Float::float64());
        return slot;
    };
    auto* slot_zero_a = make_slot(0);
    auto* slot_zero_b = make_slot(0);
    auto* slot_eight = make_slot(8);
    auto* path_a_value = arena.create<Variable>("d0", 8);
    path_a_value->set_ir_type(Float::float64());
    auto* path_b_value = arena.create<Variable>("d1", 8);
    path_b_value->set_ir_type(Float::float64());

    PcodeLowerer::CallArgumentEvidence path_a;
    path_a.stack_writes.push_back(PcodeLowerer::StackArgumentEvidence{
        0, 8, path_a_value, slot_zero_a, 0x1660});
    path_a.stack_writes.push_back(PcodeLowerer::StackArgumentEvidence{
        8, 8, arena.create<Constant>(7, 8), slot_eight, 0x1660});

    PcodeLowerer::CallArgumentEvidence path_b;
    path_b.stack_writes.push_back(PcodeLowerer::StackArgumentEvidence{
        0, 8, path_b_value, slot_zero_b, 0x1664});

    harness.lowerer.begin_basic_block({path_a, path_b});
    auto merged = harness.lowerer.call_argument_evidence();
    ASSERT_EQ(merged.stack_writes.size(), 1U);
    ASSERT_EQ(merged.stack_writes[0].offset, 0);
    ASSERT_EQ(merged.stack_writes[0].width, 8U);
    auto* merged_value = dyn_cast<Variable>(merged.stack_writes[0].storage);
    ASSERT_TRUE(merged_value != nullptr);
    ASSERT_EQ(merged_value->name(), std::string("out_0"));
    ASSERT_TRUE(merged_value->is_stack_variable());
    ASSERT_TRUE(merged.stack_writes[0].stack_slot != nullptr);

    // Without a common stack object, conflicting branch-local leaves cannot
    // be selected arbitrarily.
    path_a.stack_writes.resize(1);
    path_b.stack_writes.resize(1);
    path_a.stack_writes[0].stack_slot = nullptr;
    path_b.stack_writes[0].stack_slot = nullptr;
    harness.lowerer.begin_basic_block({path_a, path_b});
    ASSERT_TRUE(harness.lowerer.call_argument_evidence().stack_writes.empty());
}

void test_joined_stack_argument_becomes_phi_selected_call_value() {
    auto architecture = test_architecture();
    const auto base_mapper = architecture.register_mapper;
    architecture.register_mapper = [base_mapper](
        std::uint64_t offset,
        std::size_t width) -> std::optional<PcodeRegisterView> {
        if (offset == 0x308 && width == 8) {
            return PcodeRegisterView{"d1", 8, false, false, false};
        }
        return base_mapper(offset, width);
    };
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>("out_" + std::to_string(offset), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        slot->set_ir_type(Integer::uint64_t());
        return slot;
    };
    architecture.function_resolver = [](ida::Address address)
        -> std::optional<PcodeFunctionInfo> {
        if (address != 0x5000) return std::nullopt;
        PcodeFunctionInfo info;
        info.name = "stack_sink";
        info.parameter_types = {Float::float64()};
        info.return_type = Integer::int32_t();
        info.prototype_known = true;
        info.abi_parameter_locations = {
            AbiParameterLocation{0, AbiParameterStorage::Stack, 0, 0},
        };
        return info;
    };

    Harness harness(nullptr, std::move(architecture));
    auto path_a = harness.lowerer.lower_instruction(instruction(0x1670, {
        operation(0x1670, 0, "INT2FLOAT", varnode("register", 0x300, 8),
                  {varnode("const", 1, 8)}),
        memory_operation(0x1670, 1, "STORE", std::nullopt,
                         {varnode("const", 0, 8), varnode("register", 0x100, 8),
                          varnode("register", 0x300, 8)}),
    }));
    ASSERT_TRUE(path_a.has_value());
    const auto evidence_a = harness.lowerer.call_argument_evidence();

    harness.lowerer.begin_basic_block();
    auto path_b = harness.lowerer.lower_instruction(instruction(0x1674, {
        operation(0x1674, 0, "INT2FLOAT", varnode("register", 0x308, 8),
                  {varnode("const", 2, 8)}),
        memory_operation(0x1674, 1, "STORE", std::nullopt,
                         {varnode("const", 0, 8), varnode("register", 0x100, 8),
                          varnode("register", 0x308, 8)}),
    }));
    ASSERT_TRUE(path_b.has_value());
    const auto evidence_b = harness.lowerer.call_argument_evidence();

    harness.lowerer.begin_basic_block({evidence_a, evidence_b});
    auto joined_call = harness.lowerer.lower_instruction(instruction(0x1678, {
        operation(0x1678, 0, "CALL", std::nullopt,
                  {varnode("code", 0x5000, 8)}),
    }));
    ASSERT_TRUE(joined_call.has_value());
    auto* call_before_ssa = dyn_cast<Call>(assignment_at(*joined_call, 0)->value());
    ASSERT_TRUE(call_before_ssa != nullptr);
    auto* joined_slot = dyn_cast<Variable>(call_before_ssa->arg(0));
    ASSERT_TRUE(joined_slot != nullptr);
    ASSERT_EQ(joined_slot->name(), std::string("out_0"));

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* entry = harness.task.arena().create<BasicBlock>(800);
    auto* block_a = harness.task.arena().create<BasicBlock>(801);
    auto* block_b = harness.task.arena().create<BasicBlock>(802);
    auto* join = harness.task.arena().create<BasicBlock>(803);
    cfg->set_entry_block(entry);
    for (BasicBlock* block : {entry, block_a, block_b, join}) cfg->add_block(block);
    for (Instruction* instruction : *path_a) block_a->add_instruction(instruction);
    for (Instruction* instruction : *path_b) block_b->add_instruction(instruction);
    for (Instruction* instruction : *joined_call) join->add_instruction(instruction);

    const auto add_edge = [&](BasicBlock* source, BasicBlock* target, EdgeType type) {
        auto* edge = harness.task.arena().create<Edge>(source, target, type);
        source->add_successor(edge);
        target->add_predecessor(edge);
    };
    add_edge(entry, block_a, EdgeType::True);
    add_edge(entry, block_b, EdgeType::False);
    add_edge(block_a, join, EdgeType::Unconditional);
    add_edge(block_b, join, EdgeType::Unconditional);
    harness.task.set_cfg(std::move(cfg));

    SsaConstructor ssa;
    ssa.execute(harness.task);

    Phi* stack_phi = nullptr;
    Call* call_after_ssa = nullptr;
    for (Instruction* instruction : join->instructions()) {
        if (auto* phi = dyn_cast<Phi>(instruction);
            phi && phi->dest_var() && phi->dest_var()->name() == "out_0") {
            stack_phi = phi;
        }
        if (auto* assignment = dyn_cast<Assignment>(instruction)) {
            if (auto* call = dyn_cast<Call>(assignment->value())) call_after_ssa = call;
        }
    }
    ASSERT_TRUE(stack_phi != nullptr);
    ASSERT_EQ(stack_phi->dest_var()->size_bytes, 8U);
    ASSERT_EQ(stack_phi->dest_var()->kind(), VariableKind::StackLocal);
    ASSERT_TRUE(stack_phi->dest_var()->ir_type() != nullptr);
    ASSERT_EQ(stack_phi->dest_var()->ir_type()->type_kind(), TypeKind::Float);
    ASSERT_TRUE(call_after_ssa != nullptr);
    auto* selected_slot = dyn_cast<Variable>(call_after_ssa->arg(0));
    ASSERT_TRUE(selected_slot != nullptr);
    ASSERT_EQ(selected_slot->name(), std::string("out_0"));
    ASSERT_EQ(selected_slot->ssa_version(), stack_phi->dest_var()->ssa_version());
    ASSERT_EQ(stack_phi->origin_block().size(), 2U);

    harness.task.set_out_of_ssa_mode(OutOfSsaMode::Sreedhar);
    SsaDestructor destructor;
    destructor.execute(harness.task);
    ASSERT_EQ(harness.task.cfg()->blocks().size(), 4U);
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

void test_architectural_stack_pointer_write_is_metadata_only() {
    Harness harness;
    const ida::Address ea = 0x1808;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "INT_SUB",
                  varnode("register", 0x100, 8),
                  {varnode("register", 0x100, 8), varnode("const", 0x40, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    ASSERT_TRUE(lowered->empty());
    ASSERT_EQ(harness.task.frontend_support_report().implemented_ops, 1U);
}

void test_stack_address_delta_through_unique_constant() {
    auto architecture = test_architecture();
    architecture.stack_variable_resolver = [](
        DecompilerArena& arena,
        PcodeStackBase,
        ida::Address,
        std::int64_t offset,
        std::size_t width) -> Variable* {
        auto* slot = arena.create<Variable>("arg_" + std::to_string(offset), width);
        slot->set_kind(VariableKind::StackLocal);
        slot->set_stack_offset(offset);
        return slot;
    };
    Harness harness(nullptr, std::move(architecture));
    const ida::Address ea = 0x180c;
    auto lowered = harness.lowerer.lower_instruction(instruction(ea, {
        operation(ea, 0, "COPY",
                  varnode("unique", 0x2000, 8),
                  {varnode("const", 0x10, 8)}),
        operation(ea, 1, "INT_ADD",
                  varnode("unique", 0x2010, 8),
                  {varnode("register", 0x100, 8), varnode("unique", 0x2000, 8)}),
        operation(ea, 2, "COPY",
                  varnode("register", 8, 8),
                  {varnode("unique", 0x2010, 8)}),
    }));
    ASSERT_TRUE(lowered.has_value());
    ASSERT_EQ(lowered->size(), 3U);
    auto* address = dyn_cast<Operation>(assignment_at(*lowered, 2)->value());
    ASSERT_TRUE(address != nullptr);
    ASSERT_EQ(address->type(), OperationType::address_of);
    auto* slot = dyn_cast<Variable>(address->operands()[0]);
    ASSERT_TRUE(slot != nullptr);
    ASSERT_EQ(slot->name(), std::string("arg_16"));
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
    test_x86_push_arguments_exclude_call_return_address_store();
    test_x86_float_abi_memory_and_variadic_evidence();
    test_structural_recursive_signature_refinement();
    test_recursive_signature_refinement_return_classes_and_fail_closed_arity();
    test_incoming_stack_parameters_require_undefined_contiguous_slots();
    test_atomic_hfa_stack_parameter_locations_and_call_recovery();
    test_packed_hfa_stack_lane_type_survives_gp_carrier();
    test_two_register_integer_return_inference_and_call_split();
    test_call_clobber_preserves_callee_saved_stack_provenance();
    test_indirect_result_refinement_and_call_region();
    test_hfa_result_refinement_and_call_split();
    test_unknown_prototype_call_argument_write_inference();
    test_variadic_prototype_extends_fixed_arguments_from_register_evidence();
    test_darwin_variadic_prototype_uses_contiguous_stack_evidence();
    test_darwin_variadic_stack_evidence_through_copied_sp_register();
    test_joined_stack_argument_evidence_is_path_complete();
    test_joined_stack_argument_becomes_phi_selected_call_value();
    test_stack_address_provenance_through_unique_temp();
    test_architectural_stack_pointer_write_is_metadata_only();
    test_stack_address_delta_through_unique_constant();
    test_architectural_terminator_must_be_last();
    test_intra_instruction_conditional_exit_is_predicated();
    test_recursive_and_float_call_abi();
    test_float_lowering_and_exact_constant_rendering();
    test_shift_boundary_matches_sleigh_bitvector_semantics();
    test_fail_closed_validation_and_diagnostics();
    std::cout << "All P-Code lowering tests passed.\n";
    return 0;
}
