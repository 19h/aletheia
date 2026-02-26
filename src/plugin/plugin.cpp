#include <ida/idax.hpp>
#include <dewolf.hpp>

struct DeWolfPlugin : ida::plugin::Plugin {
    DeWolfPlugin() {
        ida::ui::message("DeWolfPlugin initialized.\n");
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
        ida::ui::message("DeWolf decompilation not yet implemented.\n");
        return ida::ok();
    }
};

IDAX_PLUGIN(DeWolfPlugin)
