#include <iostream>
#include <vector>
#include <cstdlib>
#include "../src/common/arena.hpp"
#include "../src/dewolf/structures/cfg.hpp"
#include "../src/dewolf/structures/dataflow.hpp"
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

int main() {
    std::cout << "Running DeWolf tests...\n";
    test_arena();
    test_dominators();
    test_ssa_phi_insertion();
    test_instruction_hierarchy();
    std::cout << "All tests passed successfully.\n";
    return 0;
}
