#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "../src/aletheia/codegen/codegen.hpp"
#include "../src/aletheia/codegen/portable_c.hpp"
#include "../src/aletheia/ssa/ssa_destructor.hpp"

using namespace aletheia;

int main() {
    DecompilerTask task(0x1000);
    auto& arena = task.arena();

    task.set_function_name("portable_eval");
    task.set_function_type(std::make_shared<const FunctionTypeDef>(
        Integer::uint64_t(), std::vector<TypePtr>{Integer::uint64_t()}));
    task.set_parameter_register(
        "x0", DecompilerTask::ParameterInfo{"input", 0, Integer::uint64_t()});

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* block = arena.create<BasicBlock>(0);
    cfg->set_entry_block(block);
    cfg->add_block(block);

    auto* input = arena.create<Variable>("x0", 8);
    input->set_kind(VariableKind::Parameter);
    input->set_parameter_index(0);
    input->set_ir_type(Integer::uint64_t());

    auto* xor_value = arena.create<Operation>(
        OperationType::bit_xor,
        std::vector<Expression*>{
            input,
            arena.create<Constant>(UINT64_C(0x5a5a5a5a5a5a5a5a), 8)},
        8);
    xor_value->set_ir_type(Integer::uint64_t());
    auto* product = arena.create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{xor_value, arena.create<Constant>(3, 8)},
        8);
    product->set_ir_type(Integer::uint64_t());
    auto* result = arena.create<Operation>(
        OperationType::add,
        std::vector<Expression*>{product, arena.create<Constant>(7, 8)},
        8);
    result->set_ir_type(Integer::uint64_t());
    block->add_instruction(arena.create<Return>(std::vector<Expression*>{result}));
    task.set_cfg(std::move(cfg));

    auto ast = std::make_unique<AbstractSyntaxForest>();
    ast->set_root(arena.create<CodeNode>(block));
    task.set_ast(std::move(ast));

    std::cout << portable_c_runtime_preamble();
    CodeVisitor visitor({.portable_c = true});
    for (const std::string& line : visitor.generate_code(task)) {
        std::cout << line << '\n';
    }

    DecompilerTask global_task(0x2000);
    auto& global_arena = global_task.arena();
    global_task.set_function_name("portable_global_roundtrip");
    global_task.set_function_type(std::make_shared<const FunctionTypeDef>(
        Integer::uint64_t(), std::vector<TypePtr>{Integer::uint64_t()}));
    global_task.set_parameter_register(
        "x0", DecompilerTask::ParameterInfo{"input", 0, Integer::uint64_t()});

    auto global_cfg = std::make_unique<ControlFlowGraph>();
    auto* global_block = global_arena.create<BasicBlock>(0);
    global_cfg->set_entry_block(global_block);
    global_cfg->add_block(global_block);

    auto* global_input = global_arena.create<Variable>("x0", 8);
    global_input->set_kind(VariableKind::Parameter);
    global_input->set_parameter_index(0);
    global_input->set_ir_type(Integer::uint64_t());
    auto* global_symbol = global_arena.create<GlobalVariable>(
        "portable_global_storage",
        8,
        global_arena.create<Constant>(0x2000, 8),
        false);
    global_symbol->set_represents_address(true);
    auto* slot_address = global_arena.create<Operation>(
        OperationType::add,
        std::vector<Expression*>{
            global_symbol,
            global_arena.create<Constant>(8, 8)},
        8);
    slot_address->set_ir_type(std::make_shared<Pointer>(Integer::uint64_t()));
    auto* slot_store = global_arena.create<Operation>(
        OperationType::deref,
        std::vector<Expression*>{slot_address},
        8);
    slot_store->set_ir_type(Integer::uint64_t());
    global_block->add_instruction(global_arena.create<Assignment>(
        slot_store,
        global_input));
    auto* slot_load = global_arena.create<Operation>(
        OperationType::deref,
        std::vector<Expression*>{slot_address},
        8);
    slot_load->set_ir_type(Integer::uint64_t());
    global_block->add_instruction(global_arena.create<Return>(
        std::vector<Expression*>{slot_load}));
    global_task.set_cfg(std::move(global_cfg));

    auto global_ast = std::make_unique<AbstractSyntaxForest>();
    global_ast->set_root(global_arena.create<CodeNode>(global_block));
    global_task.set_ast(std::move(global_ast));

    CodeVisitor global_visitor({.portable_c = true});
    for (const std::string& line : global_visitor.generate_code(global_task)) {
        std::cout << line << '\n';
    }

    std::cout << "unsigned char portable_global_storage[16];\n";

    // Exercise semantic parameter identity after out-of-SSA assigns a fresh
    // storage name. No register-name map is supplied: the function prototype
    // and Variable::parameter_index must remain sufficient for coherent C.
    DecompilerTask parameter_task(0x3000);
    auto& parameter_arena = parameter_task.arena();
    parameter_task.set_function_name("portable_parameter_identity");
    parameter_task.set_function_type(std::make_shared<const FunctionTypeDef>(
        Integer::int32_t(), std::vector<TypePtr>{Integer::int32_t()}));

    auto parameter_cfg = std::make_unique<ControlFlowGraph>();
    auto* parameter_block = parameter_arena.create<BasicBlock>(0);
    parameter_cfg->set_entry_block(parameter_block);
    parameter_cfg->add_block(parameter_block);
    auto* incoming = parameter_arena.create<Variable>("incoming", 4);
    incoming->set_kind(VariableKind::Parameter);
    incoming->set_parameter_index(0);
    incoming->set_ir_type(Integer::int32_t());
    parameter_block->add_instruction(parameter_arena.create<Return>(
        std::vector<Expression*>{incoming}));
    parameter_task.set_cfg(std::move(parameter_cfg));

    SsaDestructor destructor;
    destructor.execute(parameter_task);
    auto parameter_ast = std::make_unique<AbstractSyntaxForest>();
    parameter_ast->set_root(parameter_arena.create<CodeNode>(parameter_block));
    parameter_task.set_ast(std::move(parameter_ast));

    CodeVisitor parameter_visitor({.portable_c = true});
    for (const std::string& line : parameter_visitor.generate_code(parameter_task)) {
        std::cout << line << '\n';
    }

    DecompilerTask identifier_task(0x4000);
    auto& identifier_arena = identifier_task.arena();
    const TypePtr hostile_type = std::make_shared<const CustomType>(
        "9-bad type", 32);
    identifier_task.set_function_name("9-bad function");
    identifier_task.set_function_type(std::make_shared<const FunctionTypeDef>(
        Integer::int32_t(),
        std::vector<TypePtr>{
            Integer::int32_t(), Integer::int32_t(),
            Integer::int32_t(), hostile_type,
        }));
    identifier_task.set_parameter_register("x0", {
        .name = "switch", .index = 0, .type = Integer::int32_t()});
    identifier_task.set_parameter_register("x1", {
        .name = "a-b", .index = 1, .type = Integer::int32_t()});
    identifier_task.set_parameter_register("x2", {
        .name = "a_b", .index = 2, .type = Integer::int32_t()});
    identifier_task.set_parameter_register("x3", {
        .name = "uint64_t", .index = 3, .type = hostile_type});

    auto identifier_cfg = std::make_unique<ControlFlowGraph>();
    auto* identifier_block = identifier_arena.create<BasicBlock>(0);
    identifier_cfg->set_entry_block(identifier_block);
    identifier_cfg->add_block(identifier_block);
    const auto parameter = [&](const char* storage, int index) {
        auto* value = identifier_arena.create<Variable>(storage, 4);
        value->set_kind(VariableKind::Parameter);
        value->set_parameter_index(index);
        value->set_ir_type(index == 3 ? hostile_type : Integer::int32_t());
        return value;
    };
    auto* keyword_local = identifier_arena.create<Variable>("for", 4);
    keyword_local->set_ir_type(Integer::int32_t());
    auto* punctuated_local = identifier_arena.create<Variable>("a-b", 4);
    punctuated_local->set_ir_type(Integer::int32_t());
    auto* colliding_local = identifier_arena.create<Variable>("a_b", 4);
    colliding_local->set_ir_type(Integer::int32_t());
    identifier_block->add_instruction(identifier_arena.create<Assignment>(
        keyword_local,
        identifier_arena.create<Operation>(
            OperationType::add,
            std::vector<Expression*>{parameter("x0", 0), parameter("x1", 1)},
            4)));
    identifier_block->add_instruction(identifier_arena.create<Assignment>(
        punctuated_local,
        identifier_arena.create<Operation>(
            OperationType::add,
            std::vector<Expression*>{keyword_local, parameter("x2", 2)},
            4)));
    identifier_block->add_instruction(identifier_arena.create<Assignment>(
        colliding_local,
        identifier_arena.create<Operation>(
            OperationType::add,
            std::vector<Expression*>{punctuated_local, parameter("x3", 3)},
            4)));
    auto* incremented = identifier_arena.create<Operation>(
        OperationType::add,
        std::vector<Expression*>{
            colliding_local, identifier_arena.create<Constant>(1, 4)},
        4);
    auto* invalid_callee = identifier_arena.create<GlobalVariable>(
        "callee-name", 8);
    auto* call = identifier_arena.create<Call>(
        invalid_callee,
        std::vector<Expression*>{incremented},
        4);
    call->set_ir_type(std::make_shared<const FunctionTypeDef>(
        Integer::int32_t(), std::vector<TypePtr>{Integer::int32_t()}));
    auto* invalid_global = identifier_arena.create<GlobalVariable>(
        "9-global", 4);
    invalid_global->set_ir_type(Integer::int32_t());
    auto* identifier_result = identifier_arena.create<Operation>(
        OperationType::add,
        std::vector<Expression*>{call, invalid_global},
        4);
    identifier_block->add_instruction(identifier_arena.create<Return>(
        std::vector<Expression*>{identifier_result}));
    identifier_task.set_cfg(std::move(identifier_cfg));
    auto identifier_ast = std::make_unique<AbstractSyntaxForest>();
    identifier_ast->set_root(
        identifier_arena.create<CodeNode>(identifier_block));
    identifier_task.set_ast(std::move(identifier_ast));

    const CIdentifierDisplayInfo identifiers =
        build_c_identifier_display_info(identifier_task);
    std::cout << "int " << identifiers.global_names.at("callee-name")
              << "(int);\n";
    CodeVisitor identifier_visitor({.portable_c = true});
    for (const std::string& line : identifier_visitor.generate_code(identifier_task)) {
        std::cout << line << '\n';
    }
    std::cout << "int " << identifiers.global_names.at("9-global")
              << " = 5;\n";
    std::cout << "int " << identifiers.global_names.at("callee-name")
              << "(int value) { return value * 2; }\n";
    std::cout << "int portable_identifier_wrapper(void) { return "
              << identifiers.function_name << "(1, 2, 3, 4); }\n";

    const TypePtr aggregate_type = std::make_shared<const Struct>(
        "9 runtime record",
        64,
        std::unordered_map<std::size_t, ComplexTypeMember>{
            {0, {"switch", 0, Integer::int32_t(), std::nullopt}},
            {6, {"a-b", 6, Integer::uint16_t(), std::nullopt}},
        });
    DecompilerTask aggregate_task(0x5000);
    auto& aggregate_arena = aggregate_task.arena();
    aggregate_task.set_function_name("portable_aggregate_roundtrip");
    aggregate_task.set_function_type(std::make_shared<const FunctionTypeDef>(
        Integer::int32_t(),
        std::vector<TypePtr>{Integer::int32_t(), Integer::uint16_t()}));
    aggregate_task.set_parameter_register("w0", {
        .name = "left", .index = 0, .type = Integer::int32_t()});
    aggregate_task.set_parameter_register("w1", {
        .name = "right", .index = 1, .type = Integer::uint16_t()});

    auto aggregate_cfg = std::make_unique<ControlFlowGraph>();
    auto* aggregate_block = aggregate_arena.create<BasicBlock>(0);
    aggregate_cfg->set_entry_block(aggregate_block);
    aggregate_cfg->add_block(aggregate_block);
    auto* aggregate_value = aggregate_arena.create<Variable>("record", 8);
    aggregate_value->set_ir_type(aggregate_type);
    const auto aggregate_parameter = [&](const char* storage,
                                         int index,
                                         const TypePtr& type) {
        auto* value = aggregate_arena.create<Variable>(storage, type->size_bytes());
        value->set_kind(VariableKind::Parameter);
        value->set_parameter_index(index);
        value->set_ir_type(type);
        return value;
    };
    auto* first_member_name = aggregate_arena.create<Variable>("switch", 4);
    auto* first_member = aggregate_arena.create<Operation>(
        OperationType::member_access,
        std::vector<Expression*>{aggregate_value, first_member_name},
        4);
    first_member->set_ir_type(Integer::int32_t());
    aggregate_block->add_instruction(aggregate_arena.create<Assignment>(
        first_member,
        aggregate_parameter("w0", 0, Integer::int32_t())));
    auto* second_member_name = aggregate_arena.create<Variable>("a-b", 2);
    auto* second_member = aggregate_arena.create<Operation>(
        OperationType::member_access,
        std::vector<Expression*>{aggregate_value, second_member_name},
        2);
    second_member->set_ir_type(Integer::uint16_t());
    aggregate_block->add_instruction(aggregate_arena.create<Assignment>(
        second_member,
        aggregate_parameter("w1", 1, Integer::uint16_t())));
    auto* aggregate_sum = aggregate_arena.create<Operation>(
        OperationType::add,
        std::vector<Expression*>{first_member, second_member},
        4);
    aggregate_sum->set_ir_type(Integer::int32_t());
    aggregate_block->add_instruction(aggregate_arena.create<Return>(
        std::vector<Expression*>{aggregate_sum}));
    aggregate_task.set_cfg(std::move(aggregate_cfg));
    auto aggregate_ast = std::make_unique<AbstractSyntaxForest>();
    aggregate_ast->set_root(
        aggregate_arena.create<CodeNode>(aggregate_block));
    aggregate_task.set_ast(std::move(aggregate_ast));
    CodeVisitor aggregate_visitor({.portable_c = true});
    for (const std::string& line : aggregate_visitor.generate_code(aggregate_task)) {
        std::cout << line << '\n';
    }

    const TypePtr runtime_union = std::make_shared<const Union>(
        "runtime choice",
        64,
        std::vector<ComplexTypeMember>{
            {"word", 0, Integer::int32_t(), std::nullopt},
            {"wide", 0, Integer::int64_t(), std::nullopt},
        });
    const TypePtr runtime_enum = std::make_shared<const Enum>(
        "runtime status",
        32,
        std::unordered_map<std::int64_t, ComplexTypeMember>{
            {-1, {"not-ready", 0, nullptr, -1}},
            {7, {"switch", 0, nullptr, 7}},
        });
    const TypePtr nested_aggregate_type = std::make_shared<const Struct>(
        "runtime nested",
        128,
        std::unordered_map<std::size_t, ComplexTypeMember>{
            {0, {"payload", 0, aggregate_type, std::nullopt}},
            {12, {"tail", 12, Integer::int32_t(), std::nullopt}},
        });
    DecompilerTask nested_type_task(0x5010);
    auto nested_type_ast = std::make_unique<AbstractSyntaxForest>();
    auto* nested_type_expression = nested_type_task.arena().create<Variable>(
        "nested_record", 16);
    nested_type_expression->set_ir_type(nested_aggregate_type);
    nested_type_ast->set_root(
        nested_type_task.arena().create<ExprAstNode>(nested_type_expression));
    nested_type_task.set_ast(std::move(nested_type_ast));
    const CIdentifierDisplayInfo nested_types =
        build_c_identifier_display_info(nested_type_task);
    for (const std::string& declaration : nested_types.type_declarations) {
        std::cout << declaration << '\n';
    }
    std::cout << portable_complex_type_declaration(
        *type_dyn_cast<Union>(runtime_union.get())).text << '\n';
    std::cout << portable_complex_type_declaration(
        *type_dyn_cast<Enum>(runtime_enum.get())).text << '\n';

    const std::string union_name = portable_complex_type_name(
        *type_dyn_cast<Union>(runtime_union.get()));
    const std::string enum_name = portable_complex_type_name(
        *type_dyn_cast<Enum>(runtime_enum.get()));
    const std::string nested_name = portable_complex_type_name(
        *type_dyn_cast<Struct>(nested_aggregate_type.get()));
    const std::string enum_prefix = "aletheia_enum_"
        + encode_c_identifier_bytes("", "runtime status") + "_";
    std::cout << "int portable_complex_layouts(void) { "
              << union_name << " choice; " << nested_name << " nested; "
              << "choice.wide = 9; "
              << "nested.payload."
              << normalize_distinct_c_identifier("switch", "member")
              << " = 3; nested.tail = 4; return (int)(sizeof(choice) + "
              << "sizeof(nested) + choice.wide + nested.payload."
              << normalize_distinct_c_identifier("switch", "member")
              << " + nested.tail + "
              << encode_c_identifier_bytes(enum_prefix, "not-ready")
              << " + " << encode_c_identifier_bytes(enum_prefix, "switch")
              << "); }\n";
    std::cout << "_Static_assert(sizeof(" << enum_name
              << ") == 4, \"Aletheia enum storage mismatch\");\n";

    std::cout << R"ALETHEIA_TEST(
int main(void) {
    if (portable_eval(UINT64_C(0)) != UINT64_C(0x0f0f0f0f0f0f0f15)) return 1;
    if (portable_eval(UINT64_C(1)) != UINT64_C(0x0f0f0f0f0f0f0f18)) return 2;
    if (portable_eval(UINT64_MAX) != UINT64_C(0xf0f0f0f0f0f0f0f6)) return 3;
    if (portable_eval(UINT64_C(0x0123456789abcdef)) != UINT64_C(0x126b5db97bd4c726)) return 4;

    if (__aletheia_pow_u64(3, 0) != 1) return 5;
    if (__aletheia_pow_u64(3, 5) != 243) return 6;
    if (__aletheia_pow_u64(2, 64) != 0) return 7;
    if (__pcode_popcount(UINT64_C(0x1ff), 8) != 8) return 8;
    if (__pcode_popcount(UINT64_C(0xf0f0), 16) != 8) return 9;
    if (__pcode_lzcount(UINT64_C(1), 8) != 7) return 10;
    if (__pcode_lzcount(UINT64_C(0), 32) != 32) return 11;
    if (__pcode_lzcount(UINT64_C(0x80000000), 32) != 0) return 12;

    const double nan_value = __builtin_nan("");
    if (!__pcode_float_nan(nan_value)) return 13;
    if (__pcode_float_abs(-3.5) != 3.5) return 14;
    if (__pcode_float_sqrt(81.0) != 9.0) return 15;
    if (__pcode_ceil(2.25) != 3.0) return 16;
    if (__pcode_floor(2.75) != 2.0) return 17;
    if (__pcode_round(2.5) != 3.0) return 18;
    if (portable_global_roundtrip(UINT64_C(0)) != UINT64_C(0)) return 19;
    if (portable_global_roundtrip(UINT64_C(0x0123456789abcdef))
        != UINT64_C(0x0123456789abcdef)) return 20;
    if (portable_global_roundtrip(UINT64_MAX) != UINT64_MAX) return 21;
    if (portable_parameter_identity(-7) != -7) return 22;
    if (portable_parameter_identity(0) != 0) return 23;
    if (portable_parameter_identity(42) != 42) return 24;
    if (portable_identifier_wrapper() != 27) return 25;
    if (portable_aggregate_roundtrip(-10, 12) != 2) return 26;
    if (portable_aggregate_roundtrip(100, UINT16_C(65535)) != 65635) return 27;
    if (portable_complex_layouts() != 46) return 28;
    return 0;
}
)ALETHEIA_TEST";
    return 0;
}
