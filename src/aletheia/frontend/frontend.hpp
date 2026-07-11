#pragma once

#include <ida/idax.hpp>

#include "../pipeline/pipeline.hpp"
#include "../structures/cfg.hpp"
#include "../../idiomata/idioms.hpp"

#include <memory>
#include <string>

namespace aletheia {

class Frontend {
public:
    virtual ~Frontend() = default;

    virtual FrontendKind kind() const = 0;
    virtual void populate_task_signature(DecompilerTask& task) = 0;
    virtual ida::Result<std::unique_ptr<ControlFlowGraph>> lift_function(
        DecompilerTask& task,
        std::vector<idiomata::IdiomTag>* idiom_tags_out = nullptr) = 0;
};

bool frontend_is_available(FrontendKind kind);
std::string frontend_unavailable_reason(FrontendKind kind);
ida::Result<std::unique_ptr<Frontend>> create_frontend(
    FrontendKind kind,
    DecompilerArena& arena,
    idiomata::IdiomMatcher& idiom_matcher);

} // namespace aletheia
