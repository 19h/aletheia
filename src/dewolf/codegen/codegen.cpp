#include "codegen.hpp"
#include <ida/lines.hpp>

namespace dewolf {

std::string CExpressionGenerator::generate(DataflowObject* obj) {
    result_.clear();
    if (obj) {
        obj->accept(*this);
    }
    return result_;
}

void CExpressionGenerator::visit(Constant* c) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)c->value());
    result_ = buf;
}

void CExpressionGenerator::visit(Variable* v) {
    std::string base_name = ida::lines::tag_remove(v->name());
    if (v->ssa_version() > 0) {
        result_ = base_name + "_" + std::to_string(v->ssa_version());
    } else {
        result_ = base_name;
    }
}

void CExpressionGenerator::visit(Operation* o) {
    if (o->type() == OperationType::add && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " + " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::sub && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " - " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::mul && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " * " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::div && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " / " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::deref && !o->operands().empty()) {
        result_ = "*(" + generate(o->operands()[0]) + ")";
    } else if (o->type() == OperationType::call && !o->operands().empty()) {
        std::string func = generate(o->operands()[0]);
        result_ = func + "(";
        for (size_t i = 1; i < o->operands().size(); ++i) {
            if (i > 1) result_ += ", ";
            result_ += generate(o->operands()[i]);
        }
        result_ += ")";
    } else if (o->type() == OperationType::unknown && !o->operands().empty()) {
        std::string func = generate(o->operands()[0]);
        result_ = func + "()";
    } else {
        result_ = "unknown_op";
    }
}

void CExpressionGenerator::visit(Condition* c) {
    std::string op_str = " == ";
    switch (c->type()) {
        case OperationType::eq:  op_str = " == "; break;
        case OperationType::neq: op_str = " != "; break;
        case OperationType::lt:  op_str = " < "; break;
        case OperationType::le:  op_str = " <= "; break;
        case OperationType::gt:  op_str = " > "; break;
        case OperationType::ge:  op_str = " >= "; break;
        default: op_str = " ?? "; break;
    }
    result_ = generate(c->lhs()) + op_str + generate(c->rhs());
}

void CExpressionGenerator::visit_assignment(Assignment* i) {
    std::string lhs = generate(i->destination());
    std::string rhs = generate(i->value());
    if (lhs.empty() || lhs == "unknown_op") {
        // Void call assignment: just print the RHS
        result_ = rhs;
    } else {
        result_ = lhs + " = " + rhs;
    }
}

void CExpressionGenerator::visit_return(Return* i) {
    if (i->has_value()) {
        result_ = "return " + generate(i->values()[0]);
    } else {
        result_ = "return";
    }
}

void CExpressionGenerator::visit_phi(Phi* i) {
    std::string dest = generate(i->dest_var());
    result_ = dest + " = phi(";
    if (i->operand_list()) {
        for (size_t j = 0; j < i->operand_list()->operands().size(); ++j) {
            if (j > 0) result_ += ", ";
            result_ += generate(i->operand_list()->operands()[j]);
        }
    }
    result_ += ")";
}

void CExpressionGenerator::visit_branch(Branch* i) {
    result_ = "if (" + generate(i->condition()) + ")";
}

void CExpressionGenerator::visit_break(BreakInstr* i) {
    result_ = "break";
}

void CExpressionGenerator::visit_continue(ContinueInstr* i) {
    result_ = "continue";
}

void CExpressionGenerator::visit_comment(Comment* i) {
    result_ = "/* " + i->message() + " */";
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
                std::string expr = expr_gen_.generate(inst);
                if (!expr.empty() && expr != "unknown_op") {
                    current_line_ += expr + ";";
                    lines_.push_back(current_line_);
                    current_line_.clear();
                }
            }
        }
    } else if (SeqNode* snode = dynamic_cast<SeqNode*>(node)) {
        for (AstNode* child : snode->nodes()) {
            visit_node(child);
        }
    } else if (IfNode* inode = dynamic_cast<IfNode*>(node)) {
        indent();
        std::string cond_str = "/* condition */";
        if (inode->cond()) {
            if (auto* expr_ast = dynamic_cast<ExprAstNode*>(inode->cond())) {
                cond_str = expr_gen_.generate(expr_ast->expr());
            }
        }
        current_line_ += "if (" + cond_str + ") {";
        lines_.push_back(current_line_);
        current_line_.clear();
        
        indent_level_++;
        visit_node(inode->true_branch());
        indent_level_--;
        
        if (inode->false_branch()) {
            indent();
            current_line_ += "} else {";
            lines_.push_back(current_line_);
            current_line_.clear();
            
            indent_level_++;
            visit_node(inode->false_branch());
            indent_level_--;
        }

        indent();
        current_line_ += "}";
        lines_.push_back(current_line_);
        current_line_.clear();
    } else if (LoopNode* lnode = dynamic_cast<LoopNode*>(node)) {
        indent();
        current_line_ += "while (true) {";
        lines_.push_back(current_line_);
        current_line_.clear();
        
        indent_level_++;
        visit_node(lnode->body());
        indent_level_--;

        indent();
        current_line_ += "}";
        lines_.push_back(current_line_);
        current_line_.clear();
    }
}

void CodeVisitor::indent() {
    current_line_ = std::string(indent_level_ * 4, ' ');
}

} // namespace dewolf
