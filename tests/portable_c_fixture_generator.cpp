#include <iostream>
#include <memory>
#include <vector>

#include "../src/aletheia/codegen/codegen.hpp"
#include "../src/aletheia/codegen/portable_c.hpp"

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
    return 0;
}
)ALETHEIA_TEST";
    return 0;
}
