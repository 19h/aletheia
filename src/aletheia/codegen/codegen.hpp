#pragma once
#include "../structuring/ast.hpp"
#include "../structures/dataflow.hpp"
#include "../pipeline/pipeline.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace aletheia {

class CExpressionGenerator : public DataflowObjectVisitorInterface {
public:
    std::string generate(DataflowObject* obj);

    /// Set the parameter register -> display name mapping for parameter rendering.
    /// Key: lowercase register name (e.g., "rdi"), Value: display name (e.g., "a1").
    void set_parameter_names(const std::unordered_map<std::string, std::string>& map) {
        param_display_names_ = map;
    }

    // Expression visitors
    void visit(Constant* c) override;
    void visit(Variable* v) override;
    void visit(GlobalVariable* v) override;
    void visit(Operation* o) override;
    void visit(Call* c) override;
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
    /// Maps lowercase register name -> display name for parameter rendering.
    std::unordered_map<std::string, std::string> param_display_names_;
};

class CodeVisitor {
public:
    CodeVisitor() = default;

    std::vector<std::string> generate_code(DecompilerTask& task);
    
    // For backwards compatibility or simpler tests
    std::vector<std::string> generate_code(AbstractSyntaxForest* forest);

private:
    void visit_node(AstNode* node);
    void visit_if_chain(IfNode* inode, bool else_if_prefix);
    void indent();

    std::vector<std::string> lines_;
    std::string current_line_;
    int indent_level_ = 0;
    CExpressionGenerator expr_gen_;
};

} // namespace aletheia
