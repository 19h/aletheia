#include <ida/database.hpp>
#include <ida/function.hpp>

#include "../aletheia/pipeline/pipeline.hpp"
#include "../aletheia/codegen/codegen.hpp"
#include "../aletheia/codegen/local_declarations.hpp"
#include "../aletheia/lifter.hpp"
#include "../aletheia/ssa/ssa_constructor.hpp"
#include "../aletheia/ssa/ssa_destructor.hpp"
#include "../aletheia/pipeline/preprocessing_stages.hpp"
#include "../aletheia/pipeline/optimization_stages.hpp"
#include "../aletheia/pipeline/expressions/graph_expression_folding.hpp"
#include "../aletheia/pipeline/dataflow_analysis/dead_code_elimination.hpp"
#include "../aletheia/structuring/structuring_stage.hpp"
#include "../aletheia/structuring/instruction_length_handler.hpp"
#include "../aletheia/structuring/variable_name_generation.hpp"
#include "../aletheia/structuring/loop_name_generator.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

bool env_var_present(const char* name) {
    return std::getenv(name) != nullptr;
}

bool should_enable_structuring() {
    if (env_flag_enabled("ALETHEIA_IDUMP_DISABLE_STRUCTURING")) {
        return false;
    }
    if (env_var_present("ALETHEIA_IDUMP_ENABLE_STRUCTURING")) {
        return env_flag_enabled("ALETHEIA_IDUMP_ENABLE_STRUCTURING");
    }
    return true;
}

bool ast_has_executable_content(aletheia::AstNode* node) {
    if (!node) {
        return false;
    }

    if (ast_dyn_cast<aletheia::ExprAstNode>(node) != nullptr) {
        return false;
    }

    if (auto* code = ast_dyn_cast<aletheia::CodeNode>(node)) {
        if (!code->block()) {
            return false;
        }
        for (aletheia::Instruction* inst : code->block()->instructions()) {
             if (!aletheia::isa<aletheia::Branch>(inst)
                && !aletheia::isa<aletheia::IndirectBranch>(inst)) {
                return true;
            }
        }
        return false;
    }

    if (auto* seq = ast_dyn_cast<aletheia::SeqNode>(node)) {
        for (aletheia::AstNode* child : seq->nodes()) {
            if (ast_has_executable_content(child)) {
                return true;
            }
        }
        return false;
    }

    if (auto* if_node = ast_dyn_cast<aletheia::IfNode>(node)) {
        return if_node->condition_expr() != nullptr
            || ast_has_executable_content(if_node->true_branch())
            || ast_has_executable_content(if_node->false_branch());
    }

    if (auto* loop = ast_dyn_cast<aletheia::LoopNode>(node)) {
        return loop->condition() != nullptr || ast_has_executable_content(loop->body());
    }

    if (auto* sw = ast_dyn_cast<aletheia::SwitchNode>(node)) {
        for (aletheia::CaseNode* case_node : sw->cases()) {
            if (ast_has_executable_content(case_node)) {
                return true;
            }
        }
        return false;
    }

    if (auto* case_node = ast_dyn_cast<aletheia::CaseNode>(node)) {
        return ast_has_executable_content(case_node->body());
    }

    // Conservative default for unknown AST node subclasses.
    return true;
}

std::string_view trim_ascii(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

std::size_t count_emitted_executable_lines(const std::vector<std::string>& lines) {
    std::size_t count = 0;
    for (const std::string& line : lines) {
        std::string_view view = trim_ascii(line);
        if (view.empty()) {
            continue;
        }
        if (view == "{" || view == "}" || view == "} else {") {
            continue;
        }
        if (view.starts_with("/*")) {
            continue;
        }
        if (view.ends_with(':')) {
            continue;
        }
        if (view.starts_with("if (") || view.starts_with("while (") || view.starts_with("for (")
            || view.starts_with("switch (") || view.starts_with("do {") || view.starts_with("else if (")) {
            ++count;
            continue;
        }
        if (view.find(';') != std::string_view::npos) {
            ++count;
        }
    }
    return count;
}

bool generated_output_too_lossy(std::size_t lifted_non_control_count, const std::vector<std::string>& lines) {
    if (lifted_non_control_count < 6) {
        return false;
    }
    const std::size_t emitted = count_emitted_executable_lines(lines);
    if (emitted == 0) {
        return true;
    }
    int goto_count = 0;
    for (const auto& line : lines) {
        if (line.find("goto bb_") != std::string::npos || line.find("/* branch if") != std::string::npos) goto_count++;
    }
    if (goto_count > 0 && goto_count * 5 > emitted) return true;
    if (goto_count > 10) return true;
    return emitted * 3 < lifted_non_control_count;
}

std::size_t count_cfg_non_control_instructions(const aletheia::ControlFlowGraph* cfg) {
    if (!cfg) {
        return 0;
    }
    std::size_t count = 0;
    for (aletheia::BasicBlock* block : cfg->blocks()) {
        if (!block) {
            continue;
        }
        for (aletheia::Instruction* inst : block->instructions()) {
             if (!aletheia::isa<aletheia::Branch>(inst)
                && !aletheia::isa<aletheia::IndirectBranch>(inst)) {
                ++count;
            }
        }
    }
    return count;
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
    if (env_flag_enabled("ALETHEIA_HEADLESS")) return true;
    return false;
}

void configure_out_of_ssa_mode(aletheia::DecompilerTask& task) {
    if (const char* mode_env = std::getenv("ALETHEIA_OUT_OF_SSA_MODE"); mode_env != nullptr) {
        auto parsed = aletheia::SsaDestructor::parse_mode(mode_env);
        if (parsed.has_value()) {
            task.set_out_of_ssa_mode(*parsed);
        }
    }
}

void apply_variable_naming(aletheia::DecompilerTask& task) {
    if (const char* naming_env = std::getenv("ALETHEIA_VARIABLE_NAMING"); naming_env != nullptr) {
        std::string_view scheme{naming_env};
        if (scheme == "system_hungarian") {
            aletheia::VariableNameGeneration::apply_system_hungarian(task.ast());
        } else {
            aletheia::VariableNameGeneration::apply_default(task.ast());
        }
    } else {
        aletheia::VariableNameGeneration::apply_default(task.ast());
    }
    aletheia::LoopNameGenerator::apply_for_loop_counters(task.ast());
    aletheia::LoopNameGenerator::apply_while_loop_counters(task.ast());
}

std::string block_label(const aletheia::BasicBlock* block) {
    return "bb_" + std::to_string(block ? block->id() : 0);
}

std::pair<aletheia::Edge*, aletheia::Edge*> pick_true_false_edges(aletheia::BasicBlock* block) {
    aletheia::Edge* true_edge = nullptr;
    aletheia::Edge* false_edge = nullptr;
    if (!block) {
        return {nullptr, nullptr};
    }

    for (aletheia::Edge* edge : block->successors()) {
        if (!edge) {
            continue;
        }
        if (edge->type() == aletheia::EdgeType::True && true_edge == nullptr) {
            true_edge = edge;
        } else if (edge->type() == aletheia::EdgeType::False && false_edge == nullptr) {
            false_edge = edge;
        }
    }

    if (!true_edge && !block->successors().empty()) {
        true_edge = block->successors().front();
    }
    if (!false_edge) {
        for (aletheia::Edge* edge : block->successors()) {
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
    aletheia::BasicBlock* block,
    aletheia::CExpressionGenerator& expr_gen,
    std::vector<std::string>& lines,
    int indent_level,
    int depth,
    std::unordered_set<aletheia::BasicBlock*>& path,
    std::unordered_set<aletheia::BasicBlock*>& inlined_blocks) {
    if (!block) {
        lines.push_back(indent_of(indent_level) + "/* unknown target */");
        return;
    }
    if (depth > 8) {
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
    aletheia::Instruction* tail = insts.empty() ? nullptr : insts.back();
    const bool tail_is_branch = aletheia::isa<aletheia::Branch>(tail);
    const bool tail_is_indirect = aletheia::isa<aletheia::IndirectBranch>(tail);

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

    if (auto* branch = aletheia::dyn_cast<aletheia::Branch>(tail)) {
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
    } else if (auto* indirect = aletheia::dyn_cast<aletheia::IndirectBranch>(tail)) {
        const bool constant_target = aletheia::dyn_cast<aletheia::Constant>(indirect->expression()) != nullptr;
        const bool single_successor = block->successors().size() == 1;
        if (!(constant_target && single_successor)) {
            lines.push_back(indent_of(indent_level) + "/* indirect branch " + expr_gen.generate(indirect->expression()) + " */");
        }
        for (aletheia::Edge* edge : block->successors()) {
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
        aletheia::BasicBlock* next = block->successors()[0] ? block->successors()[0]->target() : nullptr;
        if (next) {
            emit_inline_branch_snapshot(next, expr_gen, lines, indent_level, depth + 1, path, inlined_blocks);
        }
    }

    path.erase(block);
}

std::vector<std::string> generate_cfg_fallback_code(aletheia::DecompilerTask& task) {
    std::vector<std::string> lines;
    aletheia::CExpressionGenerator expr_gen;

    // Set up parameter display name mapping.
    {
        std::unordered_map<std::string, std::string> param_names;
        for (const auto& [reg, info] : task.parameter_registers()) {
            param_names[reg] = info.name;
        }
        expr_gen.set_parameter_names(param_names);
    }

    auto global_decls = aletheia::GlobalDeclarationGenerator::generate(task);
    for (const auto& decl : global_decls) {
        lines.push_back(decl);
    }
    if (!global_decls.empty()) {
        lines.push_back("");
    }

    std::string sig = "void ";
    if (task.function_type()) {
        if (auto* func_type = type_dyn_cast<aletheia::FunctionTypeDef>(task.function_type().get())) {
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
        if (auto* func_type = type_dyn_cast<aletheia::FunctionTypeDef>(task.function_type().get())) {
            const auto& params = func_type->parameters();
            // Build index -> name map from parameter_registers.
            std::unordered_map<int, std::string> index_to_name;
            for (const auto& [reg, info] : task.parameter_registers()) {
                index_to_name[info.index] = info.name;
            }
            for (std::size_t i = 0; i < params.size(); ++i) {
                if (i > 0) sig += ", ";
                auto it = index_to_name.find(static_cast<int>(i));
                std::string pname = (it != index_to_name.end() && !it->second.empty())
                    ? it->second
                    : "a" + std::to_string(i + 1);
                sig += params[i]->to_string() + " " + pname;
            }
        }
    }
    sig += ") {";
    lines.push_back(sig);

    auto decls = aletheia::LocalDeclarationGenerator::generate(task, expr_gen);
    for (const auto& decl : decls) {
        lines.push_back("    " + decl);
    }
    if (!decls.empty()) {
        lines.push_back("");
    }

    if (task.cfg()) {
        const auto blocks = task.cfg()->blocks();
        std::unordered_set<aletheia::BasicBlock*> inlined_blocks;
        aletheia::BasicBlock* entry = task.cfg()->entry_block();

        if (entry) {
            std::unordered_set<aletheia::BasicBlock*> path;
            emit_inline_branch_snapshot(entry, expr_gen, lines, 1, 0, path, inlined_blocks);
        }

        std::size_t omitted = 0;
        for (aletheia::BasicBlock* block : blocks) {
            if (block && !inlined_blocks.contains(block)) {
                ++omitted;
            }
        }
        if (omitted > 0) {
            lines.push_back("    /* detached blocks: " + std::to_string(omitted) + " */");
            lines.push_back("");

            for (aletheia::BasicBlock* block : blocks) {
                if (!block || inlined_blocks.contains(block)) {
                    continue;
                }

                lines.push_back("    /* detached " + block_label(block) + " */");

                const auto& insts = block->instructions();
                aletheia::Instruction* tail = insts.empty() ? nullptr : insts.back();
                const bool tail_is_branch = aletheia::isa<aletheia::Branch>(tail);
                const bool tail_is_indirect = aletheia::isa<aletheia::IndirectBranch>(tail);

                std::size_t limit = insts.size();
                if ((tail_is_branch || tail_is_indirect) && limit > 0) {
                    --limit;
                }

                for (std::size_t j = 0; j < limit; ++j) {
                    std::string stmt = expr_gen.generate(insts[j]);
                    if (!stmt.empty()) {
                        lines.push_back("        " + stmt + ";");
                    }
                }

                if (auto* branch = aletheia::dyn_cast<aletheia::Branch>(tail)) {
                    auto [true_edge, false_edge] = pick_true_false_edges(block);
                    const std::string cond = expr_gen.generate(branch->condition());
                    lines.push_back("        if (" + cond + ") {");
                    lines.push_back("            /* then -> " + block_label(true_edge ? true_edge->target() : nullptr) + " */");
                    lines.push_back("        } else {");
                    lines.push_back("            /* else -> " + block_label(false_edge ? false_edge->target() : nullptr) + " */");
                    lines.push_back("        }");
                } else if (auto* indirect = aletheia::dyn_cast<aletheia::IndirectBranch>(tail)) {
                    const bool constant_target = aletheia::dyn_cast<aletheia::Constant>(indirect->expression()) != nullptr;
                    const bool single_successor = block->successors().size() == 1;
                    if (!(constant_target && single_successor)) {
                        lines.push_back("        /* indirect branch " + expr_gen.generate(indirect->expression()) + " */");
                    }
                }

                lines.push_back("");
            }
        }
    }

    lines.push_back("}");
    return lines;
}

aletheia::DecompilerPipeline build_pipeline(bool enable_structuring) {
    aletheia::DecompilerPipeline pipeline;
    pipeline.add_stage(std::make_unique<aletheia::CompilerIdiomHandlingStage>());
    pipeline.add_stage(std::make_unique<aletheia::RegisterPairHandlingStage>());
    pipeline.add_stage(std::make_unique<aletheia::RemoveGoPrologueStage>());
    pipeline.add_stage(std::make_unique<aletheia::RemoveStackCanaryStage>());
    pipeline.add_stage(std::make_unique<aletheia::RemoveNoreturnBoilerplateStage>());
    pipeline.add_stage(std::make_unique<aletheia::SwitchVariableDetectionStage>());
    pipeline.add_stage(std::make_unique<aletheia::CoherenceStage>());

    if (!enable_structuring) {
        pipeline.add_stage(std::make_unique<aletheia::GraphExpressionFoldingStage>());
        pipeline.add_stage(std::make_unique<aletheia::ExpressionSimplificationStage>());
        return pipeline;
    }

    pipeline.add_stage(std::make_unique<aletheia::InsertMissingDefinitionsStage>());
    pipeline.add_stage(std::make_unique<aletheia::MemPhiConverterStage>());
    pipeline.add_stage(std::make_unique<aletheia::SsaConstructor>());
    pipeline.add_stage(std::make_unique<aletheia::GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadComponentPrunerStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionPropagationStage>());
    pipeline.add_stage(std::make_unique<aletheia::BitFieldComparisonUnrollingStage>());
    pipeline.add_stage(std::make_unique<aletheia::TypePropagationStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadPathEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadLoopEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionPropagationMemoryStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionPropagationFunctionCallStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadCodeEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::RedundantCastsEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::IdentityEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::CommonSubexpressionEliminationStage>());
    pipeline.add_stage(std::make_unique<aletheia::ArrayAccessDetectionStage>());
    pipeline.add_stage(std::make_unique<aletheia::ExpressionSimplificationStage>());
    pipeline.add_stage(std::make_unique<aletheia::DeadComponentPrunerStage>());
    pipeline.add_stage(std::make_unique<aletheia::GraphExpressionFoldingStage>());
    pipeline.add_stage(std::make_unique<aletheia::EdgePrunerStage>());
    pipeline.add_stage(std::make_unique<aletheia::PhiFunctionFixerStage>());
    pipeline.add_stage(std::make_unique<aletheia::SsaDestructor>());
    pipeline.add_stage(std::make_unique<aletheia::EmptyBasicBlockRemoverStage>());
    pipeline.add_stage(std::make_unique<aletheia::PatternIndependentRestructuringStage>());
    pipeline.add_stage(std::make_unique<aletheia::AstExpressionSimplificationStage>());
    return pipeline;
}

std::optional<std::vector<std::string>> regenerate_conservative_fallback(
    ida::Address ea,
    idiomata::IdiomMatcher& matcher) {
    aletheia::DecompilerTask fallback_task(ea);
    configure_out_of_ssa_mode(fallback_task);

    aletheia::Lifter fallback_lifter(fallback_task.arena(), matcher);
    std::vector<idiomata::IdiomTag> idiom_tags;

    fallback_lifter.populate_task_signature(fallback_task);
    auto cfg_res = fallback_lifter.lift_function(ea, &idiom_tags);
    if (!cfg_res) {
        return std::nullopt;
    }

    fallback_task.set_cfg(std::move(*cfg_res));
    fallback_task.set_idiom_tags(std::move(idiom_tags));

    auto fallback_pipeline = build_pipeline(false);
    fallback_pipeline.run(fallback_task);

    auto fallback_ast = std::make_unique<aletheia::AbstractSyntaxForest>();
    auto* seq = fallback_task.arena().create<aletheia::SeqNode>();
    if (fallback_task.cfg()) {
        for (aletheia::BasicBlock* block : fallback_task.cfg()->blocks()) {
            seq->add_node(fallback_task.arena().create<aletheia::CodeNode>(block));
        }
    }
    fallback_ast->set_root(seq);
    fallback_task.set_ast(std::move(fallback_ast));

    aletheia::InstructionLengthHandler::apply(fallback_task.ast(), fallback_task.arena());
    if (env_flag_enabled("ALETHEIA_IDUMP_RENAME_FALLBACK")) {
        apply_variable_naming(fallback_task);
    }

    return generate_cfg_fallback_code(fallback_task);
}

std::vector<std::string> decompile_function(
    ida::Address ea,
    idiomata::IdiomMatcher& matcher,
    bool& ok,
    std::string& error_message) {
    ok = false;
    error_message.clear();

    aletheia::DecompilerTask task(ea);
    configure_out_of_ssa_mode(task);

    aletheia::Lifter lifter(task.arena(), matcher);
    std::vector<idiomata::IdiomTag> idiom_tags;

    lifter.populate_task_signature(task);

    auto cfg_res = lifter.lift_function(ea, &idiom_tags);
    if (!cfg_res) {
        error_message = "lift failed";
        return {};
    }

    const std::size_t lifted_non_control_count = count_cfg_non_control_instructions((*cfg_res).get());

    task.set_cfg(std::move(*cfg_res));
    task.set_idiom_tags(std::move(idiom_tags));

    const bool enable_structuring = should_enable_structuring();
    const bool force_structured_output = env_flag_enabled("ALETHEIA_IDUMP_FORCE_STRUCTURED_OUTPUT");
    auto pipeline = build_pipeline(enable_structuring);
    pipeline.run(task);

    if (task.failed()) {
        std::cerr << "idump: pipeline stopped at stage '" << task.failure_stage() << "'";
        if (!task.failure_message().empty()) {
            std::cerr << ": " << task.failure_message();
        }
        std::cerr << "\n";
    }

    bool using_cfg_fallback = !task.ast() || !task.ast()->root();
    if (!using_cfg_fallback && enable_structuring && !ast_has_executable_content(task.ast()->root())) {
        using_cfg_fallback = true;
    }

    if (using_cfg_fallback) {
        auto fallback_ast = std::make_unique<aletheia::AbstractSyntaxForest>();
        auto* seq = task.arena().create<aletheia::SeqNode>();
        if (task.cfg()) {
            for (aletheia::BasicBlock* block : task.cfg()->blocks()) {
                seq->add_node(task.arena().create<aletheia::CodeNode>(block));
            }
        }
        fallback_ast->set_root(seq);
        task.set_ast(std::move(fallback_ast));
    }

    if (using_cfg_fallback) {
        if (enable_structuring && !force_structured_output) {
            if (auto rebuilt = regenerate_conservative_fallback(ea, matcher); rebuilt.has_value()) {
                ok = true;
                return *rebuilt;
            }
        }

        aletheia::InstructionLengthHandler::apply(task.ast(), task.arena());
        if (env_flag_enabled("ALETHEIA_IDUMP_RENAME_FALLBACK")) {
            apply_variable_naming(task);
        }
        ok = true;
        return generate_cfg_fallback_code(task);
    }

    aletheia::InstructionLengthHandler::apply(task.ast(), task.arena());
    apply_variable_naming(task);

    aletheia::CodeVisitor visitor;
    auto structured_lines = visitor.generate_code(task);

    if (enable_structuring && !force_structured_output
        && generated_output_too_lossy(lifted_non_control_count, structured_lines)) {
        if (ea == 0) {
            std::cerr << "STRUCTURED LINES FOR FUNC 0:" << std::endl;
            for (const auto& l : structured_lines) {
                std::cerr << "  " << l << std::endl;
            }
        }
        std::cerr << "FALLING BACK FOR FUNCTION " << std::hex << ea << std::dec << std::endl;
        if (auto rebuilt = regenerate_conservative_fallback(ea, matcher); rebuilt.has_value()) {
            ok = true;
            return *rebuilt;
        }
    }

    ok = true;
    return structured_lines;
}

} // namespace

int main(int argc, char** argv) {
    CliOptions options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return 1;
    }

    if (!detect_headless(options)) {
        std::cerr << "idump: headless mode not explicit; proceeding (set --headless or ALETHEIA_HEADLESS=1).\n";
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

    idiomata::IdiomMatcher matcher;

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
