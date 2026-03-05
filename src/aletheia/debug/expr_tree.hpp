#pragma once
#include "../structures/dataflow.hpp"
#include <string>
#include <cstddef>

namespace aletheia::debug {

// Tree-formatted expression dump, one node per line with indentation
std::string expr_tree(const Expression* expr, std::size_t max_depth = 64);

// Tree-formatted with self-reference detection (for Assignment context)
std::string expr_tree_with_dest(const Expression* expr,
                                const Variable* assignment_dest,
                                std::size_t max_depth = 64);

// Expression metrics
std::size_t expression_depth(const Expression* expr);
std::size_t expression_weight(const Expression* expr);

// Detect SEMANTIC self-referencing expressions.
// Given an assignment dst=src, checks if any leaf Variable in src
// has the same name() AND ssa_version() as dst.
bool has_self_reference(const Expression* src, const Variable* dst);

} // namespace aletheia::debug
