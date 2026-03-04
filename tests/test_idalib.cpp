#include <ida/idax.hpp>
#include <ida/database.hpp>
#include <ida/name.hpp>
#include <ida/core.hpp>
#include <cctype>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>

#include "../src/aletheia/aletheia.hpp"
#include "../src/aletheia/pipeline/pipeline.hpp"
#include "../src/aletheia/pipeline/optimization_stages.hpp"
#include "../src/aletheia/pipeline/expressions/graph_expression_folding.hpp"
#include "../src/aletheia/pipeline/dataflow_analysis/dead_code_elimination.hpp"
#include "../src/aletheia/lifter.hpp"
#include "../src/aletheia/ssa/ssa_constructor.hpp"
#include "../src/aletheia/ssa/ssa_destructor.hpp"
#include "../src/aletheia/structuring/structuring_stage.hpp"
#include "../src/aletheia/codegen/codegen.hpp"

using namespace aletheia;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        throw std::runtime_error(msg); \
    } \
} while (0)

namespace {

ida::Address resolve_function_or_throw(const std::string& func_name) {
    auto ea_res = ida::name::resolve(func_name);
    if (!ea_res || *ea_res == ida::BadAddress) {
        ea_res = ida::name::resolve("_" + func_name);
    }
    TEST_ASSERT(ea_res && *ea_res != ida::BadAddress,
        "failed to resolve function: " + func_name);
    return *ea_res;
}

void test_x86_call_result_uses_rax(const std::string& func_name) {
    ida::Address ea = resolve_function_or_throw(func_name);

    DecompilerTask task(ea);
    idiomata::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);
    auto cfg_res = lifter.lift_function(ea);
    TEST_ASSERT(cfg_res.has_value(), "failed to lift function for x86 call-result test");

    std::size_t x86_call_assign_count = 0;
    std::size_t rax_dest_count = 0;
    std::size_t synthetic_ret_count = 0;

    for (auto* bb : (*cfg_res)->blocks()) {
        for (auto* inst : bb->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (!assign) {
                continue;
            }
            if (!dyn_cast<Call>(assign->value())) {
                continue;
            }

            auto insn_res = ida::instruction::decode(assign->address());
            if (!insn_res) {
                continue;
            }
            std::string mnemonic = insn_res->mnemonic();
            for (char& c : mnemonic) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (mnemonic != "call" && mnemonic != "callq") {
                continue;
            }

            ++x86_call_assign_count;
            auto* dst = dyn_cast<Variable>(assign->destination());
            TEST_ASSERT(dst != nullptr, "call assignment destination is not a variable");

            if (dst->name() == "rax") {
                ++rax_dest_count;
            }
            if (dst->name() == "ret") {
                ++synthetic_ret_count;
            }
        }
    }

    if (x86_call_assign_count == 0) {
        std::cout << "[i] test_x86_call_result_uses_rax skipped (no x86 call/callq assignments in fixture).\n";
        return;
    }
    TEST_ASSERT(rax_dest_count == x86_call_assign_count,
        "expected x86 call/callq assignments to target rax");
    TEST_ASSERT(synthetic_ret_count == 0,
        "synthetic ret destination still present in lifted call assignments");

    std::cout << "[+] test_x86_call_result_uses_rax passed.\n";
}

void test_no_self_recursive_call_artifact(const std::string& func_name) {
    ida::Address ea = resolve_function_or_throw(func_name);

    DecompilerTask task(ea);
    idiomata::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);

    auto cfg_res = lifter.lift_function(ea);
    TEST_ASSERT(cfg_res.has_value(), "failed to lift function for self-recursive artifact test");
    task.set_cfg(std::move(*cfg_res));

    bool has_x86_call = false;
    for (auto* bb : task.cfg()->blocks()) {
        for (auto* inst : bb->instructions()) {
            auto insn_res = ida::instruction::decode(inst->address());
            if (!insn_res) {
                continue;
            }
            std::string mnemonic = insn_res->mnemonic();
            for (char& c : mnemonic) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (mnemonic == "call" || mnemonic == "callq") {
                has_x86_call = true;
                break;
            }
        }
        if (has_x86_call) {
            break;
        }
    }
    if (!has_x86_call) {
        std::cout << "[i] test_no_self_recursive_call_artifact skipped (no x86 call/callq in fixture).\n";
        return;
    }

    DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<SsaConstructor>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationFunctionCallStage>());
    pipeline.add_stage(std::make_unique<GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<SsaDestructor>());
    pipeline.add_stage(std::make_unique<PatternIndependentRestructuringStage>());
    pipeline.run(task);

    TEST_ASSERT(!task.failed(), "pipeline failed in self-recursive artifact test");
    TEST_ASSERT(task.ast() && task.ast()->root(), "missing AST root in self-recursive artifact test");

    CodeVisitor visitor;
    auto lines = visitor.generate_code(task);

    const std::regex self_recursive_tmp_pattern(R"(tmp_([0-9]+)\s*=\s*tmp_\1\s*\()",
        std::regex_constants::ECMAScript);
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, self_recursive_tmp_pattern),
            "detected self-recursive tmp call artifact in generated pseudocode");
    }

    std::cout << "[+] test_no_self_recursive_call_artifact passed.\n";
}

} // namespace

void test_function(const std::string& func_name) {
    std::cout << "\n=============================================\n";
    std::cout << "Testing Aletheia Pipeline on: " << func_name << "\n";
    std::cout << "=============================================\n";

    auto ea_res = ida::name::resolve(func_name);
    if (!ea_res || *ea_res == ida::BadAddress) {
        // macOS C symbols might have a prepended underscore
        ea_res = ida::name::resolve("_" + func_name);
        if (!ea_res || *ea_res == ida::BadAddress) {
            std::cerr << "[-] Failed to resolve function: " << func_name << std::endl;
            return;
        }
    }
    
    ida::Address ea = *ea_res;
    std::cout << "[+] Found " << func_name << " at 0x" << std::hex << ea << std::dec << "\n";

    DecompilerTask task(ea);
    idiomata::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);

    auto cfg_res = lifter.lift_function(ea);
    if (!cfg_res) {
        std::cerr << "[-] Failed to lift function\n";
        return;
    }
    
    std::cout << "[+] Function lifted successfully. Basic blocks: " << (*cfg_res)->blocks().size() << "\n";
    task.set_cfg(std::move(*cfg_res));

    // Run pipeline stages
    DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<SsaConstructor>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<SsaDestructor>());
    pipeline.add_stage(std::make_unique<PatternIndependentRestructuringStage>());
    
    pipeline.run(task);

    if (task.ast() && task.ast()->root()) {
        std::cout << "[+] Structuring successful. Generated AST.\n";
        CodeVisitor visitor;
        auto lines = visitor.generate_code(task);
        std::cout << "\n[Decompiled C Code]\n";
        for (const auto& line : lines) {
            std::cout << line << "\n";
        }
    } else {
        std::cerr << "[-] Structuring failed to generate AST root.\n";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <binary_path>\n";
        return 1;
    }

    std::string binary_path = argv[1];
    
    // Initialize IDALib
    std::cout << "[*] Initializing IDA...\n";
    auto init_res = ida::database::init(argc, argv);
    if (!init_res) {
        std::cerr << "[-] Failed to init IDA: " << init_res.error().message << " | ctx: " << init_res.error().context << "\n";
        return 1;
    }

    std::cout << "[*] Opening database: " << binary_path << "\n";
    auto open_res = ida::database::open(binary_path, true); // true = run autoanalysis
    if (!open_res) {
        std::cerr << "[-] Failed to open database: " << open_res.error().message << "\n";
        return 1;
    }
    
    std::cout << "[+] Database opened and analyzed.\n";

    test_function("simple_math");
    test_function("diamond_cfg");
    test_function("loop_cfg");
    test_function("nested_cfg");
    test_x86_call_result_uses_rax("main");
    test_no_self_recursive_call_artifact("main");

    return 0;
}
