#include "lifter.hpp"
#include <ida/lines.hpp>
#include <ida/function.hpp>
#include <ida/type.hpp>
#include <ida/segment.hpp>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include "pipeline/pipeline.hpp"
#include "structures/types.hpp"

namespace dewolf {

namespace {

std::string hex_address_name(ida::Address ea) {
    std::ostringstream oss;
    oss << std::hex << ea;
    return "g_" + oss.str();
}

std::string sanitize_identifier(std::string text) {
    if (auto ptr_pos = text.find("ptr "); ptr_pos != std::string::npos) {
        text = text.substr(ptr_pos + 4);
    }
    if (auto colon_pos = text.rfind(':'); colon_pos != std::string::npos) {
        text = text.substr(colon_pos + 1);
    }

    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) || c == '[' || c == ']';
    }), text.end());

    for (char& c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_')) {
            c = '_';
        }
    }

    if (text.empty()) {
        return text;
    }
    const unsigned char first = static_cast<unsigned char>(text.front());
    if (std::isdigit(first)) {
        text = "g_" + text;
    }
    return text;
}

std::string global_name_from_operand(const ida::instruction::Operand& op, ida::Address insn_addr) {
    auto op_text = ida::instruction::operand_text(insn_addr, op.index());
    if (op_text) {
        std::string sanitized = sanitize_identifier(*op_text);
        if (!sanitized.empty()) {
            return sanitized;
        }
    }
    return hex_address_name(op.value());
}

} // namespace

void Lifter::populate_task_signature(DecompilerTask& task) {
    auto ea = task.function_address();
    
    // Default name
    std::string name = "sub_" + std::to_string(ea);
    
    auto name_res = ida::function::name_at(ea);
    if (name_res) {
        name = *name_res;
    }
    task.set_function_name(name);

    auto type_res = ida::type::retrieve(ea);
    if (type_res) {
        auto& type_info = *type_res;
        TypeParser parser;
        
        if (type_info.is_function()) {
            auto ret_res = type_info.function_return_type();
            auto args_res = type_info.function_argument_types();
            
            TypePtr ret_type = CustomType::void_type();
            if (ret_res) {
                auto ret_str = ret_res->to_string();
                if (ret_str) ret_type = parser.parse(*ret_str);
            }

            std::vector<TypePtr> params;
            if (args_res) {
                for (const auto& arg_type : *args_res) {
                    auto arg_str = arg_type.to_string();
                    if (arg_str) params.push_back(parser.parse(*arg_str));
                }
            }
            
            task.set_function_type(std::make_shared<const FunctionTypeDef>(ret_type, params));
        } else {
            auto type_str = type_info.to_string();
            if (type_str) {
                task.set_function_type(parser.parse(*type_str));
            }
        }
    }
}

ida::Result<std::unique_ptr<ControlFlowGraph>> Lifter::lift_function(
    ida::Address function_address,
    std::vector<dewolf_idioms::IdiomTag>* idiom_tags_out) {
    auto flowchart_res = ida::graph::flowchart(function_address);
    if (!flowchart_res) {
        return std::unexpected(flowchart_res.error());
    }

    auto cfg = std::make_unique<ControlFlowGraph>();
    std::unordered_map<int, BasicBlock*> block_map;

    int id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = arena_.create<BasicBlock>(id);
        block_map[id] = block;
        cfg->add_block(block);
        
        if (id == 0) {
            cfg->set_entry_block(block);
        }
        id++;
    }

    id = 0;
    for (const auto& ida_block : *flowchart_res) {
        BasicBlock* block = block_map[id];

        if (idiom_tags_out != nullptr) {
            auto block_tags = idiom_matcher_.match_block(ida_block);
            idiom_tags_out->insert(
                idiom_tags_out->end(),
                std::make_move_iterator(block_tags.begin()),
                std::make_move_iterator(block_tags.end()));
        }

        // Lift instructions
        for (ida::Address addr = ida_block.start; addr < ida_block.end; ) {
            auto insn_res = ida::instruction::decode(addr);
            if (!insn_res) {
                addr += 1;
                continue;
            }
            
            Instruction* lifted_insn = lift_instruction(*insn_res);
            if (lifted_insn) {
                block->add_instruction(lifted_insn);
            }
            
            addr += insn_res->size();
        }

        // Lift edges properly
        const bool likely_switch_dispatch =
            ida_block.successors.size() > 2 &&
            !block->instructions().empty() &&
            dynamic_cast<IndirectBranch*>(block->instructions().back()) != nullptr;

        if (likely_switch_dispatch) {
            std::vector<int> ordered_targets;
            std::unordered_map<int, std::vector<std::int64_t>> target_case_values;

            for (std::size_t case_index = 0; case_index < ida_block.successors.size(); ++case_index) {
                const int succ_id = ida_block.successors[case_index];
                if (!target_case_values.contains(succ_id)) {
                    ordered_targets.push_back(succ_id);
                }
                target_case_values[succ_id].push_back(static_cast<std::int64_t>(case_index));
            }

            for (int succ_id : ordered_targets) {
                if (!block_map.contains(succ_id)) {
                    continue;
                }
                BasicBlock* target = block_map[succ_id];
                auto case_values_it = target_case_values.find(succ_id);
                std::vector<std::int64_t> case_values;
                if (case_values_it != target_case_values.end()) {
                    case_values = case_values_it->second;
                }

                Edge* edge = arena_.create<SwitchEdge>(block, target, std::move(case_values));
                block->add_successor(edge);
                target->add_predecessor(edge);
            }
        } else if (ida_block.successors.size() == 2) {
            BasicBlock* target0 = block_map.contains(ida_block.successors[0]) ? block_map[ida_block.successors[0]] : nullptr;
            BasicBlock* target1 = block_map.contains(ida_block.successors[1]) ? block_map[ida_block.successors[1]] : nullptr;
            
            if (target0) {
                Edge* e0 = arena_.create<Edge>(block, target0, EdgeType::True);
                block->add_successor(e0);
                target0->add_predecessor(e0);
            }
            if (target1) {
                Edge* e1 = arena_.create<Edge>(block, target1, EdgeType::False);
                block->add_successor(e1);
                target1->add_predecessor(e1);
            }
        } else {
            for (int succ_id : ida_block.successors) {
                if (block_map.contains(succ_id)) {
                    BasicBlock* target = block_map[succ_id];
                    Edge* edge = arena_.create<Edge>(block, target, EdgeType::Unconditional);
                    block->add_successor(edge);
                    target->add_predecessor(edge);
                }
            }
        }
        
        id++;
    }

    return cfg;
}

std::vector<Expression*> Lifter::lift_operands(const ida::instruction::Instruction& insn) {
    std::vector<Expression*> operands;
    for (const auto& op : insn.operands()) {
        if (op.type() == ida::instruction::OperandType::None) continue;
        operands.push_back(lift_operand(op, insn.address()));
    }
    return operands;
}

Expression* Lifter::lift_operand(const ida::instruction::Operand& op, ida::Address insn_addr) {
    Expression* expr = nullptr;
    if (op.is_register()) {
        expr = arena_.create<Variable>(op.register_name(), op.byte_width());
    } else if (op.is_immediate()) {
        expr = arena_.create<Constant>(op.value(), op.byte_width());
    } else if (op.is_memory()) {
        const std::size_t width = op.byte_width() > 0 ? static_cast<std::size_t>(op.byte_width()) : 8U;
        if (op.type() == ida::instruction::OperandType::MemoryDirect) {
            const std::string global_name = global_name_from_operand(op, insn_addr);
            bool is_const = false;
            if (auto seg = ida::segment::at(op.value())) {
                is_const = !seg->permissions().write;
            }

            auto* initial_value = arena_.create<Constant>(0, width);
            auto* global = arena_.create<GlobalVariable>(global_name, width, initial_value, is_const);
            global->set_ir_type(std::make_shared<Integer>(width * 8, false));

            expr = arena_.create<Operation>(
                OperationType::deref,
                std::vector<Expression*>{global},
                width);
        } else {
            auto base = arena_.create<Variable>("mem_" + std::to_string(op.value()), width);
            expr = arena_.create<Operation>(OperationType::deref, std::vector<Expression*>{base}, width);
        }
    } else {
        auto txt_res = ida::instruction::operand_text(insn_addr, op.index());
        if (txt_res) {
            std::string clean = *txt_res;
            expr = arena_.create<Variable>(clean, op.byte_width());
        } else {
            expr = arena_.create<Variable>("op_" + std::to_string(op.index()), op.byte_width());
        }
    }
    
    // Attach types to Variable and Constant nodes during lifting
    if (expr && op.byte_width() > 0) {
        expr->set_ir_type(std::make_shared<Integer>(op.byte_width() * 8, false));
    }
    return expr;
}

// Helper: create a binary operation assignment.
// 3-operand form: dest = src1 OP src2
// 2-operand form: dest = dest OP src
Assignment* Lifter::make_binary_assign(OperationType op_type,
                                       std::vector<Expression*>& operands,
                                       ida::Address addr) {
    Assignment* assign = nullptr;
    if (operands.size() >= 3) {
        auto* rhs = arena_.create<Operation>(op_type,
            std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
        assign = arena_.create<Assignment>(operands[0], rhs);
    } else if (operands.size() == 2) {
        auto* rhs = arena_.create<Operation>(op_type,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        assign = arena_.create<Assignment>(operands[0], rhs);
    } else if (operands.size() == 1) {
        // Unary form (e.g., NEG, NOT, INC, DEC handled separately)
        assign = arena_.create<Assignment>(operands[0], operands[0]);
    }
    if (assign) assign->set_address(addr);
    return assign;
}

// Helper: create a unary operation assignment.
// dest = OP(src)  (1 operand: dest = OP(dest))
Assignment* Lifter::make_unary_assign(OperationType op_type,
                                      std::vector<Expression*>& operands,
                                      ida::Address addr) {
    if (operands.empty()) return nullptr;
    auto* rhs = arena_.create<Operation>(op_type,
        std::vector<Expression*>{operands[0]}, operands[0]->size_bytes);
    auto* assign = arena_.create<Assignment>(operands[0], rhs);
    assign->set_address(addr);
    return assign;
}

Instruction* Lifter::lift_instruction(const ida::instruction::Instruction& insn) {
    std::string mnem = insn.mnemonic();
    
    // Normalize to lowercase for mapping
    std::string lmnem = mnem;
    for (char& c : lmnem) c = static_cast<char>(std::tolower(c));

    auto operands = lift_operands(insn);
    ida::Address addr = insn.address();

    // =====================================================================
    // NOP -- skip entirely
    // =====================================================================
    if (lmnem == "nop" || lmnem == "fnop" || lmnem == "endbr64" || lmnem == "endbr32") {
        return nullptr;
    }

    // =====================================================================
    // Return instructions
    // =====================================================================
    if (lmnem == "ret" || lmnem == "retn" || lmnem == "retf") {
        auto* ret = arena_.create<Return>(std::move(operands));
        ret->set_address(addr);
        return ret;
    }

    // =====================================================================
    // x86 conditional branch (Jcc)
    // =====================================================================
    {
        OperationType cmp = OperationType::unknown;
        // Signed comparisons
        if (lmnem == "je" || lmnem == "jz")       cmp = OperationType::eq;
        else if (lmnem == "jne" || lmnem == "jnz") cmp = OperationType::neq;
        else if (lmnem == "jl" || lmnem == "jnge") cmp = OperationType::lt;
        else if (lmnem == "jle" || lmnem == "jng") cmp = OperationType::le;
        else if (lmnem == "jg" || lmnem == "jnle") cmp = OperationType::gt;
        else if (lmnem == "jge" || lmnem == "jnl") cmp = OperationType::ge;
        // Unsigned comparisons
        else if (lmnem == "jb" || lmnem == "jnae" || lmnem == "jc")  cmp = OperationType::lt_us;
        else if (lmnem == "jbe" || lmnem == "jna")                    cmp = OperationType::le_us;
        else if (lmnem == "ja" || lmnem == "jnbe")                    cmp = OperationType::gt_us;
        else if (lmnem == "jae" || lmnem == "jnb" || lmnem == "jnc") cmp = OperationType::ge_us;
        // Sign/overflow/parity (approximate as comparisons with flags)
        else if (lmnem == "js")  cmp = OperationType::lt;   // SF=1 ~ negative
        else if (lmnem == "jns") cmp = OperationType::ge;   // SF=0 ~ non-negative
        else if (lmnem == "jo")  cmp = OperationType::neq;  // OF=1 (approximate)
        else if (lmnem == "jno") cmp = OperationType::eq;   // OF=0 (approximate)
        else if (lmnem == "jp" || lmnem == "jpe")  cmp = OperationType::eq;   // PF=1 (approximate)
        else if (lmnem == "jnp" || lmnem == "jpo") cmp = OperationType::neq;  // PF=0 (approximate)

        if (cmp != OperationType::unknown) {
            Expression* lhs = !operands.empty() ? operands[0] : arena_.create<Variable>("flags", 1);
            Expression* rhs = operands.size() >= 2 ? operands[1] : arena_.create<Constant>(0, lhs->size_bytes);
            auto* cond = arena_.create<Condition>(cmp, lhs, rhs);
            auto* branch = arena_.create<Branch>(cond);
            branch->set_address(addr);
            return branch;
        }
    }

    // =====================================================================
    // ARM conditional branch instructions
    // =====================================================================
    {
        OperationType cmp = OperationType::unknown;
        if (lmnem == "b.le" || lmnem == "ble") cmp = OperationType::le;
        else if (lmnem == "b.lt" || lmnem == "blt") cmp = OperationType::lt;
        else if (lmnem == "b.ge" || lmnem == "bge") cmp = OperationType::ge;
        else if (lmnem == "b.gt" || lmnem == "bgt") cmp = OperationType::gt;
        else if (lmnem == "b.eq" || lmnem == "beq") cmp = OperationType::eq;
        else if (lmnem == "b.ne" || lmnem == "bne") cmp = OperationType::neq;
        // ARM unsigned comparisons
        else if (lmnem == "b.lo" || lmnem == "b.cc") cmp = OperationType::lt_us;
        else if (lmnem == "b.ls") cmp = OperationType::le_us;
        else if (lmnem == "b.hi") cmp = OperationType::gt_us;
        else if (lmnem == "b.hs" || lmnem == "b.cs") cmp = OperationType::ge_us;
        // ARM CBZ/CBNZ (compare and branch)
        else if (lmnem == "cbz")  cmp = OperationType::eq;
        else if (lmnem == "cbnz") cmp = OperationType::neq;
        // ARM TBZ/TBNZ (test bit and branch) -- approximate
        else if (lmnem == "tbz")  cmp = OperationType::eq;
        else if (lmnem == "tbnz") cmp = OperationType::neq;

        if (cmp != OperationType::unknown) {
            Expression* lhs = !operands.empty() ? operands[0] : arena_.create<Variable>("flags", 1);
            Expression* rhs = operands.size() >= 2 ? operands[1] : arena_.create<Constant>(0, lhs->size_bytes);
            auto* cond = arena_.create<Condition>(cmp, lhs, rhs);
            auto* branch = arena_.create<Branch>(cond);
            branch->set_address(addr);
            return branch;
        }
    }

    // =====================================================================
    // Unconditional jump
    // =====================================================================
    if (lmnem == "jmp" || lmnem == "b" || lmnem == "br") {
        // Unconditional jumps are represented by CFG edges, not instructions.
        // If there's a computed target (indirect jump), create an IndirectBranch.
        if (!operands.empty()) {
            auto* ib = arena_.create<IndirectBranch>(operands[0]);
            ib->set_address(addr);
            return ib;
        }
        return nullptr;  // Direct jump: handled by CFG edges
    }

    // =====================================================================
    // CMP / TEST -- flag-setting without storing result
    // =====================================================================
    if (lmnem == "cmp" && operands.size() >= 2) {
        auto* op = arena_.create<Operation>(OperationType::sub,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* flags = arena_.create<Variable>("flags", operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(flags, op);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "test" && operands.size() >= 2) {
        auto* op = arena_.create<Operation>(OperationType::bit_and,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* flags = arena_.create<Variable>("flags", operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(flags, op);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 CALL instruction
    // =====================================================================
    if (lmnem == "call") {
        Expression* target = !operands.empty() ? operands[0] : arena_.create<Variable>("unknown_func", 8);
        std::vector<Expression*> args(operands.begin() + (operands.empty() ? 0 : 1), operands.end());
        auto* call = arena_.create<Call>(target, std::move(args), 8);
        // Call result assigned to a synthetic return-value variable
        auto* ret_var = arena_.create<Variable>("ret", 8);
        auto* assign = arena_.create<Assignment>(ret_var, call);
        assign->set_address(addr);
        return assign;
    }
    // ARM BL (branch-and-link = call)
    if (lmnem == "bl" || lmnem == "blr") {
        Expression* target = !operands.empty() ? operands[0] : arena_.create<Variable>("unknown_func", 8);
        std::vector<Expression*> args(operands.begin() + (operands.empty() ? 0 : 1), operands.end());
        auto* call = arena_.create<Call>(target, std::move(args), 8);
        auto* ret_var = arena_.create<Variable>("x0", 8); // ARM return in x0
        auto* assign = arena_.create<Assignment>(ret_var, call);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // Arithmetic / Logic (binary operations)
    // =====================================================================
    {
        OperationType arith = OperationType::unknown;
        // x86 arithmetic
        if      (lmnem == "add")  arith = OperationType::add;
        else if (lmnem == "adc")  arith = OperationType::add_with_carry;
        else if (lmnem == "sub")  arith = OperationType::sub;
        else if (lmnem == "sbb")  arith = OperationType::sub_with_carry;
        else if (lmnem == "imul") arith = OperationType::mul;
        else if (lmnem == "and")  arith = OperationType::bit_and;
        else if (lmnem == "or")   arith = OperationType::bit_or;
        else if (lmnem == "xor")  arith = OperationType::bit_xor;
        // x86 shifts
        else if (lmnem == "shl" || lmnem == "sal") arith = OperationType::shl;
        else if (lmnem == "shr")  arith = OperationType::shr_us;
        else if (lmnem == "sar")  arith = OperationType::shr;
        // x86 rotates
        else if (lmnem == "rol")  arith = OperationType::left_rotate;
        else if (lmnem == "ror")  arith = OperationType::right_rotate;
        else if (lmnem == "rcl")  arith = OperationType::left_rotate_carry;
        else if (lmnem == "rcr")  arith = OperationType::right_rotate_carry;
        // ARM arithmetic
        else if (lmnem == "adds") arith = OperationType::add;
        else if (lmnem == "subs") arith = OperationType::sub;
        else if (lmnem == "mul")  arith = OperationType::mul;
        else if (lmnem == "sdiv") arith = OperationType::div;
        else if (lmnem == "udiv") arith = OperationType::div_us;
        // ARM logic
        else if (lmnem == "ands" || lmnem == "tst") arith = OperationType::bit_and;
        else if (lmnem == "orr")  arith = OperationType::bit_or;
        else if (lmnem == "eor")  arith = OperationType::bit_xor;
        // ARM shifts
        else if (lmnem == "lsl")  arith = OperationType::shl;
        else if (lmnem == "lsr")  arith = OperationType::shr_us;
        else if (lmnem == "asr")  arith = OperationType::shr;
        else if (lmnem == "ror" /* ARM ror */) arith = OperationType::right_rotate;
        // ARM multiply-add/sub
        else if (lmnem == "madd" && operands.size() >= 4) {
            // MADD Xd, Xn, Xm, Xa -> Xd = Xa + Xn * Xm
            auto* product = arena_.create<Operation>(OperationType::mul,
                std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
            auto* sum = arena_.create<Operation>(OperationType::add,
                std::vector<Expression*>{operands[3], product}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], sum);
            assign->set_address(addr);
            return assign;
        }
        else if (lmnem == "msub" && operands.size() >= 4) {
            // MSUB Xd, Xn, Xm, Xa -> Xd = Xa - Xn * Xm
            auto* product = arena_.create<Operation>(OperationType::mul,
                std::vector<Expression*>{operands[1], operands[2]}, operands[0]->size_bytes);
            auto* diff = arena_.create<Operation>(OperationType::sub,
                std::vector<Expression*>{operands[3], product}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], diff);
            assign->set_address(addr);
            return assign;
        }

        if (arith != OperationType::unknown) {
            auto* result = make_binary_assign(arith, operands, addr);
            if (result) return result;
        }
    }

    // =====================================================================
    // Unary operations: NOT, NEG, INC, DEC
    // =====================================================================
    if (lmnem == "not" && !operands.empty()) {
        return make_unary_assign(OperationType::bit_not, operands, addr);
    }
    if (lmnem == "neg" && !operands.empty()) {
        return make_unary_assign(OperationType::negate, operands, addr);
    }
    if ((lmnem == "inc") && !operands.empty()) {
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* rhs = arena_.create<Operation>(OperationType::add,
            std::vector<Expression*>{operands[0], one}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], rhs);
        assign->set_address(addr);
        return assign;
    }
    if ((lmnem == "dec") && !operands.empty()) {
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* rhs = arena_.create<Operation>(OperationType::sub,
            std::vector<Expression*>{operands[0], one}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], rhs);
        assign->set_address(addr);
        return assign;
    }
    // ARM MVN (bitwise NOT)
    if (lmnem == "mvn" && operands.size() >= 2) {
        auto* rhs = arena_.create<Operation>(OperationType::bit_not,
            std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], rhs);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // LEA -- load effective address (x86)
    // =====================================================================
    if (lmnem == "lea" && operands.size() >= 2) {
        // LEA dest, [address_expression]
        // The memory operand was lifted as deref(addr), strip the deref
        Expression* effective_addr = operands[1];
        if (auto* deref_op = dynamic_cast<Operation*>(effective_addr)) {
            if (deref_op->type() == OperationType::deref && !deref_op->operands().empty()) {
                effective_addr = deref_op->operands()[0];
            }
        }
        auto* assign = arena_.create<Assignment>(operands[0], effective_addr);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // Data movement: MOV, MOVSX, MOVZX, LDR, STR, etc.
    // =====================================================================
    {
        bool is_mov = (lmnem == "mov" || lmnem == "movabs");
        bool is_load = (lmnem == "ldr" || lmnem == "ldur" || lmnem == "ldrb" ||
                        lmnem == "ldrh" || lmnem == "ldrsb" || lmnem == "ldrsh" ||
                        lmnem == "ldrsw");
        bool is_store = (lmnem == "str" || lmnem == "stur" || lmnem == "strb" ||
                         lmnem == "strh");
        bool is_movsx = (lmnem == "movsx" || lmnem == "movsxd" || lmnem == "cwde" ||
                         lmnem == "cdqe" || lmnem == "cbw");
        bool is_movzx = (lmnem == "movzx");
        bool is_xchg = (lmnem == "xchg");

        if (is_movsx && operands.size() >= 2) {
            // Sign-extend: cast src to dest type
            auto* cast_op = arena_.create<Operation>(OperationType::cast,
                std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], cast_op);
            assign->set_address(addr);
            return assign;
        }
        if (is_movzx && operands.size() >= 2) {
            // Zero-extend: cast src to dest type
            auto* cast_op = arena_.create<Operation>(OperationType::cast,
                std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], cast_op);
            assign->set_address(addr);
            return assign;
        }
        if (is_xchg && operands.size() >= 2) {
            // XCHG: swap two operands. Emit as tmp = a; a = b; b = tmp;
            // For simplicity, just emit as two assignments (not perfectly atomic)
            auto* tmp = arena_.create<Variable>("xchg_tmp", operands[0]->size_bytes);
            auto* a1 = arena_.create<Assignment>(tmp, operands[0]);
            a1->set_address(addr);
            // We can only return one instruction, so just return the first assignment
            // The second would require block-level splitting. For now, treat as mov.
            auto* assign = arena_.create<Assignment>(operands[0], operands[1]);
            assign->set_address(addr);
            return assign;
        }

        if ((is_mov || is_load || is_store) && operands.size() >= 2) {
            Expression* dest;
            Expression* src;
            if (is_store) {
                dest = operands[1];
                src = operands[0];
            } else {
                dest = operands[0];
                src = operands[1];
            }
            auto* assign = arena_.create<Assignment>(dest, src);
            assign->set_address(addr);
            return assign;
        }
    }

    // =====================================================================
    // ARM STP/LDP (store/load pair)
    // =====================================================================
    if (lmnem == "stp" && operands.size() >= 3) {
        // STP Xt1, Xt2, [addr] -- store pair. Approximate as store first reg.
        auto* assign = arena_.create<Assignment>(operands[2], operands[0]);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "ldp" && operands.size() >= 3) {
        // LDP Xt1, Xt2, [addr] -- load pair. Approximate as load first reg.
        auto* assign = arena_.create<Assignment>(operands[0], operands[2]);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // ARM ADR/ADRP
    // =====================================================================
    if ((lmnem == "adr" || lmnem == "adrp") && operands.size() >= 2) {
        auto* assign = arena_.create<Assignment>(operands[0], operands[1]);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // ARM CSET / CSEL
    // =====================================================================
    if (lmnem == "cset" && operands.size() >= 2) {
        // CSET Xd, cond -> Xd = (cond ? 1 : 0)
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* zero = arena_.create<Constant>(0, operands[0]->size_bytes);
        auto* ternary = arena_.create<Operation>(OperationType::ternary,
            std::vector<Expression*>{operands[1], one, zero}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], ternary);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "csel" && operands.size() >= 4) {
        // CSEL Xd, Xn, Xm, cond -> Xd = (cond ? Xn : Xm)
        auto* ternary = arena_.create<Operation>(OperationType::ternary,
            std::vector<Expression*>{operands[3], operands[1], operands[2]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], ternary);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 CDQ/CWD/CQO -- sign-extend accumulator into edx:eax pair
    // =====================================================================
    if (lmnem == "cdq" || lmnem == "cwd" || lmnem == "cqo") {
        // These extend the sign of eax/ax/rax into edx/dx/rdx.
        // Approximate as: edx = sar(eax, 31)
        std::size_t sz = (lmnem == "cqo") ? 8 : (lmnem == "cdq") ? 4 : 2;
        std::string src_name = (lmnem == "cqo") ? "rax" : (lmnem == "cdq") ? "eax" : "ax";
        std::string dst_name = (lmnem == "cqo") ? "rdx" : (lmnem == "cdq") ? "edx" : "dx";
        auto* src = arena_.create<Variable>(src_name, sz);
        auto* shift_amt = arena_.create<Constant>(sz * 8 - 1, sz);
        auto* sar_op = arena_.create<Operation>(OperationType::shr,
            std::vector<Expression*>{src, shift_amt}, sz);
        auto* dst = arena_.create<Variable>(dst_name, sz);
        auto* assign = arena_.create<Assignment>(dst, sar_op);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 PUSH/POP -- stack operations (approximate)
    // =====================================================================
    if (lmnem == "push" && !operands.empty()) {
        // push src -> [rsp] = src; rsp -= 8
        // Approximate: just record the assignment to a stack slot
        auto* rsp = arena_.create<Variable>("rsp", 8);
        auto* deref = arena_.create<Operation>(OperationType::deref,
            std::vector<Expression*>{rsp}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(deref, operands[0]);
        assign->set_address(addr);
        return assign;
    }
    if (lmnem == "pop" && !operands.empty()) {
        // pop dest -> dest = [rsp]; rsp += 8
        auto* rsp = arena_.create<Variable>("rsp", 8);
        auto* deref = arena_.create<Operation>(OperationType::deref,
            std::vector<Expression*>{rsp}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], deref);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 BSWAP
    // =====================================================================
    if (lmnem == "bswap" && !operands.empty()) {
        // Emit as a call to a __bswap intrinsic
        auto* target = arena_.create<Variable>("__bswap", 8);
        auto* call = arena_.create<Call>(target,
            std::vector<Expression*>{operands[0]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], call);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 BT/BTS/BTR/BTC -- bit test operations
    // =====================================================================
    if ((lmnem == "bt" || lmnem == "bts" || lmnem == "btr" || lmnem == "btc")
        && operands.size() >= 2) {
        // BT base, offset: test bit. Sets CF. Approximate as bit_and + shift.
        auto* shifted = arena_.create<Operation>(OperationType::shr_us,
            std::vector<Expression*>{operands[0], operands[1]}, operands[0]->size_bytes);
        auto* one = arena_.create<Constant>(1, operands[0]->size_bytes);
        auto* bit = arena_.create<Operation>(OperationType::bit_and,
            std::vector<Expression*>{shifted, one}, 1);
        auto* flags = arena_.create<Variable>("flags", 1);
        auto* assign = arena_.create<Assignment>(flags, bit);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 BSF/BSR -- bit scan
    // =====================================================================
    if ((lmnem == "bsf" || lmnem == "bsr") && operands.size() >= 2) {
        std::string intrinsic = (lmnem == "bsf") ? "__bsf" : "__bsr";
        auto* target = arena_.create<Variable>(intrinsic, 8);
        auto* call = arena_.create<Call>(target,
            std::vector<Expression*>{operands[1]}, operands[0]->size_bytes);
        auto* assign = arena_.create<Assignment>(operands[0], call);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 CMOVcc -- conditional moves
    // =====================================================================
    {
        OperationType cmov_cmp = OperationType::unknown;
        if      (lmnem == "cmove" || lmnem == "cmovz")       cmov_cmp = OperationType::eq;
        else if (lmnem == "cmovne" || lmnem == "cmovnz")     cmov_cmp = OperationType::neq;
        else if (lmnem == "cmovl" || lmnem == "cmovnge")     cmov_cmp = OperationType::lt;
        else if (lmnem == "cmovle" || lmnem == "cmovng")     cmov_cmp = OperationType::le;
        else if (lmnem == "cmovg" || lmnem == "cmovnle")     cmov_cmp = OperationType::gt;
        else if (lmnem == "cmovge" || lmnem == "cmovnl")     cmov_cmp = OperationType::ge;
        else if (lmnem == "cmovb" || lmnem == "cmovnae" || lmnem == "cmovc")  cmov_cmp = OperationType::lt_us;
        else if (lmnem == "cmovbe" || lmnem == "cmovna")     cmov_cmp = OperationType::le_us;
        else if (lmnem == "cmova" || lmnem == "cmovnbe")     cmov_cmp = OperationType::gt_us;
        else if (lmnem == "cmovae" || lmnem == "cmovnb" || lmnem == "cmovnc") cmov_cmp = OperationType::ge_us;
        else if (lmnem == "cmovs")  cmov_cmp = OperationType::lt;
        else if (lmnem == "cmovns") cmov_cmp = OperationType::ge;

        if (cmov_cmp != OperationType::unknown && operands.size() >= 2) {
            // CMOVcc dest, src -> dest = (flags cmp 0) ? src : dest
            auto* flags = arena_.create<Variable>("flags", 1);
            auto* zero = arena_.create<Constant>(0, 1);
            auto* cond = arena_.create<Condition>(cmov_cmp, flags, zero);
            auto* ternary = arena_.create<Operation>(OperationType::ternary,
                std::vector<Expression*>{cond, operands[1], operands[0]}, operands[0]->size_bytes);
            auto* assign = arena_.create<Assignment>(operands[0], ternary);
            assign->set_address(addr);
            return assign;
        }
    }

    // =====================================================================
    // x86 SETcc -- set byte on condition
    // =====================================================================
    {
        OperationType set_cmp = OperationType::unknown;
        if      (lmnem == "sete" || lmnem == "setz")       set_cmp = OperationType::eq;
        else if (lmnem == "setne" || lmnem == "setnz")     set_cmp = OperationType::neq;
        else if (lmnem == "setl" || lmnem == "setnge")     set_cmp = OperationType::lt;
        else if (lmnem == "setle" || lmnem == "setng")     set_cmp = OperationType::le;
        else if (lmnem == "setg" || lmnem == "setnle")     set_cmp = OperationType::gt;
        else if (lmnem == "setge" || lmnem == "setnl")     set_cmp = OperationType::ge;
        else if (lmnem == "setb" || lmnem == "setnae" || lmnem == "setc")  set_cmp = OperationType::lt_us;
        else if (lmnem == "setbe" || lmnem == "setna")     set_cmp = OperationType::le_us;
        else if (lmnem == "seta" || lmnem == "setnbe")     set_cmp = OperationType::gt_us;
        else if (lmnem == "setae" || lmnem == "setnb" || lmnem == "setnc") set_cmp = OperationType::ge_us;
        else if (lmnem == "sets")  set_cmp = OperationType::lt;
        else if (lmnem == "setns") set_cmp = OperationType::ge;

        if (set_cmp != OperationType::unknown && !operands.empty()) {
            // SETcc dest -> dest = (flags cmp 0) ? 1 : 0
            auto* flags = arena_.create<Variable>("flags", 1);
            auto* zero = arena_.create<Constant>(0, 1);
            auto* cond = arena_.create<Condition>(set_cmp, flags, zero);
            auto* one = arena_.create<Constant>(1, 1);
            auto* zero_val = arena_.create<Constant>(0, 1);
            auto* ternary = arena_.create<Operation>(OperationType::ternary,
                std::vector<Expression*>{cond, one, zero_val}, 1);
            auto* assign = arena_.create<Assignment>(operands[0], ternary);
            assign->set_address(addr);
            return assign;
        }
    }

    // =====================================================================
    // x86 DIV/IDIV -- implicit edx:eax operands
    // =====================================================================
    if ((lmnem == "div" || lmnem == "idiv") && !operands.empty()) {
        // DIV src: eax = edx:eax / src, edx = edx:eax % src
        // Approximate: eax = eax / src (ignoring edx high half)
        OperationType div_op = (lmnem == "div") ? OperationType::div_us : OperationType::div;
        std::size_t sz = operands[0]->size_bytes;
        std::string acc_name = (sz == 8) ? "rax" : (sz == 4) ? "eax" : (sz == 2) ? "ax" : "al";
        auto* acc = arena_.create<Variable>(acc_name, sz);
        auto* rhs = arena_.create<Operation>(div_op,
            std::vector<Expression*>{acc, operands[0]}, sz);
        auto* assign = arena_.create<Assignment>(acc, rhs);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // x86 MUL -- unsigned multiply (implicit operands)
    // =====================================================================
    if (lmnem == "mul" && operands.size() == 1) {
        // MUL src: edx:eax = eax * src
        // Approximate: eax = eax * src
        std::size_t sz = operands[0]->size_bytes;
        std::string acc_name = (sz == 8) ? "rax" : (sz == 4) ? "eax" : (sz == 2) ? "ax" : "al";
        auto* acc = arena_.create<Variable>(acc_name, sz);
        auto* rhs = arena_.create<Operation>(OperationType::mul_us,
            std::vector<Expression*>{acc, operands[0]}, sz);
        auto* assign = arena_.create<Assignment>(acc, rhs);
        assign->set_address(addr);
        return assign;
    }

    // =====================================================================
    // Fallback: wrap as unknown Operation inside an Assignment
    // =====================================================================
    auto* op = arena_.create<Operation>(OperationType::unknown, std::move(operands), insn.size());
    auto* mnem_var = arena_.create<Variable>(lmnem, insn.size());
    auto* assign = arena_.create<Assignment>(mnem_var, op);
    assign->set_address(addr);
    return assign;
}

} // namespace dewolf
