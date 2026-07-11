#include "frontend.hpp"

#include "../lifter.hpp"

#if ALETHEIA_HAS_PCODE
#include "../pcode/pcode_lifter.hpp"
#endif

namespace aletheia {

namespace {

class NativeFrontend final : public Frontend {
public:
    NativeFrontend(DecompilerArena& arena, idiomata::IdiomMatcher& idiom_matcher)
        : lifter_(arena, idiom_matcher) {}

    FrontendKind kind() const override { return FrontendKind::Native; }

    void populate_task_signature(DecompilerTask& task) override {
        task.set_frontend_kind(FrontendKind::Native);
        lifter_.populate_task_signature(task);
    }

    ida::Result<std::unique_ptr<ControlFlowGraph>> lift_function(
        DecompilerTask& task,
        std::vector<idiomata::IdiomTag>* idiom_tags_out = nullptr) override {
        return lifter_.lift_function(task.function_address(), idiom_tags_out);
    }

private:
    Lifter lifter_;
};

} // namespace

bool frontend_is_available(FrontendKind kind) {
    switch (kind) {
        case FrontendKind::Native:
            return true;
        case FrontendKind::Pcode:
#if ALETHEIA_HAS_PCODE
            return pcode_runtime_available();
#else
            return false;
#endif
    }
    return false;
}

std::string frontend_unavailable_reason(FrontendKind kind) {
    switch (kind) {
        case FrontendKind::Native:
            return {};
        case FrontendKind::Pcode:
#if ALETHEIA_HAS_PCODE
            return pcode_runtime_unavailable_reason();
#else
            return "P-Code frontend was not built (Sleigh support unavailable)";
#endif
    }
    return "unknown frontend";
}

ida::Result<std::unique_ptr<Frontend>> create_frontend(
    FrontendKind kind,
    DecompilerArena& arena,
    idiomata::IdiomMatcher& idiom_matcher) {
    if (!frontend_is_available(kind)) {
        return std::unexpected(ida::Error::unsupported(
            "Requested frontend is unavailable",
            frontend_unavailable_reason(kind)));
    }

    switch (kind) {
        case FrontendKind::Native:
            return std::make_unique<NativeFrontend>(arena, idiom_matcher);
        case FrontendKind::Pcode:
#if ALETHEIA_HAS_PCODE
            return std::make_unique<PcodeLifter>(arena, idiom_matcher);
#else
            break;
#endif
    }

    return std::unexpected(ida::Error::unsupported(
        "Unknown frontend",
        frontend_kind_name_string(kind)));
}

} // namespace aletheia
