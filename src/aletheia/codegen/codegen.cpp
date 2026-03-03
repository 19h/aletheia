#include "codegen.hpp"
#include "local_declarations.hpp"
#include <ida/lines.hpp>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace aletheia {

namespace {

bool expressions_equivalent(Expression* lhs, Expression* rhs) {
    if (lhs == rhs) {
        return true;
    }
    auto* lvar = dyn_cast<Variable>(lhs);
    auto* rvar = dyn_cast<Variable>(rhs);
    if (lvar && rvar) {
        return lvar->name() == rvar->name() && lvar->ssa_version() == rvar->ssa_version();
    }
    return false;
}

bool is_commutative_for_compound(OperationType type) {
    switch (type) {
        case OperationType::add:
        case OperationType::add_float:
        case OperationType::mul:
        case OperationType::mul_us:
        case OperationType::mul_float:
        case OperationType::bit_and:
        case OperationType::bit_or:
        case OperationType::bit_xor:
            return true;
        default:
            return false;
    }
}

const char* compound_operator_symbol(OperationType type) {
    switch (type) {
        case OperationType::add:
        case OperationType::add_float:
            return "+";
        case OperationType::sub:
        case OperationType::sub_float:
            return "-";
        case OperationType::mul:
        case OperationType::mul_us:
        case OperationType::mul_float:
            return "*";
        case OperationType::div:
        case OperationType::div_us:
        case OperationType::div_float:
            return "/";
        case OperationType::mod:
        case OperationType::mod_us:
            return "%";
        case OperationType::bit_and:
            return "&";
        case OperationType::bit_or:
            return "|";
        case OperationType::bit_xor:
            return "^";
        case OperationType::shl:
            return "<<";
        case OperationType::shr:
        case OperationType::shr_us:
        case OperationType::sar:
            return ">>";
        default:
            return nullptr;
    }
}

std::int64_t sign_extend_to_i64(std::uint64_t value, std::size_t bytes) {
    if (bytes == 0 || bytes >= 8) {
        return static_cast<std::int64_t>(value);
    }
    const std::size_t bits = bytes * 8;
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    const std::uint64_t truncated = value & mask;
    const std::uint64_t sign_bit = std::uint64_t{1} << (bits - 1);
    if ((truncated & sign_bit) == 0) {
        return static_cast<std::int64_t>(truncated);
    }
    const std::uint64_t extended = truncated | (~mask);
    return static_cast<std::int64_t>(extended);
}

bool constant_is_plus_minus_one(Expression* expr, int& sign) {
    auto* c = dyn_cast<Constant>(expr);
    if (!c) {
        return false;
    }
    const std::size_t width = c->size_bytes == 0 ? sizeof(std::uint64_t) : c->size_bytes;
    const std::int64_t signed_value = sign_extend_to_i64(c->value(), width);
    if (signed_value == 1) {
        sign = 1;
        return true;
    }
    if (signed_value == -1) {
        sign = -1;
        return true;
    }
    return false;
}

std::size_t configured_hex_threshold() {
    const char* value = std::getenv("ALETHEIA_INT_HEX_THRESHOLD");
    if (!value || *value == '\0') {
        return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value) {
        return 0;
    }
    return static_cast<std::size_t>(parsed);
}

enum class IfBranchPreference {
    None,
    Smallest,
    Largest,
};

IfBranchPreference configured_if_branch_preference() {
    const char* value = std::getenv("ALETHEIA_IF_BRANCH_PREFERENCE");
    if (!value || *value == '\0') {
        return IfBranchPreference::None;
    }

    std::string pref(value);
    if (pref == "smallest") {
        return IfBranchPreference::Smallest;
    }
    if (pref == "largest") {
        return IfBranchPreference::Largest;
    }
    return IfBranchPreference::None;
}

std::size_t ast_node_weight(AstNode* node) {
    if (!node) {
        return 0;
    }

    if (auto* cnode = ast_dyn_cast<CodeNode>(node)) {
        if (!cnode->block()) {
            return 1;
        }
        return std::max<std::size_t>(1, cnode->block()->instructions().size());
    }
    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        std::size_t sum = 0;
        for (AstNode* child : seq->nodes()) {
            sum += ast_node_weight(child);
        }
        return std::max<std::size_t>(1, sum);
    }
    if (auto* inode = ast_dyn_cast<IfNode>(node)) {
        return 1 + ast_node_weight(inode->true_branch()) + ast_node_weight(inode->false_branch());
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        return 1 + ast_node_weight(loop->body());
    }
    if (auto* snode = ast_dyn_cast<SwitchNode>(node)) {
        std::size_t sum = 1;
        for (CaseNode* c : snode->cases()) {
            sum += ast_node_weight(c);
        }
        return sum;
    }
    if (auto* cnode = ast_dyn_cast<CaseNode>(node)) {
        return 1 + ast_node_weight(cnode->body());
    }

    return 1;
}

bool should_swap_if_branches(IfNode* inode) {
    if (!inode || !inode->true_branch() || !inode->false_branch()) {
        return false;
    }

    const bool true_is_if = ast_dyn_cast<IfNode>(inode->true_branch()) != nullptr;
    const bool false_is_if = ast_dyn_cast<IfNode>(inode->false_branch()) != nullptr;
    if (true_is_if != false_is_if) {
        return true_is_if;
    }

    const IfBranchPreference pref = configured_if_branch_preference();
    if (pref == IfBranchPreference::None) {
        return false;
    }

    const std::size_t true_weight = ast_node_weight(inode->true_branch());
    const std::size_t false_weight = ast_node_weight(inode->false_branch());

    if (pref == IfBranchPreference::Smallest) {
        return true_weight > false_weight;
    }
    if (pref == IfBranchPreference::Largest) {
        return true_weight < false_weight;
    }
    return false;
}

int precedence_for_operation(OperationType type);
int precedence_for_expression(Expression* expr);
bool needs_parentheses_for_equal_precedence_rhs(OperationType parent_type);

std::string negate_condition_string(std::string cond) {
    if (cond.empty()) {
        return "!(/* condition */)";
    }
    if (cond.starts_with("!(") && cond.ends_with(')')) {
        return cond.substr(2, cond.size() - 3);
    }
    return "!(" + cond + ")";
}

bool is_comparison_operation(OperationType type) {
    switch (type) {
        case OperationType::eq:
        case OperationType::neq:
        case OperationType::lt:
        case OperationType::le:
        case OperationType::gt:
        case OperationType::ge:
        case OperationType::lt_us:
        case OperationType::le_us:
        case OperationType::gt_us:
        case OperationType::ge_us:
            return true;
        default:
            return false;
    }
}

OperationType negate_comparison_operation(OperationType type) {
    switch (type) {
        case OperationType::eq:    return OperationType::neq;
        case OperationType::neq:   return OperationType::eq;
        case OperationType::lt:
        case OperationType::lt_us: return OperationType::ge;
        case OperationType::le:
        case OperationType::le_us: return OperationType::gt;
        case OperationType::gt:
        case OperationType::gt_us: return OperationType::le;
        case OperationType::ge:
        case OperationType::ge_us: return OperationType::lt;
        default:                   return OperationType::unknown;
    }
}

const char* comparison_symbol(OperationType type) {
    switch (type) {
        case OperationType::eq:    return " == ";
        case OperationType::neq:   return " != ";
        case OperationType::lt:
        case OperationType::lt_us: return " < ";
        case OperationType::le:
        case OperationType::le_us: return " <= ";
        case OperationType::gt:
        case OperationType::gt_us: return " > ";
        case OperationType::ge:
        case OperationType::ge_us: return " >= ";
        default:                   return nullptr;
    }
}

std::string render_comparison_expression(
    CExpressionGenerator& gen,
    OperationType op_type,
    Expression* lhs,
    Expression* rhs) {
    const char* op_str = comparison_symbol(op_type);
    if (!op_str) {
        return "/* condition */";
    }

    const int parent_prec = precedence_for_operation(op_type);
    auto render_operand = [&](Expression* operand, bool is_rhs) {
        std::string rendered = gen.generate(operand);
        const int child_prec = precedence_for_expression(operand);
        const bool needs_brackets =
            (child_prec < parent_prec) ||
            (is_rhs && child_prec == parent_prec && needs_parentheses_for_equal_precedence_rhs(op_type));
        if (needs_brackets) {
            return "(" + rendered + ")";
        }
        return rendered;
    };

    return render_operand(lhs, false) + op_str + render_operand(rhs, true);
}

std::string render_negated_condition(Expression* condition, CExpressionGenerator& gen) {
    if (!condition) {
        return "!(/* condition */)";
    }

    if (auto* cond = dyn_cast<Condition>(condition)) {
        const OperationType negated = negate_comparison_operation(cond->type());
        if (negated != OperationType::unknown) {
            return render_comparison_expression(gen, negated, cond->lhs(), cond->rhs());
        }
    }

    if (auto* op = dyn_cast<Operation>(condition)) {
        if (op->type() == OperationType::logical_not && op->operands().size() == 1) {
            return gen.generate(op->operands()[0]);
        }
        if (is_comparison_operation(op->type()) && op->operands().size() == 2) {
            const OperationType negated = negate_comparison_operation(op->type());
            if (negated != OperationType::unknown) {
                return render_comparison_expression(gen, negated, op->operands()[0], op->operands()[1]);
            }
        }
    }

    return negate_condition_string(gen.generate(condition));
}

char nibble_to_hex(unsigned value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

std::string hex_u64(std::uint64_t value) {
    if (value == 0) {
        return "0";
    }
    std::string out;
    while (value != 0) {
        out.push_back(nibble_to_hex(static_cast<unsigned>(value & 0xF)));
        value >>= 4;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

bool is_printable_ascii(std::uint8_t byte) {
    return byte >= 32 && byte <= 126;
}

std::string escape_char_literal(std::uint8_t byte) {
    switch (byte) {
        case '\\': return "'\\\\'";
        case '\'': return "'\\\''";
        case '\n': return "'\\n'";
        case '\r': return "'\\r'";
        case '\t': return "'\\t'";
        case '\0': return "'\\0'";
        default:
            return std::string("'") + static_cast<char>(byte) + "'";
    }
}

std::string escape_string_literal(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (unsigned char byte : value) {
        switch (byte) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default:
                if (is_printable_ascii(byte)) {
                    out.push_back(static_cast<char>(byte));
                } else {
                    out += "\\x";
                    out.push_back(nibble_to_hex((byte >> 4) & 0xF));
                    out.push_back(nibble_to_hex(byte & 0xF));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

bool try_format_string_array(Constant* c, std::string& out) {
    if (!c || !c->ir_type()) {
        return false;
    }
    auto* arr = type_dyn_cast<ArrayType>(c->ir_type().get());
    if (!arr || arr->count() == 0 || arr->count() > 8) {
        return false;
    }
    auto* elem = type_dyn_cast<Integer>(arr->element().get());
    if (!elem || elem->size() != 8) {
        return false;
    }

    std::string decoded;
    decoded.reserve(arr->count());
    const std::uint64_t packed = c->value();
    for (std::size_t i = 0; i < arr->count(); ++i) {
        const std::uint8_t byte = static_cast<std::uint8_t>((packed >> (i * 8)) & 0xFF);
        if (byte == 0) {
            break;
        }
        decoded.push_back(static_cast<char>(byte));
    }

    if (decoded.empty()) {
        return false;
    }

    out = escape_string_literal(decoded);
    return true;
}

std::string integer_suffix(Constant* c) {
    if (!c || !c->ir_type()) {
        return "";
    }
    auto* integer = type_dyn_cast<Integer>(c->ir_type().get());
    if (!integer) {
        return "";
    }

    std::string suffix;
    if (!integer->is_signed()) {
        suffix += 'U';
    }
    if (integer->size() >= 64) {
        suffix += 'L';
    }
    return suffix;
}

int precedence_for_operation(OperationType type) {
    switch (type) {
        case OperationType::call:
        case OperationType::member_access:
        case OperationType::field:
            return 150;

        case OperationType::negate:
        case OperationType::bit_not:
        case OperationType::logical_not:
        case OperationType::deref:
        case OperationType::address_of:
        case OperationType::cast:
        case OperationType::pointer:
        case OperationType::low:
            return 140;

        case OperationType::power:
            return 135;

        case OperationType::mul:
        case OperationType::mul_us:
        case OperationType::mul_float:
        case OperationType::div:
        case OperationType::div_us:
        case OperationType::div_float:
        case OperationType::mod:
        case OperationType::mod_us:
            return 130;

        case OperationType::add:
        case OperationType::add_with_carry:
        case OperationType::add_float:
        case OperationType::sub:
        case OperationType::sub_with_carry:
        case OperationType::sub_float:
        case OperationType::adc:
            return 120;

        case OperationType::shl:
        case OperationType::shr:
        case OperationType::shr_us:
        case OperationType::sar:
        case OperationType::left_rotate:
        case OperationType::right_rotate:
        case OperationType::left_rotate_carry:
        case OperationType::right_rotate_carry:
            return 110;

        case OperationType::eq:
        case OperationType::neq:
        case OperationType::lt:
        case OperationType::le:
        case OperationType::gt:
        case OperationType::ge:
        case OperationType::lt_us:
        case OperationType::le_us:
        case OperationType::gt_us:
        case OperationType::ge_us:
            return 100;

        case OperationType::bit_and:
            return 90;
        case OperationType::bit_xor:
            return 80;
        case OperationType::bit_or:
            return 70;
        case OperationType::logical_and:
            return 60;
        case OperationType::logical_or:
            return 50;
        case OperationType::ternary:
            return 40;
        default:
            return 30;
    }
}

int precedence_for_expression(Expression* expr) {
    if (auto* cond = dyn_cast<Condition>(expr)) {
        return precedence_for_operation(cond->type());
    }
    if (auto* op = dyn_cast<Operation>(expr)) {
        return precedence_for_operation(op->type());
    }
    return 1000; // constants/variables bind strongest for printing
}

bool needs_parentheses_for_equal_precedence_rhs(OperationType parent_type) {
    switch (parent_type) {
        case OperationType::sub:
        case OperationType::sub_with_carry:
        case OperationType::sub_float:
        case OperationType::div:
        case OperationType::div_us:
        case OperationType::div_float:
        case OperationType::mod:
        case OperationType::mod_us:
        case OperationType::shl:
        case OperationType::shr:
        case OperationType::shr_us:
        case OperationType::sar:
        case OperationType::power:
        case OperationType::eq:
        case OperationType::neq:
        case OperationType::lt:
        case OperationType::le:
        case OperationType::gt:
        case OperationType::ge:
        case OperationType::lt_us:
        case OperationType::le_us:
        case OperationType::gt_us:
        case OperationType::ge_us:
            return true;
        default:
            return false;
    }
}

bool is_unknown_placeholder(std::string_view text) {
    return text == "unknown_op" ||
           text.starts_with("__aletheia_unknown_op") ||
           text.starts_with("__aletheia_unhandled_op");
}

bool is_comment_line(std::string_view text) {
    return text.starts_with("/*") && text.ends_with("*/");
}

std::string block_label(BasicBlock* block) {
    if (!block) {
        return "bb_null";
    }
    return "bb_" + std::to_string(block->id());
}

std::pair<Edge*, Edge*> pick_true_false_edges(BasicBlock* block) {
    Edge* true_edge = nullptr;
    Edge* false_edge = nullptr;
    if (!block) {
        return {nullptr, nullptr};
    }

    for (Edge* edge : block->successors()) {
        if (!edge) {
            continue;
        }
        if (edge->type() == EdgeType::True) {
            true_edge = edge;
        } else if (edge->type() == EdgeType::False) {
            false_edge = edge;
        }
    }

    if ((!true_edge || !false_edge) && block->successors().size() == 2) {
        if (!true_edge) {
            true_edge = block->successors()[0];
        }
        if (!false_edge) {
            false_edge = block->successors()[1];
            if (false_edge == true_edge) {
                false_edge = block->successors()[0] == true_edge
                    ? block->successors()[1]
                    : block->successors()[0];
            }
        }
    }

    return {true_edge, false_edge};
}

bool is_cfg_fallback_root(AstNode* root) {
    if (!root) {
        return false;
    }

    if (auto* cnode = ast_dyn_cast<CodeNode>(root)) {
        // A single CodeNode with no successors (straight-line function body)
        // does not need fallback labels. Only enter fallback mode when the
        // block has branches requiring goto targets.
        auto* block = cnode->block();
        if (block && block->successors().empty()) {
            return false;
        }
        return true;
    }

    auto* seq = ast_dyn_cast<SeqNode>(root);
    if (!seq || seq->nodes().empty()) {
        return false;
    }

    // A sequence with a single no-successor CodeNode doesn't need labels either.
    if (seq->nodes().size() == 1) {
        if (auto* cnode = ast_dyn_cast<CodeNode>(seq->nodes()[0])) {
            auto* block = cnode->block();
            if (block && block->successors().empty()) {
                return false;
            }
        }
    }

    for (AstNode* child : seq->nodes()) {
        if (!ast_dyn_cast<CodeNode>(child)) {
            return false;
        }
    }
    return true;
}

std::string format_unknown_operation(Operation* o, CExpressionGenerator& gen, std::string_view placeholder_name) {
    std::string rendered = std::string(placeholder_name) + "(";
    if (o != nullptr) {
        const auto& ops = o->operands();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (i > 0) {
                rendered += ", ";
            }
            rendered += gen.generate(ops[i]);
        }
    }
    rendered += ")";
    return rendered;
}

} // namespace

std::string CExpressionGenerator::generate(DataflowObject* obj) {
    result_.clear();
    if (obj) {
        obj->accept(*this);
    }
    return result_;
}

void CExpressionGenerator::visit(Constant* c) {
    std::string formatted;

    if (try_format_string_array(c, formatted)) {
        result_ = formatted;
        return;
    }

    if (c->size_bytes == 1) {
        const std::uint8_t byte = static_cast<std::uint8_t>(c->value() & 0xFF);
        if (is_printable_ascii(byte)) {
            result_ = escape_char_literal(byte);
            return;
        }
    }

    const std::size_t threshold = configured_hex_threshold();
    const std::uint64_t value = c->value();
    const bool use_hex = threshold == 0 || value > threshold;

    if (use_hex) {
        formatted = "0x" + hex_u64(value);
    } else {
        formatted = std::to_string(value);
    }

    formatted += integer_suffix(c);
    result_ = formatted;
}

void CExpressionGenerator::visit(Variable* v) {
    std::string base_name = v->name();

    // Check if this variable is a parameter register with a display name.
    if (v->is_parameter() || !param_display_names_.empty()) {
        // Normalize to lowercase for lookup.
        std::string lower_name = base_name;
        for (char& c : lower_name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        auto it = param_display_names_.find(lower_name);
        if (it != param_display_names_.end()) {
            // Use the display name, but still append SSA suffix if present.
            if (v->ssa_version() > 0) {
                result_ = it->second + "_" + std::to_string(v->ssa_version());
            } else {
                result_ = it->second;
            }
            return;
        }
    }

    if (v->ssa_version() > 0) {
        result_ = base_name + "_" + std::to_string(v->ssa_version());
    } else {
        result_ = base_name;
    }
}

void CExpressionGenerator::visit(GlobalVariable* v) {
    // Global names are rendered without SSA suffixes for stable declarations.
    result_ = v->name();
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
            const int parent_prec = precedence_for_operation(type);
            auto render_operand = [&](Expression* operand, bool is_rhs) {
                std::string rendered = generate(operand);
                const int child_prec = precedence_for_expression(operand);
                const bool needs_brackets =
                    (child_prec < parent_prec) ||
                    (is_rhs && child_prec == parent_prec && needs_parentheses_for_equal_precedence_rhs(type));
                if (needs_brackets) {
                    return "(" + rendered + ")";
                }
                return rendered;
            };

            result_ = render_operand(ops[0], false) + infix + render_operand(ops[1], true);
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
                if (auto* g = dyn_cast<GlobalVariable>(ops[0])) {
                    result_ = generate(g);
                } else if (o->array_access().has_value() && o->array_access()->base && o->array_access()->index) {
                    result_ = generate(o->array_access()->base) + "[" + generate(o->array_access()->index) + "]";
                } else {
                    result_ = "*(" + generate(ops[0]) + ")";
                }
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

    // --- Unknown operation ---
    if (type == OperationType::unknown) {
        result_ = format_unknown_operation(o, *this, "__aletheia_unknown_op");
        return;
    }

    // --- Fallback ---
    result_ = format_unknown_operation(o, *this, "__aletheia_unhandled_op");
}

void CExpressionGenerator::visit(Call* c) {
    std::string func = generate(c->target());
    std::string args;
    for (size_t i = 0; i < c->arg_count(); ++i) {
        if (i > 0) args += ", ";
        args += generate(c->arg(i));
    }
    result_ = func + "(" + args + ")";
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

    const int parent_prec = precedence_for_operation(c->type());
    auto render_operand = [&](Expression* operand, bool is_rhs) {
        std::string rendered = generate(operand);
        const int child_prec = precedence_for_expression(operand);
        const bool needs_brackets =
            (child_prec < parent_prec) ||
            (is_rhs && child_prec == parent_prec && needs_parentheses_for_equal_precedence_rhs(c->type()));
        if (needs_brackets) {
            return "(" + rendered + ")";
        }
        return rendered;
    };

    result_ = render_operand(c->lhs(), false) + op_str + render_operand(c->rhs(), true);
}

void CExpressionGenerator::visit_assignment(Assignment* i) {
    if (auto* rhs_op = dyn_cast<Operation>(i->value());
        rhs_op != nullptr && rhs_op->operands().size() == 2) {
        const OperationType op_type = rhs_op->type();
        const char* op_symbol = compound_operator_symbol(op_type);
        if (op_symbol != nullptr) {
            Expression* left = rhs_op->operands()[0];
            Expression* right = rhs_op->operands()[1];

            Expression* target_operand = nullptr;
            if (expressions_equivalent(i->destination(), left)) {
                target_operand = right;
            } else if (is_commutative_for_compound(op_type) && expressions_equivalent(i->destination(), right)) {
                target_operand = left;
            }

            if (target_operand != nullptr) {
                int unit_sign = 0;
                if ((op_type == OperationType::add || op_type == OperationType::sub) &&
                    constant_is_plus_minus_one(target_operand, unit_sign)) {
                    const bool is_increment =
                        (op_type == OperationType::add && unit_sign > 0) ||
                        (op_type == OperationType::sub && unit_sign < 0);
                    result_ = generate(i->destination()) + (is_increment ? "++" : "--");
                    return;
                }

                result_ = generate(i->destination()) + " " + op_symbol + "= " + generate(target_operand);
                return;
            }
        }
    }

    std::string lhs = generate(i->destination());
    std::string rhs = generate(i->value());
    if (lhs.empty() || is_unknown_placeholder(lhs)) {
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
    cfg_fallback_mode_ = false;

    // Set up parameter display name mapping for the expression generator.
    {
        std::unordered_map<std::string, std::string> param_names;
        for (const auto& [reg, info] : task.parameter_registers()) {
            param_names[reg] = info.name;
        }
        expr_gen_.set_parameter_names(param_names);
    }

    auto global_decls = GlobalDeclarationGenerator::generate(task);
    for (const auto& decl : global_decls) {
        lines_.push_back(decl);
    }
    if (!global_decls.empty()) {
        lines_.push_back("");
    }

    // Generate function signature
    std::string sig = "void "; // default
    if (task.function_type()) {
        if (auto* func_type = type_dyn_cast<FunctionTypeDef>(task.function_type().get())) {
            sig = func_type->return_type()->to_string() + " ";
        } else {
            sig = task.function_type()->to_string() + " ";
        }
    }

    std::string name = task.function_name().empty() ? "sub_" + std::to_string(task.function_address()) : task.function_name();
    sig += name + "(";

    if (task.function_type()) {
        if (auto* func_type = type_dyn_cast<FunctionTypeDef>(task.function_type().get())) {
            const auto& params = func_type->parameters();
            // Build index -> name map from parameter_registers.
            std::unordered_map<int, std::string> index_to_name;
            for (const auto& [reg, info] : task.parameter_registers()) {
                index_to_name[info.index] = info.name;
            }
            for (size_t i = 0; i < params.size(); ++i) {
                if (i > 0) sig += ", ";
                auto it = index_to_name.find(static_cast<int>(i));
                std::string pname = (it != index_to_name.end() && !it->second.empty())
                    ? it->second
                    : "a" + std::to_string(i + 1);
                sig += params[i]->to_string() + " " + pname;
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
        cfg_fallback_mode_ = is_cfg_fallback_root(task.ast()->root());
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
    cfg_fallback_mode_ = false;

    if (forest && forest->root()) {
        cfg_fallback_mode_ = is_cfg_fallback_root(forest->root());
        visit_node(forest->root());
    }

    if (!current_line_.empty()) {
        lines_.push_back(current_line_);
    }

    return lines_;
}

void CodeVisitor::visit_node(AstNode* node) {
    if (!node) return;

    if (CodeNode* cnode = ast_dyn_cast<CodeNode>(node)) {
        BasicBlock* block = cnode->block();
        if (block) {
            if (cfg_fallback_mode_) {
                const int label_indent = indent_level_ > 0 ? indent_level_ - 1 : 0;
                lines_.push_back(std::string(label_indent * 4, ' ') + block_label(block) + ":");
            }

            for (Instruction* inst : block->instructions()) {
                indent();

                if (auto* branch = dyn_cast<Branch>(inst)) {
                    const std::string cond = expr_gen_.generate(branch->condition());
                    if (cfg_fallback_mode_) {
                        auto [true_edge, false_edge] = pick_true_false_edges(block);
                        if (true_edge && false_edge) {
                            current_line_ += "if (" + cond + ") goto " + block_label(true_edge->target())
                                + "; else goto " + block_label(false_edge->target()) + ";";
                        } else if (true_edge) {
                            current_line_ += "if (" + cond + ") goto " + block_label(true_edge->target()) + ";";
                        } else if (false_edge) {
                            current_line_ += "if (!(" + cond + ")) goto " + block_label(false_edge->target()) + ";";
                        } else {
                            current_line_ += "/* branch if (" + cond + ") */";
                        }

                        if (!current_line_.empty()) {
                            lines_.push_back(current_line_);
                            current_line_.clear();
                        }
                    } else {
                        current_line_ += "/* branch if (" + cond + ") */";
                        lines_.push_back(current_line_);
                        current_line_.clear();
                    }
                    current_line_.clear();
                    continue;
                }

                if (auto* indirect = dyn_cast<IndirectBranch>(inst)) {
                    current_line_ += "/* indirect branch " + expr_gen_.generate(indirect->expression());
                    if (cfg_fallback_mode_ && !block->successors().empty()) {
                        current_line_ += " -> ";
                        for (std::size_t i = 0; i < block->successors().size(); ++i) {
                            if (i > 0) {
                                current_line_ += ", ";
                            }
                            Edge* edge = block->successors()[i];
                            current_line_ += block_label(edge ? edge->target() : nullptr);
                        }
                    }
                    current_line_ += " */";
                    lines_.push_back(current_line_);
                    current_line_.clear();
                    continue;
                }

                std::string expr = expr_gen_.generate(inst);
                if (!expr.empty()) {
                    current_line_ += expr;
                    if (!is_comment_line(expr)) {
                        current_line_ += ";";
                    }
                    lines_.push_back(current_line_);
                    current_line_.clear();
                }
            }

            if (cfg_fallback_mode_ && !block->instructions().empty()) {
                Instruction* tail = block->instructions().back();
                const bool has_explicit_flow =
                    isa<Branch>(tail) ||
                    isa<IndirectBranch>(tail) ||
                    isa<Return>(tail) ||
                    isa<BreakInstr>(tail) ||
                    isa<ContinueInstr>(tail);

                if (!has_explicit_flow) {
                    Edge* next_edge = nullptr;
                    for (Edge* edge : block->successors()) {
                        if (!edge) {
                            continue;
                        }
                        if (edge->type() == EdgeType::Unconditional || edge->type() == EdgeType::Fallthrough) {
                            next_edge = edge;
                            break;
                        }
                    }

                    if (next_edge) {
                        indent();
                        current_line_ += "goto " + block_label(next_edge->target()) + ";";
                        lines_.push_back(current_line_);
                        current_line_.clear();
                    }
                }
            }
        }
    } else if (SeqNode* snode = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : snode->nodes()) {
            visit_node(child);
        }
    } else if (IfNode* inode = ast_dyn_cast<IfNode>(node)) {
        visit_if_chain(inode, false);
    } else if (auto* snode = ast_dyn_cast<SwitchNode>(node)) {
        indent();
        std::string cond_str = "/* switch_expr */";
        if (snode->cond()) {
            if (auto* expr_ast = ast_dyn_cast<ExprAstNode>(snode->cond())) {
                cond_str = expr_gen_.generate(expr_ast->expr());
            }
        }

        current_line_ += "switch (" + cond_str + ") {";
        lines_.push_back(current_line_);
        current_line_.clear();

        indent_level_++;
        for (CaseNode* cnode : snode->cases()) {
            visit_node(cnode);
        }
        indent_level_--;

        indent();
        current_line_ += "}";
        lines_.push_back(current_line_);
        current_line_.clear();
    } else if (auto* cnode = ast_dyn_cast<CaseNode>(node)) {
        indent();
        if (cnode->is_default()) {
            current_line_ += "default:";
        } else {
            current_line_ += "case " + std::to_string(cnode->value()) + ":";
        }
        lines_.push_back(current_line_);
        current_line_.clear();

        indent_level_++;
        visit_node(cnode->body());
        if (cnode->break_case()) {
            indent();
            current_line_ += "break;";
            lines_.push_back(current_line_);
            current_line_.clear();
        }
        indent_level_--;
    } else if (auto* dowhile = ast_dyn_cast<DoWhileLoopNode>(node)) {
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
    } else if (auto* forloop = ast_dyn_cast<ForLoopNode>(node)) {
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
    } else if (LoopNode* lnode = ast_dyn_cast<LoopNode>(node)) {
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

void CodeVisitor::visit_if_chain(IfNode* inode, bool else_if_prefix) {
    if (!inode) {
        return;
    }

    std::string cond_str = "/* condition */";
    Expression* cond_expr = nullptr;
    if (inode->cond()) {
        if (auto* expr_ast = ast_dyn_cast<ExprAstNode>(inode->cond())) {
            cond_expr = expr_ast->expr();
            cond_str = expr_gen_.generate(cond_expr);
        }
    }

    AstNode* true_branch = inode->true_branch();
    AstNode* false_branch = inode->false_branch();
    const bool true_emits = node_emits_code(true_branch);
    const bool false_emits = node_emits_code(false_branch);

    if (!true_emits && !false_emits) {
        return;
    }

    bool swapped_for_emission = false;
    if (!true_emits && false_emits) {
        std::swap(true_branch, false_branch);
        cond_str = cond_expr ? render_negated_condition(cond_expr, expr_gen_) : negate_condition_string(cond_str);
        swapped_for_emission = true;
    }

    if (!swapped_for_emission && should_swap_if_branches(inode)) {
        std::swap(true_branch, false_branch);
        cond_str = cond_expr ? render_negated_condition(cond_expr, expr_gen_) : negate_condition_string(cond_str);
    }

    indent();
    current_line_ += else_if_prefix ? "} else if (" : "if (";
    current_line_ += cond_str + ") {";
    lines_.push_back(current_line_);
    current_line_.clear();

    indent_level_++;
    visit_node(true_branch);
    indent_level_--;

    if (auto* nested_if = ast_dyn_cast<IfNode>(false_branch)) {
        visit_if_chain(nested_if, true);
        return;
    }

    if (false_branch != nullptr && node_emits_code(false_branch)) {
        indent();
        current_line_ += "} else {";
        lines_.push_back(current_line_);
        current_line_.clear();

        indent_level_++;
        visit_node(false_branch);
        indent_level_--;
    }

    indent();
    current_line_ += "}";
    lines_.push_back(current_line_);
    current_line_.clear();
}

bool CodeVisitor::node_emits_code(AstNode* node) {
    if (!node) {
        return false;
    }

    if (auto* cnode = ast_dyn_cast<CodeNode>(node)) {
        BasicBlock* block = cnode->block();
        if (!block) {
            return false;
        }

        if (cfg_fallback_mode_) {
            return true;
        }

        for (Instruction* inst : block->instructions()) {
            if (!inst) {
                continue;
            }
            if (isa<Branch>(inst)) {
                return true;
            }
            if (isa<IndirectBranch>(inst)) {
                return true;
            }
            if (!expr_gen_.generate(inst).empty()) {
                return true;
            }
        }
        return false;
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            if (node_emits_code(child)) {
                return true;
            }
        }
        return false;
    }

    if (auto* ifn = ast_dyn_cast<IfNode>(node)) {
        return node_emits_code(ifn->true_branch()) || node_emits_code(ifn->false_branch());
    }

    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        return node_emits_code(loop->body());
    }

    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->cases()) {
            if (node_emits_code(case_node)) {
                return true;
            }
        }
        return false;
    }

    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        return node_emits_code(case_node->body());
    }

    return true;
}

void CodeVisitor::indent() {
    current_line_ = std::string(indent_level_ * 4, ' ');
}

} // namespace aletheia
