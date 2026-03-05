#pragma once

#include "codegen.hpp"
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <string>
#include <functional>
#include <optional>

namespace aletheia {

class VariableCollector : public DataflowObjectVisitorInterface {
public:
    void visit(Constant* c) override {}
    void visit(Variable* v) override {
        variables_.insert(v);
    }
    void visit(Operation* o) override {
        if (!o) {
            return;
        }
        if (!active_expr_nodes_.insert(o).second) {
            return;
        }
        for (auto* op : o->operands()) if (op) op->accept(*this);
        active_expr_nodes_.erase(o);
    }
    void visit(Call* c) override {
        if (!c) {
            return;
        }
        if (!active_expr_nodes_.insert(c).second) {
            return;
        }
        if (c->target()) c->target()->accept(*this);
        for (size_t i = 0; i < c->arg_count(); ++i) {
            if (c->arg(i)) c->arg(i)->accept(*this);
        }
        active_expr_nodes_.erase(c);
    }
    void visit(ListOperation* lo) override {
        if (!lo) {
            return;
        }
        if (!active_expr_nodes_.insert(lo).second) {
            return;
        }
        for (auto* op : lo->operands()) if (op) op->accept(*this);
        active_expr_nodes_.erase(lo);
    }
    void visit(Condition* c) override {
        if (!c) {
            return;
        }
        if (!active_expr_nodes_.insert(c).second) {
            return;
        }
        if (c->lhs()) c->lhs()->accept(*this);
        if (c->rhs()) c->rhs()->accept(*this);
        active_expr_nodes_.erase(c);
    }

    void visit_assignment(Assignment* i) override {
        if (i->destination()) i->destination()->accept(*this);
        if (i->value()) i->value()->accept(*this);
    }
    void visit_branch(Branch* i) override {
        if (i->condition()) i->condition()->accept(*this);
    }
    void visit_indirect_branch(IndirectBranch* i) override {
        if (i->expression()) i->expression()->accept(*this);
    }
    void visit_return(Return* i) override {
        if (!i) return;
        // Keep return traversal conservative: malformed ASTs may contain
        // non-expression payloads in return value slots, which can recurse
        // indefinitely via visitor dispatch. Collect direct globals only.
        for (auto* v : i->values()) {
            if (auto* gv = dyn_cast<GlobalVariable>(v)) {
                variables_.insert(gv);
            }
        }
    }
    void visit_phi(Phi* i) override {
        if (i->dest_var()) i->dest_var()->accept(*this);
        if (i->operand_list()) i->operand_list()->accept(*this);
    }
    void visit_relation(Relation* i) override {
        if (i->destination()) i->destination()->accept(*this);
        if (i->value()) i->value()->accept(*this);
    }

    void visit_break(BreakInstr* i) override {}
    void visit_continue(ContinueInstr* i) override {}
    void visit_comment(Comment* i) override {}

    void traverse(AstNode* node) {
        if (!node) return;
        if (auto* expr_node = ast_dyn_cast<ExprAstNode>(node)) {
            if (expr_node->expr()) expr_node->expr()->accept(*this);
        } else if (auto* cnode = ast_dyn_cast<CodeNode>(node)) {
            if (cnode->block()) {
                for (auto* inst : cnode->block()->instructions()) {
                    if (inst) inst->accept(*this);
                }
            }
        } else if (auto* snode = ast_dyn_cast<SeqNode>(node)) {
            for (auto* child : snode->nodes()) traverse(child);
        } else if (auto* inode = ast_dyn_cast<IfNode>(node)) {
            traverse(inode->cond());
            traverse(inode->true_branch());
            traverse(inode->false_branch());
        } else if (auto* lnode = ast_dyn_cast<LoopNode>(node)) {
            if (lnode->condition()) lnode->condition()->accept(*this);
            
            // ForLoopNode might have declaration and modification instructions
            if (auto* fnode = ast_dyn_cast<ForLoopNode>(node)) {
                if (fnode->declaration()) fnode->declaration()->accept(*this);
                if (fnode->modification()) fnode->modification()->accept(*this);
            }
            
            traverse(lnode->body());
        } else if (auto* swnode = ast_dyn_cast<SwitchNode>(node)) {
            traverse(swnode->cond());
            for (auto* c : swnode->cases()) traverse(c);
        } else if (auto* casenode = ast_dyn_cast<CaseNode>(node)) {
            traverse(casenode->body());
        }
    }

    const std::unordered_set<Variable*>& variables() const { return variables_; }

private:
    std::unordered_set<Variable*> variables_;
    std::unordered_set<Return*> active_returns_;
    std::unordered_set<const Expression*> active_expr_nodes_;
};

class LocalDeclarationGenerator {
public:
    static std::vector<std::string> generate(DecompilerTask& task, CExpressionGenerator& expr_gen) {
        VariableCollector collector;
        if (task.ast() && task.ast()->root()) {
            collector.traverse(task.ast()->root());
        }

        std::unordered_set<std::string> declared_param_names;
        std::unordered_map<int, TypePtr> declared_param_types;
        if (task.function_type()) {
            if (auto* fn_ty = type_dyn_cast<FunctionTypeDef>(task.function_type().get())) {
                const int declared_param_count = static_cast<int>(fn_ty->parameters().size());
                for (int i = 0; i < declared_param_count; ++i) {
                    declared_param_types[i] = fn_ty->parameters()[static_cast<std::size_t>(i)];
                }
                std::unordered_map<int, std::string> best_name_for_index;
                for (const auto& [reg, param] : task.parameter_registers()) {
                    if (param.index < 0 || param.index >= declared_param_count) {
                        continue;
                    }
                    auto it = best_name_for_index.find(param.index);
                    if (it == best_name_for_index.end() || param.name.size() > it->second.size()) {
                        best_name_for_index[param.index] = param.name;
                    }
                }
                for (int i = 0; i < declared_param_count; ++i) {
                    auto it = best_name_for_index.find(i);
                    if (it != best_name_for_index.end() && !it->second.empty()) {
                        declared_param_names.insert(it->second);
                    } else {
                        declared_param_names.insert("a" + std::to_string(i + 1));
                    }
                }
            }
        } else {
            declared_param_names = task.parameter_names();
        }

        TypePtr signed_return_integer_type = nullptr;
        if (task.function_type()) {
            if (auto* fn_ty = type_dyn_cast<FunctionTypeDef>(task.function_type().get())) {
                if (auto* ret_int = type_dyn_cast<Integer>(fn_ty->return_type().get())) {
                    if (ret_int->is_signed()) {
                        signed_return_integer_type = fn_ty->return_type();
                    }
                }
            }
        }

        std::unordered_map<const Variable*, TypePtr> preferred_decl_types;

        auto integer_width_bits_for_type = [](const TypePtr& type) -> std::optional<std::size_t> {
            if (!type) {
                return std::nullopt;
            }
            if (auto* integer = type_dyn_cast<Integer>(type.get())) {
                return integer->size();
            }
            return std::nullopt;
        };

        auto integer_width_bits_for_var = [&](const Variable* var) -> std::optional<std::size_t> {
            if (!var) {
                return std::nullopt;
            }
            if (auto bits = integer_width_bits_for_type(var->ir_type()); bits.has_value()) {
                return bits;
            }
            if (var->size_bytes > 0) {
                return var->size_bytes * 8;
            }
            return std::nullopt;
        };

        auto maybe_prefer_signed_copy_type = [&](Variable* dst, Variable* src) {
            if (!dst || !src) {
                return;
            }
            const auto dst_bits = integer_width_bits_for_var(dst);
            if (!dst_bits.has_value()) {
                return;
            }

            if (src->is_parameter() && src->parameter_index() >= 0) {
                auto pit = declared_param_types.find(src->parameter_index());
                if (pit != declared_param_types.end() && pit->second) {
                    if (auto* param_int = type_dyn_cast<Integer>(pit->second.get())) {
                        const auto param_bits = integer_width_bits_for_type(pit->second);
                        if (param_int->is_signed()
                            && param_bits.has_value()
                            && *param_bits == *dst_bits) {
                            preferred_decl_types[dst] = pit->second;
                            return;
                        }
                    }
                }
            }

            if (!src->ir_type()) {
                return;
            }
            auto* src_int = type_dyn_cast<Integer>(src->ir_type().get());
            if (!src_int || !src_int->is_signed()) {
                return;
            }

            const auto src_bits = integer_width_bits_for_var(src);
            if (!src_bits.has_value() || *src_bits != *dst_bits) {
                return;
            }

            preferred_decl_types[dst] = src->ir_type();
        };

        auto maybe_prefer_return_type = [&](Variable* value_var) {
            if (!value_var || !signed_return_integer_type) {
                return;
            }
            const auto ret_bits = integer_width_bits_for_type(signed_return_integer_type);
            const auto var_bits = integer_width_bits_for_var(value_var);
            if (!ret_bits.has_value() || !var_bits.has_value() || *ret_bits != *var_bits) {
                return;
            }

            preferred_decl_types[value_var] = signed_return_integer_type;
        };

        std::function<void(AstNode*)> analyze_for_type_hints = [&](AstNode* node) {
            if (!node) {
                return;
            }

            if (auto* cnode = ast_dyn_cast<CodeNode>(node)) {
                if (!cnode->block()) {
                    return;
                }
                for (Instruction* inst : cnode->block()->instructions()) {
                    if (!inst) {
                        continue;
                    }

                    if (auto* assign = dyn_cast<Assignment>(inst)) {
                        auto* dst = dyn_cast<Variable>(assign->destination());
                        auto* src = dyn_cast<Variable>(assign->value());
                        if (dst && src) {
                            maybe_prefer_signed_copy_type(dst, src);
                        }
                    } else if (auto* ret = dyn_cast<Return>(inst)) {
                        if (ret->values().size() == 1) {
                            auto* ret_var = dyn_cast<Variable>(ret->values()[0]);
                            if (ret_var) {
                                maybe_prefer_return_type(ret_var);
                            }
                        }
                    }
                }
                return;
            }

            if (auto* snode = ast_dyn_cast<SeqNode>(node)) {
                for (AstNode* child : snode->nodes()) {
                    analyze_for_type_hints(child);
                }
                return;
            }

            if (auto* inode = ast_dyn_cast<IfNode>(node)) {
                analyze_for_type_hints(inode->true_branch());
                analyze_for_type_hints(inode->false_branch());
                return;
            }

            if (auto* lnode = ast_dyn_cast<LoopNode>(node)) {
                analyze_for_type_hints(lnode->body());
                return;
            }

            if (auto* swnode = ast_dyn_cast<SwitchNode>(node)) {
                for (CaseNode* c : swnode->cases()) {
                    analyze_for_type_hints(c);
                }
                return;
            }

            if (auto* casenode = ast_dyn_cast<CaseNode>(node)) {
                analyze_for_type_hints(casenode->body());
            }
        };

        if (task.ast() && task.ast()->root()) {
            analyze_for_type_hints(task.ast()->root());
        }

        auto strip_unsigned_prefix = [](const std::string& type_name) {
            constexpr std::string_view kPrefix{"unsigned "};
            if (type_name.rfind(std::string(kPrefix), 0) == 0) {
                return type_name.substr(kPrefix.size());
            }
            return type_name;
        };

        auto prefer_type_for_name = [&](const std::string& existing, const std::string& candidate) {
            const std::string existing_base = strip_unsigned_prefix(existing);
            const std::string candidate_base = strip_unsigned_prefix(candidate);
            const bool existing_unsigned = existing_base != existing;
            const bool candidate_unsigned = candidate_base != candidate;

            if (existing_base == candidate_base && existing_unsigned && !candidate_unsigned) {
                return candidate;
            }
            return existing;
        };

        std::map<std::string, std::string> var_to_type;
        
        for (auto* var : collector.variables()) {
            // Skip global variables.
            if (isa<GlobalVariable>(var)) {
                continue;
            }
            
            std::string var_name = expr_gen.generate(var);

            // Skip names that are already declared in the function signature.
            if (declared_param_names.contains(var_name)) {
                continue;
            }

            std::string type_str = "int"; // Default fallback.
            TypePtr decl_type = var->ir_type();
            if (auto it = preferred_decl_types.find(var); it != preferred_decl_types.end() && it->second) {
                decl_type = it->second;
            }
            if (decl_type) {
                type_str = decl_type->to_string();
            }
            auto it = var_to_type.find(var_name);
            if (it == var_to_type.end()) {
                var_to_type[var_name] = type_str;
            } else {
                it->second = prefer_type_for_name(it->second, type_str);
            }
        }

        // Group by type string -> sorted set of variable names.
        std::map<std::string, std::set<std::string>> type_to_vars;
        for (const auto& [var_name, type_str] : var_to_type) {
            type_to_vars[type_str].insert(var_name);
        }

        std::vector<std::string> decls;
        for (const auto& [type_str, vars] : type_to_vars) {
            std::string line = type_str + " ";
            bool first = true;
            for (const auto& v : vars) {
                if (!first) line += ", ";
                line += v;
                first = false;
            }
            line += ";";
            decls.push_back(line);
        }
        
        return decls;
    }
};

/// Collects GlobalVariables but skips Call targets (function names).
class GlobalVariableCollector : public DataflowObjectVisitorInterface {
public:
    void visit(Constant* c) override {}
    void visit(Variable* v) override {}
    void visit(GlobalVariable* gv) override { variables_.insert(gv); }
    void visit(Operation* o) override {
        if (!o) {
            return;
        }
        if (!active_expr_nodes_.insert(o).second) {
            return;
        }
        for (auto* op : o->operands()) if (op) op->accept(*this);
        active_expr_nodes_.erase(o);
    }
    void visit(Call* c) override {
        if (!c) {
            return;
        }
        if (!active_expr_nodes_.insert(c).second) {
            return;
        }
        // Skip the call target — it's a function name, not a data global.
        // Only visit the call arguments.
        for (size_t i = 0; i < c->arg_count(); ++i) {
            if (c->arg(i)) c->arg(i)->accept(*this);
        }
        active_expr_nodes_.erase(c);
    }
    void visit(ListOperation* lo) override {
        if (!lo) {
            return;
        }
        if (!active_expr_nodes_.insert(lo).second) {
            return;
        }
        for (auto* op : lo->operands()) if (op) op->accept(*this);
        active_expr_nodes_.erase(lo);
    }
    void visit(Condition* c) override {
        if (!c) {
            return;
        }
        if (!active_expr_nodes_.insert(c).second) {
            return;
        }
        if (c->lhs()) c->lhs()->accept(*this);
        if (c->rhs()) c->rhs()->accept(*this);
        active_expr_nodes_.erase(c);
    }

    void visit_assignment(Assignment* i) override {
        if (i->destination()) i->destination()->accept(*this);
        if (i->value()) i->value()->accept(*this);
    }
    void visit_branch(Branch* i) override {
        if (i->condition()) i->condition()->accept(*this);
    }
    void visit_indirect_branch(IndirectBranch* i) override {
        if (i->expression()) i->expression()->accept(*this);
    }
    void visit_return(Return* i) override {
        if (!i) return;
        // Keep return traversal conservative: malformed ASTs may contain
        // non-expression payloads in return value slots, which can recurse
        // indefinitely via visitor dispatch. Collect direct globals only.
        for (auto* v : i->values()) {
            if (auto* gv = dyn_cast<GlobalVariable>(v)) {
                variables_.insert(gv);
            }
        }
    }
    void visit_phi(Phi* i) override {
        if (i->dest_var()) i->dest_var()->accept(*this);
        if (i->operand_list()) i->operand_list()->accept(*this);
    }
    void visit_relation(Relation* i) override {
        if (i->destination()) i->destination()->accept(*this);
        if (i->value()) i->value()->accept(*this);
    }

    void visit_break(BreakInstr* i) override {}
    void visit_continue(ContinueInstr* i) override {}
    void visit_comment(Comment* i) override {}

    void traverse(AstNode* node) {
        if (!node) return;
        if (auto* expr_node = ast_dyn_cast<ExprAstNode>(node)) {
            if (expr_node->expr()) expr_node->expr()->accept(*this);
        } else if (auto* cnode = ast_dyn_cast<CodeNode>(node)) {
            if (cnode->block()) {
                for (auto* inst : cnode->block()->instructions()) {
                    if (inst) inst->accept(*this);
                }
            }
        } else if (auto* snode = ast_dyn_cast<SeqNode>(node)) {
            for (auto* child : snode->nodes()) traverse(child);
        } else if (auto* ifnode = ast_dyn_cast<IfNode>(node)) {
            if (ifnode->cond()) traverse(ifnode->cond());
            if (ifnode->true_branch()) traverse(ifnode->true_branch());
            if (ifnode->false_branch()) traverse(ifnode->false_branch());
        } else if (auto* loop = ast_dyn_cast<WhileLoopNode>(node)) {
            if (loop->condition()) loop->condition()->accept(*this);
            if (loop->body()) traverse(loop->body());
        } else if (auto* loop = ast_dyn_cast<DoWhileLoopNode>(node)) {
            if (loop->condition()) loop->condition()->accept(*this);
            if (loop->body()) traverse(loop->body());
        } else if (auto* loop = ast_dyn_cast<ForLoopNode>(node)) {
            if (loop->condition()) loop->condition()->accept(*this);
            if (loop->body()) traverse(loop->body());
        } else if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
            if (sw->cond()) traverse(sw->cond());
            for (auto* c : sw->cases()) traverse(c);
        } else if (auto* cs = ast_dyn_cast<CaseNode>(node)) {
            if (cs->body()) traverse(cs->body());
        }
    }

    const std::set<Variable*>& variables() const { return variables_; }
private:
    std::set<Variable*> variables_;
    std::unordered_set<const Expression*> active_expr_nodes_;
};

class GlobalDeclarationGenerator {
public:
    static std::vector<std::string> generate(DecompilerTask& task) {
        auto is_valid_identifier = [](const std::string& name) {
            if (name.empty()) return false;
            const unsigned char first = static_cast<unsigned char>(name.front());
            if (!(std::isalpha(first) || first == '_')) return false;
            for (unsigned char c : name) {
                if (!(std::isalnum(c) || c == '_')) {
                    return false;
                }
            }
            return true;
        };

        GlobalVariableCollector collector;
        if (task.ast() && task.ast()->root()) {
            collector.traverse(task.ast()->root());
        }

        std::map<std::string, GlobalVariable*> globals_by_name;
        for (auto* var : collector.variables()) {
            auto* gv = dyn_cast<GlobalVariable>(var);
            if (!gv) continue;
            globals_by_name.try_emplace(gv->name(), gv);
        }

        std::vector<std::string> decls;
        for (const auto& [name, gv] : globals_by_name) {
            if (!is_valid_identifier(name)) {
                continue;
            }
            std::string type_str = "int";
            if (gv->ir_type()) {
                type_str = gv->ir_type()->to_string();
            }

            std::string line = "extern ";
            if (gv->is_constant()) {
                line += "const ";
            }
            line += type_str + " " + name + ";";
            decls.push_back(std::move(line));
        }

        return decls;
    }
};

} // namespace aletheia
