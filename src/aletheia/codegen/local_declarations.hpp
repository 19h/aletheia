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
        for (std::size_t index = 0; index < o->operands().size(); ++index) {
            if (index == 1
                && (o->type() == OperationType::member_access
                    || o->type() == OperationType::field)) {
                continue;
            }
            if (o->operands()[index]) o->operands()[index]->accept(*this);
        }
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
        if (!i || !active_returns_.insert(i).second) return;
        for (Expression* value : i->values()) {
            if (!value) continue;
            switch (value->node_kind()) {
                case NodeKind::Constant:
                case NodeKind::Variable:
                case NodeKind::GlobalVariable:
                case NodeKind::Operation:
                case NodeKind::Call:
                case NodeKind::ListOperation:
                case NodeKind::Condition:
                    value->accept(*this);
                    break;
                default:
                    // A malformed Return can contain an Instruction pointer
                    // cast into its expression payload. Never dispatch it as
                    // an expression; the invariant checker reports the IR
                    // defect independently.
                    break;
            }
        }
        active_returns_.erase(i);
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
    static std::vector<std::string> generate(
        DecompilerTask& task,
        CExpressionGenerator& expr_gen,
        bool one_variable_per_declaration = false,
        const std::unordered_set<std::string>* emitted_parameter_names = nullptr) {
        VariableCollector collector;
        if (task.ast() && task.ast()->root()) {
            collector.traverse(task.ast()->root());
        }

        std::unordered_set<std::string> declared_param_names;
        std::unordered_map<int, TypePtr> declared_param_types;
        int declared_param_count = 0;
        if (task.function_type()) {
            if (auto* fn_ty = type_dyn_cast<FunctionTypeDef>(task.function_type().get())) {
                declared_param_count = static_cast<int>(fn_ty->parameters().size());
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
            for (const auto& [_, param] : task.parameter_registers()) {
                if (param.index >= 0) {
                    declared_param_count = std::max(declared_param_count, param.index + 1);
                }
            }
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
        std::unordered_set<std::string> pointer_role_names;
        std::unordered_map<std::string, std::unordered_set<std::string>>
            identity_copy_sources;

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
                        Expression* destination = assign->destination();
                        while (auto* cast = dyn_cast<Operation>(destination)) {
                            if (cast->type() != OperationType::cast
                                || cast->operands().size() != 1) {
                                break;
                            }
                            destination = cast->operands()[0];
                        }
                        auto* dst = dyn_cast<Variable>(destination);
                        auto* src = dyn_cast<Variable>(assign->value());
                        if (dst && src) {
                            maybe_prefer_signed_copy_type(dst, src);
                            identity_copy_sources[expr_gen.generate(dst)].insert(
                                expr_gen.generate(src));
                        }
                        if (dst && assign->value()) {
                            TypePtr value_type = assign->value()->ir_type();
                            const std::size_t typed_width = value_type
                                ? value_type->size_bytes() : 0;
                            const std::size_t value_width = std::max(
                                typed_width, assign->value()->size_bytes);
                            if (value_width > dst->size_bytes) {
                                preferred_decl_types[dst] = value_type
                                    && typed_width == value_width
                                    ? value_type
                                    : std::make_shared<const Integer>(value_width * 8, false);
                            } else if (value_type
                                       && value_type->type_kind() == TypeKind::Float
                                       && typed_width == dst->size_bytes
                                       && ((!dst->is_stack_variable())
                                           || (dst->ir_type()
                                               && dst->ir_type()->type_kind()
                                                   == TypeKind::Float))) {
                                preferred_decl_types[dst] = value_type;
                            }
                        }
                        if (auto* call = dyn_cast<Call>(assign->value())) {
                            auto* function_type = call->ir_type()
                                ? type_dyn_cast<FunctionTypeDef>(call->ir_type().get())
                                : nullptr;
                            if (function_type) {
                                const std::size_t count = std::min(
                                    call->arg_count(), function_type->parameters().size());
                                for (std::size_t argument_index = 0;
                                     argument_index < count;
                                     ++argument_index) {
                                    const TypePtr& parameter_type =
                                        function_type->parameters()[argument_index];
                                    if (!parameter_type
                                        || parameter_type->type_kind() != TypeKind::Pointer) {
                                        continue;
                                    }
                                    Expression* argument = call->arg(argument_index);
                                    while (auto* cast = dyn_cast<Operation>(argument)) {
                                        if (cast->type() != OperationType::cast
                                            || cast->operands().size() != 1) {
                                            break;
                                        }
                                        argument = cast->operands()[0];
                                    }
                                    if (auto* argument_variable = dyn_cast<Variable>(argument)) {
                                        preferred_decl_types[argument_variable] =
                                            std::make_shared<const CustomType>("uintptr_t", 64);
                                        pointer_role_names.insert(
                                            expr_gen.generate(argument_variable));
                                    }
                                }
                            }
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

        // Pointer use can occur after a register/load value has been copied
        // into a return/argument temporary. Propagate that role backward over
        // identity copies so the source is not declared as a 32-bit call
        // result and truncated before the pointer-typed sink.
        bool pointer_roles_changed = true;
        while (pointer_roles_changed) {
            pointer_roles_changed = false;
            std::vector<std::string> known_roles(
                pointer_role_names.begin(), pointer_role_names.end());
            for (const std::string& destination_name : known_roles) {
                auto sources = identity_copy_sources.find(destination_name);
                if (sources == identity_copy_sources.end()) continue;
                for (const std::string& source_name : sources->second) {
                    pointer_roles_changed |= pointer_role_names.insert(source_name).second;
                }
            }
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
        std::map<std::string, std::size_t> var_to_width;
        std::map<std::string, TypePtr> var_to_decl_type;
        
        for (auto* var : collector.variables()) {
            // Skip global variables.
            if (isa<GlobalVariable>(var)) {
                continue;
            }
            
            std::string var_name = expr_gen.generate(var);

            // Use the exact identifiers emitted by the caller. Recomputing a
            // preferred name here from unordered ABI aliases can select a
            // different equal-length alias and suppress a real local.
            const auto& signature_names = emitted_parameter_names
                ? *emitted_parameter_names : declared_param_names;
            if (signature_names.contains(var_name)) {
                continue;
            }

            std::string type_str = "int"; // Default fallback.
            TypePtr decl_type = var->ir_type();
            if (auto extent = task.local_byte_array_extents().find(var_name);
                extent != task.local_byte_array_extents().end()
                && extent->second > 0) {
                decl_type = std::make_shared<const ArrayType>(
                    Integer::uint8_t(), extent->second);
            }
            if ((!decl_type || decl_type->type_kind() != TypeKind::Array)
                && (preferred_decl_types.find(var) != preferred_decl_types.end())) {
                auto it = preferred_decl_types.find(var);
                if (it != preferred_decl_types.end() && it->second) {
                    decl_type = it->second;
                }
            }
            if (decl_type && !type_isa<UnknownType>(decl_type.get())
                && decl_type->to_string() != "unknown type") {
                type_str = render_codegen_type(
                    decl_type, one_variable_per_declaration);
                expr_gen.set_local_declared_type(var_name, decl_type);
            } else {
                const std::size_t width_bits = var->size_bytes > 0
                    ? var->size_bytes * 8 : 32;
                TypePtr fallback_type = std::make_shared<const Integer>(width_bits, false);
                type_str = fallback_type->to_string();
                decl_type = fallback_type;
                expr_gen.set_local_declared_type(var_name, fallback_type);
            }
            auto it = var_to_type.find(var_name);
            if (it == var_to_type.end()) {
                var_to_type[var_name] = type_str;
                var_to_width[var_name] = std::max(
                    var->size_bytes, decl_type ? decl_type->size_bytes() : std::size_t{0});
                var_to_decl_type[var_name] = decl_type;
            } else {
                const std::size_t existing_width = var_to_width[var_name];
                const std::size_t candidate_width = std::max(
                    var->size_bytes, decl_type ? decl_type->size_bytes() : std::size_t{0});
                const bool existing_pointer = var_to_decl_type[var_name]
                    && var_to_decl_type[var_name]->type_kind() == TypeKind::Pointer;
                const bool candidate_pointer = decl_type
                    && decl_type->type_kind() == TypeKind::Pointer;
                if (existing_pointer != candidate_pointer
                    && std::max(existing_width, candidate_width) >= 8) {
                    it->second = "uintptr_t";
                    var_to_width[var_name] = std::max(existing_width, candidate_width);
                    var_to_decl_type[var_name] = std::make_shared<const CustomType>(
                        "uintptr_t", var_to_width[var_name] * 8);
                } else if (candidate_width > existing_width) {
                    it->second = type_str;
                    var_to_width[var_name] = candidate_width;
                    var_to_decl_type[var_name] = decl_type;
                } else if (candidate_width == existing_width) {
                    const bool existing_float = var_to_decl_type[var_name]
                        && var_to_decl_type[var_name]->type_kind() == TypeKind::Float;
                    const bool candidate_float = decl_type
                        && decl_type->type_kind() == TypeKind::Float;
                    const std::string preferred = candidate_float && !existing_float
                        ? type_str : prefer_type_for_name(it->second, type_str);
                    if (preferred != it->second || (candidate_float && !existing_float)) {
                        it->second = type_str;
                        var_to_decl_type[var_name] = decl_type;
                    }
                }
            }
        }

        for (const std::string& name : pointer_role_names) {
            if (!var_to_type.contains(name)) continue;
            var_to_type[name] = "uintptr_t";
            var_to_width[name] = std::max<std::size_t>(var_to_width[name], 8);
            var_to_decl_type[name] = std::make_shared<const CustomType>(
                "uintptr_t", 64);
        }

        for (const auto& [name, type] : var_to_decl_type) {
            if (type) expr_gen.set_local_declared_type(name, type);
        }

        // Group by type string -> sorted set of variable names.
        std::map<std::string, std::set<std::string>> type_to_vars;
        std::vector<std::string> array_decls;
        for (const auto& [var_name, type_str] : var_to_type) {
            auto declared = var_to_decl_type.find(var_name);
            auto* array = declared != var_to_decl_type.end() && declared->second
                ? type_dyn_cast<ArrayType>(declared->second.get()) : nullptr;
            if (array && array->element() && array->count() > 0) {
                const bool abi_result_buffer =
                    task.local_byte_array_extents().contains(var_name);
                array_decls.push_back(
                    std::string(abi_result_buffer ? "_Alignas(16) " : "")
                    + render_codegen_declaration(
                        declared->second,
                        var_name,
                        one_variable_per_declaration) + ";");
                continue;
            }
            type_to_vars[type_str].insert(var_name);
        }

        std::vector<std::string> decls = std::move(array_decls);
        for (const auto& [type_str, vars] : type_to_vars) {
            if (one_variable_per_declaration) {
                for (const auto& var : vars) {
                    decls.push_back(type_str + " " + var + ";");
                }
                continue;
            }
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
        for (auto* v : i->values()) {
            if (v) v->accept(*this);
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
    static std::vector<std::string> generate(
        DecompilerTask& task,
        const std::unordered_map<std::string, std::string>* identifier_names = nullptr,
        bool portable_c = false) {
        GlobalVariableCollector collector;
        if (task.ast() && task.ast()->root()) {
            collector.traverse(task.ast()->root());
        }
        if (task.cfg()) {
            for (BasicBlock* block : task.cfg()->blocks()) {
                if (!block) continue;
                for (Instruction* instruction : block->instructions()) {
                    if (instruction) instruction->accept(collector);
                }
            }
        }

        std::map<std::string, GlobalVariable*> globals_by_name;
        for (auto* var : collector.variables()) {
            auto* gv = dyn_cast<GlobalVariable>(var);
            if (!gv) continue;
            if (is_c_quoted_literal(gv->name())) continue;
            std::string emitted_name = normalize_distinct_c_identifier(
                gv->name(), "symbol");
            if (identifier_names) {
                if (auto it = identifier_names->find(gv->name());
                    it != identifier_names->end()) {
                    emitted_name = it->second;
                }
            }
            globals_by_name.try_emplace(std::move(emitted_name), gv);
        }

        std::vector<std::string> decls;
        for (const auto& [name, gv] : globals_by_name) {
            std::string line = "extern ";
            if (gv->is_constant() && !gv->represents_address()) {
                line += "const ";
            }
            if (gv->represents_address()) {
                line += "unsigned char " + name + "[];";
            } else {
                line += render_codegen_declaration(
                    gv->ir_type() ? gv->ir_type() : Integer::int32_t(),
                    name,
                    portable_c) + ";";
            }
            decls.push_back(std::move(line));
        }

        return decls;
    }
};

} // namespace aletheia
