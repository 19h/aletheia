#pragma once
#include "../structures/dataflow.hpp"
#include "../structures/cfg.hpp"
#include "../structuring/ast.hpp"
#include <string>
#include <unordered_set>

namespace aletheia::debug {

// ---- Expression serialization ----
// visited: pointer-cycle / DAG detection
std::string ir_to_string(const Expression* expr,
                         std::unordered_set<const Expression*>* visited = nullptr);

// ---- Instruction serialization ----
std::string ir_to_string(const Instruction* inst);

// ---- Variable metadata dump ----
std::string variable_info(const Variable* var);

// ---- Enum-to-string helpers ----
const char* operation_type_name(OperationType type);
const char* variable_kind_name(VariableKind kind);
const char* edge_type_name(EdgeType type);
const char* edge_property_name(EdgeProperty prop);

// ---- CFG dump ----
std::string cfg_to_string(const ControlFlowGraph* cfg);

// ---- AST dump ----
std::string ast_to_string(const AbstractSyntaxForest* ast);
std::string ast_node_to_string(const AstNode* node, int indent = 0,
                                int max_depth = 64);

} // namespace aletheia::debug
