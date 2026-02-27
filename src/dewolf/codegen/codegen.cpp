#include "codegen.hpp"
#include "local_declarations.hpp"
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
    auto type = o->type();
    auto& ops = o->operands();

    // --- Binary infix operators ---
    if (ops.size() == 2) {
        const char* infix = nullptr;
        switch (type) {
            // Arithmetic (signed)
            case OperationType::add:
            case OperationType::add_with_carry:
            case OperationType::add_float:      infix = " + "; break;
            case OperationType::sub:
            case OperationType::sub_with_carry:
            case OperationType::sub_float:      infix = " - "; break;
            case OperationType::mul:
            case OperationType::mul_us:
            case OperationType::mul_float:      infix = " * "; break;
            case OperationType::div:
            case OperationType::div_us:
            case OperationType::div_float:      infix = " / "; break;
            case OperationType::mod:
            case OperationType::mod_us:         infix = " % "; break;
            case OperationType::power:          infix = " ** "; break;
            // Bitwise
            case OperationType::bit_and:        infix = " & "; break;
            case OperationType::bit_or:         infix = " | "; break;
            case OperationType::bit_xor:        infix = " ^ "; break;
            // Shifts
            case OperationType::shl:            infix = " << "; break;
            case OperationType::shr:
            case OperationType::shr_us:
            case OperationType::sar:            infix = " >> "; break;
            // Logical
            case OperationType::logical_and:    infix = " && "; break;
            case OperationType::logical_or:     infix = " || "; break;
            // Comparison (signed)
            case OperationType::eq:             infix = " == "; break;
            case OperationType::neq:            infix = " != "; break;
            case OperationType::lt:
            case OperationType::lt_us:          infix = " < "; break;
            case OperationType::le:
            case OperationType::le_us:          infix = " <= "; break;
            case OperationType::gt:
            case OperationType::gt_us:          infix = " > "; break;
            case OperationType::ge:
            case OperationType::ge_us:          infix = " >= "; break;
            // Carry addition
            case OperationType::adc:            infix = " + "; break;
            default: break;
        }
        if (infix) {
            result_ = generate(ops[0]) + infix + generate(ops[1]);
            return;
        }
    }

    // --- Unary prefix operators ---
    if (ops.size() == 1) {
        switch (type) {
            case OperationType::negate:
                result_ = "-(" + generate(ops[0]) + ")";
                return;
            case OperationType::bit_not:
                result_ = "~(" + generate(ops[0]) + ")";
                return;
            case OperationType::logical_not:
                result_ = "!(" + generate(ops[0]) + ")";
                return;
            case OperationType::deref:
                result_ = "*(" + generate(ops[0]) + ")";
                return;
            case OperationType::address_of:
                result_ = "&(" + generate(ops[0]) + ")";
                return;
            case OperationType::low:
                result_ = "(uint8_t)(" + generate(ops[0]) + ")";
                return;
            case OperationType::cast:
                // cast with 1 operand: type info should come from the Operation's ir_type
                if (o->ir_type()) {
                    result_ = "(" + o->ir_type()->to_string() + ")" + generate(ops[0]);
                } else {
                    result_ = "(cast)" + generate(ops[0]);
                }
                return;
            case OperationType::pointer:
                result_ = "&" + generate(ops[0]);
                return;
            default: break;
        }
    }

    // --- Ternary: a ? b : c ---
    if (type == OperationType::ternary && ops.size() == 3) {
        result_ = "(" + generate(ops[0]) + " ? " + generate(ops[1]) + " : " + generate(ops[2]) + ")";
        return;
    }

    // --- Member access: a.b or a->b ---
    if (type == OperationType::member_access && ops.size() == 2) {
        result_ = generate(ops[0]) + "." + generate(ops[1]);
        return;
    }

    // --- Field access by offset ---
    if (type == OperationType::field && ops.size() == 2) {
        result_ = generate(ops[0]) + "." + generate(ops[1]);
        return;
    }

    // --- Rotates: emit as function-style ---
    if (ops.size() == 2) {
        const char* rot_name = nullptr;
        switch (type) {
            case OperationType::left_rotate:        rot_name = "__ROL__"; break;
            case OperationType::right_rotate:       rot_name = "__ROR__"; break;
            case OperationType::left_rotate_carry:  rot_name = "__RCL__"; break;
            case OperationType::right_rotate_carry: rot_name = "__RCR__"; break;
            default: break;
        }
        if (rot_name) {
            result_ = std::string(rot_name) + "(" + generate(ops[0]) + ", " + generate(ops[1]) + ")";
            return;
        }
    }

    // --- Function call (legacy: plain Operation with call type, not a Call object) ---
    if (type == OperationType::call && !ops.empty()) {
        std::string func = generate(ops[0]);
        result_ = func + "(";
        for (size_t i = 1; i < ops.size(); ++i) {
            if (i > 1) result_ += ", ";
            result_ += generate(ops[i]);
        }
        result_ += ")";
        return;
    }

    // --- List operation ---
    if (type == OperationType::list_op) {
        result_.clear();
        for (size_t i = 0; i < ops.size(); ++i) {
            if (i > 0) result_ += ", ";
            result_ += generate(ops[i]);
        }
        return;
    }

    // --- Unknown with operands (likely a function call from lifter) ---
    if (type == OperationType::unknown && !ops.empty()) {
        std::string func = generate(ops[0]);
        result_ = func + "()";
        return;
    }

    // --- Fallback ---
    result_ = "unknown_op";
}

void CExpressionGenerator::visit(Call* c) {
    std::string func = generate(c->target());
    result_ = func + "(";
    for (size_t i = 0; i < c->arg_count(); ++i) {
        if (i > 0) result_ += ", ";
        result_ += generate(c->arg(i));
    }
    result_ += ")";
}

void CExpressionGenerator::visit(Condition* c) {
    std::string op_str = " == ";
    switch (c->type()) {
        case OperationType::eq:    op_str = " == "; break;
        case OperationType::neq:   op_str = " != "; break;
        case OperationType::lt:
        case OperationType::lt_us: op_str = " < "; break;
        case OperationType::le:
        case OperationType::le_us: op_str = " <= "; break;
        case OperationType::gt:
        case OperationType::gt_us: op_str = " > "; break;
        case OperationType::ge:
        case OperationType::ge_us: op_str = " >= "; break;
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

std::vector<std::string> CodeVisitor::generate_code(DecompilerTask& task) {
    lines_.clear();
    current_line_.clear();
    indent_level_ = 0;

    // Generate function signature
    std::string sig = "void "; // default
    if (task.function_type()) {
        if (auto* func_type = dynamic_cast<const FunctionTypeDef*>(task.function_type().get())) {
            sig = func_type->return_type()->to_string() + " ";
        } else {
            sig = task.function_type()->to_string() + " ";
        }
    }

    std::string name = task.function_name().empty() ? "sub_" + std::to_string(task.function_address()) : task.function_name();
    sig += name + "(";

    if (task.function_type()) {
        if (auto* func_type = dynamic_cast<const FunctionTypeDef*>(task.function_type().get())) {
            const auto& params = func_type->parameters();
            for (size_t i = 0; i < params.size(); ++i) {
                if (i > 0) sig += ", ";
                sig += params[i]->to_string() + " a" + std::to_string(i + 1); // a1, a2, ...
            }
        }
    }
    sig += ") {";

    lines_.push_back(sig);
    indent_level_++;

    auto decls = LocalDeclarationGenerator::generate(task, expr_gen_);
    for (const auto& decl : decls) {
        indent();
        lines_.push_back(current_line_ + decl);
        current_line_.clear();
    }
    
    if (!decls.empty()) {
        lines_.push_back(""); // empty line after declarations
    }

    if (task.ast() && task.ast()->root()) {
        visit_node(task.ast()->root());
    }

    if (!current_line_.empty()) {
        lines_.push_back(current_line_);
        current_line_.clear();
    }

    indent_level_--;
    lines_.push_back("}");

    return lines_;
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
    } else if (auto* dowhile = dynamic_cast<DoWhileLoopNode*>(node)) {
        indent();
        current_line_ += "do {";
        lines_.push_back(current_line_);
        current_line_.clear();

        indent_level_++;
        visit_node(dowhile->body());
        indent_level_--;

        indent();
        if (dowhile->condition()) {
            current_line_ += "} while (" + expr_gen_.generate(dowhile->condition()) + ");";
        } else {
            current_line_ += "} while (true);";
        }
        lines_.push_back(current_line_);
        current_line_.clear();
    } else if (auto* forloop = dynamic_cast<ForLoopNode*>(node)) {
        indent();
        std::string decl_str = forloop->declaration() ? expr_gen_.generate(forloop->declaration()) : "";
        std::string cond_str = forloop->condition() ? expr_gen_.generate(forloop->condition()) : "";
        std::string mod_str = forloop->modification() ? expr_gen_.generate(forloop->modification()) : "";
        current_line_ += "for (" + decl_str + "; " + cond_str + "; " + mod_str + ") {";
        lines_.push_back(current_line_);
        current_line_.clear();

        indent_level_++;
        visit_node(forloop->body());
        indent_level_--;

        indent();
        current_line_ += "}";
        lines_.push_back(current_line_);
        current_line_.clear();
    } else if (LoopNode* lnode = dynamic_cast<LoopNode*>(node)) {
        // WhileLoopNode or any other LoopNode subclass
        indent();
        if (lnode->condition()) {
            current_line_ += "while (" + expr_gen_.generate(lnode->condition()) + ") {";
        } else {
            current_line_ += "while (true) {";
        }
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
