#pragma once
#include "../structuring/ast.hpp"
#include "../structures/dataflow.hpp"
#include <string>
#include <vector>

namespace dewolf {

class CExpressionGenerator : public DataflowObjectVisitorInterface {
public:
    std::string generate(DataflowObject* obj);

    void visit(Constant* c) override;
    void visit(Variable* v) override;
    void visit(Operation* o) override;

private:
    std::string result_;
};

class CodeVisitor {
public:
    CodeVisitor() = default;

    std::vector<std::string> generate_code(AbstractSyntaxForest* forest);

private:
    void visit_node(AstNode* node);
    void indent();

    std::vector<std::string> lines_;
    std::string current_line_;
    int indent_level_ = 0;
    CExpressionGenerator expr_gen_;
};

} // namespace dewolf
