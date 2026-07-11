#pragma once

#include <ida/idax.hpp>

#include "../frontend/frontend.hpp"

namespace aletheia {

class PcodeLifter final : public Frontend {
public:
    PcodeLifter(DecompilerArena& arena, idiomata::IdiomMatcher& idiom_matcher);

    FrontendKind kind() const override { return FrontendKind::Pcode; }
    void populate_task_signature(DecompilerTask& task) override;
    ida::Result<std::unique_ptr<ControlFlowGraph>> lift_function(
        DecompilerTask& task,
        std::vector<idiomata::IdiomTag>* idiom_tags_out = nullptr) override;

private:
    DecompilerArena& arena_;
    idiomata::IdiomMatcher& idiom_matcher_;
};

bool pcode_runtime_available();
std::string pcode_runtime_unavailable_reason();

} // namespace aletheia
