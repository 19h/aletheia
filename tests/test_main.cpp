#include <iostream>
#include <vector>
#include <cstdlib>
#include "../src/common/arena.hpp"
#include "../src/dewolf/structures/cfg.hpp"
#include "../src/dewolf/structures/dataflow.hpp"
#include "../src/dewolf/structures/types.hpp"
#include "../src/dewolf/ssa/dominators.hpp"
#include "../src/dewolf/ssa/ssa_constructor.hpp"

#include "../src/dewolf/pipeline/pipeline.hpp"

#define ASSERT_TRUE(cond) if (!(cond)) { \
    std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    std::exit(1); \
}
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

using namespace dewolf;

void test_arena() {
    DecompilerArena arena;
    auto* b1 = arena.create<BasicBlock>(1);
    auto* b2 = arena.create<BasicBlock>(2);
    ASSERT_EQ(b1->id(), 1);
    ASSERT_EQ(b2->id(), 2);
    
    // Test large allocations
    for (int i = 0; i < 10000; i++) {
        arena.create<BasicBlock>(i);
    }
    
    std::cout << "[+] test_arena passed.\n";
}

void test_dominators() {
    DecompilerArena arena;
    ControlFlowGraph cfg;

    // Diamond pattern: A -> B, A -> C, B -> D, C -> D
    auto* A = arena.create<BasicBlock>(1);
    auto* B = arena.create<BasicBlock>(2);
    auto* C = arena.create<BasicBlock>(3);
    auto* D = arena.create<BasicBlock>(4);

    cfg.set_entry_block(A);
    cfg.add_block(A); cfg.add_block(B); cfg.add_block(C); cfg.add_block(D);

    auto* e1 = arena.create<Edge>(A, B, EdgeType::Unconditional);
    auto* e2 = arena.create<Edge>(A, C, EdgeType::Unconditional);
    auto* e3 = arena.create<Edge>(B, D, EdgeType::Unconditional);
    auto* e4 = arena.create<Edge>(C, D, EdgeType::Unconditional);

    A->add_successor(e1); B->add_predecessor(e1);
    A->add_successor(e2); C->add_predecessor(e2);
    B->add_successor(e3); D->add_predecessor(e3);
    C->add_successor(e4); D->add_predecessor(e4);

    DominatorTree dom(cfg);

    // Dominance checks
    ASSERT_EQ(dom.idom(B), A);
    ASSERT_EQ(dom.idom(C), A);
    ASSERT_EQ(dom.idom(D), A);
    ASSERT_EQ(dom.idom(A), nullptr);

    ASSERT_TRUE(dom.strictly_dominates(A, B));
    ASSERT_TRUE(dom.strictly_dominates(A, D));
    ASSERT_TRUE(!dom.strictly_dominates(B, C));
    ASSERT_TRUE(!dom.strictly_dominates(B, D));

    // Frontier checks
    auto df_B = dom.dominance_frontier(B);
    ASSERT_EQ(df_B.size(), 1);
    ASSERT_EQ(df_B[0], D);

    auto df_A = dom.dominance_frontier(A);
    ASSERT_EQ(df_A.size(), 0);

    std::cout << "[+] test_dominators passed.\n";
}

void test_ssa_phi_insertion() {
    DecompilerArena arena;

    DecompilerTask task(0);
    task.set_cfg(std::make_unique<ControlFlowGraph>());
    ControlFlowGraph* tcfg = task.cfg();

    auto* A = task.arena().create<BasicBlock>(1);
    auto* B = task.arena().create<BasicBlock>(2);
    auto* C = task.arena().create<BasicBlock>(3);
    auto* D = task.arena().create<BasicBlock>(4);

    tcfg->set_entry_block(A);
    tcfg->add_block(A); tcfg->add_block(B); tcfg->add_block(C); tcfg->add_block(D);

    auto* e1 = task.arena().create<Edge>(A, B, EdgeType::Unconditional);
    auto* e2 = task.arena().create<Edge>(A, C, EdgeType::Unconditional);
    auto* e3 = task.arena().create<Edge>(B, D, EdgeType::Unconditional);
    auto* e4 = task.arena().create<Edge>(C, D, EdgeType::Unconditional);

    A->add_successor(e1); B->add_predecessor(e1);
    A->add_successor(e2); C->add_predecessor(e2);
    B->add_successor(e3); D->add_predecessor(e3);
    C->add_successor(e4); D->add_predecessor(e4);

    // Insert definitions of variable "x" in B and C using the new Assignment class
    auto* varX_B = task.arena().create<Variable>("x", 4);
    auto* valB = task.arena().create<Constant>(42, 4);
    auto* assignB = task.arena().create<Assignment>(varX_B, valB);
    B->add_instruction(assignB);

    auto* varX_C = task.arena().create<Variable>("x", 4);
    auto* valC = task.arena().create<Constant>(99, 4);
    auto* assignC = task.arena().create<Assignment>(varX_C, valC);
    C->add_instruction(assignC);

    SsaConstructor ssa;
    ssa.execute(task);

    // With definitions in B and C, dominance frontier is D.
    // We should expect a Phi node inserted in D.
    bool found_phi = false;
    for (Instruction* inst : D->instructions()) {
        if (dynamic_cast<Phi*>(inst)) {
            found_phi = true;
            break;
        }
    }
    ASSERT_TRUE(found_phi);
    
    std::cout << "[+] test_ssa_phi_insertion passed.\n";
}

void test_instruction_hierarchy() {
    DecompilerArena arena;
    
    // Test Assignment
    auto* dest = arena.create<Variable>("x", 4);
    auto* val = arena.create<Constant>(42, 4);
    auto* assign = arena.create<Assignment>(dest, val);
    
    ASSERT_TRUE(dynamic_cast<Instruction*>(assign) != nullptr);
    ASSERT_TRUE(dynamic_cast<Assignment*>(assign) != nullptr);
    ASSERT_EQ(assign->destination(), dest);
    ASSERT_EQ(assign->value(), val);
    
    auto defs = assign->definitions();
    ASSERT_EQ(defs.size(), 1);
    ASSERT_EQ(defs[0], dest);
    
    // Test Branch
    auto* lhs = arena.create<Variable>("a", 4);
    auto* rhs = arena.create<Constant>(0, 4);
    auto* cond = arena.create<Condition>(OperationType::le, lhs, rhs);
    auto* branch = arena.create<Branch>(cond);
    
    ASSERT_TRUE(dynamic_cast<Instruction*>(branch) != nullptr);
    ASSERT_TRUE(is_branch(branch));
    ASSERT_EQ(branch->condition(), cond);
    
    auto reqs = branch->requirements();
    ASSERT_EQ(reqs.size(), 1);
    ASSERT_EQ(reqs[0], lhs);
    
    // Test Return
    auto* ret_val = arena.create<Variable>("W0", 4);
    auto* ret = arena.create<Return>(std::vector<Expression*>{ret_val});
    ASSERT_TRUE(is_return(ret));
    ASSERT_TRUE(ret->has_value());
    
    // Test Phi
    auto* phi_dest = arena.create<Variable>("x", 4);
    auto* phi_src1 = arena.create<Variable>("x", 4);
    auto* phi_src2 = arena.create<Variable>("x", 4);
    auto* phi_ops = arena.create<ListOperation>(std::vector<Expression*>{phi_src1, phi_src2});
    auto* phi = arena.create<Phi>(phi_dest, phi_ops);
    
    ASSERT_TRUE(dynamic_cast<Assignment*>(phi) != nullptr);
    ASSERT_TRUE(is_phi(phi));
    ASSERT_EQ(phi->dest_var(), phi_dest);
    ASSERT_EQ(phi->operand_list()->operands().size(), 2);
    
    // Test Break, Continue, Comment
    auto* brk = arena.create<BreakInstr>();
    auto* cont = arena.create<ContinueInstr>();
    auto* comment = arena.create<Comment>("test comment");
    
    ASSERT_TRUE(dynamic_cast<Instruction*>(brk) != nullptr);
    ASSERT_TRUE(dynamic_cast<Instruction*>(cont) != nullptr);
    ASSERT_EQ(comment->message(), "test comment");
    
    // Test Condition negate
    ASSERT_EQ(Condition::negate_comparison(OperationType::eq), OperationType::neq);
    ASSERT_EQ(Condition::negate_comparison(OperationType::lt), OperationType::ge);
    
    std::cout << "[+] test_instruction_hierarchy passed.\n";
}

void test_type_system() {
    // Basic integer types
    auto i32 = Integer::int32_t();
    ASSERT_EQ(i32->to_string(), "int");
    ASSERT_EQ(i32->size(), 32);
    ASSERT_EQ(i32->size_bytes(), 4);
    
    auto u64 = Integer::uint64_t();
    ASSERT_EQ(u64->to_string(), "unsigned long");
    ASSERT_EQ(u64->size(), 64);
    
    // Signedness
    auto* i32_raw = dynamic_cast<const Integer*>(i32.get());
    ASSERT_TRUE(i32_raw != nullptr);
    ASSERT_TRUE(i32_raw->is_signed());
    
    auto* u64_raw = dynamic_cast<const Integer*>(u64.get());
    ASSERT_TRUE(u64_raw != nullptr);
    ASSERT_TRUE(!u64_raw->is_signed());
    
    // Float types
    auto f32 = Float::float32();
    ASSERT_EQ(f32->to_string(), "float");
    auto f64 = Float::float64();
    ASSERT_EQ(f64->to_string(), "double");
    
    // CustomType
    auto void_t = CustomType::void_type();
    ASSERT_EQ(void_t->to_string(), "void");
    ASSERT_EQ(void_t->size(), 0);
    
    auto bool_t = CustomType::bool_type();
    ASSERT_EQ(bool_t->to_string(), "bool");
    ASSERT_TRUE(bool_t->is_boolean());
    
    // Pointer types
    auto ptr_int = std::make_shared<const Pointer>(i32, 64);
    ASSERT_EQ(ptr_int->to_string(), "int *");
    ASSERT_EQ(ptr_int->size(), 64);
    
    // Nested pointer
    auto ptr_ptr = std::make_shared<const Pointer>(ptr_int, 64);
    ASSERT_EQ(ptr_ptr->to_string(), "int **");
    
    // Array type
    auto arr = std::make_shared<const ArrayType>(i32, 10);
    ASSERT_EQ(arr->to_string(), "int [10]");
    ASSERT_EQ(arr->size(), 320);
    ASSERT_EQ(arr->count(), 10);
    
    // FunctionTypeDef
    auto func_t = std::make_shared<const FunctionTypeDef>(
        i32, std::vector<TypePtr>{i32, ptr_int});
    ASSERT_EQ(func_t->to_string(), "int(int, int *)");
    
    // Equality
    auto i32_2 = Integer::int32_t();
    ASSERT_TRUE(*i32 == *i32_2);     // same factory -> same instance, but also value-equal
    ASSERT_TRUE(!(*i32 == *u64));    // different types
    ASSERT_TRUE(!(*i32 == *f32));    // cross-type inequality
    
    // UnknownType
    auto unk = UnknownType::instance();
    ASSERT_EQ(unk->to_string(), "unknown type");
    
    // TypeParser
    TypeParser parser(64);
    auto parsed_int = parser.parse("int");
    ASSERT_EQ(parsed_int->to_string(), "int");
    
    auto parsed_ptr = parser.parse("char *");
    ASSERT_EQ(parsed_ptr->to_string(), "char *");
    
    auto parsed_unsigned = parser.parse("unsigned int");
    ASSERT_EQ(parsed_unsigned->to_string(), "unsigned int");
    
    auto parsed_void = parser.parse("void");
    ASSERT_EQ(parsed_void->to_string(), "void");
    
    // Verify IR nodes can hold types
    DecompilerArena arena;
    auto* var = arena.create<Variable>("eax", 4);
    ASSERT_TRUE(var->ir_type() == nullptr);  // default: no type
    var->set_ir_type(i32);
    ASSERT_TRUE(var->ir_type() != nullptr);
    ASSERT_EQ(var->ir_type()->to_string(), "int");
    
    auto* con = arena.create<Constant>(42, 4);
    con->set_ir_type(u64);
    ASSERT_EQ(con->ir_type()->to_string(), "unsigned long");
    
    std::cout << "[+] test_type_system passed.\n";
}

int main() {
    std::cout << "Running DeWolf tests...\n";
    test_arena();
    test_dominators();
    test_ssa_phi_insertion();
    test_instruction_hierarchy();
    test_type_system();
    std::cout << "All tests passed successfully.\n";
    return 0;
}
