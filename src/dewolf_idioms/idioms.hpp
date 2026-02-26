#pragma once
#include <ida/idax.hpp>
#include <ida/instruction.hpp>
#include <ida/graph.hpp>
#include <vector>
#include <string>

namespace dewolf_idioms {

struct IdiomTag {
    ida::Address address;
    std::string tag_name;
    std::string metadata;
};

class IdiomMatcher {
public:
    IdiomMatcher() = default;

    std::vector<IdiomTag> match_block(const ida::graph::BasicBlock& block);

private:
    void match_magic_division(const ida::graph::BasicBlock& block, std::vector<IdiomTag>& tags);
};

} // namespace dewolf_idioms
