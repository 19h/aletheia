#include <ida/idax.hpp>
#include <aletheia.hpp>
#include "../aletheia/pipeline/pipeline.hpp"
#include "../aletheia/codegen/codegen.hpp"
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
#include <ida/ui.hpp>
#include <ida/database.hpp>
#include <ida/event.hpp>
#include <ida/lines.hpp>
#include <cstdlib>
#include <string_view>

struct AletheiaPlugin : ida::plugin::Plugin {
    ida::event::Token event_token_{0};

    AletheiaPlugin() {
        ida::ui::message("AletheiaPlugin initialized.\n");
        // Bind widget refresh to IDB patch events
        auto token_res = ida::event::on_byte_patched([this](ida::Address ea, std::uint32_t old_value) {
            ida::ui::message("Aletheia: Byte patched at " + std::to_string(ea) + ". Should re-trigger DecompilerTask.\n");
        });
        if (token_res) {
            event_token_ = std::move(*token_res);
        }
    }

    ida::plugin::Info info() const override {
        return {
            .name    = "Aletheia Decompiler",
            .hotkey  = "Ctrl-Shift-D",
            .comment = "Aletheia: native C++23 IDA decompiler (based on DeWolf research)",
            .help    = "Press Ctrl-Shift-D to decompile current function",
        };
    }

    ida::Status run(std::size_t) override {
        auto screen_ea_res = ida::ui::screen_address();
        if (!screen_ea_res) return std::unexpected(screen_ea_res.error());

        ida::Address ea = *screen_ea_res;

        ida::ui::message("Aletheia: Decompiling function at " + std::to_string(ea) + "...\n");

        aletheia::DecompilerTask task(ea);

        if (const char* mode_env = std::getenv("ALETHEIA_OUT_OF_SSA_MODE"); mode_env != nullptr) {
            auto parsed = aletheia::SsaDestructor::parse_mode(mode_env);
            if (parsed.has_value()) {
                task.set_out_of_ssa_mode(*parsed);
            } else {
                ida::ui::message(std::string("Aletheia: unknown ALETHEIA_OUT_OF_SSA_MODE='") + mode_env
                                 + "', using default lift_minimal.\n");
            }
        }

        idiomata::IdiomMatcher matcher;
        aletheia::Lifter lifter(task.arena(), matcher);
        std::vector<idiomata::IdiomTag> idiom_tags;

        lifter.populate_task_signature(task);

        auto cfg_res = lifter.lift_function(ea, &idiom_tags);
        if (!cfg_res) {
            ida::ui::warning("Failed to lift function.");
            return std::unexpected(cfg_res.error());
        }
        
        task.set_cfg(std::move(*cfg_res));
        task.set_idiom_tags(std::move(idiom_tags));

        aletheia::DecompilerPipeline pipeline;
        pipeline.add_stage(std::make_unique<aletheia::CompilerIdiomHandlingStage>());
        pipeline.add_stage(std::make_unique<aletheia::RegisterPairHandlingStage>());
        pipeline.add_stage(std::make_unique<aletheia::RemoveGoPrologueStage>());
        pipeline.add_stage(std::make_unique<aletheia::RemoveStackCanaryStage>());
        pipeline.add_stage(std::make_unique<aletheia::RemoveNoreturnBoilerplateStage>());
        pipeline.add_stage(std::make_unique<aletheia::InsertMissingDefinitionsStage>());
        pipeline.add_stage(std::make_unique<aletheia::SwitchVariableDetectionStage>());
        pipeline.add_stage(std::make_unique<aletheia::MemPhiConverterStage>());
        pipeline.add_stage(std::make_unique<aletheia::CoherenceStage>());
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
        pipeline.add_stage(std::make_unique<aletheia::PatternIndependentRestructuringStage>());
        
        pipeline.run(task);

        aletheia::InstructionLengthHandler::apply(task.ast(), task.arena());

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

        std::vector<std::string> lines;
        lines.push_back(ida::lines::colstr("// Aletheia Decompiled Output", ida::lines::Color::RegularComment));
        
        if (task.ast() && task.ast()->root()) {
            aletheia::CodeVisitor visitor;
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
            "Aletheia Decompiler",
            lines
        );
        if (!viewer_res) {
            ida::ui::warning("Failed to create Aletheia viewer.");
            return std::unexpected(viewer_res.error());
        }

        return ida::ok();
    }
};

IDAX_PLUGIN(AletheiaPlugin)
