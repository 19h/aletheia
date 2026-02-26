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
    result_ = ida::lines::tag_remove(v->name());
}

void CExpressionGenerator::visit(Operation* o) {
    if (o->type() == OperationType::assign && o->operands().size() == 2) {
        std::string lhs = generate(o->operands()[0]);
        std::string rhs = generate(o->operands()[1]);
        result_ = lhs + " = " + rhs;
    } else if (o->type() == OperationType::add && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " + " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::sub && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " - " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::mul && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " * " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::div && o->operands().size() == 2) {
        result_ = generate(o->operands()[0]) + " / " + generate(o->operands()[1]);
    } else if (o->type() == OperationType::deref && !o->operands().empty()) {
        result_ = "*(" + generate(o->operands()[0]) + ")";
    } else if (o->type() >= OperationType::eq && o->type() <= OperationType::ge && o->operands().size() == 2) {
        std::string op_str = " == ";
        if (o->type() == OperationType::neq) op_str = " != ";
        if (o->type() == OperationType::lt) op_str = " < ";
        if (o->type() == OperationType::le) op_str = " <= ";
        if (o->type() == OperationType::gt) op_str = " > ";
        if (o->type() == OperationType::ge) op_str = " >= ";
        result_ = generate(o->operands()[0]) + op_str + generate(o->operands()[1]);
    } else if (o->type() == OperationType::call && !o->operands().empty()) {
        std::string func = generate(o->operands()[0]);
        if (func == "ret") {
            result_ = "return";
            if (o->operands().size() > 1) {
                result_ += " " + generate(o->operands()[1]);
            }
        } else {
            result_ = func + "()";
        }
    } else if (o->type() == OperationType::unknown && !o->operands().empty()) {
        std::string func = generate(o->operands()[0]);
        result_ = func + "()";
    } else {
        // Unary op logic like b.le() -> ble flag
        if (o->type() == OperationType::le) result_ = "FLAG <= 0";
        else if (o->type() == OperationType::lt) result_ = "FLAG < 0";
        else if (o->type() == OperationType::ge) result_ = "FLAG >= 0";
        else if (o->type() == OperationType::gt) result_ = "FLAG > 0";
        else if (o->type() == OperationType::eq) result_ = "FLAG == 0";
        else if (o->type() == OperationType::neq) result_ = "FLAG != 0";
        else result_ = "unknown_op";
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
                std::string expr = expr_gen_.generate(inst->operation());
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
