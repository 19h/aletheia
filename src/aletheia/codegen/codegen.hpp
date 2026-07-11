#pragma once
#include "../structuring/ast.hpp"
#include "../structures/dataflow.hpp"
#include "../pipeline/pipeline.hpp"
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace aletheia {

struct CCodegenOptions {
    /// Emit ISO/GNU C expressions with explicit integer-address memory casts
    /// and translation-unit support hooks. The default remains readable
    /// pseudocode for the interactive decompiler view.
    bool portable_c = false;
};

class CExpressionGenerator : public DataflowObjectVisitorInterface {
public:
    explicit CExpressionGenerator(CCodegenOptions options = {})
        : options_(options) {}

    std::string generate(DataflowObject* obj);
    void reset_diagnostics() { unresolved_semantics_ = false; }
    bool has_unresolved_semantics() const { return unresolved_semantics_; }

    /// Set the parameter register -> display name mapping for parameter rendering.
    /// Key: lowercase register name (e.g., "rdi"), Value: display name (e.g., "a1").
    void set_parameter_names(const std::unordered_map<std::string, std::string>& map) {
        param_display_names_ = map;
    }
    void set_parameter_types(const std::unordered_map<std::string, TypePtr>& map) {
        param_declared_types_ = map;
    }
    void set_local_declared_type(std::string name, TypePtr type) {
        local_declared_types_[std::move(name)] = std::move(type);
    }

    // Expression visitors
    void visit(Constant* c) override;
    void visit(Variable* v) override;
    void visit(GlobalVariable* v) override;
    void visit(Operation* o) override;
    void visit(ListOperation* o) override;
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
    std::unordered_set<const DataflowObject*> active_nodes_;
    /// Maps lowercase register name -> display name for parameter rendering.
    std::unordered_map<std::string, std::string> param_display_names_;
    std::unordered_map<std::string, TypePtr> param_declared_types_;
    std::unordered_map<std::string, TypePtr> local_declared_types_;
    CCodegenOptions options_;
    bool unresolved_semantics_ = false;
};

class CodeVisitor {
public:
    explicit CodeVisitor(CCodegenOptions options = {})
        : options_(options), expr_gen_(options) {}

    std::vector<std::string> generate_code(DecompilerTask& task);
    
    // For backwards compatibility or simpler tests
    std::vector<std::string> generate_code(AbstractSyntaxForest* forest);
    bool has_unresolved_semantics() const {
        return expr_gen_.has_unresolved_semantics();
    }

private:
    void visit_node(AstNode* node);
    void visit_if_chain(IfNode* inode, bool else_if_prefix);
    bool node_emits_code(AstNode* node);
    void indent();

    std::vector<std::string> lines_;
    std::string current_line_;
    int indent_level_ = 0;
    bool cfg_fallback_mode_ = false;
    CCodegenOptions options_;
    std::unordered_set<BasicBlock*> emitted_blocks_;
    CExpressionGenerator expr_gen_;
};

} // namespace aletheia
