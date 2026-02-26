#pragma once
#include "../structuring/ast.hpp"
#include "../structures/dataflow.hpp"
#include <string>
#include <vector>

namespace dewolf {

class CExpressionGenerator : public DataflowObjectVisitorInterface {
public:
    std::string generate(DataflowObject* obj);

    // Expression visitors
    void visit(Constant* c) override;
    void visit(Variable* v) override;
    void visit(Operation* o) override;
    void visit(Condition* c) override;

    // Instruction visitors
    void visit_assignment(Assignment* i) override;
    void visit_return(Return* i) override;
    void visit_phi(Phi* i) override;
    void visit_branch(Branch* i) override;
    void visit_break(BreakInstr* i) override;
    void visit_continue(ContinueInstr* i) override;
    void visit_comment(Comment* i) override;

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
