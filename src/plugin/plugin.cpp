#include <ida/idax.hpp>
#include <dewolf.hpp>
#include "../dewolf/pipeline/pipeline.hpp"
#include "../dewolf/codegen/codegen.hpp"
#include <ida/ui.hpp>
#include <ida/database.hpp>
#include <ida/event.hpp>
#include <ida/lines.hpp>

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

        // Stub out full pipeline run
        ida::ui::message("DeWolf: Decompiling function at " + std::to_string(ea) + "...\n");

        std::vector<std::string> lines;
        lines.push_back(ida::lines::colstr("// DeWolf Decompiled Output", ida::lines::Color::RegularComment));
        lines.push_back(ida::lines::colstr(std::string("// Function: ") + std::to_string(ea), ida::lines::Color::RegularComment));
        lines.push_back(ida::lines::colstr("void ", ida::lines::Color::Keyword) + ida::lines::colstr("func", ida::lines::Color::CodeName) + ida::lines::colstr("() {", ida::lines::Color::Symbol));
        lines.push_back(ida::lines::colstr("    // TODO: Connect pipeline and codegen here", ida::lines::Color::RegularComment));
        lines.push_back(ida::lines::colstr("}", ida::lines::Color::Symbol));

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

