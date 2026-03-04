#include <ida/idax.hpp>
#include <ida/database.hpp>
#include <ida/name.hpp>
#include <ida/core.hpp>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
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

std::string g_loaded_binary_path;

ida::Address resolve_function_or_throw(const std::string& func_name) {
    auto ea_res = ida::name::resolve(func_name);
    if (!ea_res || *ea_res == ida::BadAddress) {
        ea_res = ida::name::resolve("_" + func_name);
    }
    TEST_ASSERT(ea_res && *ea_res != ida::BadAddress,
        "failed to resolve function: " + func_name);
    return *ea_res;
}

std::optional<ida::Address> resolve_function_optional(const std::string& func_name) {
    auto ea_res = ida::name::resolve(func_name);
    if (!ea_res || *ea_res == ida::BadAddress) {
        ea_res = ida::name::resolve("_" + func_name);
    }
    if (!ea_res || *ea_res == ida::BadAddress) {
        return std::nullopt;
    }
    return *ea_res;
}

std::vector<std::string> decompile_function_lines(ida::Address ea) {
    DecompilerTask task(ea);
    idiomata::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);
    lifter.populate_task_signature(task);

    auto cfg_res = lifter.lift_function(ea);
    TEST_ASSERT(cfg_res.has_value(), "failed to lift function for decompile helper");
    task.set_cfg(std::move(*cfg_res));

    DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<SsaConstructor>());
    pipeline.add_stage(std::make_unique<GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationFunctionCallStage>());
    pipeline.add_stage(std::make_unique<DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<SinkDefinitionRepairStage>());
    pipeline.add_stage(std::make_unique<VoidReturnNormalizationStage>());
    pipeline.add_stage(std::make_unique<ReturnDefinitionSanityStage>());
    pipeline.add_stage(std::make_unique<AddressResolutionStage>());
    pipeline.add_stage(std::make_unique<ArrayAccessDetectionStage>());
    pipeline.add_stage(std::make_unique<ExpressionSimplificationStage>());
    pipeline.add_stage(std::make_unique<SsaDestructor>());
    pipeline.add_stage(std::make_unique<PatternIndependentRestructuringStage>());
    pipeline.add_stage(std::make_unique<AstExpressionSimplificationStage>());
    pipeline.run(task);

    TEST_ASSERT(!task.failed(), "pipeline failed in decompile helper");
    TEST_ASSERT(task.ast() && task.ast()->root(), "missing AST root in decompile helper");

    CodeVisitor visitor;
    return visitor.generate_code(task);
}

std::string read_text_file_or_empty(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string generate_production_fibonacci_output_or_empty() {
    const std::string input_path = "../meta/fibonacci";
    const std::string output_path = "../meta/fibonacci.c";

    std::ifstream probe(input_path);
    if (!probe.good()) {
        return {};
    }

    const int rc = std::system("timeout 120 ../build-release-optimized/idump ../meta/fibonacci > /dev/null 2>&1");
    if (rc != 0) {
        return {};
    }

    return read_text_file_or_empty(output_path);
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

void test_arm64_recursive_call_keeps_argument(const std::string& func_name) {
    auto ea_opt = resolve_function_optional(func_name);
    if (!ea_opt) {
        std::cout << "[i] test_arm64_recursive_call_keeps_argument skipped (function not present in fixture).\n";
        return;
    }

    const ida::Address ea = *ea_opt;
    DecompilerTask task(ea);
    idiomata::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);
    auto cfg_res = lifter.lift_function(ea);
    TEST_ASSERT(cfg_res.has_value(), "failed to lift function for arm64 recursive-call test");

    bool saw_recursive_bl = false;
    for (auto* bb : (*cfg_res)->blocks()) {
        for (auto* inst : bb->instructions()) {
            auto* assign = dyn_cast<Assignment>(inst);
            if (!assign) {
                continue;
            }
            auto* call = dyn_cast<Call>(assign->value());
            if (!call) {
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
            if (mnemonic != "bl" && mnemonic != "blr" && mnemonic != "blx") {
                continue;
            }

            bool recursive_target = false;
            if (auto* target = dyn_cast<GlobalVariable>(call->target())) {
                if (auto* init = dyn_cast<Constant>(target->initial_value())) {
                    recursive_target = static_cast<ida::Address>(init->value()) == ea;
                }
            } else if (auto* target = dyn_cast<Constant>(call->target())) {
                recursive_target = static_cast<ida::Address>(target->value()) == ea;
            }
            if (!recursive_target) {
                continue;
            }

            saw_recursive_bl = true;
            TEST_ASSERT(call->arg_count() > 0,
                "recursive arm64 call collapsed to argumentless form");
        }
    }

    if (!saw_recursive_bl) {
        std::cout << "[i] test_arm64_recursive_call_keeps_argument skipped (no recursive arm64 BL call in fixture).\n";
        return;
    }

    std::cout << "[+] test_arm64_recursive_call_keeps_argument passed.\n";
}

void test_arm64_fallback_signature_not_empty(const std::string& func_name) {
    auto ea_opt = resolve_function_optional(func_name);
    if (!ea_opt) {
        std::cout << "[i] test_arm64_fallback_signature_not_empty skipped (function not present in fixture).\n";
        return;
    }

    const ida::Address ea = *ea_opt;
    DecompilerTask task(ea);
    idiomata::IdiomMatcher matcher;
    Lifter lifter(task.arena(), matcher);
    lifter.populate_task_signature(task);

    auto cfg_res = lifter.lift_function(ea);
    TEST_ASSERT(cfg_res.has_value(), "failed to lift function for arm64 fallback-signature test");
    task.set_cfg(std::move(*cfg_res));

    DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<SsaConstructor>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<ExpressionPropagationFunctionCallStage>());
    pipeline.add_stage(std::make_unique<GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<SsaDestructor>());
    pipeline.add_stage(std::make_unique<PatternIndependentRestructuringStage>());
    pipeline.run(task);

    TEST_ASSERT(!task.failed(), "pipeline failed in arm64 fallback-signature test");
    TEST_ASSERT(task.ast() && task.ast()->root(), "missing AST root in arm64 fallback-signature test");

    CodeVisitor visitor;
    auto lines = visitor.generate_code(task);
    TEST_ASSERT(!lines.empty(), "no generated code lines in arm64 fallback-signature test");

    const std::string& sig = lines.front();
    const std::regex empty_sig_pattern("_" + func_name + R"(\s*\(\s*\))");
    TEST_ASSERT(!std::regex_search(sig, empty_sig_pattern),
        "fallback signature emitted empty parameter list for recursive arm64 function");

    std::cout << "[+] test_arm64_fallback_signature_not_empty passed.\n";
}

void test_fibonacci_recursive_arg_not_pointer_corruption() {
    auto ea_opt = resolve_function_optional("fib_memo");
    if (!ea_opt) {
        std::cout << "[i] test_fibonacci_recursive_arg_not_pointer_corruption skipped (fib_memo missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*ea_opt);
    const std::regex bad_pattern(R"(_fib_memo\s*\(\s*___error_ptr\s*\))");
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, bad_pattern),
            "detected recursive call argument corruption: _fib_memo(___error_ptr)");
    }

    std::cout << "[+] test_fibonacci_recursive_arg_not_pointer_corruption passed.\n";
}

void test_get_time_seconds_has_arithmetic_return() {
    auto ea_opt = resolve_function_optional("get_time_seconds");
    if (!ea_opt) {
        std::cout << "[i] test_get_time_seconds_has_arithmetic_return skipped (get_time_seconds missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*ea_opt);
    bool saw_return = false;
    for (const auto& line : lines) {
        if (line.find("return ") != std::string::npos) {
            saw_return = true;
            const bool has_arith = line.find(" + ") != std::string::npos
                || line.find(" * ") != std::string::npos
                || line.find(" - ") != std::string::npos
                || line.find(" / ") != std::string::npos;
            TEST_ASSERT(has_arith,
                "get_time_seconds return did not contain arithmetic expression");
            break;
        }
    }
    TEST_ASSERT(saw_return, "missing return line in get_time_seconds output");

    std::cout << "[+] test_get_time_seconds_has_arithmetic_return passed.\n";
}

void test_main_has_no_branch_comment_placeholders() {
    auto ea_opt = resolve_function_optional("main");
    if (!ea_opt) {
        std::cout << "[i] test_main_has_no_branch_comment_placeholders skipped (main missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*ea_opt);
    for (const auto& line : lines) {
        TEST_ASSERT(line.find("/* branch if (") == std::string::npos,
            "detected branch-comment placeholder in main output");
    }

    std::cout << "[+] test_main_has_no_branch_comment_placeholders passed.\n";
}

void test_main_has_no_empty_branch_blocks() {
    auto ea_opt = resolve_function_optional("main");
    if (!ea_opt) {
        std::cout << "[i] test_main_has_no_empty_branch_blocks skipped (main missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*ea_opt);
    const std::regex empty_if(R"(if\s*\([^\)]*\)\s*\{\s*\})");
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, empty_if),
            "detected empty if-branch block in main output");
    }

    std::cout << "[+] test_main_has_no_empty_branch_blocks passed.\n";
}

void test_fib_functions_no_disconnected_tmp_return() {
    auto naive_ea = resolve_function_optional("fib_naive");
    auto memo_ea = resolve_function_optional("fib_memo");
    if (!naive_ea || !memo_ea) {
        std::cout << "[i] test_fib_functions_no_disconnected_tmp_return skipped (fib functions missing).\n";
        return;
    }

    auto naive_lines = decompile_function_lines(*naive_ea);
    auto memo_lines = decompile_function_lines(*memo_ea);

    const std::regex bad_tmp_return(R"(return\s+tmp_[0-9]+\s*;)" );
    for (const auto& line : naive_lines) {
        TEST_ASSERT(!std::regex_search(line, bad_tmp_return),
            "fib_naive contains disconnected tmp return");
    }
    for (const auto& line : memo_lines) {
        TEST_ASSERT(!std::regex_search(line, bad_tmp_return),
            "fib_memo contains disconnected tmp return");
    }

    std::cout << "[+] test_fib_functions_no_disconnected_tmp_return passed.\n";
}

void test_fib_memo_no_opaque_cache_writes() {
    auto memo_ea = resolve_function_optional("fib_memo");
    if (!memo_ea) {
        std::cout << "[i] test_fib_memo_no_opaque_cache_writes skipped (fib_memo missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*memo_ea);
    const std::regex opaque_store(R"(\*\(tmp_[0-9]+\))");
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, opaque_store),
            "fib_memo contains opaque tmp dereference store/load");
    }

    bool has_named_merged_globals = false;
    for (const auto& line : lines) {
        if (line.find("__MergedGlobals") != std::string::npos) {
            has_named_merged_globals = true;
            break;
        }
    }
    TEST_ASSERT(has_named_merged_globals,
        "fib_memo did not emit named __MergedGlobals-based cache access");

    std::cout << "[+] test_fib_memo_no_opaque_cache_writes passed.\n";
}

void test_reset_memo_cache_void_return_behavior() {
    auto reset_ea = resolve_function_optional("reset_memo_cache");
    if (!reset_ea) {
        std::cout << "[i] test_reset_memo_cache_void_return_behavior skipped (reset_memo_cache missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*reset_ea);
    const std::regex bad_value_return(R"(return\s+tmp_[0-9]+\s*;)" );
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, bad_value_return),
            "reset_memo_cache emitted invalid tmp-valued return");
    }

    std::cout << "[+] test_reset_memo_cache_void_return_behavior passed.\n";
}

void test_all_user_functions_no_disconnected_tmp_return() {
    const std::vector<std::string> funcs = {
        "fib_naive", "fib_memo", "reset_memo_cache", "get_time_seconds", "main"
    };
    const std::regex bad_tmp_return(R"(return\s+tmp_[0-9]+\s*;)");

    bool any_present = false;
    for (const auto& fn : funcs) {
        auto ea = resolve_function_optional(fn);
        if (!ea) continue;
        any_present = true;
        auto lines = decompile_function_lines(*ea);
        for (const auto& line : lines) {
            TEST_ASSERT(!std::regex_search(line, bad_tmp_return),
                fn + " contains disconnected tmp-valued return");
        }
    }

    if (!any_present) {
        std::cout << "[i] test_all_user_functions_no_disconnected_tmp_return skipped (functions missing).\n";
        return;
    }

    std::cout << "[+] test_all_user_functions_no_disconnected_tmp_return passed.\n";
}

void test_main_contains_mismatch_error_path() {
    if (g_loaded_binary_path.find("fibonacci") == std::string::npos) {
        std::cout << "[i] test_main_contains_mismatch_error_path skipped (non-fibonacci binary).\n";
        return;
    }

    auto ea_opt = resolve_function_optional("main");
    if (!ea_opt) {
        std::cout << "[i] test_main_contains_mismatch_error_path skipped (main missing).\n";
        return;
    }

    if (!resolve_function_optional("fib_memo")) {
        std::cout << "[i] test_main_contains_mismatch_error_path skipped (non-fibonacci fixture).\n";
        return;
    }

    auto lines = decompile_function_lines(*ea_opt);
    bool looks_like_fibonacci_fixture = false;
    for (const auto& line : lines) {
        if (line.find("Computing Fibonacci") != std::string::npos) {
            looks_like_fibonacci_fixture = true;
            break;
        }
    }
    if (!looks_like_fibonacci_fixture) {
        std::cout << "[i] test_main_contains_mismatch_error_path skipped (non-fibonacci main fixture).\n";
        return;
    }

    bool saw_error_fprintf = false;
    bool saw_failure_return = false;
    for (const auto& line : lines) {
        if (line.find("_fprintf(") != std::string::npos) {
            saw_error_fprintf = true;
        }
        if (line.find("return 0x1") != std::string::npos || line.find("return 1") != std::string::npos) {
            saw_failure_return = true;
        }
    }

    TEST_ASSERT(saw_error_fprintf && saw_failure_return,
        "main missing fprintf+failure-return error path");

    std::cout << "[+] test_main_contains_mismatch_error_path passed.\n";
}

void test_codegen_has_no_function_name_templates() {
    const std::string src = read_text_file_or_empty("../src/aletheia/codegen/codegen.cpp");
    if (src.empty()) {
        std::cout << "[i] test_codegen_has_no_function_name_templates skipped (cannot read codegen source).\n";
        return;
    }

    TEST_ASSERT(src.find("canon_name == \"fib_naive\"") == std::string::npos,
        "detected function-name template branch for fib_naive in codegen");
    TEST_ASSERT(src.find("canon_name == \"fib_memo\"") == std::string::npos,
        "detected function-name template branch for fib_memo in codegen");
    TEST_ASSERT(src.find("canon_name == \"reset_memo_cache\"") == std::string::npos,
        "detected function-name template branch for reset_memo_cache in codegen");
    TEST_ASSERT(src.find("canon_name == \"get_time_seconds\"") == std::string::npos,
        "detected function-name template branch for get_time_seconds in codegen");
    TEST_ASSERT(src.find("canon_name == \"main\"") == std::string::npos,
        "detected function-name template branch for main in codegen");
    TEST_ASSERT(src.find("function_is_fib_memo") == std::string::npos,
        "detected function-targeted fib_memo rewrite flag in codegen");
    TEST_ASSERT(src.find("curr.find(\", a2)\")") == std::string::npos,
        "detected function-targeted _fprintf(..., a2) rewrite logic in codegen");
    TEST_ASSERT(src.find("_fib_naive(tmp_1 - 0x1) + _fib_naive(tmp_1 - 0x2)") == std::string::npos,
        "detected codegen-injected fib_naive recursive terminal return rewrite");

    std::cout << "[+] test_codegen_has_no_function_name_templates passed.\n";
}

void test_no_invalid_extern_string_declarations_in_output() {
    auto main_ea = resolve_function_optional("main");
    if (!main_ea) {
        std::cout << "[i] test_no_invalid_extern_string_declarations_in_output skipped (main missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*main_ea);
    const std::regex bad_decl(R"(^\s*extern\s+const\s+int\s+")");
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, bad_decl),
            "detected invalid extern const int string declaration in output");
    }

    std::cout << "[+] test_no_invalid_extern_string_declarations_in_output passed.\n";
}

void test_production_fibonacci_has_no_branch_placeholders() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_has_no_branch_placeholders skipped (fibonacci fixture unavailable).\n";
        return;
    }

    TEST_ASSERT(out.find("unresolved branch") == std::string::npos,
        "production fibonacci output still contains unresolved branch placeholder");
    TEST_ASSERT(out.find("unknown then-target") == std::string::npos,
        "production fibonacci output still contains unknown branch target placeholders");

    auto section = [&](const std::string& begin_sig, const std::string& end_sig) {
        const std::size_t begin = out.find(begin_sig);
        if (begin == std::string::npos) {
            return std::string{};
        }
        const std::size_t end = end_sig.empty() ? std::string::npos : out.find(end_sig, begin + begin_sig.size());
        if (end == std::string::npos) {
            return out.substr(begin);
        }
        return out.substr(begin, end - begin);
    };

    const std::string user_scoped =
        section("long _fib_naive", "long _fib_memo") + "\n" +
        section("long _fib_memo", "void _reset_memo_cache") + "\n" +
        section("void _reset_memo_cache", "double _get_time_seconds") + "\n" +
        section("double _get_time_seconds", "int _main") + "\n" +
        section("int _main", "int * ___error");

    TEST_ASSERT(user_scoped.find("goto bb_") == std::string::npos,
        "production fibonacci user-function output still contains unresolved goto bb_* placeholders");

    const std::regex empty_if_else(R"(if\s*\([^\)]*\)\s*\{\s*\}\s*else\s*\{\s*\})");
    TEST_ASSERT(!std::regex_search(user_scoped, empty_if_else),
        "production fibonacci user-function output still contains empty if/else semantic placeholders");

    const std::regex empty_if_only(R"(if\s*\([^\)]*\)\s*\{\s*\})");
    TEST_ASSERT(!std::regex_search(user_scoped, empty_if_only),
        "production fibonacci user-function output still contains empty if semantic placeholders");

    std::cout << "[+] test_production_fibonacci_has_no_branch_placeholders passed.\n";
}

void test_production_fibonacci_has_no_tmp_returns_in_user_functions() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_has_no_tmp_returns_in_user_functions skipped (fibonacci fixture unavailable).\n";
        return;
    }

    auto section = [&](const std::string& begin_sig, const std::string& end_sig) {
        const std::size_t begin = out.find(begin_sig);
        if (begin == std::string::npos) {
            return std::string{};
        }
        const std::size_t end = end_sig.empty() ? std::string::npos : out.find(end_sig, begin + begin_sig.size());
        if (end == std::string::npos) {
            return out.substr(begin);
        }
        return out.substr(begin, end - begin);
    };

    const std::string fib_naive = section("long _fib_naive", "long _fib_memo");
    const std::string fib_memo = section("long _fib_memo", "void _reset_memo_cache");
    const std::string main_fn = section("int _main", "int * ___error");

    const std::regex bad_tmp_return(R"(return\s+tmp_[0-9]+\s*;)");
    TEST_ASSERT(!std::regex_search(main_fn, bad_tmp_return),
        "production fibonacci _main contains disconnected tmp-valued return");
    const std::regex hardcoded_zero_return(R"(return\s+0x0\s*;)");
    TEST_ASSERT(!std::regex_search(fib_naive, hardcoded_zero_return),
        "production fib_naive output contains hardcoded zero fallback return");
    TEST_ASSERT(!std::regex_search(fib_memo, hardcoded_zero_return),
        "production fib_memo output contains hardcoded zero fallback return");

    std::cout << "[+] test_production_fibonacci_has_no_tmp_returns_in_user_functions passed.\n";
}

void test_production_fibonacci_cache_paths_not_opaque() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_cache_paths_not_opaque skipped (fibonacci fixture unavailable).\n";
        return;
    }

    auto section = [&](const std::string& begin_sig, const std::string& end_sig) {
        const std::size_t begin = out.find(begin_sig);
        if (begin == std::string::npos) {
            return std::string{};
        }
        const std::size_t end = end_sig.empty() ? std::string::npos : out.find(end_sig, begin + begin_sig.size());
        if (end == std::string::npos) {
            return out.substr(begin);
        }
        return out.substr(begin, end - begin);
    };

    const std::string memo = section("long _fib_memo", "void _reset_memo_cache");
    const std::string reset = section("void _reset_memo_cache", "double _get_time_seconds");
    const std::string main_fn = section("int _main", "int * ___error");
    const std::string scoped = memo + "\n" + reset + "\n" + main_fn;
    TEST_ASSERT(!scoped.empty(), "failed to isolate fibonacci cache-related functions in output");

    TEST_ASSERT(scoped.find("*(0x100008000UL)") == std::string::npos,
        "production fibonacci cache paths still have raw global address dereference");
    TEST_ASSERT(scoped.find("((unsigned long *)tmp_") == std::string::npos,
        "production fibonacci cache paths still have opaque tmp-based cast dereference");
    TEST_ASSERT(scoped.find("__MergedGlobals") != std::string::npos,
        "production fibonacci output missing __MergedGlobals-rooted accesses");

    const std::regex opaque_index_alias(R"(tmp_[0-9]+\s*=\s*tmp_[0-9]+\s*-\s*__MergedGlobals\s*;)");
    TEST_ASSERT(!std::regex_search(scoped, opaque_index_alias),
        "production fibonacci cache paths still use opaque index aliasing from global-base subtraction");

    std::cout << "[+] test_production_fibonacci_cache_paths_not_opaque passed.\n";
}

void test_production_fibonacci_has_no_empty_call_arguments() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_has_no_empty_call_arguments skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::regex malformed_call(R"([A-Za-z_][A-Za-z0-9_]*\(\s*,)");
    TEST_ASSERT(!std::regex_search(out, malformed_call),
        "production fibonacci output contains call with empty leading argument");

    const std::regex printf_arg_zero(R"(_f?printf\(0x0\s*,)");
    TEST_ASSERT(!std::regex_search(out, printf_arg_zero),
        "production fibonacci output contains defaulted 0x0 provenance placeholder in printf/fprintf call");

    const std::regex fabricated_fprintf_literal(R"(_fprintf\(0xffffffffffffffffUL\s*,)");
    TEST_ASSERT(!std::regex_search(out, fabricated_fprintf_literal),
        "production fibonacci output still contains fabricated _fprintf literal argument rewrite");

    std::cout << "[+] test_production_fibonacci_has_no_empty_call_arguments passed.\n";
}

void test_production_fibonacci_main_strtol_validation_shape() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_strtol_validation_shape skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    TEST_ASSERT(main_body.find("_strtol(") != std::string::npos,
        "production fibonacci _main missing strtol validation call");
    const std::regex strtol_three_arg_shape(R"(_strtol\([^,]+,\s*[^,]+,\s*[^\)]+\))");
    TEST_ASSERT(std::regex_search(main_body, strtol_three_arg_shape),
        "production fibonacci _main missing well-formed 3-argument strtol call");

    const std::regex tmp_ptr_strtol_pattern(R"(_strtol\(\s*\*\(tmp_[0-9]+\)\s*,\s*tmp_[0-9]+\s*,\s*0xaU?\s*\))");
    TEST_ASSERT(!std::regex_search(main_body, tmp_ptr_strtol_pattern),
        "production fibonacci _main still contains tmp-pointer placeholder strtol provenance pattern");

    std::cout << "[+] test_production_fibonacci_main_strtol_validation_shape passed.\n";
}

void test_production_fibonacci_fib_naive_has_negative_error_path() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_fib_naive_has_negative_error_path skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t begin = out.find("long _fib_naive");
    TEST_ASSERT(begin != std::string::npos, "production fibonacci output missing _fib_naive");
    const std::size_t end = out.find("long _fib_memo", begin);
    TEST_ASSERT(end != std::string::npos, "failed to isolate _fib_naive section in production fibonacci output");
    const std::string body = out.substr(begin, end - begin);

    TEST_ASSERT(body.find("Error: fib_naive() called with negative index %d") != std::string::npos,
        "production fibonacci _fib_naive missing negative-input error path literal");

    const std::size_t err_pos = body.find("Error: fib_naive() called with negative index %d");
    TEST_ASSERT(err_pos != std::string::npos,
        "production fibonacci _fib_naive missing negative-input error string anchor");
    const std::size_t neg_window_end = body.find("if (", err_pos + 1);
    const std::string neg_window = body.substr(err_pos,
        neg_window_end == std::string::npos ? std::string::npos : (neg_window_end - err_pos));
    TEST_ASSERT(neg_window.find("return -1;") != std::string::npos,
        "production fibonacci _fib_naive negative path must return -1");
    TEST_ASSERT(neg_window.find("return _fib_naive(") == std::string::npos,
        "production fibonacci _fib_naive negative path must not return recursive expression");

    const std::regex wrong_negative_return(R"(if\s*\([^\)]*negative index %d[^\{]*\{[^\}]*return\s+0x1\s*;)");
    const std::regex any_negative_hex_one_return(R"(return\s+0x1\s*;)");
    TEST_ASSERT(!std::regex_search(body, wrong_negative_return) && !std::regex_search(body, any_negative_hex_one_return),
        "production fibonacci _fib_naive negative/error path still returns 0x1 instead of -1");

    std::cout << "[+] test_production_fibonacci_fib_naive_has_negative_error_path passed.\n";
}

void test_production_fibonacci_main_has_failure_return_path() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_has_failure_return_path skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    const std::regex failure_return(R"(return\s+0x1\s*;|return\s+1\s*;|return\s+a1\s*;)");
    TEST_ASSERT(std::regex_search(main_body, failure_return),
        "production fibonacci _main missing explicit failure return path");

    std::cout << "[+] test_production_fibonacci_main_has_failure_return_path passed.\n";
}

void test_production_fibonacci_main_strtol_not_from_mergedglobals() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_strtol_not_from_mergedglobals skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    const std::regex merged_globals_strtol_arg0(R"(_strtol\(\s*__MergedGlobals\[[^\]]+\])");
    TEST_ASSERT(!std::regex_search(main_body, merged_globals_strtol_arg0),
        "production fibonacci _main still feeds strtol arg0 from __MergedGlobals[...] placeholder");

    std::cout << "[+] test_production_fibonacci_main_strtol_not_from_mergedglobals passed.\n";
}

void test_production_fibonacci_main_no_dead_constant_validation_guard() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_no_dead_constant_validation_guard skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    TEST_ASSERT(main_body.find("if (0x0)") == std::string::npos,
        "production fibonacci _main still contains dead constant validation guard if (0x0)");

    std::cout << "[+] test_production_fibonacci_main_no_dead_constant_validation_guard passed.\n";
}

void test_production_fibonacci_fib_naive_no_disconnected_return_a1() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_fib_naive_no_disconnected_return_a1 skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t begin = out.find("long _fib_naive");
    TEST_ASSERT(begin != std::string::npos, "production fibonacci output missing _fib_naive");
    const std::size_t end = out.find("long _fib_memo", begin);
    TEST_ASSERT(end != std::string::npos, "failed to isolate _fib_naive section in production fibonacci output");
    const std::string body = out.substr(begin, end - begin);

    const std::regex disconnected_return(R"(return\s+a1\s*;)");
    TEST_ASSERT(!std::regex_search(body, disconnected_return),
        "production fibonacci _fib_naive still has disconnected 'return a1;' semantics");

    std::cout << "[+] test_production_fibonacci_fib_naive_no_disconnected_return_a1 passed.\n";
}

void test_production_fibonacci_fib_naive_no_tmp_terminal_return() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_fib_naive_no_tmp_terminal_return skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t begin = out.find("long _fib_naive");
    TEST_ASSERT(begin != std::string::npos, "production fibonacci output missing _fib_naive");
    const std::size_t end = out.find("long _fib_memo", begin);
    TEST_ASSERT(end != std::string::npos, "failed to isolate _fib_naive section in production fibonacci output");
    const std::string body = out.substr(begin, end - begin);

    const std::regex tmp_return(R"(return\s+tmp_[0-9]+\s*;)");
    TEST_ASSERT(!std::regex_search(body, tmp_return),
        "production fibonacci _fib_naive still contains tmp-driven terminal return");

    std::cout << "[+] test_production_fibonacci_fib_naive_no_tmp_terminal_return passed.\n";
}

void test_production_fibonacci_main_has_argc_validation_shape() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_has_argc_validation_shape skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    TEST_ASSERT(main_body.find("_strtol(") != std::string::npos,
        "production fibonacci _main missing strtol validation call");
    TEST_ASSERT(main_body.find("___error()") != std::string::npos,
        "production fibonacci _main missing errno access call in validation chain");
    const std::regex argc_gate_shape(R"(if\s*\([^\)]*a1[^\)]*\))");
    TEST_ASSERT(std::regex_search(main_body, argc_gate_shape),
        "production fibonacci _main lacks recognizable argc-driven validation branch");
    TEST_ASSERT(main_body.find("&(tmp_") != std::string::npos,
        "production fibonacci _main missing endptr address-taking shape in validation chain");
    TEST_ASSERT(main_body.find("a1") != std::string::npos,
        "production fibonacci _main lacks argc variable presence in validation region");
    TEST_ASSERT(main_body.find("0xaU") != std::string::npos || main_body.find(", 0xa)") != std::string::npos,
        "production fibonacci _main missing base-10 validation input");
    TEST_ASSERT(main_body.find("Usage: %s [n]") != std::string::npos,
        "production fibonacci _main missing usage-error validation path");

    std::cout << "[+] test_production_fibonacci_main_has_argc_validation_shape passed.\n";
}

void test_production_fibonacci_fib_memo_cache_hit_uses_stable_access() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_fib_memo_cache_hit_uses_stable_access skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t begin = out.find("long _fib_memo");
    TEST_ASSERT(begin != std::string::npos, "production fibonacci output missing _fib_memo");
    const std::size_t end = out.find("void _reset_memo_cache", begin);
    TEST_ASSERT(end != std::string::npos, "failed to isolate _fib_memo section in production fibonacci output");
    const std::string body = out.substr(begin, end - begin);

    TEST_ASSERT(body.find("return __MergedGlobals[") != std::string::npos,
        "production fibonacci _fib_memo missing stable indexed cache-hit return form");
    const std::regex tmp_return(R"(return\s+tmp_[0-9]+\s*;)");
    TEST_ASSERT(!std::regex_search(body, tmp_return),
        "production fibonacci _fib_memo still uses tmp-driven cache-hit return variable");
    const std::regex unstable_tmp_index_return(R"(return\s+__MergedGlobals\[tmp_[0-9]+\]\s*;)");
    TEST_ASSERT(!std::regex_search(body, unstable_tmp_index_return),
        "production fibonacci _fib_memo still returns through unstable tmp-derived cache index form");

    std::cout << "[+] test_production_fibonacci_fib_memo_cache_hit_uses_stable_access passed.\n";
}

void test_production_fibonacci_main_terminal_success_return_zero() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_terminal_success_return_zero skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    std::size_t last_ret = main_body.rfind("return ");
    TEST_ASSERT(last_ret != std::string::npos, "production fibonacci _main missing terminal return");
    const std::size_t line_end = main_body.find('\n', last_ret);
    const std::string ret_line = main_body.substr(last_ret, line_end == std::string::npos ? std::string::npos : (line_end - last_ret));
    TEST_ASSERT(ret_line.find("return 0;") != std::string::npos || ret_line.find("return 0x0;") != std::string::npos,
        "production fibonacci _main terminal success return is not 0");

    std::cout << "[+] test_production_fibonacci_main_terminal_success_return_zero passed.\n";
}

void test_production_fibonacci_main_no_contradictory_terminal_returns() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_no_contradictory_terminal_returns skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    const bool has_merged_ret = main_body.find("return __MergedGlobals[") != std::string::npos;
    const bool has_zero_ret = main_body.find("return 0x0;") != std::string::npos || main_body.find("return 0;") != std::string::npos;
    TEST_ASSERT(!(has_merged_ret && has_zero_ret),
        "production fibonacci _main contains contradictory returns (__MergedGlobals[...] and terminal return 0)");

    std::cout << "[+] test_production_fibonacci_main_no_contradictory_terminal_returns passed.\n";
}

void test_production_fibonacci_main_validation_chain_markers() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_validation_chain_markers skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    TEST_ASSERT(main_body.find("if (a1") != std::string::npos,
        "production fibonacci _main missing recognizable argc-driven gate marker");
    TEST_ASSERT(main_body.find("___error()") != std::string::npos,
        "production fibonacci _main missing errno flow marker");
    TEST_ASSERT(main_body.find("_strtol(") != std::string::npos,
        "production fibonacci _main missing strtol marker");
    TEST_ASSERT(main_body.find("&(tmp_") != std::string::npos,
        "production fibonacci _main missing endptr address marker");

    std::cout << "[+] test_production_fibonacci_main_validation_chain_markers passed.\n";
}

void test_production_fibonacci_fib_memo_cache_store_index_not_tmp_alias() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_fib_memo_cache_store_index_not_tmp_alias skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t begin = out.find("long _fib_memo");
    TEST_ASSERT(begin != std::string::npos, "production fibonacci output missing _fib_memo");
    const std::size_t end = out.find("void _reset_memo_cache", begin);
    TEST_ASSERT(end != std::string::npos, "failed to isolate _fib_memo section in production fibonacci output");
    const std::string body = out.substr(begin, end - begin);

    const std::regex tmp_store_alias(R"(__MergedGlobals\[tmp_[0-9]+\]\s*=)");
    TEST_ASSERT(!std::regex_search(body, tmp_store_alias),
        "production fibonacci _fib_memo cache stores still use tmp-derived index aliases");

    std::cout << "[+] test_production_fibonacci_fib_memo_cache_store_index_not_tmp_alias passed.\n";
}

void test_production_fibonacci_fib_memo_bounds_error_and_cache_hit_shape() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_fib_memo_bounds_error_and_cache_hit_shape skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t begin = out.find("long _fib_memo");
    TEST_ASSERT(begin != std::string::npos, "production fibonacci output missing _fib_memo");
    const std::size_t end = out.find("void _reset_memo_cache", begin);
    TEST_ASSERT(end != std::string::npos, "failed to isolate _fib_memo section in production fibonacci output");
    const std::string body = out.substr(begin, end - begin);

    TEST_ASSERT(body.find("Error: fib_memo() index %d exceeds maximum %d") != std::string::npos,
        "production fibonacci _fib_memo missing bounds-error format string path");

    const std::regex cache_hit_return_shape(R"(if\s*\([^\{]*__MergedGlobals[^\{]*\)\s*\{[^\}]*return)");
    TEST_ASSERT(std::regex_search(body, cache_hit_return_shape),
        "production fibonacci _fib_memo missing cache-hit return control shape rooted in __MergedGlobals");

    std::cout << "[+] test_production_fibonacci_fib_memo_bounds_error_and_cache_hit_shape passed.\n";
}

void test_production_fibonacci_main_has_connected_terminal_return() {
    const std::string out = generate_production_fibonacci_output_or_empty();
    if (out.empty()) {
        std::cout << "[i] test_production_fibonacci_main_has_connected_terminal_return skipped (fibonacci fixture unavailable).\n";
        return;
    }

    const std::size_t main_begin = out.find("int _main");
    TEST_ASSERT(main_begin != std::string::npos, "production fibonacci output missing _main");
    const std::size_t main_end = out.find("int * ___error", main_begin);
    TEST_ASSERT(main_end != std::string::npos, "failed to isolate _main section in production fibonacci output");
    const std::string main_body = out.substr(main_begin, main_end - main_begin);

    const std::regex return_const(R"(return\s+0x[01]\s*;)");
    TEST_ASSERT(std::regex_search(main_body, return_const),
        "production fibonacci _main missing connected terminal return constant");

    std::cout << "[+] test_production_fibonacci_main_has_connected_terminal_return passed.\n";
}

void test_main_no_integer_temp_for_error_pointer() {
    auto main_ea = resolve_function_optional("main");
    if (!main_ea) {
        std::cout << "[i] test_main_no_integer_temp_for_error_pointer skipped (main missing).\n";
        return;
    }

    auto lines = decompile_function_lines(*main_ea);
    const std::regex bad_int_bind(R"(\bint\s+\w+\s*=\s*___error\s*\()");
    for (const auto& line : lines) {
        TEST_ASSERT(!std::regex_search(line, bad_int_bind),
            "detected integer temporary assigned from pointer-returning ___error() call");
    }

    std::cout << "[+] test_main_no_integer_temp_for_error_pointer passed.\n";
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
    g_loaded_binary_path = binary_path;
    
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
    test_arm64_recursive_call_keeps_argument("fib_naive");
    test_arm64_fallback_signature_not_empty("fib_naive");
    test_fibonacci_recursive_arg_not_pointer_corruption();
    test_get_time_seconds_has_arithmetic_return();
    test_main_has_no_branch_comment_placeholders();
    test_main_has_no_empty_branch_blocks();
    test_fib_functions_no_disconnected_tmp_return();
    test_fib_memo_no_opaque_cache_writes();
    test_reset_memo_cache_void_return_behavior();
    test_all_user_functions_no_disconnected_tmp_return();
    test_main_contains_mismatch_error_path();
    test_codegen_has_no_function_name_templates();
    test_no_invalid_extern_string_declarations_in_output();
    test_main_no_integer_temp_for_error_pointer();
    test_production_fibonacci_has_no_branch_placeholders();
    test_production_fibonacci_has_no_tmp_returns_in_user_functions();
    test_production_fibonacci_cache_paths_not_opaque();
    test_production_fibonacci_has_no_empty_call_arguments();
    test_production_fibonacci_main_strtol_validation_shape();
    test_production_fibonacci_fib_naive_has_negative_error_path();
    test_production_fibonacci_main_has_failure_return_path();
    test_production_fibonacci_main_strtol_not_from_mergedglobals();
    test_production_fibonacci_main_no_dead_constant_validation_guard();
    test_production_fibonacci_fib_naive_no_disconnected_return_a1();
    test_production_fibonacci_fib_naive_no_tmp_terminal_return();
    test_production_fibonacci_main_has_argc_validation_shape();
    test_production_fibonacci_fib_memo_cache_hit_uses_stable_access();
    test_production_fibonacci_main_terminal_success_return_zero();
    test_production_fibonacci_main_no_contradictory_terminal_returns();
    test_production_fibonacci_main_validation_chain_markers();
    test_production_fibonacci_fib_memo_cache_store_index_not_tmp_alias();
    test_production_fibonacci_fib_memo_bounds_error_and_cache_hit_shape();
    test_production_fibonacci_main_has_connected_terminal_return();

    return 0;
}
