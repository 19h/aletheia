#include "codegen.hpp"

namespace dewolf {

std::string CExpressionGenerator::generate(DataflowObject* obj) {
    result_.clear();
    if (obj) {
        obj->accept(*this);
    }
    return result_;
}

void CExpressionGenerator::visit(Constant* c) {
    result_ = std::to_string(c->value());
}

void CExpressionGenerator::visit(Variable* v) {
    result_ = v->name();
}

void CExpressionGenerator::visit(Operation* o) {
    // Very basic serialization logic
    if (o->type() == OperationType::assign && o->operands().size() == 2) {
        std::string lhs = generate(o->operands()[0]);
        std::string rhs = generate(o->operands()[1]);
        result_ = lhs + " = " + rhs;
    } else {
        result_ = "op(...)";
    }
}

std::vector<std::string> CodeVisitor::generate_code(AbstractSyntaxForest* forest) {
    lines_.clear();
    current_line_.clear();
    indent_level_ = 0;

    if (forest && forest->root()) {
        visit_node(forest->root());
    }

    if (!current_line_.empty()) {
        lines_.push_back(current_line_);
    }

    return lines_;
}

void CodeVisitor::visit_node(AstNode* node) {
    if (!node) return;

    if (CodeNode* cnode = dynamic_cast<CodeNode*>(node)) {
        BasicBlock* block = cnode->block();
        if (block) {
            for (Instruction* inst : block->instructions()) {
                indent();
                current_line_ += expr_gen_.generate(inst->operation()) + ";";
                lines_.push_back(current_line_);
                current_line_.clear();
            }
        }
    } else if (SeqNode* snode = dynamic_cast<SeqNode*>(node)) {
        for (AstNode* child : snode->nodes()) {
            visit_node(child);
        }
    } else if (IfNode* inode = dynamic_cast<IfNode*>(node)) {
        indent();
        current_line_ += "if (...) {";
        lines_.push_back(current_line_);
        current_line_.clear();
        
        indent_level_++;
        // visit true branch
        indent_level_--;
        
        indent();
        current_line_ += "}";
        lines_.push_back(current_line_);
        current_line_.clear();
    }
    // and so on for LoopNode, CaseNode, SwitchNode, etc.
}

void CodeVisitor::indent() {
    current_line_ = std::string(indent_level_ * 4, ' ');
}

} // namespace dewolf
