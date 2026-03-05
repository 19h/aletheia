#include "ir_serializer.hpp"
#include <sstream>
#include <format>

namespace aletheia::debug {

// ---- Enum-to-string helpers ----

const char* operation_type_name(OperationType type) {
    switch (type) {
        case OperationType::add:                return "add";
        case OperationType::add_with_carry:     return "add_with_carry";
        case OperationType::sub:                return "sub";
        case OperationType::sub_with_carry:     return "sub_with_carry";
        case OperationType::mul:                return "mul";
        case OperationType::mul_us:             return "mul_us";
        case OperationType::div:                return "div";
        case OperationType::div_us:             return "div_us";
        case OperationType::mod:                return "mod";
        case OperationType::mod_us:             return "mod_us";
        case OperationType::negate:             return "negate";
        case OperationType::power:              return "power";
        case OperationType::add_float:          return "add_float";
        case OperationType::sub_float:          return "sub_float";
        case OperationType::mul_float:          return "mul_float";
        case OperationType::div_float:          return "div_float";
        case OperationType::bit_and:            return "bit_and";
        case OperationType::bit_or:             return "bit_or";
        case OperationType::bit_xor:            return "bit_xor";
        case OperationType::bit_not:            return "bit_not";
        case OperationType::shl:                return "shl";
        case OperationType::shr:                return "shr";
        case OperationType::shr_us:             return "shr_us";
        case OperationType::sar:                return "sar";
        case OperationType::left_rotate:        return "left_rotate";
        case OperationType::right_rotate:       return "right_rotate";
        case OperationType::left_rotate_carry:  return "left_rotate_carry";
        case OperationType::right_rotate_carry: return "right_rotate_carry";
        case OperationType::logical_and:        return "logical_and";
        case OperationType::logical_or:         return "logical_or";
        case OperationType::logical_not:        return "logical_not";
        case OperationType::eq:                 return "eq";
        case OperationType::neq:                return "neq";
        case OperationType::lt:                 return "lt";
        case OperationType::le:                 return "le";
        case OperationType::gt:                 return "gt";
        case OperationType::ge:                 return "ge";
        case OperationType::lt_us:              return "lt_us";
        case OperationType::le_us:              return "le_us";
        case OperationType::gt_us:              return "gt_us";
        case OperationType::ge_us:              return "ge_us";
        case OperationType::deref:              return "deref";
        case OperationType::address_of:         return "address_of";
        case OperationType::member_access:      return "member_access";
        case OperationType::cast:               return "cast";
        case OperationType::pointer:            return "pointer";
        case OperationType::low:                return "low";
        case OperationType::field:              return "field";
        case OperationType::ternary:            return "ternary";
        case OperationType::call:               return "call";
        case OperationType::list_op:            return "list_op";
        case OperationType::adc:                return "adc";
        case OperationType::unknown:            return "unknown";
        default: {
            // Future-proof: return a static buffer for unknown values
            static thread_local char buf[32];
            std::snprintf(buf, sizeof(buf), "unknown_op(%d)", static_cast<int>(type));
            return buf;
        }
    }
}

const char* variable_kind_name(VariableKind kind) {
    switch (kind) {
        case VariableKind::Register:      return "Register";
        case VariableKind::StackLocal:    return "StackLocal";
        case VariableKind::StackArgument: return "StackArgument";
        case VariableKind::Parameter:     return "Parameter";
        case VariableKind::Temporary:     return "Temporary";
        default:                          return "Unknown";
    }
}

const char* edge_type_name(EdgeType type) {
    switch (type) {
        case EdgeType::Unconditional: return "Unconditional";
        case EdgeType::True:          return "True";
        case EdgeType::False:         return "False";
        case EdgeType::Switch:        return "Switch";
        case EdgeType::Fallthrough:   return "Fallthrough";
        default:                      return "Unknown";
    }
}

const char* edge_property_name(EdgeProperty prop) {
    switch (prop) {
        case EdgeProperty::Tree:       return "Tree";
        case EdgeProperty::Back:       return "Back";
        case EdgeProperty::Forward:    return "Forward";
        case EdgeProperty::Cross:      return "Cross";
        case EdgeProperty::Retreating: return "Retreating";
        case EdgeProperty::NonLoop:    return "NonLoop";
        default:                       return "Unknown";
    }
}

// ---- Size formatting helper ----

static std::string size_tag(std::size_t size_bytes) {
    return std::format("[i{}]", size_bytes * 8);
}

// ---- Expression serialization ----

std::string ir_to_string(const Expression* expr,
                         std::unordered_set<const Expression*>* visited) {
    if (!expr) return "<null>";

    // DAG/cycle detection via pointer identity
    if (visited) {
        if (visited->contains(expr)) {
            return std::format("<shared: {}>", static_cast<const void*>(expr));
        }
        visited->insert(expr);
    }

    switch (expr->node_kind()) {
        case NodeKind::Constant: {
            auto* c = cast<Constant>(expr);
            return std::format("Constant(0x{:x} {})", c->value(), size_tag(c->size_bytes));
        }

        case NodeKind::GlobalVariable: {
            auto* gv = cast<GlobalVariable>(expr);
            return std::format("GlobalVar:{} {}",
                               gv->name(), size_tag(gv->size_bytes));
        }

        case NodeKind::Variable: {
            auto* v = cast<Variable>(expr);
            return std::format("var:{}_{} {}",
                               v->name(), v->ssa_version(), size_tag(v->size_bytes));
        }

        case NodeKind::Call: {
            auto* call = cast<Call>(expr);
            std::ostringstream ss;
            ss << "Call(" << ir_to_string(call->target(), visited) << ", [";
            for (std::size_t i = 0; i < call->arg_count(); ++i) {
                if (i > 0) ss << ", ";
                ss << ir_to_string(call->arg(i), visited);
            }
            ss << "])";
            return ss.str();
        }

        case NodeKind::Condition: {
            auto* cond = cast<Condition>(expr);
            return std::format("Condition({}, {}, {})",
                               operation_type_name(cond->type()),
                               ir_to_string(cond->lhs(), visited),
                               ir_to_string(cond->rhs(), visited));
        }

        case NodeKind::Operation: {
            auto* op = cast<Operation>(expr);
            std::ostringstream ss;
            ss << "Operation(" << operation_type_name(op->type());
            for (auto* operand : op->operands()) {
                ss << ", " << ir_to_string(operand, visited);
            }
            ss << ")";
            return ss.str();
        }

        case NodeKind::ListOperation: {
            auto* lo = cast<ListOperation>(expr);
            std::ostringstream ss;
            ss << "List(";
            bool first = true;
            for (auto* operand : lo->operands()) {
                if (!first) ss << ", ";
                first = false;
                ss << ir_to_string(operand, visited);
            }
            ss << ")";
            return ss.str();
        }

        default:
            return std::format("<unknown_expr(kind={})>",
                               static_cast<int>(expr->node_kind()));
    }
}

// ---- Instruction serialization ----

static std::string address_prefix(Address addr) {
    if (addr == 0) return "";
    return std::format("[0x{:x}] ", addr);
}

std::string ir_to_string(const Instruction* inst) {
    if (!inst) return "<null>";

    switch (inst->node_kind()) {
        case NodeKind::MemPhi: {
            auto* phi = cast<MemPhi>(inst);
            std::ostringstream ss;
            ss << "MemPhi(" << ir_to_string(phi->dest_var());
            auto* oplist = phi->operand_list();
            if (!phi->origin_block().empty()) {
                ss << ", [";
                bool first = true;
                for (auto& [block, expr] : phi->origin_block()) {
                    if (!first) ss << ", ";
                    first = false;
                    ss << "bb_" << block->id() << ": " << ir_to_string(expr);
                }
                ss << "]";
            } else if (oplist) {
                ss << ", [";
                bool first = true;
                for (auto* op : oplist->operands()) {
                    if (!first) ss << ", ";
                    first = false;
                    ss << ir_to_string(op);
                }
                ss << "] <<NO ORIGIN MAP>>";
            }
            ss << ")";
            return ss.str();
        }

        case NodeKind::Phi: {
            auto* phi = cast<Phi>(inst);
            std::ostringstream ss;
            ss << "Phi(" << ir_to_string(phi->dest_var());
            auto* oplist = phi->operand_list();
            if (!phi->origin_block().empty()) {
                ss << ", [";
                bool first = true;
                for (auto& [block, expr] : phi->origin_block()) {
                    if (!first) ss << ", ";
                    first = false;
                    ss << "bb_" << block->id() << ": " << ir_to_string(expr);
                }
                ss << "]";
            } else if (oplist) {
                ss << ", [";
                bool first = true;
                for (auto* op : oplist->operands()) {
                    if (!first) ss << ", ";
                    first = false;
                    ss << ir_to_string(op);
                }
                ss << "] <<NO ORIGIN MAP>>";
            }
            ss << ")";
            return ss.str();
        }

        case NodeKind::Assignment: {
            auto* assign = cast<Assignment>(inst);
            return std::format("{}Assignment({}, {})",
                               address_prefix(assign->address()),
                               ir_to_string(assign->destination()),
                               ir_to_string(assign->value()));
        }

        case NodeKind::Branch: {
            auto* branch = cast<Branch>(inst);
            return std::format("{}Branch({})",
                               address_prefix(branch->address()),
                               ir_to_string(branch->condition()));
        }

        case NodeKind::IndirectBranch: {
            auto* ib = cast<IndirectBranch>(inst);
            return std::format("{}IndirectBranch({})",
                               address_prefix(ib->address()),
                               ir_to_string(ib->expression()));
        }

        case NodeKind::Return: {
            auto* ret = cast<Return>(inst);
            if (!ret->has_value()) {
                return std::format("{}Return(void)", address_prefix(ret->address()));
            }
            std::ostringstream ss;
            ss << address_prefix(ret->address()) << "Return(";
            bool first = true;
            for (auto* val : ret->values()) {
                if (!first) ss << ", ";
                first = false;
                ss << ir_to_string(val);
            }
            ss << ")";
            return ss.str();
        }

        case NodeKind::Relation: {
            auto* rel = cast<Relation>(inst);
            return std::format("{}Relation({} -> {})",
                               address_prefix(rel->address()),
                               ir_to_string(rel->destination()),
                               ir_to_string(rel->value()));
        }

        case NodeKind::BreakInstr:
            return "Break";

        case NodeKind::ContinueInstr:
            return "Continue";

        case NodeKind::Comment: {
            auto* comment = cast<Comment>(inst);
            return std::format("Comment(\"{}\")", comment->message());
        }

        default:
            return std::format("<unknown_inst(kind={})>",
                               static_cast<int>(inst->node_kind()));
    }
}

// ---- Variable metadata dump ----

std::string variable_info(const Variable* var) {
    if (!var) return "<null>";

    std::ostringstream ss;

    if (isa<GlobalVariable>(var)) {
        auto* gv = cast<GlobalVariable>(var);
        ss << "GlobalVariable(name=\"" << gv->name()
           << "\", size=" << (gv->size_bytes * 8) << "b"
           << ", kind=" << variable_kind_name(gv->kind())
           << ", is_constant=" << (gv->is_constant() ? "true" : "false")
           << ", initial_value=" << ir_to_string(gv->initial_value())
           << ", type=" << (gv->ir_type() ? gv->ir_type()->to_string() : "<unresolved>")
           << ")";
    } else {
        ss << "Variable(name=\"" << var->name()
           << "\", ssa=" << var->ssa_version()
           << ", size=" << (var->size_bytes * 8) << "b"
           << ", kind=" << variable_kind_name(var->kind())
           << ", param_idx=" << var->parameter_index()
           << ", stack_off=" << var->stack_offset()
           << ", aliased=" << (var->is_aliased() ? "true" : "false")
           << ", type=" << (var->ir_type() ? var->ir_type()->to_string() : "<unresolved>")
           << ")";
    }

    return ss.str();
}

// ---- CFG dump ----

std::string cfg_to_string(const ControlFlowGraph* cfg) {
    if (!cfg) return "<null cfg>";

    std::ostringstream ss;
    const auto& edge_props = cfg->edge_properties();

    for (auto* block : cfg->blocks()) {
        ss << "bb_" << block->id();
        if (block == cfg->entry_block()) ss << " [entry]";

        // Predecessors
        ss << " preds=[";
        bool first = true;
        for (auto* pred : block->predecessors()) {
            if (!first) ss << ", ";
            first = false;
            ss << "bb_" << pred->source()->id();
        }
        ss << "]";

        // Successors with edge type and properties
        ss << " -> [";
        first = true;
        for (auto* succ : block->successors()) {
            if (!first) ss << ", ";
            first = false;
            ss << "bb_" << succ->target()->id() << "(";
            ss << edge_type_name(succ->type());

            // SwitchEdge case values
            if (succ->edge_kind() == EdgeKind::SwitchEdge) {
                auto* sw_edge = static_cast<const SwitchEdge*>(succ);
                if (sw_edge->is_default()) {
                    ss << ": default";
                } else {
                    ss << ": case ";
                    bool first_case = true;
                    for (auto val : sw_edge->case_values()) {
                        if (!first_case) ss << ",";
                        first_case = false;
                        ss << val;
                    }
                }
            }

            // Edge property
            auto it = edge_props.find(succ);
            if (it != edge_props.end()) {
                ss << "/" << edge_property_name(it->second);
            }

            ss << ")";
        }
        ss << "]\n";

        // Instructions
        for (auto* inst : block->instructions()) {
            ss << "  " << ir_to_string(inst) << "\n";
        }
    }

    return ss.str();
}

// ---- AST dump ----

std::string ast_node_to_string(const AstNode* node, int indent, int max_depth) {
    if (!node) return "<null>";
    if (max_depth <= 0) return std::string(indent, ' ') + "... (depth limit reached)";

    std::string pad(indent, ' ');
    std::ostringstream ss;

    switch (node->ast_kind()) {
        case AstKind::CodeNode: {
            auto* cn = ast_cast<CodeNode>(node);
            ss << pad << "CodeNode(bb_" << cn->block()->id() << "):\n";
            for (auto* inst : cn->block()->instructions()) {
                ss << pad << "  " << ir_to_string(inst) << "\n";
            }
            break;
        }

        case AstKind::ExprAstNode: {
            auto* en = ast_cast<ExprAstNode>(node);
            ss << pad << "ExprAstNode(" << ir_to_string(en->expr()) << ")\n";
            break;
        }

        case AstKind::SeqNode: {
            auto* sn = ast_cast<SeqNode>(node);
            ss << pad << "SeqNode:\n";
            for (auto* child : sn->nodes()) {
                ss << ast_node_to_string(child, indent + 2, max_depth - 1);
            }
            break;
        }

        case AstKind::IfNode: {
            auto* in = ast_cast<IfNode>(node);
            ss << pad << "IfNode:\n";
            ss << pad << "  cond: " << ast_node_to_string(in->cond(), 0, max_depth - 1);
            ss << pad << "  true:\n";
            ss << ast_node_to_string(in->true_branch(), indent + 4, max_depth - 1);
            if (in->false_branch()) {
                ss << pad << "  false:\n";
                ss << ast_node_to_string(in->false_branch(), indent + 4, max_depth - 1);
            }
            break;
        }

        case AstKind::WhileLoopNode: {
            auto* wn = ast_cast<WhileLoopNode>(node);
            ss << pad << "WhileLoopNode:\n";
            if (wn->is_endless()) {
                ss << pad << "  cond: <endless>\n";
            } else {
                ss << pad << "  cond: " << ir_to_string(wn->condition()) << "\n";
            }
            ss << pad << "  body:\n";
            ss << ast_node_to_string(wn->body(), indent + 4, max_depth - 1);
            break;
        }

        case AstKind::DoWhileLoopNode: {
            auto* dn = ast_cast<DoWhileLoopNode>(node);
            ss << pad << "DoWhileLoopNode:\n";
            ss << pad << "  body:\n";
            ss << ast_node_to_string(dn->body(), indent + 4, max_depth - 1);
            if (dn->condition()) {
                ss << pad << "  cond: " << ir_to_string(dn->condition()) << "\n";
            } else {
                ss << pad << "  cond: <endless>\n";
            }
            break;
        }

        case AstKind::ForLoopNode: {
            auto* fn = ast_cast<ForLoopNode>(node);
            ss << pad << "ForLoopNode:\n";
            if (fn->declaration()) {
                ss << pad << "  init: " << ir_to_string(fn->declaration()) << "\n";
            }
            if (fn->condition()) {
                ss << pad << "  cond: " << ir_to_string(fn->condition()) << "\n";
            } else {
                ss << pad << "  cond: <endless>\n";
            }
            if (fn->modification()) {
                ss << pad << "  step: " << ir_to_string(fn->modification()) << "\n";
            }
            ss << pad << "  body:\n";
            ss << ast_node_to_string(fn->body(), indent + 4, max_depth - 1);
            break;
        }

        case AstKind::SwitchNode: {
            auto* sn = ast_cast<SwitchNode>(node);
            ss << pad << "SwitchNode:\n";
            ss << pad << "  cond: " << ast_node_to_string(sn->cond(), 0, max_depth - 1);
            for (auto* c : sn->cases()) {
                ss << ast_node_to_string(c, indent + 2, max_depth - 1);
            }
            break;
        }

        case AstKind::CaseNode: {
            auto* cn = ast_cast<CaseNode>(node);
            if (cn->is_default()) {
                ss << pad << "CaseNode(default):\n";
            } else {
                ss << pad << "CaseNode(" << cn->value() << "):\n";
            }
            ss << ast_node_to_string(cn->body(), indent + 2, max_depth - 1);
            break;
        }

        case AstKind::BreakNode:
            ss << pad << "BreakNode\n";
            break;

        case AstKind::ContinueNode:
            ss << pad << "ContinueNode\n";
            break;

        default:
            ss << pad << "<unknown_ast(kind=" << static_cast<int>(node->ast_kind()) << ")>\n";
            break;
    }

    return ss.str();
}

std::string ast_to_string(const AbstractSyntaxForest* ast) {
    if (!ast) return "<null ast>";
    if (!ast->root()) return "<empty ast>";
    return ast_node_to_string(ast->root());
}

} // namespace aletheia::debug
