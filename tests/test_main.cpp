#include <iostream>
#include <vector>
#include <cstdlib>
#include <utility>
#include <unordered_set>
#include "../src/common/arena.hpp"
#include "../src/dewolf/structures/cfg.hpp"
#include "../src/dewolf/structures/dataflow.hpp"
#include "../src/dewolf/structures/types.hpp"
#include "../src/dewolf/structuring/ast.hpp"
#include "../src/dewolf/structuring/loop_structurer.hpp"
#include "../src/dewolf/ssa/dominators.hpp"
#include "../src/dewolf/ssa/ssa_constructor.hpp"
#include "../src/dewolf/ssa/ssa_destructor.hpp"
#include "../src/dewolf/ssa/minimal_variable_renamer.hpp"
#include "../src/dewolf/ssa/conditional_variable_renamer.hpp"

#include "../src/dewolf/pipeline/pipeline.hpp"
#include "../src/dewolf/pipeline/preprocessing_stages.hpp"
#include "../src/dewolf_logic/range_simplifier.hpp"



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

void test_loop_structurer() {
    DecompilerArena arena;
    
    // -- Test WhileLoopRule --
    // Build: while(true) { if (x > 0) break; y = 1; }
    // Expected: while (x <= 0) { y = 1; }
    {
        auto* blk_break = arena.create<BasicBlock>(100);
        blk_break->add_instruction(arena.create<BreakInstr>());
        auto* break_code = arena.create<CodeNode>(blk_break);
        
        auto* x = arena.create<Variable>("x", 4);
        auto* zero = arena.create<Constant>(0, 4);
        auto* cond = arena.create<Condition>(OperationType::gt, x, zero);
        auto* cond_ast = arena.create<ExprAstNode>(cond);
        auto* if_break = arena.create<IfNode>(cond_ast, break_code, nullptr);
        
        auto* blk_body = arena.create<BasicBlock>(101);
        auto* y = arena.create<Variable>("y", 4);
        auto* one = arena.create<Constant>(1, 4);
        blk_body->add_instruction(arena.create<Assignment>(y, one));
        auto* body_code = arena.create<CodeNode>(blk_body);
        
        auto* seq = arena.create<SeqNode>();
        seq->add_node(if_break);
        seq->add_node(body_code);
        
        auto* loop = arena.create<WhileLoopNode>(seq);
        ASSERT_TRUE(loop->is_endless());
        
        AstNode* result = LoopStructurer::refine_loop(arena, loop);
        auto* refined = dynamic_cast<WhileLoopNode*>(result);
        ASSERT_TRUE(refined != nullptr);
        ASSERT_TRUE(!refined->is_endless());
        ASSERT_TRUE(refined->condition() != nullptr);
        // Condition should be negated: gt -> le
        auto* new_cond = dynamic_cast<Condition*>(refined->condition());
        ASSERT_TRUE(new_cond != nullptr);
        ASSERT_EQ(new_cond->type(), OperationType::le);
    }
    
    // -- Test DoWhileLoopRule --
    // Build: while(true) { y = 1; if (x > 0) break; }
    // Expected: do { y = 1; } while (x <= 0)
    {
        auto* blk_body = arena.create<BasicBlock>(200);
        auto* y = arena.create<Variable>("y", 4);
        auto* one = arena.create<Constant>(1, 4);
        blk_body->add_instruction(arena.create<Assignment>(y, one));
        auto* body_code = arena.create<CodeNode>(blk_body);
        
        auto* blk_break = arena.create<BasicBlock>(201);
        blk_break->add_instruction(arena.create<BreakInstr>());
        auto* break_code = arena.create<CodeNode>(blk_break);
        
        auto* x = arena.create<Variable>("x", 4);
        auto* zero = arena.create<Constant>(0, 4);
        auto* cond = arena.create<Condition>(OperationType::gt, x, zero);
        auto* cond_ast = arena.create<ExprAstNode>(cond);
        auto* if_break = arena.create<IfNode>(cond_ast, break_code, nullptr);
        
        auto* seq = arena.create<SeqNode>();
        seq->add_node(body_code);
        seq->add_node(if_break);
        
        auto* loop = arena.create<WhileLoopNode>(seq);
        AstNode* result = LoopStructurer::refine_loop(arena, loop);
        auto* dowhile = dynamic_cast<DoWhileLoopNode*>(result);
        ASSERT_TRUE(dowhile != nullptr);
        ASSERT_TRUE(dowhile->condition() != nullptr);
        auto* new_cond = dynamic_cast<Condition*>(dowhile->condition());
        ASSERT_TRUE(new_cond != nullptr);
        ASSERT_EQ(new_cond->type(), OperationType::le);
    }
    
    // -- Test SequenceRule --
    // Build: while(true) { y = 1; break; }
    // Expected: { y = 1; } (no loop)
    {
        auto* blk = arena.create<BasicBlock>(300);
        auto* y = arena.create<Variable>("y", 4);
        auto* one = arena.create<Constant>(1, 4);
        blk->add_instruction(arena.create<Assignment>(y, one));
        blk->add_instruction(arena.create<BreakInstr>());
        auto* code = arena.create<CodeNode>(blk);
        
        auto* seq = arena.create<SeqNode>();
        seq->add_node(code);
        
        auto* loop = arena.create<WhileLoopNode>(seq);
        AstNode* result = LoopStructurer::refine_loop(arena, loop);
        // Should NOT be a loop anymore
        ASSERT_TRUE(dynamic_cast<LoopNode*>(result) == nullptr);
    }
    
    // -- Test ConditionToSequenceRule --
    // Build: while(true) { if(cond) { A } else { B; break; } }
    // Expected: while(cond) { A }; B
    {
        auto* blk_a = arena.create<BasicBlock>(400);
        auto* a_var = arena.create<Variable>("a", 4);
        auto* a_val = arena.create<Constant>(1, 4);
        blk_a->add_instruction(arena.create<Assignment>(a_var, a_val));
        auto* code_a = arena.create<CodeNode>(blk_a);
        
        auto* blk_b = arena.create<BasicBlock>(401);
        auto* b_var = arena.create<Variable>("b", 4);
        auto* b_val = arena.create<Constant>(2, 4);
        blk_b->add_instruction(arena.create<Assignment>(b_var, b_val));
        blk_b->add_instruction(arena.create<BreakInstr>());
        auto* code_b = arena.create<CodeNode>(blk_b);
        
        auto* x = arena.create<Variable>("x", 4);
        auto* zero = arena.create<Constant>(0, 4);
        auto* cond = arena.create<Condition>(OperationType::gt, x, zero);
        auto* cond_ast = arena.create<ExprAstNode>(cond);
        auto* if_node = arena.create<IfNode>(cond_ast, code_a, code_b);
        
        auto* loop = arena.create<WhileLoopNode>(static_cast<AstNode*>(if_node));
        AstNode* result = LoopStructurer::refine_loop(arena, loop);
        
        // Result should be a SeqNode: [WhileLoopNode, code_b]
        auto* result_seq = dynamic_cast<SeqNode*>(result);
        ASSERT_TRUE(result_seq != nullptr);
        ASSERT_TRUE(result_seq->size() >= 1);
        auto* new_while = dynamic_cast<WhileLoopNode*>(result_seq->first());
        ASSERT_TRUE(new_while != nullptr);
        ASSERT_TRUE(!new_while->is_endless());
    }
    
    std::cout << "[+] test_loop_structurer passed.\n";
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

void test_range_simplifier() {
    using namespace dewolf_logic;
    DecompilerArena arena;

    // --- Test 1: Contradictory must-values are unfulfillable ---
    {
        ExpressionValues ev(32);
        ev.update_with(OperationType::eq, 5, false);  // x == 5
        ev.update_with(OperationType::eq, 10, false); // x == 10
        ASSERT_TRUE(ev.is_unfulfillable());
    }

    // --- Test 2: Must-value in forbidden set is unfulfillable ---
    {
        ExpressionValues ev(32);
        ev.update_with(OperationType::eq, 5, false);  // x == 5
        ev.update_with(OperationType::neq, 5, false); // x != 5
        ASSERT_TRUE(ev.is_unfulfillable());
    }

    // --- Test 3: Upper < lower is unfulfillable ---
    {
        ExpressionValues ev(32);
        // x <= 3 AND x >= 10  => unfulfillable
        ev.update_with(OperationType::le, 3, false);   // x <= 3
        ev.update_with(OperationType::ge, 10, false);  // x >= 10
        ASSERT_TRUE(ev.is_unfulfillable());
    }

    // --- Test 4: Compatible constraints are NOT unfulfillable ---
    {
        ExpressionValues ev(32);
        ev.update_with(OperationType::le, 100, false);  // x <= 100
        ev.update_with(OperationType::ge, 0, false);    // x >= 0
        ASSERT_TRUE(!ev.is_unfulfillable());
    }

    // --- Test 5: Simplify adds must-value when lower == upper ---
    {
        ExpressionValues ev(32);
        ev.update_with(OperationType::le, 5, false);  // x <= 5
        ev.update_with(OperationType::ge, 5, false);  // x >= 5
        ev.simplify();
        ASSERT_TRUE(!ev.must_values().empty());
        ASSERT_TRUE(ev.must_values().count(5));
    }

    // --- Test 6: BoundRelation::from() extracts from a Condition ---
    {
        auto* var = arena.create<Variable>("eax", 4);
        auto* c10 = arena.create<Constant>(10, 4);
        auto* cond = arena.create<Condition>(OperationType::lt, var, c10, 1);
        auto br = BoundRelation::from(cond);
        ASSERT_TRUE(br.has_value());
        ASSERT_EQ(br->op, OperationType::lt);
        ASSERT_EQ(br->constant_value, 10);
        ASSERT_TRUE(!br->constant_is_lhs);
        ASSERT_EQ(br->variable_expr, var);
    }

    // --- Test 7: BoundRelation::from() rejects two-variable expressions ---
    {
        auto* v1 = arena.create<Variable>("eax", 4);
        auto* v2 = arena.create<Variable>("ebx", 4);
        auto* cond = arena.create<Condition>(OperationType::lt, v1, v2, 1);
        auto br = BoundRelation::from(cond);
        ASSERT_TRUE(!br.has_value());
    }

    // --- Test 8: SingleRangeSimplifier detects tautology ---
    {
        auto* var = arena.create<Variable>("eax", 4);
        // x <= 2147483647 (INT32_MAX) for signed 32-bit => always true
        auto* max_c = arena.create<Constant>(2147483647ULL, 4);
        auto* cond = arena.create<Condition>(OperationType::le, var, max_c, 1);
        auto* result = SingleRangeSimplifier::simplify(cond, arena);
        ASSERT_TRUE(result != nullptr);
        auto* c = dynamic_cast<Constant*>(result);
        ASSERT_TRUE(c != nullptr);
        ASSERT_EQ(c->value(), 1ULL); // true
    }

    // --- Test 9: SingleRangeSimplifier detects unfulfillable strict ---
    {
        auto* var = arena.create<Variable>("eax", 4);
        // INT32_MAX < x for signed 32-bit => false (nothing > MAX)
        auto* max_c = arena.create<Constant>(2147483647ULL, 4);
        auto* cond = arena.create<Condition>(OperationType::lt, max_c, var, 1);
        auto* result = SingleRangeSimplifier::simplify(cond, arena);
        ASSERT_TRUE(result != nullptr);
        auto* c = dynamic_cast<Constant*>(result);
        ASSERT_TRUE(c != nullptr);
        ASSERT_EQ(c->value(), 0ULL); // false
    }

    // --- Test 10: RangeSimplifier::is_unfulfillable on AND of contradicting ---
    {
        auto* var = arena.create<Variable>("eax", 4);
        auto* c3 = arena.create<Constant>(3, 4);
        auto* c10 = arena.create<Constant>(10, 4);
        auto* le3 = arena.create<Condition>(OperationType::le, var, c3, 1);
        auto* ge10 = arena.create<Condition>(OperationType::ge, var, c10, 1);
        auto* and_op = arena.create<Operation>(OperationType::logical_and,
            std::vector<Expression*>{le3, ge10}, 1);

        RangeSimplifier rs;
        ASSERT_TRUE(rs.is_unfulfillable(and_op));
    }

    // --- Test 11: RangeSimplifier::is_unfulfillable returns false for satisfiable ---
    {
        auto* var = arena.create<Variable>("eax", 4);
        auto* c3 = arena.create<Constant>(3, 4);
        auto* c10 = arena.create<Constant>(10, 4);
        auto* ge3 = arena.create<Condition>(OperationType::ge, var, c3, 1);
        auto* le10 = arena.create<Condition>(OperationType::le, var, c10, 1);
        auto* and_op = arena.create<Operation>(OperationType::logical_and,
            std::vector<Expression*>{ge3, le10}, 1);

        RangeSimplifier rs;
        ASSERT_TRUE(!rs.is_unfulfillable(and_op));
    }

    std::cout << "[+] test_range_simplifier passed.\n";
}


#include "../src/dewolf/ssa/phi_dependency_resolver.hpp"

void test_phi_dependency() {
    dewolf::DecompilerArena arena;
    dewolf::ControlFlowGraph cfg;
    
    auto* bb = arena.create<dewolf::BasicBlock>(0x1000);
    cfg.add_block(bb);
    
    auto* var_a = arena.create<dewolf::Variable>("a", 4); var_a->set_ssa_version(1);
    auto* var_b = arena.create<dewolf::Variable>("b", 4); var_b->set_ssa_version(1);
    
    auto* var_a0 = arena.create<dewolf::Variable>("a", 4); var_a0->set_ssa_version(0);
    auto* var_b0 = arena.create<dewolf::Variable>("b", 4); var_b0->set_ssa_version(0);

    // Phi1: a_1 = phi(a_0, b_1)
    auto* phi1_ops = arena.create<dewolf::ListOperation>(std::vector<dewolf::Expression*>{var_a0, var_b});
    auto* phi1 = arena.create<dewolf::Phi>(var_a, phi1_ops);

    // Phi2: b_1 = phi(b_0, a_1)
    auto* phi2_ops = arena.create<dewolf::ListOperation>(std::vector<dewolf::Expression*>{var_b0, var_a});
    auto* phi2 = arena.create<dewolf::Phi>(var_b, phi2_ops);
    
    // Add in reverse topological order or arbitrary order
    bb->add_instruction(phi1);
    bb->add_instruction(phi2);
    
    dewolf::PhiDependencyResolver::resolve(arena, cfg);
    
    auto insts = bb->instructions();
    // We expect the cycle to be broken. One of the Phis should have its definition renamed.
    // And an assignment should be added.
    bool found_copy = false;
    bool found_renamed_phi = false;
    for (auto* inst : insts) {
        if (auto* assign = dynamic_cast<dewolf::Assignment*>(inst)) {
            if (!dynamic_cast<dewolf::Phi*>(inst)) {
                if (auto* dest = dynamic_cast<dewolf::Variable*>(assign->destination())) {
                    if (dest->name() == "a" || dest->name() == "b") {
                        found_copy = true;
                    }
                }
            }
        }
        if (auto* phi = dynamic_cast<dewolf::Phi*>(inst)) {
            if (phi->dest_var()->name().find("copy_") != std::string::npos) {
                found_renamed_phi = true;
            }
        }
    }
    
    if (!found_copy || !found_renamed_phi) {
        std::cerr << "test_phi_dependency FAILED: missing copy or renamed phi.\n";
        exit(1);
    }
    
    std::cout << "[+] test_phi_dependency passed.\n";
}

void test_minimal_variable_renamer() {
    DecompilerTask task(0x6000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(30);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* a1 = task.arena().create<Variable>("a", 4);
    a1->set_ssa_version(1);
    auto* b1 = task.arena().create<Variable>("b", 4);
    b1->set_ssa_version(1);
    auto* c1 = task.arena().create<Variable>("c", 4);
    c1->set_ssa_version(1);

    auto* assign_a = task.arena().create<Assignment>(a1, task.arena().create<Constant>(1, 4));
    auto* assign_b = task.arena().create<Assignment>(b1, task.arena().create<Constant>(2, 4));
    auto* add_ab = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{a1, b1}, 4);
    auto* assign_c = task.arena().create<Assignment>(c1, add_ab);
    auto* ret = task.arena().create<Return>(std::vector<Expression*>{b1});

    bb->add_instruction(assign_a);
    bb->add_instruction(assign_b);
    bb->add_instruction(assign_c);
    bb->add_instruction(ret);

    MinimalVariableRenamer::rename(task.arena(), *task.cfg());

    std::unordered_set<std::string> names;
    std::unordered_set<std::string> interfering;
    for (Instruction* inst : bb->instructions()) {
        std::vector<Variable*> vars = inst->requirements();
        auto defs = inst->definitions();
        vars.insert(vars.end(), defs.begin(), defs.end());
        for (Variable* v : vars) {
            ASSERT_TRUE(v != nullptr);
            ASSERT_EQ(v->ssa_version(), 0);
            names.insert(v->name());
            if (inst == assign_c || inst == ret) {
                interfering.insert(v->name());
            }
        }
    }

    // There should be fewer non-SSA names than original SSA variables.
    ASSERT_TRUE(names.size() <= 2);
    // Two simultaneously-live values (`a`/`b` around `c = a + b`) should still differ.
    ASSERT_TRUE(interfering.size() >= 2);

    std::cout << "[+] test_minimal_variable_renamer passed.\n";
}

void test_conditional_variable_renamer() {
    DecompilerTask task(0x6100);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(31);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* arg0 = task.arena().create<Variable>("arg0", 4);
    arg0->set_ssa_version(0);
    auto* a1 = task.arena().create<Variable>("a", 4);
    a1->set_ssa_version(1);
    auto* b1 = task.arena().create<Variable>("b", 4);
    b1->set_ssa_version(1);
    auto* c1 = task.arena().create<Variable>("c", 4);
    c1->set_ssa_version(1);

    auto* assign_a = task.arena().create<Assignment>(a1, arg0);
    auto* plus_one = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{a1, task.arena().create<Constant>(1, 4)},
        4);
    auto* assign_b = task.arena().create<Assignment>(b1, plus_one);
    auto* assign_c = task.arena().create<Assignment>(c1, task.arena().create<Constant>(2, 4));
    auto* ret = task.arena().create<Return>(std::vector<Expression*>{b1});

    bb->add_instruction(assign_a);
    bb->add_instruction(assign_b);
    bb->add_instruction(assign_c);
    bb->add_instruction(ret);

    ConditionalVariableRenamer::rename(task.arena(), *task.cfg());

    const auto& insts = bb->instructions();
    Assignment* dep_assign = nullptr;
    Assignment* const_assign = nullptr;
    std::unordered_set<std::string> seen_names;

    for (Instruction* inst : insts) {
        auto* asg = dynamic_cast<Assignment*>(inst);
        if (asg == nullptr) {
            continue;
        }
        auto* dst = dynamic_cast<Variable*>(asg->destination());
        ASSERT_TRUE(dst != nullptr);
        ASSERT_EQ(dst->ssa_version(), 0);
        seen_names.insert(dst->name());

        if (auto* op = dynamic_cast<Operation*>(asg->value())) {
            if (op->type() == OperationType::add) {
                dep_assign = asg;
            }
        }
        if (auto* c = dynamic_cast<Constant*>(asg->value())) {
            if (c->value() == 2) {
                const_assign = asg;
            }
        }
    }

    ASSERT_TRUE(dep_assign != nullptr);
    ASSERT_TRUE(const_assign != nullptr);

    auto* dep_dst = dynamic_cast<Variable*>(dep_assign->destination());
    auto* const_dst = dynamic_cast<Variable*>(const_assign->destination());
    ASSERT_TRUE(dep_dst != nullptr && const_dst != nullptr);
    // Dependency-weighted path and unrelated constant path should not collapse together.
    ASSERT_TRUE(dep_dst->name() != const_dst->name());

    // There should still be at least two names after conditional merging.
    ASSERT_TRUE(seen_names.size() >= 2);

    std::cout << "[+] test_conditional_variable_renamer passed.\n";
}

void test_out_of_ssa_mode_config() {
    auto run_mode = [](OutOfSsaMode mode) {
        DecompilerTask task(0x6200);

        auto cfg = std::make_unique<ControlFlowGraph>();
        auto* bb = task.arena().create<BasicBlock>(32);
        cfg->set_entry_block(bb);
        cfg->add_block(bb);
        task.set_cfg(std::move(cfg));
        task.set_out_of_ssa_mode(mode);

        auto* x1 = task.arena().create<Variable>("x", 4);
        x1->set_ssa_version(1);
        auto* assign = task.arena().create<Assignment>(x1, task.arena().create<Constant>(5, 4));
        auto* ret = task.arena().create<Return>(std::vector<Expression*>{x1});
        bb->add_instruction(assign);
        bb->add_instruction(ret);

        SsaDestructor stage;
        stage.execute(task);

        auto* out_assign = dynamic_cast<Assignment*>(bb->instructions()[0]);
        auto* dst = dynamic_cast<Variable*>(out_assign->destination());
        ASSERT_TRUE(dst != nullptr);
        return std::make_pair(dst->name(), dst->ssa_version());
    };

    auto [default_name, default_version] = run_mode(OutOfSsaMode::LiftMinimal);
    ASSERT_TRUE(default_name.starts_with("var_"));
    ASSERT_EQ(default_version, 0);

    auto [simple_name, simple_version] = run_mode(OutOfSsaMode::Simple);
    ASSERT_EQ(simple_name, "x_1");
    ASSERT_EQ(simple_version, 0);

    auto [sreedhar_name, sreedhar_version] = run_mode(OutOfSsaMode::Sreedhar);
    ASSERT_EQ(sreedhar_name, "x");
    ASSERT_EQ(sreedhar_version, 1);

    auto parsed_default = SsaDestructor::parse_mode("lift_minimal");
    ASSERT_TRUE(parsed_default.has_value());
    ASSERT_EQ(*parsed_default, OutOfSsaMode::LiftMinimal);

    auto parsed_cond = SsaDestructor::parse_mode("conditional");
    ASSERT_TRUE(parsed_cond.has_value());
    ASSERT_EQ(*parsed_cond, OutOfSsaMode::Conditional);

    auto parsed_bad = SsaDestructor::parse_mode("not-a-mode");
    ASSERT_TRUE(!parsed_bad.has_value());

    std::cout << "[+] test_out_of_ssa_mode_config passed.\n";
}


#include "../src/dewolf/codegen/codegen.hpp"



#include "../src/dewolf/codegen/codegen.hpp"
void test_codegen_dump() {
    dewolf::DecompilerArena arena;
    
    // x = y + 10
    auto* var_x = arena.create<dewolf::Variable>("x", 4);
    auto* var_y = arena.create<dewolf::Variable>("y", 4);
    auto* const_10 = arena.create<dewolf::Constant>(10, 4);
    auto* add_op = arena.create<dewolf::Operation>(dewolf::OperationType::add, std::vector<dewolf::Expression*>{var_y, const_10}, 4);
    auto* assign1 = arena.create<dewolf::Assignment>(var_x, add_op);
    
    // if (x > 5) { z = 1; } else { z = 0; }
    auto* const_5 = arena.create<dewolf::Constant>(5, 4);
    auto* cond = arena.create<dewolf::Condition>(dewolf::OperationType::gt, var_x, const_5);
    
    auto* var_z = arena.create<dewolf::Variable>("z", 4);
    auto* const_1 = arena.create<dewolf::Constant>(1, 4);
    auto* assign_true = arena.create<dewolf::Assignment>(var_z, const_1);
    
    auto* const_0 = arena.create<dewolf::Constant>(0, 4);
    auto* assign_false = arena.create<dewolf::Assignment>(var_z, const_0);
    
    auto* bb1 = arena.create<dewolf::BasicBlock>(100);
    bb1->add_instruction(assign1);
    auto* node1 = arena.create<dewolf::CodeNode>(bb1);
    
    auto* bb_true = arena.create<dewolf::BasicBlock>(101);
    bb_true->add_instruction(assign_true);
    auto* node_true = arena.create<dewolf::CodeNode>(bb_true);
    
    auto* bb_false = arena.create<dewolf::BasicBlock>(102);
    bb_false->add_instruction(assign_false);
    auto* node_false = arena.create<dewolf::CodeNode>(bb_false);
    
    auto* cond_ast = arena.create<dewolf::ExprAstNode>(cond);
    auto* if_node = arena.create<dewolf::IfNode>(cond_ast, node_true, node_false);
    
    auto* seq = arena.create<dewolf::SeqNode>();
    seq->add_node(node1);
    seq->add_node(if_node);
    
    dewolf::CodeVisitor visitor;
    
    auto* forest = arena.create<dewolf::AbstractSyntaxForest>();
    forest->set_root(seq);
    auto lines = visitor.generate_code(forest);
    
    std::cout << "\n--- PSEUDOCODE OUTPUT ---\n";
    for (const auto& line : lines) {
        std::cout << line << "\n";
    }
    std::cout << "-------------------------\n\n";
}

void test_compiler_idiom_handling_stage() {
    DecompilerTask task(0x1000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* block = task.arena().create<BasicBlock>(1);
    cfg->set_entry_block(block);
    cfg->add_block(block);
    task.set_cfg(std::move(cfg));

    auto* eax = task.arena().create<Variable>("eax", 4);
    auto* c3 = task.arena().create<Constant>(3, 4);
    auto* c7 = task.arena().create<Constant>(7, 4);
    auto* c9 = task.arena().create<Constant>(9, 4);

    auto* inst1_rhs = task.arena().create<Operation>(OperationType::mul, std::vector<Expression*>{eax, c3}, 4);
    auto* inst1 = task.arena().create<Assignment>(eax, inst1_rhs);
    inst1->set_address(0x1000);

    auto* inst2_rhs = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{eax, c7}, 4);
    auto* inst2 = task.arena().create<Assignment>(eax, inst2_rhs);
    inst2->set_address(0x1002);

    auto* inst3_rhs = task.arena().create<Operation>(OperationType::sub, std::vector<Expression*>{eax, c9}, 4);
    auto* inst3 = task.arena().create<Assignment>(eax, inst3_rhs);
    inst3->set_address(0x1004);

    block->add_instruction(inst1);
    block->add_instruction(inst2);
    block->add_instruction(inst3);

    dewolf_idioms::IdiomTag tag;
    tag.address = 0x1000;
    tag.length = 3;
    tag.operation = "division unsigned";
    tag.operand = "eax";
    tag.constant = 5;
    task.set_idiom_tags(std::vector<dewolf_idioms::IdiomTag>{tag});

    CompilerIdiomHandlingStage stage;
    stage.execute(task);

    const auto& rewritten = block->instructions();
    ASSERT_EQ(rewritten.size(), 1);

    auto* assign = dynamic_cast<Assignment*>(rewritten[0]);
    ASSERT_TRUE(assign != nullptr);

    auto* lhs = dynamic_cast<Variable*>(assign->destination());
    ASSERT_TRUE(lhs != nullptr);
    ASSERT_EQ(lhs->name(), "eax");

    auto* op = dynamic_cast<Operation*>(assign->value());
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->type(), OperationType::div_us);
    ASSERT_EQ(op->operands().size(), 2);

    auto* divisor = dynamic_cast<Constant*>(op->operands()[1]);
    ASSERT_TRUE(divisor != nullptr);
    ASSERT_EQ(divisor->value(), 5);

    std::cout << "[+] test_compiler_idiom_handling_stage passed.\n";
}

void test_register_pair_handling_stage() {
    DecompilerTask task(0x2000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* block = task.arena().create<BasicBlock>(2);
    cfg->set_entry_block(block);
    cfg->add_block(block);
    task.set_cfg(std::move(cfg));

    auto* tmp = task.arena().create<Variable>("tmp64", 8);
    auto* edx = task.arena().create<Variable>("edx", 4);
    auto* eax = task.arena().create<Variable>("eax", 4);
    auto* shift = task.arena().create<Constant>(32, 4);
    auto* high = task.arena().create<Operation>(OperationType::shl, std::vector<Expression*>{edx, shift}, 8);
    auto* pair_expr = task.arena().create<Operation>(OperationType::bit_or, std::vector<Expression*>{high, eax}, 8);
    auto* assign = task.arena().create<Assignment>(tmp, pair_expr);
    assign->set_address(0x2000);
    block->add_instruction(assign);

    RegisterPairHandlingStage stage;
    stage.execute(task);

    const auto& insts = block->instructions();
    ASSERT_EQ(insts.size(), 1);
    auto* rewritten_assign = dynamic_cast<Assignment*>(insts[0]);
    ASSERT_TRUE(rewritten_assign != nullptr);

    auto* value_var = dynamic_cast<Variable*>(rewritten_assign->value());
    ASSERT_TRUE(value_var != nullptr);
    ASSERT_EQ(value_var->name(), "edx_eax_pair");
    ASSERT_EQ(value_var->size_bytes, 8);

    std::cout << "[+] test_register_pair_handling_stage passed.\n";
}

void test_switch_variable_detection_stage() {
    DecompilerTask task(0x3000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* switch_block = task.arena().create<BasicBlock>(3);
    auto* case_a = task.arena().create<BasicBlock>(4);
    auto* case_b = task.arena().create<BasicBlock>(5);
    cfg->set_entry_block(switch_block);
    cfg->add_block(switch_block);
    cfg->add_block(case_a);
    cfg->add_block(case_b);
    task.set_cfg(std::move(cfg));

    auto* arg = task.arena().create<Variable>("arg0", 4);
    auto* idx = task.arena().create<Variable>("idx", 4);
    auto* scaled = task.arena().create<Variable>("scaled", 8);
    auto* jtab_addr = task.arena().create<Variable>("jtab_addr", 8);
    auto* jump_target = task.arena().create<Variable>("jump_target", 8);

    auto* copy_idx = task.arena().create<Assignment>(idx, arg);
    copy_idx->set_address(0x3000);

    auto* sh_const = task.arena().create<Constant>(2, 4);
    auto* sh_expr = task.arena().create<Operation>(OperationType::shl, std::vector<Expression*>{idx, sh_const}, 8);
    auto* def_scaled = task.arena().create<Assignment>(scaled, sh_expr);
    def_scaled->set_address(0x3002);

    auto* base = task.arena().create<Constant>(0x1000, 8);
    auto* addr_expr = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{scaled, base}, 8);
    auto* def_addr = task.arena().create<Assignment>(jtab_addr, addr_expr);
    def_addr->set_address(0x3004);

    auto* deref = task.arena().create<Operation>(OperationType::deref, std::vector<Expression*>{jtab_addr}, 8);
    auto* def_target = task.arena().create<Assignment>(jump_target, deref);
    def_target->set_address(0x3006);

    auto* ib = task.arena().create<IndirectBranch>(jump_target);
    ib->set_address(0x3008);

    switch_block->add_instruction(copy_idx);
    switch_block->add_instruction(def_scaled);
    switch_block->add_instruction(def_addr);
    switch_block->add_instruction(def_target);
    switch_block->add_instruction(ib);

    auto* edge_a = task.arena().create<Edge>(switch_block, case_a, EdgeType::Switch);
    auto* edge_b = task.arena().create<Edge>(switch_block, case_b, EdgeType::Switch);
    switch_block->add_successor(edge_a);
    switch_block->add_successor(edge_b);
    case_a->add_predecessor(edge_a);
    case_b->add_predecessor(edge_b);

    SwitchVariableDetectionStage stage;
    stage.execute(task);

    auto* new_expr = dynamic_cast<Variable*>(ib->expression());
    ASSERT_TRUE(new_expr != nullptr);
    ASSERT_EQ(new_expr->name(), "idx");

    std::cout << "[+] test_switch_variable_detection_stage passed.\n";
}

void test_remove_go_prologue_stage() {
    DecompilerTask task(0x4000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* root = task.arena().create<BasicBlock>(10);
    auto* start = task.arena().create<BasicBlock>(11);
    auto* morestack = task.arena().create<BasicBlock>(12);

    cfg->set_entry_block(root);
    cfg->add_block(root);
    cfg->add_block(start);
    cfg->add_block(morestack);
    task.set_cfg(std::move(cfg));

    auto* ret_addr = task.arena().create<Variable>("__return_addr", 8);
    auto* lhs = task.arena().create<Operation>(OperationType::address_of, std::vector<Expression*>{ret_addr}, 8);

    auto* r14 = task.arena().create<Variable>("r14", 8);
    auto* plus_off = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{r14, task.arena().create<Constant>(0x10, 8)},
        8);
    auto* rhs = task.arena().create<Operation>(OperationType::deref, std::vector<Expression*>{plus_off}, 8);

    auto* cond = task.arena().create<Condition>(OperationType::le_us, lhs, rhs);
    auto* branch = task.arena().create<Branch>(cond);
    branch->set_address(0x4000);
    root->add_instruction(branch);

    auto* root_to_start = task.arena().create<Edge>(root, start, EdgeType::False);
    auto* root_to_morestack = task.arena().create<Edge>(root, morestack, EdgeType::True);
    auto* morestack_to_root = task.arena().create<Edge>(morestack, root, EdgeType::Unconditional);

    root->add_successor(root_to_start);
    root->add_successor(root_to_morestack);
    start->add_predecessor(root_to_start);
    morestack->add_predecessor(root_to_morestack);
    morestack->add_successor(morestack_to_root);
    root->add_predecessor(morestack_to_root);

    auto* call_target = task.arena().create<Variable>("runtime_morestack_noctxt", 8);
    auto* call_expr = task.arena().create<Call>(call_target, std::vector<Expression*>{}, 8);
    auto* call_dst = task.arena().create<Variable>("tmp", 8);
    auto* call_assign = task.arena().create<Assignment>(call_dst, call_expr);
    call_assign->set_address(0x4010);
    morestack->add_instruction(call_assign);

    RemoveGoPrologueStage stage;
    stage.execute(task);

    ASSERT_EQ(task.cfg()->blocks().size(), 2);
    ASSERT_EQ(root->successors().size(), 1);
    ASSERT_EQ(root->successors()[0]->target(), start);
    ASSERT_TRUE(root->instructions().empty() || dynamic_cast<Branch*>(root->instructions().back()) == nullptr);

    std::cout << "[+] test_remove_go_prologue_stage passed.\n";
}

void test_remove_stack_canary_stage() {
    DecompilerTask task(0x5000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* root = task.arena().create<BasicBlock>(20);
    auto* ok = task.arena().create<BasicBlock>(21);
    auto* mid = task.arena().create<BasicBlock>(22);   // empty relay block on fail path
    auto* fail = task.arena().create<BasicBlock>(23);

    cfg->set_entry_block(root);
    cfg->add_block(root);
    cfg->add_block(ok);
    cfg->add_block(mid);
    cfg->add_block(fail);
    task.set_cfg(std::move(cfg));

    auto* fsbase = task.arena().create<Variable>("fsbase", 8);
    auto* off = task.arena().create<Constant>(0x28, 8);
    auto* guard_addr = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{fsbase, off}, 8);
    auto* guard = task.arena().create<Operation>(OperationType::deref, std::vector<Expression*>{guard_addr}, 8);
    auto* zero = task.arena().create<Constant>(0, 8);
    auto* cond = task.arena().create<Condition>(OperationType::eq, guard, zero);
    auto* branch = task.arena().create<Branch>(cond);
    branch->set_address(0x5000);
    root->add_instruction(branch);

    auto* ok_ret = task.arena().create<Return>(std::vector<Expression*>{task.arena().create<Constant>(1, 8)});
    ok_ret->set_address(0x5002);
    ok->add_instruction(ok_ret);

    auto* call_target = task.arena().create<Variable>("__stack_chk_fail", 8);
    auto* call_expr = task.arena().create<Call>(call_target, std::vector<Expression*>{}, 8);
    auto* call_dst = task.arena().create<Variable>("tmp_canary", 8);
    auto* call_assign = task.arena().create<Assignment>(call_dst, call_expr);
    call_assign->set_address(0x5010);
    fail->add_instruction(call_assign);

    auto* e_true = task.arena().create<Edge>(root, ok, EdgeType::True);
    auto* e_false = task.arena().create<Edge>(root, mid, EdgeType::False);
    auto* e_mid_fail = task.arena().create<Edge>(mid, fail, EdgeType::Unconditional);
    root->add_successor(e_true);
    root->add_successor(e_false);
    ok->add_predecessor(e_true);
    mid->add_predecessor(e_false);
    mid->add_successor(e_mid_fail);
    fail->add_predecessor(e_mid_fail);

    RemoveStackCanaryStage stage;
    stage.execute(task);

    ASSERT_EQ(task.cfg()->blocks().size(), 2);
    ASSERT_EQ(root->successors().size(), 1);
    ASSERT_EQ(root->successors()[0]->target(), ok);
    ASSERT_TRUE(root->instructions().empty() || dynamic_cast<Branch*>(root->instructions().back()) == nullptr);

    std::cout << "[+] test_remove_stack_canary_stage passed.\n";
}

int main() {
    test_codegen_dump();
    test_phi_dependency();
    std::cout << "Running DeWolf tests...\n";
    test_arena();
    test_dominators();
    test_ssa_phi_insertion();
    test_instruction_hierarchy();
    test_minimal_variable_renamer();
    test_conditional_variable_renamer();
    test_out_of_ssa_mode_config();
    test_compiler_idiom_handling_stage();
    test_register_pair_handling_stage();
    test_switch_variable_detection_stage();
    test_remove_go_prologue_stage();
    test_remove_stack_canary_stage();
    test_type_system();
    test_loop_structurer();
    test_range_simplifier();
    std::cout << "All tests passed successfully.\n";
    return 0;
}
