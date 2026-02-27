#include <ida/idax.hpp>
#include <dewolf.hpp>
#include "../dewolf/pipeline/pipeline.hpp"
#include "../dewolf/codegen/codegen.hpp"
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
#include <ida/ui.hpp>
#include <ida/database.hpp>
#include <ida/event.hpp>
#include <ida/lines.hpp>
#include <cstdlib>
#include <string_view>

struct DeWolfPlugin : ida::plugin::Plugin {
    ida::event::Token event_token_{0};

    DeWolfPlugin() {
        ida::ui::message("DeWolfPlugin initialized.\n");
        // Bind widget refresh to IDB patch events
        auto token_res = ida::event::on_byte_patched([this](ida::Address ea, std::uint32_t old_value) {
            ida::ui::message("DeWolf: Byte patched at " + std::to_string(ea) + ". Should re-trigger DecompilerTask.\n");
        });
        if (token_res) {
            event_token_ = std::move(*token_res);
        }
    }

    ida::plugin::Info info() const override {
        return {
            .name    = "DeWolf Decompiler",
            .hotkey  = "Ctrl-Shift-D",
            .comment = "Native C++ port of DeWolf decompiler",
            .help    = "Press Ctrl-Shift-D to decompile current function",
        };
    }

    ida::Status run(std::size_t) override {
        auto screen_ea_res = ida::ui::screen_address();
        if (!screen_ea_res) return std::unexpected(screen_ea_res.error());

        ida::Address ea = *screen_ea_res;

        ida::ui::message("DeWolf: Decompiling function at " + std::to_string(ea) + "...\n");

        dewolf::DecompilerTask task(ea);

        if (const char* mode_env = std::getenv("DEWOLF_OUT_OF_SSA_MODE"); mode_env != nullptr) {
            auto parsed = dewolf::SsaDestructor::parse_mode(mode_env);
            if (parsed.has_value()) {
                task.set_out_of_ssa_mode(*parsed);
            } else {
                ida::ui::message(std::string("DeWolf: unknown DEWOLF_OUT_OF_SSA_MODE='") + mode_env
                                 + "', using default lift_minimal.\n");
            }
        }

        dewolf_idioms::IdiomMatcher matcher;
        dewolf::Lifter lifter(task.arena(), matcher);
        std::vector<dewolf_idioms::IdiomTag> idiom_tags;

        lifter.populate_task_signature(task);

        auto cfg_res = lifter.lift_function(ea, &idiom_tags);
        if (!cfg_res) {
            ida::ui::warning("Failed to lift function.");
            return std::unexpected(cfg_res.error());
        }
        
        task.set_cfg(std::move(*cfg_res));
        task.set_idiom_tags(std::move(idiom_tags));

        dewolf::DecompilerPipeline pipeline;
        pipeline.add_stage(std::make_unique<dewolf::CompilerIdiomHandlingStage>());
        pipeline.add_stage(std::make_unique<dewolf::RegisterPairHandlingStage>());
        pipeline.add_stage(std::make_unique<dewolf::RemoveGoPrologueStage>());
        pipeline.add_stage(std::make_unique<dewolf::RemoveStackCanaryStage>());
        pipeline.add_stage(std::make_unique<dewolf::RemoveNoreturnBoilerplateStage>());
        pipeline.add_stage(std::make_unique<dewolf::InsertMissingDefinitionsStage>());
        pipeline.add_stage(std::make_unique<dewolf::SwitchVariableDetectionStage>());
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
        pipeline.add_stage(std::make_unique<dewolf::PatternIndependentRestructuringStage>());
        
        pipeline.run(task);

        dewolf::InstructionLengthHandler::apply(task.ast(), task.arena());

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

        std::vector<std::string> lines;
        lines.push_back(ida::lines::colstr("// DeWolf Decompiled Output", ida::lines::Color::RegularComment));
        
        if (task.ast() && task.ast()->root()) {
            dewolf::CodeVisitor visitor;
            auto code_lines = visitor.generate_code(task);
            for (const auto& line : code_lines) {
                // TODO: proper syntax highlighting
                lines.push_back(line);
            }
        } else {
            lines.push_back(ida::lines::colstr("// Structuring failed to generate AST.", ida::lines::Color::Error));
        }

        // UI Integration
        auto viewer_res = ida::ui::create_custom_viewer(
            "DeWolf Decompiler",
            lines
        );
        if (!viewer_res) {
            ida::ui::warning("Failed to create DeWolf viewer.");
            return std::unexpected(viewer_res.error());
        }

        return ida::ok();
    }
};

IDAX_PLUGIN(DeWolfPlugin)
