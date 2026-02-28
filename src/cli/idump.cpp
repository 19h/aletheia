#include <ida/database.hpp>
#include <ida/function.hpp>

#include "../dewolf/pipeline/pipeline.hpp"
#include "../dewolf/codegen/codegen.hpp"
#include "../dewolf/codegen/local_declarations.hpp"
#include "../dewolf/lifter.hpp"
#include "../dewolf/ssa/ssa_constructor.hpp"
#include "../dewolf/ssa/ssa_destructor.hpp"
#include "../dewolf/pipeline/preprocessing_stages.hpp"
#include "../dewolf/pipeline/optimization_stages.hpp"
#include "../dewolf/pipeline/expressions/graph_expression_folding.hpp"
#include "../dewolf/pipeline/dataflow_analysis/dead_code_elimination.hpp"
#include "../dewolf/structuring/structuring_stage.hpp"
#include "../dewolf/structuring/instruction_length_handler.hpp"
#include "../dewolf/structuring/variable_name_generation.hpp"
#include "../dewolf/structuring/loop_name_generator.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

struct CliOptions {
    std::string input_binary;
    std::string output_path;
    bool explicit_headless = false;
};

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string_view v{value};
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

void print_usage() {
    std::cerr << "Usage: idump <binary> [-o <output.c>] [--headless]\n";
}

bool parse_args(int argc, char** argv, CliOptions& options) {
    if (argc < 2) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            options.explicit_headless = true;
            continue;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) return false;
            options.output_path = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            return false;
        }
        if (options.input_binary.empty()) {
            options.input_binary = arg;
        } else {
            return false;
        }
    }

    return !options.input_binary.empty();
}

bool detect_headless(const CliOptions& options) {
    if (options.explicit_headless) return true;
    if (env_flag_enabled("DEWOLF_HEADLESS")) return true;
    return false;
}

void configure_out_of_ssa_mode(dewolf::DecompilerTask& task) {
    if (const char* mode_env = std::getenv("DEWOLF_OUT_OF_SSA_MODE"); mode_env != nullptr) {
        auto parsed = dewolf::SsaDestructor::parse_mode(mode_env);
        if (parsed.has_value()) {
            task.set_out_of_ssa_mode(*parsed);
        }
    }
}

void apply_variable_naming(dewolf::DecompilerTask& task) {
    if (const char* naming_env = std::getenv("DEWOLF_VARIABLE_NAMING"); naming_env != nullptr) {
        std::string_view scheme{naming_env};
        if (scheme == "system_hungarian") {
            dewolf::VariableNameGeneration::apply_system_hungarian(task.ast());
        } else {
            dewolf::VariableNameGeneration::apply_default(task.ast());
        }
    } else {
        dewolf::VariableNameGeneration::apply_default(task.ast());
    }
    dewolf::LoopNameGenerator::apply_for_loop_counters(task.ast());
    dewolf::LoopNameGenerator::apply_while_loop_counters(task.ast());
}

std::string block_label(const dewolf::BasicBlock* block) {
    return "bb_" + std::to_string(block ? block->id() : 0);
}

std::pair<dewolf::Edge*, dewolf::Edge*> pick_true_false_edges(dewolf::BasicBlock* block) {
    dewolf::Edge* true_edge = nullptr;
    dewolf::Edge* false_edge = nullptr;
    if (!block) {
        return {nullptr, nullptr};
    }

    for (dewolf::Edge* edge : block->successors()) {
        if (!edge) {
            continue;
        }
        if (edge->type() == dewolf::EdgeType::True && true_edge == nullptr) {
            true_edge = edge;
        } else if (edge->type() == dewolf::EdgeType::False && false_edge == nullptr) {
            false_edge = edge;
        }
    }

    if (!true_edge && !block->successors().empty()) {
        true_edge = block->successors().front();
    }
    if (!false_edge) {
        for (dewolf::Edge* edge : block->successors()) {
            if (edge != true_edge) {
                false_edge = edge;
                break;
            }
        }
    }

    return {true_edge, false_edge};
}

std::string indent_of(int level) {
    return std::string(static_cast<std::size_t>(level) * 4, ' ');
}

void emit_inline_branch_snapshot(
    dewolf::BasicBlock* block,
    dewolf::CExpressionGenerator& expr_gen,
    std::vector<std::string>& lines,
    int indent_level,
    int depth,
    std::unordered_set<dewolf::BasicBlock*>& path,
    std::unordered_set<dewolf::BasicBlock*>& inlined_blocks) {
    if (!block) {
        lines.push_back(indent_of(indent_level) + "/* unknown target */");
        return;
    }
    if (depth > 3) {
        lines.push_back(indent_of(indent_level) + "/* ... -> " + block_label(block) + " */");
        return;
    }
    if (path.contains(block)) {
        lines.push_back(indent_of(indent_level) + "/* loop -> " + block_label(block) + " */");
        return;
    }

    path.insert(block);
    inlined_blocks.insert(block);

    const auto& insts = block->instructions();
    dewolf::Instruction* tail = insts.empty() ? nullptr : insts.back();
    const bool tail_is_branch = dynamic_cast<dewolf::Branch*>(tail) != nullptr;
    const bool tail_is_indirect = dynamic_cast<dewolf::IndirectBranch*>(tail) != nullptr;

    std::size_t limit = insts.size();
    if ((tail_is_branch || tail_is_indirect) && limit > 0) {
        --limit;
    }

    for (std::size_t j = 0; j < limit; ++j) {
        std::string stmt = expr_gen.generate(insts[j]);
        if (!stmt.empty()) {
            lines.push_back(indent_of(indent_level) + stmt + ";");
        }
    }

    if (auto* branch = dynamic_cast<dewolf::Branch*>(tail)) {
        auto [true_edge, false_edge] = pick_true_false_edges(block);
        const std::string cond = expr_gen.generate(branch->condition());
        lines.push_back(indent_of(indent_level) + "if (" + cond + ") {");
        emit_inline_branch_snapshot(
            true_edge ? true_edge->target() : nullptr,
            expr_gen,
            lines,
            indent_level + 1,
            depth + 1,
            path,
            inlined_blocks);
        lines.push_back(indent_of(indent_level) + "} else {");
        emit_inline_branch_snapshot(
            false_edge ? false_edge->target() : nullptr,
            expr_gen,
            lines,
            indent_level + 1,
            depth + 1,
            path,
            inlined_blocks);
        lines.push_back(indent_of(indent_level) + "}");
    } else if (auto* indirect = dynamic_cast<dewolf::IndirectBranch*>(tail)) {
        lines.push_back(indent_of(indent_level) + "/* indirect branch " + expr_gen.generate(indirect->expression()) + " */");
        for (dewolf::Edge* edge : block->successors()) {
            emit_inline_branch_snapshot(
                edge ? edge->target() : nullptr,
                expr_gen,
                lines,
                indent_level,
                depth + 1,
                path,
                inlined_blocks);
        }
    } else if (block->successors().size() == 1) {
        dewolf::BasicBlock* next = block->successors()[0] ? block->successors()[0]->target() : nullptr;
        if (next) {
            emit_inline_branch_snapshot(next, expr_gen, lines, indent_level, depth + 1, path, inlined_blocks);
        }
    }

    path.erase(block);
}

std::vector<std::string> generate_cfg_fallback_code(dewolf::DecompilerTask& task) {
    std::vector<std::string> lines;
    dewolf::CExpressionGenerator expr_gen;

    auto global_decls = dewolf::GlobalDeclarationGenerator::generate(task);
    for (const auto& decl : global_decls) {
        lines.push_back(decl);
    }
    if (!global_decls.empty()) {
        lines.push_back("");
    }

    std::string sig = "void ";
    if (task.function_type()) {
        if (auto* func_type = dynamic_cast<const dewolf::FunctionTypeDef*>(task.function_type().get())) {
            sig = func_type->return_type()->to_string() + " ";
        } else {
            sig = task.function_type()->to_string() + " ";
        }
    }

    std::string name = task.function_name().empty()
        ? "sub_" + std::to_string(task.function_address())
        : task.function_name();
    sig += name + "(";

    if (task.function_type()) {
        if (auto* func_type = dynamic_cast<const dewolf::FunctionTypeDef*>(task.function_type().get())) {
            const auto& params = func_type->parameters();
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (i > 0) sig += ", ";
                sig += params[i]->to_string() + " a" + std::to_string(i + 1);
            }
        }
    }
    sig += ") {";
    lines.push_back(sig);

    auto decls = dewolf::LocalDeclarationGenerator::generate(task, expr_gen);
    for (const auto& decl : decls) {
        lines.push_back("    " + decl);
    }
    if (!decls.empty()) {
        lines.push_back("");
    }

    if (task.cfg()) {
        const auto blocks = task.cfg()->blocks();
        std::unordered_set<dewolf::BasicBlock*> inlined_blocks;
        dewolf::BasicBlock* entry = task.cfg()->entry_block();

        if (entry) {
            std::unordered_set<dewolf::BasicBlock*> path;
            emit_inline_branch_snapshot(entry, expr_gen, lines, 1, 0, path, inlined_blocks);
        }

        std::size_t omitted = 0;
        for (dewolf::BasicBlock* block : blocks) {
            if (block && !inlined_blocks.contains(block)) {
                ++omitted;
            }
        }
        if (omitted > 0) {
            lines.push_back("    /* omitted " + std::to_string(omitted) + " detached blocks */");
        }
    }

    lines.push_back("}");
    return lines;
}

dewolf::DecompilerPipeline build_pipeline() {
    dewolf::DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<dewolf::CompilerIdiomHandlingStage>());
    pipeline.add_stage(std::make_unique<dewolf::RegisterPairHandlingStage>());
    pipeline.add_stage(std::make_unique<dewolf::RemoveGoPrologueStage>());
    pipeline.add_stage(std::make_unique<dewolf::RemoveStackCanaryStage>());
    pipeline.add_stage(std::make_unique<dewolf::RemoveNoreturnBoilerplateStage>());
    pipeline.add_stage(std::make_unique<dewolf::InsertMissingDefinitionsStage>());
    pipeline.add_stage(std::make_unique<dewolf::SwitchVariableDetectionStage>());
    pipeline.add_stage(std::make_unique<dewolf::MemPhiConverterStage>());
    pipeline.add_stage(std::make_unique<dewolf::CoherenceStage>());
    pipeline.add_stage(std::make_unique<dewolf::SsaConstructor>());
    pipeline.add_stage(std::make_unique<dewolf::GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<dewolf::DeadComponentPrunerStage>());
    pipeline.add_stage(std::make_unique<dewolf::ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<dewolf::BitFieldComparisonUnrollingStage>());
    pipeline.add_stage(std::make_unique<dewolf::TypePropagationStage>());
    pipeline.add_stage(std::make_unique<dewolf::DeadPathEliminationStage>());
    pipeline.add_stage(std::make_unique<dewolf::DeadLoopEliminationStage>());
    pipeline.add_stage(std::make_unique<dewolf::ExpressionPropagationMemoryStage>());
    pipeline.add_stage(std::make_unique<dewolf::ExpressionPropagationFunctionCallStage>());
    pipeline.add_stage(std::make_unique<dewolf::DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<dewolf::RedundantCastsEliminationStage>());
    pipeline.add_stage(std::make_unique<dewolf::IdentityEliminationStage>());
    pipeline.add_stage(std::make_unique<dewolf::CommonSubexpressionEliminationStage>());
    pipeline.add_stage(std::make_unique<dewolf::ArrayAccessDetectionStage>());
    pipeline.add_stage(std::make_unique<dewolf::ExpressionSimplificationStage>());
    pipeline.add_stage(std::make_unique<dewolf::DeadComponentPrunerStage>());
    pipeline.add_stage(std::make_unique<dewolf::GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<dewolf::EdgePrunerStage>());
    pipeline.add_stage(std::make_unique<dewolf::PhiFunctionFixerStage>());
    pipeline.add_stage(std::make_unique<dewolf::SsaDestructor>());
    if (env_flag_enabled("DEWOLF_IDUMP_ENABLE_STRUCTURING")) {
        pipeline.add_stage(std::make_unique<dewolf::PatternIndependentRestructuringStage>());
    }
    return pipeline;
}

std::vector<std::string> decompile_function(
    ida::Address ea,
    dewolf_idioms::IdiomMatcher& matcher,
    bool& ok,
    std::string& error_message) {
    ok = false;
    error_message.clear();

    dewolf::DecompilerTask task(ea);
    configure_out_of_ssa_mode(task);

    dewolf::Lifter lifter(task.arena(), matcher);
    std::vector<dewolf_idioms::IdiomTag> idiom_tags;

    lifter.populate_task_signature(task);

    auto cfg_res = lifter.lift_function(ea, &idiom_tags);
    if (!cfg_res) {
        error_message = "lift failed";
        return {};
    }

    task.set_cfg(std::move(*cfg_res));
    task.set_idiom_tags(std::move(idiom_tags));

    auto pipeline = build_pipeline();
    pipeline.run(task);

    if (task.failed()) {
        std::cerr << "idump: pipeline stopped at stage '" << task.failure_stage() << "'";
        if (!task.failure_message().empty()) {
            std::cerr << ": " << task.failure_message();
        }
        std::cerr << "\n";
    }

    bool using_cfg_fallback = false;
    if (!task.ast() || !task.ast()->root()) {
        using_cfg_fallback = true;
        auto fallback_ast = std::make_unique<dewolf::AbstractSyntaxForest>();
        auto* seq = task.arena().create<dewolf::SeqNode>();
        if (task.cfg()) {
            for (dewolf::BasicBlock* block : task.cfg()->blocks()) {
                seq->add_node(task.arena().create<dewolf::CodeNode>(block));
            }
        }
        fallback_ast->set_root(seq);
        task.set_ast(std::move(fallback_ast));
    }

    dewolf::InstructionLengthHandler::apply(task.ast(), task.arena());
    apply_variable_naming(task);

    if (using_cfg_fallback) {
        ok = true;
        return generate_cfg_fallback_code(task);
    }

    dewolf::CodeVisitor visitor;
    ok = true;
    return visitor.generate_code(task);
}

} // namespace

int main(int argc, char** argv) {
    CliOptions options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 1;
    }

    if (!detect_headless(options)) {
        std::cerr << "idump: headless mode not explicit; proceeding (set --headless or DEWOLF_HEADLESS=1).\n";
    }

    ida::database::RuntimeOptions runtime_options;
    runtime_options.quiet = true;
    auto init_res = ida::database::init(runtime_options);
    if (!init_res) {
        std::cerr << "idump: failed to initialize idalib runtime.\n";
        return 1;
    }

    auto open_res = ida::database::open(options.input_binary, ida::database::OpenMode::Analyze);
    if (!open_res) {
        std::cerr << "idump: failed to open input binary: " << options.input_binary << "\n";
        return 1;
    }

    std::filesystem::path output_path = options.output_path.empty()
        ? std::filesystem::path(options.input_binary).replace_extension(".c")
        : std::filesystem::path(options.output_path);

    std::ofstream out(output_path);
    if (!out.is_open()) {
        std::cerr << "idump: failed to open output file: " << output_path << "\n";
        ida::database::close(false);
        return 1;
    }

    dewolf_idioms::IdiomMatcher matcher;

    std::size_t total_functions = 0;
    std::size_t decompiled_functions = 0;

    for (const auto& fn : ida::function::all()) {
        ++total_functions;
        bool ok = false;
        std::string error_message;
        auto lines = decompile_function(fn.start(), matcher, ok, error_message);

        if (!ok) {
            out << "/* decompilation failed at " << std::hex << fn.start() << std::dec << ": "
                << error_message << " */\n\n";
            continue;
        }

        ++decompiled_functions;
        for (const auto& line : lines) {
            out << line << '\n';
        }
        out << '\n';
    }

    out.flush();
    ida::database::close(false);

    std::cerr << "idump: wrote " << decompiled_functions << "/" << total_functions
              << " functions to " << output_path << "\n";
    return 0;
}
