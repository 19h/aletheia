#include <iostream>
#include <vector>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <unordered_set>
#include "../src/common/arena.hpp"
#include "../src/aletheia/structures/cfg.hpp"
#include "../src/aletheia/structures/dataflow.hpp"
#include "../src/aletheia/structures/types.hpp"
#include "../src/aletheia/structuring/ast.hpp"
#include "../src/aletheia/structuring/condition_handler.hpp"
#include "../src/aletheia/structuring/loop_structurer.hpp"
#include "../src/aletheia/structuring/cbr/cbr.hpp"
#include "../src/aletheia/structuring/car/car.hpp"
#include "../src/aletheia/structuring/reachability.hpp"
#include "../src/aletheia/structuring/structuring_stage.hpp"
#include "../src/aletheia/structuring/instruction_length_handler.hpp"
#include "../src/aletheia/structuring/variable_name_generation.hpp"
#include "../src/aletheia/structuring/loop_name_generator.hpp"
#include "../src/aletheia/ssa/dominators.hpp"
#include "../src/aletheia/ssa/ssa_constructor.hpp"
#include "../src/aletheia/ssa/ssa_destructor.hpp"
#include "../src/aletheia/ssa/minimal_variable_renamer.hpp"
#include "../src/aletheia/ssa/conditional_variable_renamer.hpp"

#include "../src/aletheia/pipeline/pipeline.hpp"
#include "../src/aletheia/pipeline/preprocessing_stages.hpp"
#include "../src/aletheia/pipeline/optimization_stages.hpp"
#include "../src/logos/range_simplifier.hpp"
#include "../src/logos/operation_simplifier.hpp"
#include "../src/logos/normal_form.hpp"
#include "../src/logos/world.hpp"



#define ASSERT_TRUE(cond) if (!(cond)) { \
    std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    std::exit(1); \
}
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

using namespace aletheia;

class PipelineTestStage final : public PipelineStage {
public:
    PipelineTestStage(std::string stage_name,
                      std::vector<std::string> deps,
                      bool* executed,
                      bool throw_error = false)
        : stage_name_(std::move(stage_name)),
          deps_(std::move(deps)),
          executed_(executed),
          throw_error_(throw_error) {}

    const char* name() const override { return stage_name_.c_str(); }

    std::vector<std::string> dependencies() const override {
        return deps_;
    }

    void execute(DecompilerTask&) override {
        if (executed_) {
            *executed_ = true;
        }
        if (throw_error_) {
            throw std::runtime_error("stage boom: " + stage_name_);
        }
    }

private:
    std::string stage_name_;
    std::vector<std::string> deps_;
    bool* executed_ = nullptr;
    bool throw_error_ = false;
};

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

void test_pipeline_stage_tracking() {
    {
        DecompilerTask task(0x1110);
        DecompilerPipeline pipeline;

        bool ran_a = false;
        bool ran_b = false;
        bool ran_c = false;

        pipeline.add_stage(std::make_unique<PipelineTestStage>(
            "A",
            std::vector<std::string>{},
            &ran_a));
        pipeline.add_stage(std::make_unique<PipelineTestStage>(
            "B",
            std::vector<std::string>{"A"},
            &ran_b));
        pipeline.add_stage(std::make_unique<PipelineTestStage>(
            "C",
            std::vector<std::string>{"Missing"},
            &ran_c));

        pipeline.run(task);

        ASSERT_TRUE(ran_a);
        ASSERT_TRUE(ran_b);
        ASSERT_TRUE(!ran_c);
        ASSERT_TRUE(task.failed());
        ASSERT_EQ(task.failure_stage(), std::string("C"));
        ASSERT_TRUE(task.failure_message().find("missing dependencies") != std::string::npos);

        ASSERT_EQ(task.stage_records().size(), 3);
        ASSERT_EQ(task.stage_records()[0].status, StageExecutionStatus::Success);
        ASSERT_EQ(task.stage_records()[1].status, StageExecutionStatus::Success);
        ASSERT_EQ(task.stage_records()[2].status, StageExecutionStatus::SkippedMissingDependency);
    }

    {
        DecompilerTask task(0x1120);
        DecompilerPipeline pipeline;

        bool ran_x = false;
        bool ran_y = false;
        bool ran_z = false;

        pipeline.add_stage(std::make_unique<PipelineTestStage>(
            "X",
            std::vector<std::string>{},
            &ran_x));
        pipeline.add_stage(std::make_unique<PipelineTestStage>(
            "Y",
            std::vector<std::string>{},
            &ran_y,
            true));
        pipeline.add_stage(std::make_unique<PipelineTestStage>(
            "Z",
            std::vector<std::string>{},
            &ran_z));

        pipeline.run(task);

        ASSERT_TRUE(ran_x);
        ASSERT_TRUE(ran_y);
        ASSERT_TRUE(!ran_z);
        ASSERT_TRUE(task.failed());
        ASSERT_EQ(task.failure_stage(), std::string("Y"));
        ASSERT_TRUE(task.failure_message().find("stage boom") != std::string::npos);

        ASSERT_EQ(task.stage_records().size(), 2);
        ASSERT_EQ(task.stage_records()[0].status, StageExecutionStatus::Success);
        ASSERT_EQ(task.stage_records()[1].status, StageExecutionStatus::Failed);
    }

    std::cout << "[+] test_pipeline_stage_tracking passed.\n";
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

void test_phi_lifting_edge_splitting() {
    DecompilerTask task(0x5000);
    task.set_out_of_ssa_mode(OutOfSsaMode::Sreedhar);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* entry = task.arena().create<BasicBlock>(200);
    auto* pred = task.arena().create<BasicBlock>(201);
    auto* other = task.arena().create<BasicBlock>(202);
    auto* phi_bb = task.arena().create<BasicBlock>(203);

    cfg->set_entry_block(entry);
    cfg->add_block(entry);
    cfg->add_block(pred);
    cfg->add_block(other);
    cfg->add_block(phi_bb);

    auto* e0 = task.arena().create<Edge>(entry, pred, EdgeType::Unconditional);
    entry->add_successor(e0);
    pred->add_predecessor(e0);

    auto* e_true = task.arena().create<Edge>(pred, phi_bb, EdgeType::True);
    auto* e_false = task.arena().create<Edge>(pred, other, EdgeType::False);
    pred->add_successor(e_true);
    pred->add_successor(e_false);
    phi_bb->add_predecessor(e_true);
    other->add_predecessor(e_false);

    auto* e_other = task.arena().create<Edge>(other, phi_bb, EdgeType::Unconditional);
    other->add_successor(e_other);
    phi_bb->add_predecessor(e_other);

    auto* cond_v = task.arena().create<Variable>("cond", 1);
    auto* p_src = task.arena().create<Variable>("p_src", 4);
    auto* o_src = task.arena().create<Variable>("o_src", 4);

    pred->add_instruction(task.arena().create<Assignment>(p_src, task.arena().create<Constant>(1, 4)));
    pred->add_instruction(task.arena().create<Branch>(
        task.arena().create<Condition>(OperationType::neq, cond_v, task.arena().create<Constant>(0, 1), 1)));

    other->add_instruction(task.arena().create<Assignment>(o_src, task.arena().create<Constant>(2, 4)));

    auto* dst = task.arena().create<Variable>("dst", 4);
    auto* phi_ops = task.arena().create<ListOperation>(std::vector<Expression*>{p_src, o_src}, 4);
    auto* phi = task.arena().create<Phi>(dst, phi_ops);
    phi->update_phi_function({{pred, p_src}, {other, o_src}});
    phi_bb->add_instruction(phi);
    phi_bb->add_instruction(task.arena().create<Return>(std::vector<Expression*>{dst}));

    task.set_cfg(std::move(cfg));

    SsaDestructor dtor;
    dtor.execute(task);

    bool direct_pred_to_phi = false;
    BasicBlock* split_block = nullptr;
    for (Edge* e : pred->successors()) {
        if (e->target() == phi_bb) {
            direct_pred_to_phi = true;
        } else if (e->target() != other) {
            split_block = e->target();
        }
    }
    ASSERT_TRUE(!direct_pred_to_phi);
    ASSERT_TRUE(split_block != nullptr);

    bool split_reaches_phi = false;
    bool split_has_copy = false;
    for (Edge* e : split_block->successors()) {
        if (e->target() == phi_bb) {
            split_reaches_phi = true;
        }
    }
    for (Instruction* inst : split_block->instructions()) {
        if (dynamic_cast<Assignment*>(inst) != nullptr) {
            split_has_copy = true;
            break;
        }
    }
    ASSERT_TRUE(split_reaches_phi);
    ASSERT_TRUE(split_has_copy);

    std::cout << "[+] test_phi_lifting_edge_splitting passed.\n";
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
    using namespace logos;
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

void test_logic_operation_simplifier() {
    using namespace logos;

    LogicDag dag;

    // Constant folding: true && false -> false.
    auto* c1 = dag.create_node<DagConstant>(1);
    auto* c0 = dag.create_node<DagConstant>(0);
    auto* and_const = dag.create_node<DagOperation>(LogicOp::And);
    and_const->add_child(c1);
    and_const->add_child(c0);
    DagNode* folded = simplify_node(dag, and_const);
    auto* folded_const = dynamic_cast<DagConstant*>(folded);
    ASSERT_TRUE(folded_const != nullptr);
    ASSERT_EQ(folded_const->value(), 0);

    // De Morgan: !(a || b) -> !a && !b.
    auto* a = dag.create_node<DagVariable>("a");
    auto* b = dag.create_node<DagVariable>("b");
    auto* or_ab = dag.create_node<DagOperation>(LogicOp::Or);
    or_ab->add_child(a);
    or_ab->add_child(b);
    auto* not_or = dag.create_node<DagOperation>(LogicOp::Not);
    not_or->add_child(or_ab);
    DagNode* demorgan = simplify_node(dag, not_or);
    auto* demorgan_op = dynamic_cast<DagOperation*>(demorgan);
    ASSERT_TRUE(demorgan_op != nullptr);
    ASSERT_EQ(demorgan_op->op(), LogicOp::And);
    ASSERT_EQ(demorgan_op->children().size(), 2);

    // Associative flattening: a && (b && c) -> a && b && c.
    auto* c = dag.create_node<DagVariable>("c");
    auto* inner_and = dag.create_node<DagOperation>(LogicOp::And);
    inner_and->add_child(b);
    inner_and->add_child(c);
    auto* outer_and = dag.create_node<DagOperation>(LogicOp::And);
    outer_and->add_child(a);
    outer_and->add_child(inner_and);
    DagNode* flattened = simplify_node(dag, outer_and);
    auto* flattened_op = dynamic_cast<DagOperation*>(flattened);
    ASSERT_TRUE(flattened_op != nullptr);
    ASSERT_EQ(flattened_op->op(), LogicOp::And);
    ASSERT_EQ(flattened_op->children().size(), 3);

    // Collision detection: a && !a -> false.
    auto* not_a = dag.create_node<DagOperation>(LogicOp::Not);
    not_a->add_child(a);
    auto* coll = dag.create_node<DagOperation>(LogicOp::And);
    coll->add_child(a);
    coll->add_child(a); // duplicate removal path
    coll->add_child(not_a);
    DagNode* collided = simplify_node(dag, coll);
    auto* collided_const = dynamic_cast<DagConstant*>(collided);
    ASSERT_TRUE(collided_const != nullptr);
    ASSERT_EQ(collided_const->value(), 0);

    std::cout << "[+] test_logic_operation_simplifier passed.\n";
}

void test_logic_normal_form_conversion() {
    using namespace logos;

    {
        LogicDag dag;
        auto* a = dag.create_node<DagVariable>("a");
        auto* b = dag.create_node<DagVariable>("b");
        auto* c = dag.create_node<DagVariable>("c");

        auto* and_bc = dag.create_node<DagOperation>(LogicOp::And);
        and_bc->add_child(b);
        and_bc->add_child(c);

        auto* or_expr = dag.create_node<DagOperation>(LogicOp::Or);
        or_expr->add_child(a);
        or_expr->add_child(and_bc);

        DagNode* cnf = ToCnfVisitor::convert(dag, or_expr);
        auto* cnf_op = dynamic_cast<DagOperation*>(cnf);
        ASSERT_TRUE(cnf_op != nullptr);
        ASSERT_EQ(cnf_op->op(), LogicOp::And);
        ASSERT_EQ(cnf_op->children().size(), 2);

        for (DagNode* child : cnf_op->children()) {
            auto* clause = dynamic_cast<DagOperation*>(child);
            ASSERT_TRUE(clause != nullptr);
            ASSERT_EQ(clause->op(), LogicOp::Or);
            ASSERT_EQ(clause->children().size(), 2);
        }
    }

    {
        LogicDag dag;
        auto* a = dag.create_node<DagVariable>("a");
        auto* b = dag.create_node<DagVariable>("b");
        auto* c = dag.create_node<DagVariable>("c");

        auto* or_bc = dag.create_node<DagOperation>(LogicOp::Or);
        or_bc->add_child(b);
        or_bc->add_child(c);

        auto* and_expr = dag.create_node<DagOperation>(LogicOp::And);
        and_expr->add_child(a);
        and_expr->add_child(or_bc);

        DagNode* dnf = ToDnfVisitor::convert(dag, and_expr);
        auto* dnf_op = dynamic_cast<DagOperation*>(dnf);
        ASSERT_TRUE(dnf_op != nullptr);
        ASSERT_EQ(dnf_op->op(), LogicOp::Or);
        ASSERT_EQ(dnf_op->children().size(), 2);

        for (DagNode* child : dnf_op->children()) {
            auto* term = dynamic_cast<DagOperation*>(child);
            ASSERT_TRUE(term != nullptr);
            ASSERT_EQ(term->op(), LogicOp::And);
            ASSERT_EQ(term->children().size(), 2);
        }
    }

    std::cout << "[+] test_logic_normal_form_conversion passed.\n";
}

void test_world_map_condition_no_simplification() {
    using namespace logos;

    LogicDag source;
    auto* a = source.create_node<DagVariable>("a");
    auto* b = source.create_node<DagVariable>("b");
    auto* one = source.create_node<DagConstant>(1);

    auto* eq = source.create_node<DagOperation>(LogicOp::Eq);
    eq->add_child(a);
    eq->add_child(one);

    auto* neg_eq = source.create_node<DagOperation>(LogicOp::Not);
    neg_eq->add_child(eq);

    auto* and_node = source.create_node<DagOperation>(LogicOp::And);
    and_node->add_child(neg_eq);
    and_node->add_child(b);
    and_node->add_child(b); // duplicate child must remain (no simplification)

    World world;
    DagNode* mapped_1 = world.map_condition(and_node);
    DagNode* mapped_2 = world.map_condition(and_node);

    ASSERT_TRUE(mapped_1 != nullptr);
    ASSERT_TRUE(mapped_1 == mapped_2); // stable DAG mapping
    ASSERT_TRUE(mapped_1 != and_node);  // copied into world DAG

    auto* mapped_and = dynamic_cast<DagOperation*>(mapped_1);
    ASSERT_TRUE(mapped_and != nullptr);
    ASSERT_EQ(mapped_and->op(), LogicOp::And);
    ASSERT_EQ(mapped_and->children().size(), 3);

    auto* mapped_not = dynamic_cast<DagOperation*>(mapped_and->children()[0]);
    ASSERT_TRUE(mapped_not != nullptr);
    ASSERT_EQ(mapped_not->op(), LogicOp::Not);

    auto* mapped_eq = dynamic_cast<DagOperation*>(mapped_not->children()[0]);
    ASSERT_TRUE(mapped_eq != nullptr);
    ASSERT_EQ(mapped_eq->op(), LogicOp::Eq);

    auto* mapped_a = dynamic_cast<DagVariable*>(mapped_eq->children()[0]);
    auto* mapped_one = dynamic_cast<DagConstant*>(mapped_eq->children()[1]);
    ASSERT_TRUE(mapped_a != nullptr);
    ASSERT_TRUE(mapped_one != nullptr);
    ASSERT_EQ(mapped_a->name(), std::string("a"));
    ASSERT_EQ(mapped_one->value(), 1ULL);

    auto* mapped_b0 = dynamic_cast<DagVariable*>(mapped_and->children()[1]);
    auto* mapped_b1 = dynamic_cast<DagVariable*>(mapped_and->children()[2]);
    ASSERT_TRUE(mapped_b0 != nullptr && mapped_b1 != nullptr);
    ASSERT_TRUE(mapped_b0 == mapped_b1); // DAG sharing preserved
    ASSERT_EQ(mapped_b0->name(), std::string("b"));

    ASSERT_TRUE(world.map_condition(nullptr) == nullptr);

    std::cout << "[+] test_world_map_condition_no_simplification passed.\n";
}

void test_range_simplifier_legacy_dag_path() {
    using namespace logos;

    {
        LogicDag dag;
        auto* x = dag.create_node<DagVariable>("x");
        auto* c10 = dag.create_node<DagConstant>(10);

        auto* ge = dag.create_node<DagOperation>(LogicOp::Ge);
        ge->add_child(x);
        ge->add_child(c10);

        auto* lt = dag.create_node<DagOperation>(LogicOp::Lt);
        lt->add_child(x);
        lt->add_child(c10);

        auto* root = dag.create_node<DagOperation>(LogicOp::And);
        root->add_child(ge);
        root->add_child(lt);

        RangeSimplifier rs;
        ASSERT_TRUE(rs.is_unfulfillable(root));

        DagNode* simplified = rs.simplify(root);
        auto* c = dynamic_cast<DagConstant*>(simplified);
        ASSERT_TRUE(c != nullptr);
        ASSERT_EQ(c->value(), 0ULL);
    }

    {
        LogicDag dag;
        auto* x = dag.create_node<DagVariable>("x");
        auto* c2 = dag.create_node<DagConstant>(2);
        auto* c5 = dag.create_node<DagConstant>(5);

        auto* ge2 = dag.create_node<DagOperation>(LogicOp::Ge);
        ge2->add_child(x);
        ge2->add_child(c2);

        auto* ge5 = dag.create_node<DagOperation>(LogicOp::Ge);
        ge5->add_child(x);
        ge5->add_child(c5);

        auto* root = dag.create_node<DagOperation>(LogicOp::And);
        root->add_child(ge2);
        root->add_child(ge5);

        RangeSimplifier rs;
        DagNode* simplified = rs.simplify(root);
        auto* rel = dynamic_cast<DagOperation*>(simplified);
        ASSERT_TRUE(rel != nullptr);
        ASSERT_EQ(rel->op(), LogicOp::Ge);
        ASSERT_EQ(rel->children().size(), 2);
        auto* rhs = dynamic_cast<DagConstant*>(rel->children()[1]);
        ASSERT_TRUE(rhs != nullptr);
        ASSERT_EQ(rhs->value(), 5ULL);
    }

    std::cout << "[+] test_range_simplifier_legacy_dag_path passed.\n";
}

void test_z3_converter_no_hard_false_fallback() {
    z3::context ctx;
    logos::Z3Converter converter(ctx);
    aletheia::DecompilerArena arena;

    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* y = arena.create<aletheia::Variable>("y", 4);

    auto* unknown_op = arena.create<aletheia::Operation>(
        aletheia::OperationType::unknown,
        std::vector<aletheia::Expression*>{x},
        4);

    auto unknown_cond = converter.convert_to_condition(unknown_op);
    ASSERT_TRUE(unknown_cond.is_satisfiable());
    ASSERT_TRUE(unknown_cond.negate().is_satisfiable());

    auto* power_op = arena.create<aletheia::Operation>(
        aletheia::OperationType::power,
        std::vector<aletheia::Expression*>{x, y},
        4);
    auto power_cond = converter.convert_to_condition(power_op);
    ASSERT_TRUE(power_cond.is_satisfiable());
    ASSERT_TRUE(power_cond.negate().is_satisfiable());

    auto* add_expr = arena.create<aletheia::Operation>(
        aletheia::OperationType::add,
        std::vector<aletheia::Expression*>{x, arena.create<aletheia::Constant>(1, 4)},
        4);
    auto* cmp_expr = arena.create<aletheia::Condition>(
        aletheia::OperationType::gt,
        add_expr,
        arena.create<aletheia::Constant>(0, 4),
        1);
    auto cmp_cond = converter.convert_to_condition(cmp_expr);
    ASSERT_TRUE(cmp_cond.is_satisfiable());

    std::cout << "[+] test_z3_converter_no_hard_false_fallback passed.\n";
}


#include "../src/aletheia/ssa/phi_dependency_resolver.hpp"

void test_phi_dependency() {
    aletheia::DecompilerArena arena;
    aletheia::ControlFlowGraph cfg;
    
    auto* bb = arena.create<aletheia::BasicBlock>(0x1000);
    cfg.add_block(bb);
    
    auto* var_a = arena.create<aletheia::Variable>("a", 4); var_a->set_ssa_version(1);
    auto* var_b = arena.create<aletheia::Variable>("b", 4); var_b->set_ssa_version(1);
    
    auto* var_a0 = arena.create<aletheia::Variable>("a", 4); var_a0->set_ssa_version(0);
    auto* var_b0 = arena.create<aletheia::Variable>("b", 4); var_b0->set_ssa_version(0);

    // Phi1: a_1 = phi(a_0, b_1)
    auto* phi1_ops = arena.create<aletheia::ListOperation>(std::vector<aletheia::Expression*>{var_a0, var_b});
    auto* phi1 = arena.create<aletheia::Phi>(var_a, phi1_ops);

    // Phi2: b_1 = phi(b_0, a_1)
    auto* phi2_ops = arena.create<aletheia::ListOperation>(std::vector<aletheia::Expression*>{var_b0, var_a});
    auto* phi2 = arena.create<aletheia::Phi>(var_b, phi2_ops);
    
    // Add in reverse topological order or arbitrary order
    bb->add_instruction(phi1);
    bb->add_instruction(phi2);
    
    aletheia::PhiDependencyResolver::resolve(arena, cfg);
    
    auto insts = bb->instructions();
    // We expect the cycle to be broken. One of the Phis should have its definition renamed.
    // And an assignment should be added.
    bool found_copy = false;
    bool found_renamed_phi = false;
    for (auto* inst : insts) {
        if (auto* assign = dynamic_cast<aletheia::Assignment*>(inst)) {
            if (!dynamic_cast<aletheia::Phi*>(inst)) {
                if (auto* dest = dynamic_cast<aletheia::Variable*>(assign->destination())) {
                    if (dest->name() == "a" || dest->name() == "b") {
                        found_copy = true;
                    }
                }
            }
        }
        if (auto* phi = dynamic_cast<aletheia::Phi*>(inst)) {
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


#include "../src/aletheia/codegen/codegen.hpp"



#include "../src/aletheia/codegen/codegen.hpp"
void test_codegen_dump() {
    aletheia::DecompilerArena arena;
    
    // x = y + 10
    auto* var_x = arena.create<aletheia::Variable>("x", 4);
    auto* var_y = arena.create<aletheia::Variable>("y", 4);
    auto* const_10 = arena.create<aletheia::Constant>(10, 4);
    auto* add_op = arena.create<aletheia::Operation>(aletheia::OperationType::add, std::vector<aletheia::Expression*>{var_y, const_10}, 4);
    auto* assign1 = arena.create<aletheia::Assignment>(var_x, add_op);
    
    // if (x > 5) { z = 1; } else { z = 0; }
    auto* const_5 = arena.create<aletheia::Constant>(5, 4);
    auto* cond = arena.create<aletheia::Condition>(aletheia::OperationType::gt, var_x, const_5);
    
    auto* var_z = arena.create<aletheia::Variable>("z", 4);
    auto* const_1 = arena.create<aletheia::Constant>(1, 4);
    auto* assign_true = arena.create<aletheia::Assignment>(var_z, const_1);
    
    auto* const_0 = arena.create<aletheia::Constant>(0, 4);
    auto* assign_false = arena.create<aletheia::Assignment>(var_z, const_0);
    
    auto* bb1 = arena.create<aletheia::BasicBlock>(100);
    bb1->add_instruction(assign1);
    auto* node1 = arena.create<aletheia::CodeNode>(bb1);
    
    auto* bb_true = arena.create<aletheia::BasicBlock>(101);
    bb_true->add_instruction(assign_true);
    auto* node_true = arena.create<aletheia::CodeNode>(bb_true);
    
    auto* bb_false = arena.create<aletheia::BasicBlock>(102);
    bb_false->add_instruction(assign_false);
    auto* node_false = arena.create<aletheia::CodeNode>(bb_false);
    
    auto* cond_ast = arena.create<aletheia::ExprAstNode>(cond);
    auto* if_node = arena.create<aletheia::IfNode>(cond_ast, node_true, node_false);
    
    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(node1);
    seq->add_node(if_node);
    
    aletheia::CodeVisitor visitor;
    
    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);
    auto lines = visitor.generate_code(forest);
    
    std::cout << "\n--- PSEUDOCODE OUTPUT ---\n";
    for (const auto& line : lines) {
        std::cout << line << "\n";
    }
    std::cout << "-------------------------\n\n";
}

void test_codegen_switch_case() {
    aletheia::DecompilerArena arena;

    auto* sel = arena.create<aletheia::Variable>("sel", 4);
    auto* z = arena.create<aletheia::Variable>("z", 4);

    auto* bb_case0 = arena.create<aletheia::BasicBlock>(110);
    bb_case0->add_instruction(arena.create<aletheia::Assignment>(z, arena.create<aletheia::Constant>(1, 4)));
    auto* node_case0 = arena.create<aletheia::CodeNode>(bb_case0);

    auto* bb_default = arena.create<aletheia::BasicBlock>(111);
    bb_default->add_instruction(arena.create<aletheia::Assignment>(z, arena.create<aletheia::Constant>(0, 4)));
    auto* node_default = arena.create<aletheia::CodeNode>(bb_default);

    auto* case0 = arena.create<aletheia::CaseNode>(0, node_case0, false, true);
    auto* def = arena.create<aletheia::CaseNode>(0, node_default, true, true);

    auto* sw = arena.create<aletheia::SwitchNode>(arena.create<aletheia::ExprAstNode>(sel));
    sw->add_case(case0);
    sw->add_case(def);

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(sw);

    aletheia::CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);

    auto has_substr = [&](const std::string& needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    ASSERT_TRUE(has_substr("switch (sel) {"));
    ASSERT_TRUE(has_substr("case 0:"));
    ASSERT_TRUE(has_substr("default:"));
    ASSERT_TRUE(has_substr("break;"));

    std::cout << "[+] test_codegen_switch_case passed.\n";
}

void test_codegen_loop_variants() {
    aletheia::DecompilerArena arena;

    auto* i = arena.create<aletheia::Variable>("i", 4);
    auto* ten = arena.create<aletheia::Constant>(10, 4);

    auto* while_cond = arena.create<aletheia::Condition>(aletheia::OperationType::lt, i, ten, 1);
    auto* do_cond = arena.create<aletheia::Condition>(aletheia::OperationType::lt, i, ten, 1);
    auto* for_cond = arena.create<aletheia::Condition>(aletheia::OperationType::lt, i, ten, 1);

    auto* bb_while = arena.create<aletheia::BasicBlock>(120);
    bb_while->add_instruction(arena.create<aletheia::Assignment>(i, arena.create<aletheia::Constant>(1, 4)));
    auto* while_body = arena.create<aletheia::CodeNode>(bb_while);
    auto* while_node = arena.create<aletheia::WhileLoopNode>(while_body, while_cond);

    auto* bb_do = arena.create<aletheia::BasicBlock>(121);
    bb_do->add_instruction(arena.create<aletheia::Assignment>(i, arena.create<aletheia::Constant>(2, 4)));
    auto* do_body = arena.create<aletheia::CodeNode>(bb_do);
    auto* do_node = arena.create<aletheia::DoWhileLoopNode>(do_body, do_cond);

    auto* bb_for = arena.create<aletheia::BasicBlock>(122);
    bb_for->add_instruction(arena.create<aletheia::Assignment>(i, arena.create<aletheia::Constant>(3, 4)));
    auto* for_body = arena.create<aletheia::CodeNode>(bb_for);
    auto* for_decl = arena.create<aletheia::Assignment>(i, arena.create<aletheia::Constant>(0, 4));
    auto* for_mod = arena.create<aletheia::Assignment>(
        i,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{i, arena.create<aletheia::Constant>(1, 4)},
            4));
    auto* for_node = arena.create<aletheia::ForLoopNode>(for_body, for_cond, for_decl, for_mod);

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(while_node);
    seq->add_node(do_node);
    seq->add_node(for_node);

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);

    aletheia::CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);

    auto has_substr = [&](const std::string& needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    ASSERT_TRUE(has_substr("while (i < 0xa) {"));
    ASSERT_TRUE(has_substr("do {"));
    ASSERT_TRUE(has_substr("} while (i < 0xa);"));
    ASSERT_TRUE(has_substr("for (i = 0x0; i < 0xa; i++) {"));

    std::cout << "[+] test_codegen_loop_variants passed.\n";
}

void test_codegen_operator_precedence() {
    aletheia::DecompilerArena arena;

    auto* a = arena.create<aletheia::Variable>("a", 4);
    auto* b = arena.create<aletheia::Variable>("b", 4);
    auto* c = arena.create<aletheia::Variable>("c", 4);

    auto* out1 = arena.create<aletheia::Variable>("p1", 4);
    auto* out2 = arena.create<aletheia::Variable>("p2", 4);
    auto* out3 = arena.create<aletheia::Variable>("p3", 4);

    auto* expr1 = arena.create<aletheia::Operation>(
        aletheia::OperationType::mul,
        std::vector<aletheia::Expression*>{
            arena.create<aletheia::Operation>(
                aletheia::OperationType::add,
                std::vector<aletheia::Expression*>{a, b},
                4),
            c},
        4);
    auto* expr2 = arena.create<aletheia::Operation>(
        aletheia::OperationType::add,
        std::vector<aletheia::Expression*>{
            a,
            arena.create<aletheia::Operation>(
                aletheia::OperationType::mul,
                std::vector<aletheia::Expression*>{b, c},
                4)},
        4);
    auto* expr3 = arena.create<aletheia::Operation>(
        aletheia::OperationType::sub,
        std::vector<aletheia::Expression*>{
            a,
            arena.create<aletheia::Operation>(
                aletheia::OperationType::sub,
                std::vector<aletheia::Expression*>{b, c},
                4)},
        4);

    auto* bb = arena.create<aletheia::BasicBlock>(160);
    bb->add_instruction(arena.create<aletheia::Assignment>(out1, expr1));
    bb->add_instruction(arena.create<aletheia::Assignment>(out2, expr2));
    bb->add_instruction(arena.create<aletheia::Assignment>(out3, expr3));

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(arena.create<aletheia::CodeNode>(bb));

    aletheia::CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);

    auto has_substr = [&](const std::string& needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    ASSERT_TRUE(has_substr("p1 = (a + b) * c;"));
    ASSERT_TRUE(has_substr("p2 = a + b * c;"));
    ASSERT_TRUE(has_substr("p3 = a - (b - c);"));

    std::cout << "[+] test_codegen_operator_precedence passed.\n";
}

void test_codegen_compound_assignment_increment() {
    aletheia::DecompilerArena arena;

    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* y = arena.create<aletheia::Variable>("y", 4);

    auto* bb = arena.create<aletheia::BasicBlock>(171);
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{x, arena.create<aletheia::Constant>(1, 4)},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::sub,
            std::vector<aletheia::Expression*>{x, arena.create<aletheia::Constant>(1, 4)},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{x, y},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{y, x},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::sub,
            std::vector<aletheia::Expression*>{y, x},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::bit_and,
            std::vector<aletheia::Expression*>{x, y},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::logical_and,
            std::vector<aletheia::Expression*>{x, y},
            4)));

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(arena.create<aletheia::CodeNode>(bb));

    aletheia::CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);

    auto has_substr = [&](const std::string& needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    ASSERT_TRUE(has_substr("x++;"));
    ASSERT_TRUE(has_substr("x--;"));
    ASSERT_TRUE(has_substr("x += y;"));
    ASSERT_TRUE(has_substr("x = y - x;"));
    ASSERT_TRUE(has_substr("x &= y;"));
    ASSERT_TRUE(has_substr("x = x && y;"));

    std::cout << "[+] test_codegen_compound_assignment_increment passed.\n";
}

void test_codegen_constant_formatting() {
    aletheia::DecompilerArena arena;
    aletheia::CExpressionGenerator gen;

    auto* char_const = arena.create<aletheia::Constant>(static_cast<std::uint64_t>('A'), 1);
    ASSERT_EQ(gen.generate(char_const), "'A'");

    auto* str_const = arena.create<aletheia::Constant>(0x006968, 3);
    str_const->set_ir_type(std::make_shared<const aletheia::ArrayType>(aletheia::Integer::char_type(), 3));
    ASSERT_EQ(gen.generate(str_const), "\"hi\"");

    const char* previous_threshold = std::getenv("ALETHEIA_INT_HEX_THRESHOLD");
    std::string previous_threshold_value = previous_threshold ? previous_threshold : "";
    setenv("ALETHEIA_INT_HEX_THRESHOLD", "16", 1);

    auto* dec_const = arena.create<aletheia::Constant>(10, 4);
    auto* hex_const = arena.create<aletheia::Constant>(32, 4);
    ASSERT_EQ(gen.generate(dec_const), "10");
    ASSERT_EQ(gen.generate(hex_const), "0x20");

    if (previous_threshold) {
        setenv("ALETHEIA_INT_HEX_THRESHOLD", previous_threshold_value.c_str(), 1);
    } else {
        unsetenv("ALETHEIA_INT_HEX_THRESHOLD");
    }

    auto* u32 = arena.create<aletheia::Constant>(1, 4);
    u32->set_ir_type(aletheia::Integer::uint32_t());
    auto* u64 = arena.create<aletheia::Constant>(1, 8);
    u64->set_ir_type(aletheia::Integer::uint64_t());
    auto* s64 = arena.create<aletheia::Constant>(1, 8);
    s64->set_ir_type(aletheia::Integer::int64_t());

    ASSERT_EQ(gen.generate(u32), "0x1U");
    ASSERT_EQ(gen.generate(u64), "0x1UL");
    ASSERT_EQ(gen.generate(s64), "0x1L");

    std::cout << "[+] test_codegen_constant_formatting passed.\n";
}

void test_codegen_unknown_operation_placeholder() {
    aletheia::DecompilerArena arena;

    auto* out = arena.create<aletheia::Variable>("out", 4);
    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* y = arena.create<aletheia::Variable>("y", 4);

    auto* unknown_expr = arena.create<aletheia::Operation>(
        aletheia::OperationType::unknown,
        std::vector<aletheia::Expression*>{x, y},
        4);

    auto* bb = arena.create<aletheia::BasicBlock>(172);
    bb->add_instruction(arena.create<aletheia::Assignment>(out, unknown_expr));

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(arena.create<aletheia::CodeNode>(bb));

    aletheia::CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);

    bool found_placeholder = false;
    for (const auto& line : lines) {
        if (line.find("__aletheia_unknown_op(") != std::string::npos) {
            found_placeholder = true;
            break;
        }
    }

    ASSERT_TRUE(found_placeholder);

    std::cout << "[+] test_codegen_unknown_operation_placeholder passed.\n";
}

void test_codegen_fallback_branch_marker() {
    aletheia::DecompilerArena arena;

    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* cond = arena.create<aletheia::Condition>(
        aletheia::OperationType::neq,
        x,
        arena.create<aletheia::Constant>(0, 4),
        1);

    auto* bb = arena.create<aletheia::BasicBlock>(173);
    bb->add_instruction(arena.create<aletheia::Branch>(cond));

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(arena.create<aletheia::CodeNode>(bb));

    aletheia::CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);

    bool has_comment_marker = false;
    bool has_empty_if = false;
    for (const auto& line : lines) {
        if (line.find("/* branch if (") != std::string::npos) {
            has_comment_marker = true;
        }
        if (line.find("if (") != std::string::npos && line.find(");") != std::string::npos) {
            has_empty_if = true;
        }
    }

    ASSERT_TRUE(has_comment_marker);
    ASSERT_TRUE(!has_empty_if);

    std::cout << "[+] test_codegen_fallback_branch_marker passed.\n";
}

void test_codegen_if_branch_swapping() {
    aletheia::DecompilerArena arena;

    auto has_substr = [](const std::vector<std::string>& lines, const std::string& needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    // Else-if chain heuristic: if exactly one branch is an IfNode, move it to false branch.
    {
        auto* x = arena.create<aletheia::Variable>("x", 1);
        auto* y = arena.create<aletheia::Variable>("y", 1);

        auto* tb = arena.create<aletheia::BasicBlock>(910);
        tb->add_instruction(arena.create<aletheia::Assignment>(
            arena.create<aletheia::Variable>("a", 4),
            arena.create<aletheia::Constant>(1, 4)));
        auto* fb = arena.create<aletheia::BasicBlock>(911);
        fb->add_instruction(arena.create<aletheia::Assignment>(
            arena.create<aletheia::Variable>("b", 4),
            arena.create<aletheia::Constant>(2, 4)));
        auto* else_block = arena.create<aletheia::BasicBlock>(912);
        else_block->add_instruction(arena.create<aletheia::Assignment>(
            arena.create<aletheia::Variable>("c", 4),
            arena.create<aletheia::Constant>(3, 4)));

        auto* nested_if = arena.create<aletheia::IfNode>(
            arena.create<aletheia::ExprAstNode>(y),
            arena.create<aletheia::CodeNode>(tb),
            arena.create<aletheia::CodeNode>(fb));
        auto* outer_if = arena.create<aletheia::IfNode>(
            arena.create<aletheia::ExprAstNode>(x),
            nested_if,
            arena.create<aletheia::CodeNode>(else_block));

        auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
        forest->set_root(outer_if);

        aletheia::CodeVisitor visitor;
        auto lines = visitor.generate_code(forest);
        ASSERT_TRUE(has_substr(lines, "if (!(x)) {"));
        ASSERT_TRUE(has_substr(lines, "} else if (y) {"));
    }

    // Configurable preference: `smallest` puts smaller branch on true side.
    const char* previous_pref = std::getenv("ALETHEIA_IF_BRANCH_PREFERENCE");
    std::string previous_pref_value = previous_pref ? previous_pref : "";
    setenv("ALETHEIA_IF_BRANCH_PREFERENCE", "smallest", 1);

    {
        auto* x = arena.create<aletheia::Variable>("x", 1);

        auto* heavy = arena.create<aletheia::BasicBlock>(913);
        heavy->add_instruction(arena.create<aletheia::Assignment>(
            arena.create<aletheia::Variable>("h1", 4),
            arena.create<aletheia::Constant>(11, 4)));
        heavy->add_instruction(arena.create<aletheia::Assignment>(
            arena.create<aletheia::Variable>("h2", 4),
            arena.create<aletheia::Constant>(12, 4)));

        auto* light = arena.create<aletheia::BasicBlock>(914);
        light->add_instruction(arena.create<aletheia::Assignment>(
            arena.create<aletheia::Variable>("l1", 4),
            arena.create<aletheia::Constant>(21, 4)));

        auto* root_if = arena.create<aletheia::IfNode>(
            arena.create<aletheia::ExprAstNode>(x),
            arena.create<aletheia::CodeNode>(heavy),
            arena.create<aletheia::CodeNode>(light));

        auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
        forest->set_root(root_if);

        aletheia::CodeVisitor visitor;
        auto lines = visitor.generate_code(forest);
        ASSERT_TRUE(has_substr(lines, "if (!(x)) {"));
    }

    if (previous_pref) {
        setenv("ALETHEIA_IF_BRANCH_PREFERENCE", previous_pref_value.c_str(), 1);
    } else {
        unsetenv("ALETHEIA_IF_BRANCH_PREFERENCE");
    }

    std::cout << "[+] test_codegen_if_branch_swapping passed.\n";
}

void test_array_access_detection_stage() {
    DecompilerTask task(0x7b00);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(78);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* arr = task.arena().create<Variable>("arr", 8);
    arr->set_ir_type(std::make_shared<const Pointer>(Integer::int32_t()));
    auto* idx = task.arena().create<Variable>("i", 4);
    auto* out = task.arena().create<Variable>("out", 4);

    auto* scaled = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{idx, task.arena().create<Constant>(4, 4)},
        4);
    auto* addr = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{arr, scaled},
        8);
    auto* deref = task.arena().create<Operation>(
        OperationType::deref,
        std::vector<Expression*>{addr},
        4);

    bb->add_instruction(task.arena().create<Assignment>(out, deref));

    ArrayAccessDetectionStage stage;
    stage.execute(task);

    ASSERT_TRUE(deref->array_access().has_value());
    ASSERT_TRUE(deref->array_access()->base == arr);
    ASSERT_TRUE(deref->array_access()->index == idx);
    ASSERT_EQ(deref->array_access()->element_size, 4);
    ASSERT_TRUE(deref->array_access()->confidence);

    auto* forest = task.arena().create<AbstractSyntaxForest>();
    forest->set_root(task.arena().create<CodeNode>(bb));

    CodeVisitor visitor;
    auto lines = visitor.generate_code(forest);
    bool found = false;
    for (const auto& line : lines) {
        if (line.find("out = arr[i];") != std::string::npos) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    std::cout << "[+] test_array_access_detection_stage passed.\n";
}

void test_global_variable_declarations() {
    DecompilerTask task(0x7d00);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(80);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* g_counter = task.arena().create<GlobalVariable>(
        "g_counter",
        4,
        task.arena().create<Constant>(0, 4),
        true);
    g_counter->set_ir_type(Integer::int32_t());

    auto* out = task.arena().create<Variable>("out", 4);
    out->set_ir_type(Integer::int32_t());

    auto* deref = task.arena().create<Operation>(
        OperationType::deref,
        std::vector<Expression*>{g_counter},
        4);
    bb->add_instruction(task.arena().create<Assignment>(out, deref));
    bb->add_instruction(task.arena().create<Return>(std::vector<Expression*>{out}));

    auto ast = std::make_unique<AbstractSyntaxForest>();
    ast->set_root(task.arena().create<CodeNode>(bb));
    task.set_ast(std::move(ast));

    CodeVisitor visitor;
    auto lines = visitor.generate_code(task);

    bool has_global_decl = false;
    bool has_load_use = false;
    bool has_local_global_decl = false;
    for (const auto& line : lines) {
        if (line == "extern const int g_counter;") {
            has_global_decl = true;
        }
        if (line.find("out = g_counter;") != std::string::npos) {
            has_load_use = true;
        }
        if (line.find("int g_counter;") != std::string::npos && line.find("extern") == std::string::npos) {
            has_local_global_decl = true;
        }
    }

    ASSERT_TRUE(has_global_decl);
    ASSERT_TRUE(has_load_use);
    ASSERT_TRUE(!has_local_global_decl);

    std::cout << "[+] test_global_variable_declarations passed.\n";
}

void test_variable_name_generation_default() {
    aletheia::DecompilerArena arena;

    auto* eax = arena.create<aletheia::Variable>("eax", 4);
    eax->set_ssa_version(3);
    auto* ebx = arena.create<aletheia::Variable>("ebx", 4);
    ebx->set_ssa_version(5);
    auto* ecx = arena.create<aletheia::Variable>("ecx", 4);
    ecx->set_ssa_version(1);

    auto* bb = arena.create<aletheia::BasicBlock>(164);
    bb->add_instruction(arena.create<aletheia::Assignment>(
        eax,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{ebx, arena.create<aletheia::Constant>(1, 4)},
            4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(
        ecx,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{eax, ebx},
            4)));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(arena.create<aletheia::CodeNode>(bb));
    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);

    aletheia::VariableNameGeneration::apply_default(forest);

    ASSERT_TRUE(eax->name().starts_with("var_"));
    ASSERT_TRUE(ebx->name().starts_with("var_"));
    ASSERT_TRUE(ecx->name().starts_with("var_"));
    ASSERT_TRUE(eax->name() != ebx->name());
    ASSERT_TRUE(eax->name() != ecx->name());
    ASSERT_TRUE(ebx->name() != ecx->name());
    ASSERT_EQ(eax->ssa_version(), 0);
    ASSERT_EQ(ebx->ssa_version(), 0);
    ASSERT_EQ(ecx->ssa_version(), 0);

    std::cout << "[+] test_variable_name_generation_default passed.\n";
}

void test_variable_name_generation_system_hungarian() {
    aletheia::DecompilerArena arena;

    auto* int_var = arena.create<aletheia::Variable>("x", 4);
    int_var->set_ir_type(aletheia::Integer::int32_t());
    int_var->set_ssa_version(2);

    auto* ptr_var = arena.create<aletheia::Variable>("p", 8);
    ptr_var->set_ir_type(std::make_shared<const aletheia::Pointer>(aletheia::Integer::int32_t()));
    ptr_var->set_ssa_version(4);

    auto* float_var = arena.create<aletheia::Variable>("f", 4);
    float_var->set_ir_type(aletheia::Float::float32());
    float_var->set_ssa_version(1);

    auto* bb = arena.create<aletheia::BasicBlock>(165);
    bb->add_instruction(arena.create<aletheia::Assignment>(int_var, arena.create<aletheia::Constant>(1, 4)));
    bb->add_instruction(arena.create<aletheia::Assignment>(ptr_var, int_var));
    bb->add_instruction(arena.create<aletheia::Assignment>(float_var, int_var));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(arena.create<aletheia::CodeNode>(bb));
    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);

    aletheia::VariableNameGeneration::apply_system_hungarian(forest);

    ASSERT_TRUE(int_var->name().starts_with("iVar"));
    ASSERT_TRUE(ptr_var->name().starts_with("pVar"));
    ASSERT_TRUE(float_var->name().starts_with("fVar"));
    ASSERT_EQ(int_var->ssa_version(), 0);
    ASSERT_EQ(ptr_var->ssa_version(), 0);
    ASSERT_EQ(float_var->ssa_version(), 0);

    std::cout << "[+] test_variable_name_generation_system_hungarian passed.\n";
}

void test_variable_name_generation_skips_globals() {
    aletheia::DecompilerArena arena;

    auto* g_value = arena.create<aletheia::GlobalVariable>(
        "g_value",
        4,
        arena.create<aletheia::Constant>(0, 4),
        false);
    g_value->set_ir_type(aletheia::Integer::int32_t());

    auto* x = arena.create<aletheia::Variable>("eax", 4);
    x->set_ssa_version(2);

    auto* bb = arena.create<aletheia::BasicBlock>(172);
    bb->add_instruction(arena.create<aletheia::Assignment>(
        x,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::deref,
            std::vector<aletheia::Expression*>{g_value},
            4)));

    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(arena.create<aletheia::CodeNode>(bb));

    aletheia::VariableNameGeneration::apply_default(forest);

    ASSERT_EQ(g_value->name(), "g_value");
    ASSERT_TRUE(x->name().starts_with("var_"));

    std::cout << "[+] test_variable_name_generation_skips_globals passed.\n";
}

void test_loop_name_generator_for_counters() {
    aletheia::DecompilerArena arena;

    auto* idx_a = arena.create<aletheia::Variable>("var_10", 4);
    auto* idx_b = arena.create<aletheia::Variable>("var_11", 4);

    auto* body_a_block = arena.create<aletheia::BasicBlock>(166);
    body_a_block->add_instruction(arena.create<aletheia::Assignment>(
        idx_a,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{idx_a, arena.create<aletheia::Constant>(1, 4)},
            4)));

    auto* body_b_block = arena.create<aletheia::BasicBlock>(167);
    body_b_block->add_instruction(arena.create<aletheia::Assignment>(
        idx_b,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{idx_b, arena.create<aletheia::Constant>(1, 4)},
            4)));

    auto* for_a = arena.create<aletheia::ForLoopNode>(
        arena.create<aletheia::CodeNode>(body_a_block),
        arena.create<aletheia::Condition>(aletheia::OperationType::lt, idx_a, arena.create<aletheia::Constant>(5, 4), 1),
        arena.create<aletheia::Assignment>(idx_a, arena.create<aletheia::Constant>(0, 4)),
        arena.create<aletheia::Assignment>(
            idx_a,
            arena.create<aletheia::Operation>(
                aletheia::OperationType::add,
                std::vector<aletheia::Expression*>{idx_a, arena.create<aletheia::Constant>(1, 4)},
                4)));

    auto* for_b = arena.create<aletheia::ForLoopNode>(
        arena.create<aletheia::CodeNode>(body_b_block),
        arena.create<aletheia::Condition>(aletheia::OperationType::lt, idx_b, arena.create<aletheia::Constant>(3, 4), 1),
        arena.create<aletheia::Assignment>(idx_b, arena.create<aletheia::Constant>(0, 4)),
        arena.create<aletheia::Assignment>(
            idx_b,
            arena.create<aletheia::Operation>(
                aletheia::OperationType::add,
                std::vector<aletheia::Expression*>{idx_b, arena.create<aletheia::Constant>(1, 4)},
                4)));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(for_a);
    seq->add_node(for_b);
    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);

    aletheia::LoopNameGenerator::apply_for_loop_counters(forest);

    ASSERT_EQ(idx_a->name(), "i");
    ASSERT_EQ(idx_b->name(), "j");
    ASSERT_EQ(idx_a->ssa_version(), 0);
    ASSERT_EQ(idx_b->ssa_version(), 0);

    std::cout << "[+] test_loop_name_generator_for_counters passed.\n";
}

void test_loop_name_generator_while_counters() {
    aletheia::DecompilerArena arena;

    auto* w0 = arena.create<aletheia::Variable>("var_20", 4);
    auto* w1 = arena.create<aletheia::Variable>("var_21", 4);

    auto* body0 = arena.create<aletheia::BasicBlock>(168);
    body0->add_instruction(arena.create<aletheia::Assignment>(
        w0,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{w0, arena.create<aletheia::Constant>(1, 4)},
            4)));

    auto* body1 = arena.create<aletheia::BasicBlock>(169);
    body1->add_instruction(arena.create<aletheia::Assignment>(
        w1,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{w1, arena.create<aletheia::Constant>(1, 4)},
            4)));

    auto* while0 = arena.create<aletheia::WhileLoopNode>(
        arena.create<aletheia::CodeNode>(body0),
        arena.create<aletheia::Condition>(aletheia::OperationType::lt, w0, arena.create<aletheia::Constant>(10, 4), 1));
    auto* while1 = arena.create<aletheia::WhileLoopNode>(
        arena.create<aletheia::CodeNode>(body1),
        arena.create<aletheia::Condition>(aletheia::OperationType::lt, w1, arena.create<aletheia::Constant>(5, 4), 1));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(while0);
    seq->add_node(while1);
    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);

    aletheia::LoopNameGenerator::apply_while_loop_counters(forest);

    ASSERT_EQ(w0->name(), "counter");
    ASSERT_EQ(w1->name(), "counter1");
    ASSERT_EQ(w0->ssa_version(), 0);
    ASSERT_EQ(w1->ssa_version(), 0);

    std::cout << "[+] test_loop_name_generator_while_counters passed.\n";
}

void test_instruction_length_handler() {
    aletheia::DecompilerArena arena;

    auto* a = arena.create<aletheia::Variable>("a", 4);
    auto* b = arena.create<aletheia::Variable>("b", 4);
    auto* c = arena.create<aletheia::Variable>("c", 4);
    auto* d = arena.create<aletheia::Variable>("d", 4);
    auto* out = arena.create<aletheia::Variable>("out", 4);

    auto* mul_ab = arena.create<aletheia::Operation>(
        aletheia::OperationType::mul,
        std::vector<aletheia::Expression*>{a, b},
        4);
    auto* mul_cd = arena.create<aletheia::Operation>(
        aletheia::OperationType::mul,
        std::vector<aletheia::Expression*>{c, d},
        4);
    auto* add_expr = arena.create<aletheia::Operation>(
        aletheia::OperationType::add,
        std::vector<aletheia::Expression*>{mul_ab, mul_cd},
        4);

    auto* mul_ab_ret = arena.create<aletheia::Operation>(
        aletheia::OperationType::mul,
        std::vector<aletheia::Expression*>{a, b},
        4);
    auto* mul_cd_ret = arena.create<aletheia::Operation>(
        aletheia::OperationType::mul,
        std::vector<aletheia::Expression*>{c, d},
        4);
    auto* add_expr_ret = arena.create<aletheia::Operation>(
        aletheia::OperationType::add,
        std::vector<aletheia::Expression*>{mul_ab_ret, mul_cd_ret},
        4);

    auto* bb = arena.create<aletheia::BasicBlock>(170);
    auto* assign = arena.create<aletheia::Assignment>(out, add_expr);
    auto* ret = arena.create<aletheia::Return>(std::vector<aletheia::Expression*>{add_expr_ret});
    bb->add_instruction(assign);
    bb->add_instruction(ret);

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(arena.create<aletheia::CodeNode>(bb));
    auto* forest = arena.create<aletheia::AbstractSyntaxForest>();
    forest->set_root(seq);

    aletheia::InstructionLengthBounds bounds;
    bounds.assignment_instr = 2;
    bounds.call_operation = 1;
    bounds.return_instr = 2;
    aletheia::InstructionLengthHandler::apply(forest, arena, bounds);

    const auto& instrs = bb->instructions();
    ASSERT_TRUE(instrs.size() >= 4);
    ASSERT_TRUE(dynamic_cast<aletheia::Assignment*>(instrs[0]) != nullptr);
    ASSERT_TRUE(dynamic_cast<aletheia::Assignment*>(instrs[1]) != nullptr);
    ASSERT_TRUE(dynamic_cast<aletheia::Assignment*>(instrs[instrs.size() - 2]) != nullptr);
    ASSERT_TRUE(dynamic_cast<aletheia::Return*>(instrs.back()) != nullptr);

    auto* final_assign = dynamic_cast<aletheia::Assignment*>(instrs[instrs.size() - 2]);
    ASSERT_TRUE(final_assign != nullptr);
    auto* final_add = dynamic_cast<aletheia::Operation*>(final_assign->value());
    ASSERT_TRUE(final_add != nullptr);
    ASSERT_TRUE(dynamic_cast<aletheia::Variable*>(final_add->operands()[0]) != nullptr);
    ASSERT_TRUE(dynamic_cast<aletheia::Variable*>(final_add->operands()[1]) != nullptr);

    auto* final_ret = dynamic_cast<aletheia::Return*>(instrs.back());
    ASSERT_TRUE(final_ret != nullptr);
    ASSERT_EQ(final_ret->values().size(), 1);
    auto* ret_expr = final_ret->values()[0];
    auto* ret_var = dynamic_cast<aletheia::Variable*>(ret_expr);
    auto* ret_op = dynamic_cast<aletheia::Operation*>(ret_expr);
    ASSERT_TRUE(ret_var != nullptr || ret_op != nullptr);
    if (ret_op != nullptr) {
        ASSERT_TRUE(ret_op->operands().size() <= 2);
        for (auto* operand : ret_op->operands()) {
            ASSERT_TRUE(dynamic_cast<aletheia::Variable*>(operand) != nullptr ||
                        dynamic_cast<aletheia::Constant*>(operand) != nullptr);
        }
    }

    std::cout << "[+] test_instruction_length_handler passed.\n";
}

void test_condition_handler() {
    z3::context ctx;
    ConditionHandler handler(ctx);
    aletheia::DecompilerArena arena;

    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* c0 = arena.create<aletheia::Constant>(0, 4);
    auto* c1 = arena.create<aletheia::Constant>(1, 4);

    auto* eq = arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, c0, 1);
    auto eq_symbol = handler.add_condition(eq);
    auto eq_name = handler.symbol_for_condition(eq);
    ASSERT_TRUE(eq_name.has_value());
    ASSERT_TRUE(handler.condition_for_symbol(*eq_name) == eq);

    auto eq_props = handler.case_properties_for_symbol(*eq_name);
    ASSERT_TRUE(eq_props.has_value());
    ASSERT_TRUE(eq_props->expression == x);
    ASSERT_TRUE(eq_props->constant == c0);
    ASSERT_TRUE(!eq_props->negated);

    auto* neq = arena.create<aletheia::Condition>(aletheia::OperationType::neq, x, c1, 1);
    auto neq_symbol = handler.add_condition(neq);
    auto neq_name = handler.symbol_for_condition(neq);
    ASSERT_TRUE(neq_name.has_value());
    ASSERT_TRUE(handler.condition_for_symbol(*neq_name) == neq);

    auto neq_props = handler.case_properties_for_symbol(*neq_name);
    ASSERT_TRUE(neq_props.has_value());
    ASSERT_TRUE(neq_props->expression == x);
    ASSERT_TRUE(neq_props->constant == c1);
    ASSERT_TRUE(neq_props->negated);

    auto eq_symbol_again = handler.add_condition(eq);
    ASSERT_EQ(eq_symbol.expression().to_string(), eq_symbol_again.expression().to_string());
    ASSERT_TRUE(eq_symbol.expression().to_string() != neq_symbol.expression().to_string());

    std::cout << "[+] test_condition_handler passed.\n";
}

void test_transition_cfg_switch_tags() {
    aletheia::DecompilerTask task(0x7f00);

    auto cfg = std::make_unique<aletheia::ControlFlowGraph>();
    auto* dispatch = task.arena().create<aletheia::BasicBlock>(150);
    auto* case_a = task.arena().create<aletheia::BasicBlock>(151);
    auto* case_b = task.arena().create<aletheia::BasicBlock>(152);
    auto* case_default = task.arena().create<aletheia::BasicBlock>(153);

    cfg->set_entry_block(dispatch);
    cfg->add_block(dispatch);
    cfg->add_block(case_a);
    cfg->add_block(case_b);
    cfg->add_block(case_default);

    auto* selector = task.arena().create<aletheia::Variable>("sel", 4);
    dispatch->add_instruction(task.arena().create<aletheia::IndirectBranch>(selector));

    auto* edge_a = task.arena().create<aletheia::SwitchEdge>(
        dispatch,
        case_a,
        std::vector<std::int64_t>{1, 3});
    auto* edge_b = task.arena().create<aletheia::SwitchEdge>(
        dispatch,
        case_b,
        std::vector<std::int64_t>{2});
    auto* edge_default = task.arena().create<aletheia::SwitchEdge>(
        dispatch,
        case_default,
        std::vector<std::int64_t>{},
        true);

    dispatch->add_successor(edge_a);
    dispatch->add_successor(edge_b);
    dispatch->add_successor(edge_default);
    case_a->add_predecessor(edge_a);
    case_b->add_predecessor(edge_b);
    case_default->add_predecessor(edge_default);

    task.set_cfg(std::move(cfg));

    aletheia::PatternIndependentRestructuringStage stage;
    aletheia::TransitionCFG tcfg(task.arena());
    stage.build_initial_transition_cfg(task, tcfg);

    ASSERT_TRUE(tcfg.entry() != nullptr);

    std::optional<logos::LogicCondition> tag_a;
    std::optional<logos::LogicCondition> tag_b;
    std::optional<logos::LogicCondition> tag_default;

    for (auto* tedge : tcfg.entry()->successors()) {
        auto* sink_block = tedge->sink()->ast_node()->get_original_block();
        if (sink_block == case_a) {
            tag_a = tedge->tag();
        } else if (sink_block == case_b) {
            tag_b = tedge->tag();
        } else if (sink_block == case_default) {
            tag_default = tedge->tag();
        }
    }

    ASSERT_TRUE(tag_a.has_value());
    ASSERT_TRUE(tag_b.has_value());
    ASSERT_TRUE(tag_default.has_value());

    ASSERT_TRUE(tag_a->expression().is_or());

    logos::LogicCondition all_cases(tag_a->expression() || tag_b->expression());
    ASSERT_TRUE(tag_default->is_complementary_to(all_cases));
    ASSERT_TRUE(tag_a->does_imply(all_cases));
    ASSERT_TRUE(tag_b->does_imply(all_cases));

    std::cout << "[+] test_transition_cfg_switch_tags passed.\n";
}

void test_cbr_complementary_conditions() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* c0 = arena.create<aletheia::Constant>(0, 4);
    auto* cond_eq = arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, c0, 1);
    auto* cond_neq = arena.create<aletheia::Condition>(aletheia::OperationType::neq, x, c0, 1);

    auto* y = arena.create<aletheia::Variable>("y", 4);

    auto* bb_true = arena.create<aletheia::BasicBlock>(140);
    bb_true->add_instruction(arena.create<aletheia::Assignment>(y, arena.create<aletheia::Constant>(1, 4)));
    auto* bb_false = arena.create<aletheia::BasicBlock>(141);
    bb_false->add_instruction(arena.create<aletheia::Assignment>(y, arena.create<aletheia::Constant>(2, 4)));

    auto* if_true = arena.create<aletheia::IfNode>(arena.create<aletheia::ExprAstNode>(cond_eq), arena.create<aletheia::CodeNode>(bb_true));
    auto* if_false = arena.create<aletheia::IfNode>(arena.create<aletheia::ExprAstNode>(cond_neq), arena.create<aletheia::CodeNode>(bb_false));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(if_true);
    seq->add_node(if_false);

    auto refined = aletheia::ConditionBasedRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* refined_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(refined_seq != nullptr);
    ASSERT_EQ(refined_seq->nodes().size(), 1);

    auto* merged = dynamic_cast<aletheia::IfNode*>(refined_seq->nodes()[0]);
    ASSERT_TRUE(merged != nullptr);
    ASSERT_TRUE(merged->false_branch() != nullptr);

    std::cout << "[+] test_cbr_complementary_conditions passed.\n";
}

void test_cbr_cnf_subexpression_grouping() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* x = arena.create<aletheia::Variable>("x", 4);
    auto* y = arena.create<aletheia::Variable>("y", 4);
    auto* z = arena.create<aletheia::Variable>("z", 4);

    auto* cond_a = arena.create<aletheia::Condition>(
        aletheia::OperationType::eq,
        x,
        arena.create<aletheia::Constant>(0, 4),
        1);
    auto* cond_b = arena.create<aletheia::Condition>(
        aletheia::OperationType::gt,
        y,
        arena.create<aletheia::Constant>(1, 4),
        1);
    auto* cond_c = arena.create<aletheia::Condition>(
        aletheia::OperationType::lt,
        z,
        arena.create<aletheia::Constant>(5, 4),
        1);

    auto* and1 = arena.create<aletheia::Operation>(
        aletheia::OperationType::logical_and,
        std::vector<aletheia::Expression*>{cond_a, cond_b},
        1);
    auto* and2 = arena.create<aletheia::Operation>(
        aletheia::OperationType::logical_and,
        std::vector<aletheia::Expression*>{cond_a, cond_c},
        1);

    auto* out = arena.create<aletheia::Variable>("out", 4);
    auto* bb1 = arena.create<aletheia::BasicBlock>(142);
    bb1->add_instruction(arena.create<aletheia::Assignment>(out, arena.create<aletheia::Constant>(1, 4)));
    auto* bb2 = arena.create<aletheia::BasicBlock>(143);
    bb2->add_instruction(arena.create<aletheia::Assignment>(out, arena.create<aletheia::Constant>(2, 4)));

    auto* if1 = arena.create<aletheia::IfNode>(arena.create<aletheia::ExprAstNode>(and1), arena.create<aletheia::CodeNode>(bb1));
    auto* if2 = arena.create<aletheia::IfNode>(arena.create<aletheia::ExprAstNode>(and2), arena.create<aletheia::CodeNode>(bb2));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(if1);
    seq->add_node(if2);

    auto refined = aletheia::ConditionBasedRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* refined_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(refined_seq != nullptr);
    ASSERT_EQ(refined_seq->nodes().size(), 1);

    auto* outer_if = dynamic_cast<aletheia::IfNode*>(refined_seq->nodes()[0]);
    ASSERT_TRUE(outer_if != nullptr);
    auto* outer_cond_ast = dynamic_cast<aletheia::ExprAstNode*>(outer_if->cond());
    ASSERT_TRUE(outer_cond_ast != nullptr);
    ASSERT_TRUE(outer_cond_ast->expr() == cond_a);

    auto* grouped_seq = dynamic_cast<aletheia::SeqNode*>(outer_if->true_branch());
    ASSERT_TRUE(grouped_seq != nullptr);
    ASSERT_EQ(grouped_seq->nodes().size(), 2);

    auto* grouped_if1 = dynamic_cast<aletheia::IfNode*>(grouped_seq->nodes()[0]);
    auto* grouped_if2 = dynamic_cast<aletheia::IfNode*>(grouped_seq->nodes()[1]);
    ASSERT_TRUE(grouped_if1 != nullptr && grouped_if2 != nullptr);

    auto* grouped_cond1 = dynamic_cast<aletheia::ExprAstNode*>(grouped_if1->cond());
    auto* grouped_cond2 = dynamic_cast<aletheia::ExprAstNode*>(grouped_if2->cond());
    ASSERT_TRUE(grouped_cond1 != nullptr && grouped_cond2 != nullptr);
    ASSERT_TRUE(grouped_cond1->expr() == cond_b);
    ASSERT_TRUE(grouped_cond2->expr() == cond_c);

    std::cout << "[+] test_cbr_cnf_subexpression_grouping passed.\n";
}

void test_car_initial_switch_constructor() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* x = arena.create<aletheia::Variable>("x", 4);

    auto* bb1 = arena.create<aletheia::BasicBlock>(150);
    bb1->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("v1", 4), arena.create<aletheia::Constant>(1, 4)));
    auto* bb2 = arena.create<aletheia::BasicBlock>(151);
    bb2->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("v2", 4), arena.create<aletheia::Constant>(2, 4)));
    auto* bb_def = arena.create<aletheia::BasicBlock>(152);
    bb_def->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("vd", 4), arena.create<aletheia::Constant>(3, 4)));

    auto* if2 = arena.create<aletheia::IfNode>(
        arena.create<aletheia::ExprAstNode>(arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, arena.create<aletheia::Constant>(2, 4), 1)),
        arena.create<aletheia::CodeNode>(bb2),
        arena.create<aletheia::CodeNode>(bb_def));
    auto* if1 = arena.create<aletheia::IfNode>(
        arena.create<aletheia::ExprAstNode>(arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, arena.create<aletheia::Constant>(1, 4), 1)),
        arena.create<aletheia::CodeNode>(bb1),
        if2);

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(if1);

    auto* refined = aletheia::ConditionAwareRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* out_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(out_seq != nullptr);
    ASSERT_EQ(out_seq->nodes().size(), 1);
    auto* sw = dynamic_cast<aletheia::SwitchNode*>(out_seq->nodes()[0]);
    ASSERT_TRUE(sw != nullptr);
    ASSERT_EQ(sw->cases().size(), 3);

    std::cout << "[+] test_car_initial_switch_constructor passed.\n";
}

void test_car_switch_extractor_and_missing_case_sequence() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* x = arena.create<aletheia::Variable>("x", 4);

    auto* bb_case1 = arena.create<aletheia::BasicBlock>(153);
    bb_case1->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("a", 4), arena.create<aletheia::Constant>(11, 4)));
    auto* bb_case2 = arena.create<aletheia::BasicBlock>(154);
    bb_case2->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("b", 4), arena.create<aletheia::Constant>(22, 4)));
    auto* bb_case3 = arena.create<aletheia::BasicBlock>(155);
    bb_case3->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("c", 4), arena.create<aletheia::Constant>(33, 4)));
    auto* bb_default = arena.create<aletheia::BasicBlock>(156);
    bb_default->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("d", 4), arena.create<aletheia::Constant>(44, 4)));

    auto* inner_switch = arena.create<aletheia::SwitchNode>(arena.create<aletheia::ExprAstNode>(x));
    inner_switch->add_case(arena.create<aletheia::CaseNode>(2, arena.create<aletheia::CodeNode>(bb_case2)));

    auto* wrapper_if = arena.create<aletheia::IfNode>(
        arena.create<aletheia::ExprAstNode>(arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, arena.create<aletheia::Constant>(1, 4), 1)),
        arena.create<aletheia::CodeNode>(bb_case1),
        inner_switch);

    auto* sibling_if = arena.create<aletheia::IfNode>(
        arena.create<aletheia::ExprAstNode>(arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, arena.create<aletheia::Constant>(3, 4), 1)),
        arena.create<aletheia::CodeNode>(bb_case3),
        arena.create<aletheia::CodeNode>(bb_default));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(wrapper_if);
    seq->add_node(sibling_if);

    auto* refined = aletheia::ConditionAwareRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* out_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(out_seq != nullptr);
    ASSERT_EQ(out_seq->nodes().size(), 1);
    auto* sw = dynamic_cast<aletheia::SwitchNode*>(out_seq->nodes()[0]);
    ASSERT_TRUE(sw != nullptr);

    bool has1 = false, has2 = false, has3 = false, has_default = false;
    for (auto* c : sw->cases()) {
        if (c->is_default()) has_default = true;
        if (!c->is_default() && c->value() == 1) has1 = true;
        if (!c->is_default() && c->value() == 2) has2 = true;
        if (!c->is_default() && c->value() == 3) has3 = true;
    }
    ASSERT_TRUE(has1 && has2 && has3 && has_default);

    std::cout << "[+] test_car_switch_extractor_and_missing_case_sequence passed.\n";
}

void test_car_missing_case_finder_condition() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* x = arena.create<aletheia::Variable>("x", 4);

    auto* bb_nested = arena.create<aletheia::BasicBlock>(157);
    bb_nested->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("m", 4), arena.create<aletheia::Constant>(55, 4)));
    auto* bb_rest = arena.create<aletheia::BasicBlock>(158);
    bb_rest->add_instruction(arena.create<aletheia::Assignment>(arena.create<aletheia::Variable>("n", 4), arena.create<aletheia::Constant>(66, 4)));

    auto* nested_if = arena.create<aletheia::IfNode>(
        arena.create<aletheia::ExprAstNode>(arena.create<aletheia::Condition>(aletheia::OperationType::eq, x, arena.create<aletheia::Constant>(4, 4), 1)),
        arena.create<aletheia::CodeNode>(bb_nested),
        arena.create<aletheia::CodeNode>(bb_rest));

    auto* sw = arena.create<aletheia::SwitchNode>(arena.create<aletheia::ExprAstNode>(x));
    sw->add_case(arena.create<aletheia::CaseNode>(1, nested_if));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(sw);

    auto* refined = aletheia::ConditionAwareRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* out_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(out_seq != nullptr);
    auto* out_sw = dynamic_cast<aletheia::SwitchNode*>(out_seq->nodes()[0]);
    ASSERT_TRUE(out_sw != nullptr);

    bool has_case1 = false;
    bool has_case4 = false;
    for (auto* c : out_sw->cases()) {
        if (!c->is_default() && c->value() == 1) has_case1 = true;
        if (!c->is_default() && c->value() == 4) has_case4 = true;
    }
    ASSERT_TRUE(has_case1 && has_case4);

    std::cout << "[+] test_car_missing_case_finder_condition passed.\n";
}

void test_guarded_do_while_rewrite() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* i = arena.create<aletheia::Variable>("i", 4);
    auto* cond = arena.create<aletheia::Condition>(
        aletheia::OperationType::lt,
        i,
        arena.create<aletheia::Constant>(10, 4),
        1);

    auto* bb_body = arena.create<aletheia::BasicBlock>(159);
    bb_body->add_instruction(arena.create<aletheia::Assignment>(
        i,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{i, arena.create<aletheia::Constant>(1, 4)},
            4)));

    auto* guarded = arena.create<aletheia::IfNode>(
        arena.create<aletheia::ExprAstNode>(cond),
        arena.create<aletheia::DoWhileLoopNode>(arena.create<aletheia::CodeNode>(bb_body), cond));

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(guarded);

    auto* refined = aletheia::ConditionAwareRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* out_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(out_seq != nullptr);
    ASSERT_EQ(out_seq->nodes().size(), 1);

    auto* while_node = dynamic_cast<aletheia::WhileLoopNode*>(out_seq->nodes()[0]);
    ASSERT_TRUE(while_node != nullptr);
    ASSERT_TRUE(while_node->condition() != nullptr);
    ASSERT_TRUE(dynamic_cast<aletheia::CodeNode*>(while_node->body()) != nullptr);

    std::cout << "[+] test_guarded_do_while_rewrite passed.\n";
}

void test_while_loop_replacer() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* i = arena.create<aletheia::Variable>("i", 4);
    auto* sum = arena.create<aletheia::Variable>("sum", 4);

    auto* init_block = arena.create<aletheia::BasicBlock>(161);
    auto* init_assign = arena.create<aletheia::Assignment>(i, arena.create<aletheia::Constant>(0, 4));
    init_block->add_instruction(init_assign);
    auto* init_node = arena.create<aletheia::CodeNode>(init_block);

    auto* body_work_block = arena.create<aletheia::BasicBlock>(162);
    body_work_block->add_instruction(arena.create<aletheia::Assignment>(
        sum,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{sum, i},
            4)));

    auto* body_update_block = arena.create<aletheia::BasicBlock>(163);
    auto* mod_assign = arena.create<aletheia::Assignment>(
        i,
        arena.create<aletheia::Operation>(
            aletheia::OperationType::add,
            std::vector<aletheia::Expression*>{i, arena.create<aletheia::Constant>(1, 4)},
            4));
    body_update_block->add_instruction(mod_assign);

    auto* body_seq = arena.create<aletheia::SeqNode>();
    body_seq->add_node(arena.create<aletheia::CodeNode>(body_work_block));
    body_seq->add_node(arena.create<aletheia::CodeNode>(body_update_block));

    auto* cond = arena.create<aletheia::Condition>(
        aletheia::OperationType::lt,
        i,
        arena.create<aletheia::Constant>(10, 4),
        1);
    auto* while_node = arena.create<aletheia::WhileLoopNode>(body_seq, cond);

    auto* seq = arena.create<aletheia::SeqNode>();
    seq->add_node(init_node);
    seq->add_node(while_node);

    auto* refined = aletheia::ConditionAwareRefinement::refine(
        arena,
        ctx,
        seq,
        std::unordered_map<aletheia::TransitionBlock*, logos::LogicCondition>{});

    auto* out_seq = dynamic_cast<aletheia::SeqNode*>(refined);
    ASSERT_TRUE(out_seq != nullptr);
    ASSERT_EQ(out_seq->nodes().size(), 1);

    auto* for_node = dynamic_cast<aletheia::ForLoopNode*>(out_seq->nodes()[0]);
    ASSERT_TRUE(for_node != nullptr);
    ASSERT_TRUE(for_node->declaration() == init_assign);
    ASSERT_TRUE(for_node->modification() == mod_assign);

    // The update assignment should be removed from loop body after extraction.
    const auto& update_insts = body_update_block->instructions();
    ASSERT_TRUE(update_insts.empty());

    std::cout << "[+] test_while_loop_replacer passed.\n";
}

void test_sibling_reachability() {
    aletheia::DecompilerArena arena;
    z3::context ctx;

    auto* b1 = arena.create<aletheia::TransitionBlock>(arena.create<aletheia::SeqNode>());
    auto* b2 = arena.create<aletheia::TransitionBlock>(arena.create<aletheia::SeqNode>());
    auto* b3 = arena.create<aletheia::TransitionBlock>(arena.create<aletheia::SeqNode>());

    aletheia::TransitionCFG cfg(arena);
    cfg.set_entry(b1);
    // Intentionally non-topological insertion order
    cfg.add_block(b3);
    cfg.add_block(b2);
    cfg.add_block(b1);
    cfg.add_edge(b1, b2, logos::LogicCondition(ctx.bool_val(true)));

    aletheia::ReachabilityGraph reach(&cfg);
    aletheia::SiblingReachability siblings(reach);
    auto ordered = siblings.order_blocks(cfg.blocks());

    std::size_t pos_b1 = 0;
    std::size_t pos_b2 = 0;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (ordered[i] == b1) pos_b1 = i;
        if (ordered[i] == b2) pos_b2 = i;
    }
    ASSERT_TRUE(pos_b1 < pos_b2);

    std::cout << "[+] test_sibling_reachability passed.\n";
}

void test_case_dependency_graph_ordering() {
    aletheia::DecompilerArena arena;

    auto* empty = arena.create<aletheia::SeqNode>();
    auto* c3 = arena.create<aletheia::CaseNode>(3, empty, false, true);
    auto* def = arena.create<aletheia::CaseNode>(0, empty, true, true);
    auto* c1 = arena.create<aletheia::CaseNode>(1, empty, false, true);
    auto* c2 = arena.create<aletheia::CaseNode>(2, empty, false, true);

    std::vector<aletheia::CaseNode*> ordered = aletheia::CaseDependencyGraph::order_cases({c3, def, c1, c2});
    ASSERT_EQ(ordered.size(), 4);
    ASSERT_TRUE(!ordered[0]->is_default() && ordered[0]->value() == 1);
    ASSERT_TRUE(!ordered[1]->is_default() && ordered[1]->value() == 2);
    ASSERT_TRUE(!ordered[2]->is_default() && ordered[2]->value() == 3);
    ASSERT_TRUE(ordered[3]->is_default());

    std::cout << "[+] test_case_dependency_graph_ordering passed.\n";
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

    idiomata::IdiomTag tag;
    tag.address = 0x1000;
    tag.length = 3;
    tag.operation = "division unsigned";
    tag.operand = "eax";
    tag.constant = 5;
    task.set_idiom_tags(std::vector<idiomata::IdiomTag>{tag});

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

void test_mem_phi_converter_stage() {
    {
        DecompilerTask task(0x3a00);

        auto cfg = std::make_unique<ControlFlowGraph>();
        auto* bb = task.arena().create<BasicBlock>(6);
        cfg->set_entry_block(bb);
        cfg->add_block(bb);
        task.set_cfg(std::move(cfg));

        auto* aliased_v1 = task.arena().create<Variable>("v", 8);
        aliased_v1->set_ssa_version(1);
        aliased_v1->set_aliased(true);
        bb->add_instruction(task.arena().create<Assignment>(aliased_v1, task.arena().create<Constant>(1, 8)));

        auto* mem3 = task.arena().create<Variable>("mem", 8);
        mem3->set_ssa_version(3);
        auto* mem4 = task.arena().create<Variable>("mem", 8);
        mem4->set_ssa_version(4);
        auto* mem5 = task.arena().create<Variable>("mem", 8);
        mem5->set_ssa_version(5);

        auto* memphi_args = task.arena().create<ListOperation>(std::vector<Expression*>{mem4, mem3}, 8);
        auto* memphi = task.arena().create<MemPhi>(mem5, memphi_args);
        bb->add_instruction(memphi);

        MemPhiConverterStage stage;
        stage.execute(task);

        const auto& insts = bb->instructions();
        ASSERT_EQ(insts.size(), 2);
        ASSERT_TRUE(dynamic_cast<MemPhi*>(insts[1]) == nullptr);

        auto* phi = dynamic_cast<Phi*>(insts[1]);
        ASSERT_TRUE(phi != nullptr);
        auto* phi_dst = phi->dest_var();
        ASSERT_TRUE(phi_dst != nullptr);
        ASSERT_EQ(phi_dst->name(), "v");
        ASSERT_EQ(phi_dst->ssa_version(), 5);
        ASSERT_TRUE(phi_dst->is_aliased());

        auto* phi_ops = phi->operand_list();
        ASSERT_TRUE(phi_ops != nullptr);
        ASSERT_EQ(phi_ops->operands().size(), 2);
        auto* op0 = dynamic_cast<Variable*>(phi_ops->operands()[0]);
        auto* op1 = dynamic_cast<Variable*>(phi_ops->operands()[1]);
        ASSERT_TRUE(op0 != nullptr && op1 != nullptr);
        ASSERT_EQ(op0->name(), "v");
        ASSERT_EQ(op1->name(), "v");
        ASSERT_EQ(op0->ssa_version(), 4);
        ASSERT_EQ(op1->ssa_version(), 3);
        ASSERT_TRUE(op0->is_aliased() && op1->is_aliased());
    }

    {
        DecompilerTask task(0x3a10);

        auto cfg = std::make_unique<ControlFlowGraph>();
        auto* bb = task.arena().create<BasicBlock>(7);
        cfg->set_entry_block(bb);
        cfg->add_block(bb);
        task.set_cfg(std::move(cfg));

        auto* mem1 = task.arena().create<Variable>("mem", 8);
        mem1->set_ssa_version(1);
        auto* mem2 = task.arena().create<Variable>("mem", 8);
        mem2->set_ssa_version(2);
        auto* mem3 = task.arena().create<Variable>("mem", 8);
        mem3->set_ssa_version(3);

        auto* memphi_args = task.arena().create<ListOperation>(std::vector<Expression*>{mem2, mem1}, 8);
        auto* memphi = task.arena().create<MemPhi>(mem3, memphi_args);
        bb->add_instruction(memphi);

        MemPhiConverterStage stage;
        stage.execute(task);

        ASSERT_TRUE(bb->instructions().empty());
    }

    std::cout << "[+] test_mem_phi_converter_stage passed.\n";
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

void test_remove_noreturn_boilerplate_stage() {
    DecompilerTask task(0x5100);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* entry = task.arena().create<BasicBlock>(24);
    auto* noreturn_block = task.arena().create<BasicBlock>(25);
    auto* dead_tail = task.arena().create<BasicBlock>(26);

    cfg->set_entry_block(entry);
    cfg->add_block(entry);
    cfg->add_block(noreturn_block);
    cfg->add_block(dead_tail);
    task.set_cfg(std::move(cfg));

    auto* e0 = task.arena().create<Edge>(entry, noreturn_block, EdgeType::Unconditional);
    entry->add_successor(e0);
    noreturn_block->add_predecessor(e0);

    auto* e1 = task.arena().create<Edge>(noreturn_block, dead_tail, EdgeType::Unconditional);
    noreturn_block->add_successor(e1);
    dead_tail->add_predecessor(e1);

    auto* call_target = task.arena().create<Variable>("__assert_fail", 8);
    auto* call_expr = task.arena().create<Call>(call_target, std::vector<Expression*>{}, 8);
    auto* call_dst = task.arena().create<Variable>("tmp_nr", 8);
    auto* call_assign = task.arena().create<Assignment>(call_dst, call_expr);
    call_assign->set_address(0x5101);
    noreturn_block->add_instruction(call_assign);

    auto* dead_dst = task.arena().create<Variable>("dead_after_nr", 8);
    auto* dead_assign = task.arena().create<Assignment>(dead_dst, task.arena().create<Constant>(1, 8));
    dead_assign->set_address(0x5102);
    noreturn_block->add_instruction(dead_assign);

    dead_tail->add_instruction(task.arena().create<Return>(std::vector<Expression*>{task.arena().create<Constant>(0, 8)}));

    RemoveNoreturnBoilerplateStage stage;
    stage.execute(task);

    ASSERT_EQ(noreturn_block->instructions().size(), 1);
    ASSERT_EQ(noreturn_block->successors().size(), 0);
    ASSERT_EQ(task.cfg()->blocks().size(), 2);

    std::cout << "[+] test_remove_noreturn_boilerplate_stage passed.\n";
}

void test_insert_missing_definitions_stage() {
    DecompilerTask task(0x5200);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* entry = task.arena().create<BasicBlock>(27);
    auto* left = task.arena().create<BasicBlock>(28);
    auto* right = task.arena().create<BasicBlock>(29);
    cfg->set_entry_block(entry);
    cfg->add_block(entry);
    cfg->add_block(left);
    cfg->add_block(right);
    task.set_cfg(std::move(cfg));

    auto* e_true = task.arena().create<Edge>(entry, left, EdgeType::True);
    auto* e_false = task.arena().create<Edge>(entry, right, EdgeType::False);
    entry->add_successor(e_true);
    entry->add_successor(e_false);
    left->add_predecessor(e_true);
    right->add_predecessor(e_false);

    auto* mem_v1 = task.arena().create<Variable>("mem", 8);
    mem_v1->set_ssa_version(1);
    mem_v1->set_aliased(true);
    entry->add_instruction(task.arena().create<Assignment>(mem_v1, task.arena().create<Constant>(7, 8)));

    auto* cond_v = task.arena().create<Variable>("cond", 1);
    auto* branch = task.arena().create<Branch>(
        task.arena().create<Condition>(OperationType::neq, cond_v, task.arena().create<Constant>(0, 1), 1));
    entry->add_instruction(branch);

    auto* mem_v2_left = task.arena().create<Variable>("mem", 8);
    mem_v2_left->set_ssa_version(2);
    mem_v2_left->set_aliased(true);
    auto* out_l = task.arena().create<Variable>("out_l", 8);
    left->add_instruction(task.arena().create<Assignment>(out_l, mem_v2_left));

    auto* mem_v2_right = task.arena().create<Variable>("mem", 8);
    mem_v2_right->set_ssa_version(2);
    mem_v2_right->set_aliased(true);
    auto* out_r = task.arena().create<Variable>("out_r", 8);
    right->add_instruction(task.arena().create<Assignment>(out_r, mem_v2_right));

    InsertMissingDefinitionsStage stage;
    stage.execute(task);

    ASSERT_EQ(entry->instructions().size(), 3);
    auto* inserted = dynamic_cast<Assignment*>(entry->instructions()[1]);
    ASSERT_TRUE(inserted != nullptr);

    auto* lhs = dynamic_cast<Variable*>(inserted->destination());
    auto* rhs = dynamic_cast<Variable*>(inserted->value());
    ASSERT_TRUE(lhs != nullptr && rhs != nullptr);
    ASSERT_EQ(lhs->name(), "mem");
    ASSERT_EQ(lhs->ssa_version(), 2);
    ASSERT_TRUE(lhs->is_aliased());
    ASSERT_EQ(rhs->name(), "mem");
    ASSERT_EQ(rhs->ssa_version(), 1);
    ASSERT_TRUE(rhs->is_aliased());

    ASSERT_EQ(left->instructions().size(), 1);
    ASSERT_EQ(right->instructions().size(), 1);

    std::cout << "[+] test_insert_missing_definitions_stage passed.\n";
}

void test_phi_function_fixer_stage() {
    DecompilerTask task(0x5300);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* entry = task.arena().create<BasicBlock>(30);
    auto* left = task.arena().create<BasicBlock>(31);
    auto* right = task.arena().create<BasicBlock>(32);
    auto* join = task.arena().create<BasicBlock>(33);
    cfg->set_entry_block(entry);
    cfg->add_block(entry);
    cfg->add_block(left);
    cfg->add_block(right);
    cfg->add_block(join);
    task.set_cfg(std::move(cfg));

    auto* e0 = task.arena().create<Edge>(entry, left, EdgeType::True);
    auto* e1 = task.arena().create<Edge>(entry, right, EdgeType::False);
    auto* e2 = task.arena().create<Edge>(left, join, EdgeType::Unconditional);
    auto* e3 = task.arena().create<Edge>(right, join, EdgeType::Unconditional);
    entry->add_successor(e0);
    entry->add_successor(e1);
    left->add_predecessor(e0);
    right->add_predecessor(e1);
    left->add_successor(e2);
    right->add_successor(e3);
    join->add_predecessor(e2);
    join->add_predecessor(e3);

    auto* x1 = task.arena().create<Variable>("x", 4);
    x1->set_ssa_version(1);
    auto* x2 = task.arena().create<Variable>("x", 4);
    x2->set_ssa_version(2);
    auto* x3 = task.arena().create<Variable>("x", 4);
    x3->set_ssa_version(3);

    left->add_instruction(task.arena().create<Assignment>(x1, task.arena().create<Constant>(1, 4)));
    right->add_instruction(task.arena().create<Assignment>(x2, task.arena().create<Constant>(2, 4)));

    auto* phi_ops = task.arena().create<ListOperation>(std::vector<Expression*>{x1, x2}, 4);
    auto* phi = task.arena().create<Phi>(x3, phi_ops);
    join->add_instruction(phi);

    PhiFunctionFixerStage stage;
    stage.execute(task);

    ASSERT_EQ(phi->origin_block().size(), 2);
    ASSERT_TRUE(phi->origin_block().contains(left));
    ASSERT_TRUE(phi->origin_block().contains(right));
    ASSERT_TRUE(phi->origin_block().at(left) == x1);
    ASSERT_TRUE(phi->origin_block().at(right) == x2);

    std::cout << "[+] test_phi_function_fixer_stage passed.\n";
}

void test_identity_elimination_stage() {
    DecompilerTask task(0x7000);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(40);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* a1 = task.arena().create<Variable>("a", 4); a1->set_ssa_version(1);
    auto* b1 = task.arena().create<Variable>("b", 4); b1->set_ssa_version(1);
    auto* c1 = task.arena().create<Variable>("c", 4); c1->set_ssa_version(1);
    auto* p1 = task.arena().create<Variable>("p", 4); p1->set_ssa_version(1);
    auto* out1 = task.arena().create<Variable>("out", 4); out1->set_ssa_version(1);

    auto* def_a = task.arena().create<Assignment>(a1, task.arena().create<Constant>(10, 4));
    auto* def_b = task.arena().create<Assignment>(b1, a1);
    auto* def_c = task.arena().create<Assignment>(c1, b1);

    auto* phi_ops = task.arena().create<ListOperation>(std::vector<Expression*>{c1, b1});
    auto* phi = task.arena().create<Phi>(p1, phi_ops);

    auto* add = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{p1, task.arena().create<Constant>(1, 4)},
        4);
    auto* use = task.arena().create<Assignment>(out1, add);

    bb->add_instruction(def_a);
    bb->add_instruction(def_b);
    bb->add_instruction(def_c);
    bb->add_instruction(phi);
    bb->add_instruction(use);

    IdentityEliminationStage stage;
    stage.execute(task);

    const auto& insts = bb->instructions();
    ASSERT_TRUE(insts.size() <= 3);

    bool found_out = false;
    for (Instruction* inst : insts) {
        ASSERT_TRUE(dynamic_cast<Phi*>(inst) == nullptr);

        if (auto* asg = dynamic_cast<Assignment*>(inst)) {
            auto* dst = dynamic_cast<Variable*>(asg->destination());
            if (!dst) continue;
            if (dst->name() == "out") {
                auto* op = dynamic_cast<Operation*>(asg->value());
                ASSERT_TRUE(op != nullptr);
                ASSERT_EQ(op->type(), OperationType::add);
                auto* lhs = dynamic_cast<Variable*>(op->operands()[0]);
                ASSERT_TRUE(lhs != nullptr);
                ASSERT_EQ(lhs->name(), "a");
                ASSERT_EQ(lhs->ssa_version(), 1);
                found_out = true;
            }
        }

        std::vector<Variable*> vars = inst->requirements();
        auto defs = inst->definitions();
        vars.insert(vars.end(), defs.begin(), defs.end());
        for (Variable* v : vars) {
            ASSERT_TRUE(v != nullptr);
            ASSERT_TRUE(v->name() != "b");
            ASSERT_TRUE(v->name() != "c");
            ASSERT_TRUE(v->name() != "p");
        }
    }

    ASSERT_TRUE(found_out);

    std::cout << "[+] test_identity_elimination_stage passed.\n";
}

void test_common_subexpression_existing_replacer_stage() {
    DecompilerTask task(0x7100);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* a = task.arena().create<BasicBlock>(50);
    auto* b = task.arena().create<BasicBlock>(51);
    auto* c = task.arena().create<BasicBlock>(52);

    cfg->set_entry_block(a);
    cfg->add_block(a);
    cfg->add_block(b);
    cfg->add_block(c);
    task.set_cfg(std::move(cfg));

    auto* e_ab = task.arena().create<Edge>(a, b, EdgeType::Unconditional);
    auto* e_ac = task.arena().create<Edge>(a, c, EdgeType::Unconditional);
    a->add_successor(e_ab);
    a->add_successor(e_ac);
    b->add_predecessor(e_ab);
    c->add_predecessor(e_ac);

    auto* x = task.arena().create<Variable>("x", 4);
    auto* y = task.arena().create<Variable>("y", 4);
    auto* u = task.arena().create<Variable>("u", 4);
    auto* v = task.arena().create<Variable>("v", 4);

    auto* t = task.arena().create<Variable>("t", 4);
    auto* z = task.arena().create<Variable>("z", 4);
    auto* p = task.arena().create<Variable>("p", 4);
    auto* q = task.arena().create<Variable>("q", 4);

    auto* xy = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{x, y}, 4);
    auto* a_def = task.arena().create<Assignment>(t, xy);
    a->add_instruction(a_def);

    auto* uv_b = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{u, v}, 4);
    auto* b_def = task.arena().create<Assignment>(p, uv_b);
    b->add_instruction(b_def);

    auto* xy_c = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{x, y}, 4);
    auto* c_use_dom = task.arena().create<Assignment>(z, xy_c);
    auto* uv_c = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{u, v}, 4);
    auto* c_use_nondom = task.arena().create<Assignment>(q, uv_c);
    c->add_instruction(c_use_dom);
    c->add_instruction(c_use_nondom);

    CommonSubexpressionEliminationStage stage;
    stage.execute(task);

    const auto& c_insts = c->instructions();
    ASSERT_EQ(c_insts.size(), 2);

    auto* first = dynamic_cast<Assignment*>(c_insts[0]);
    auto* second = dynamic_cast<Assignment*>(c_insts[1]);
    ASSERT_TRUE(first != nullptr && second != nullptr);

    // Dominated expression should be replaced by defining variable t.
    auto* first_rhs_var = dynamic_cast<Variable*>(first->value());
    ASSERT_TRUE(first_rhs_var != nullptr);
    ASSERT_EQ(first_rhs_var->name(), "t");

    // Non-dominated sibling expression must not be replaced by the sibling-def var p.
    if (auto* second_rhs_var = dynamic_cast<Variable*>(second->value())) {
        ASSERT_TRUE(second_rhs_var->name() != "p");
    } else {
        auto* second_rhs_op = dynamic_cast<Operation*>(second->value());
        ASSERT_TRUE(second_rhs_op != nullptr);
        ASSERT_EQ(second_rhs_op->type(), OperationType::add);
        auto* lhs = dynamic_cast<Variable*>(second_rhs_op->operands()[0]);
        auto* rhs = dynamic_cast<Variable*>(second_rhs_op->operands()[1]);
        ASSERT_TRUE(lhs != nullptr && rhs != nullptr);
        ASSERT_EQ(lhs->name(), "u");
        ASSERT_EQ(rhs->name(), "v");
    }

    std::cout << "[+] test_common_subexpression_existing_replacer_stage passed.\n";
}

void test_common_subexpression_definition_generator_stage() {
    DecompilerTask task(0x7200);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(60);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    auto* y = task.arena().create<Variable>("y", 4);

    auto* sum1 = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{x, y}, 4);
    auto* mul1 = task.arena().create<Operation>(OperationType::mul, std::vector<Expression*>{sum1, task.arena().create<Constant>(2, 4)}, 4);
    auto* a = task.arena().create<Variable>("a", 4);
    auto* inst1 = task.arena().create<Assignment>(a, mul1);

    auto* sum2 = task.arena().create<Operation>(OperationType::add, std::vector<Expression*>{x, y}, 4);
    auto* mul2 = task.arena().create<Operation>(OperationType::mul, std::vector<Expression*>{sum2, task.arena().create<Constant>(3, 4)}, 4);
    auto* b = task.arena().create<Variable>("b", 4);
    auto* inst2 = task.arena().create<Assignment>(b, mul2);

    bb->add_instruction(inst1);
    bb->add_instruction(inst2);

    CommonSubexpressionEliminationStage stage;
    stage.execute(task);

    const auto& insts = bb->instructions();
    ASSERT_EQ(insts.size(), 3);

    auto* def = dynamic_cast<Assignment*>(insts[0]);
    ASSERT_TRUE(def != nullptr);
    auto* def_var = dynamic_cast<Variable*>(def->destination());
    auto* def_val = dynamic_cast<Operation*>(def->value());
    ASSERT_TRUE(def_var != nullptr);
    ASSERT_TRUE(def_val != nullptr);
    ASSERT_TRUE(def_var->name().starts_with("cse_"));
    ASSERT_EQ(def_val->type(), OperationType::add);

    auto* use1 = dynamic_cast<Assignment*>(insts[1]);
    auto* use2 = dynamic_cast<Assignment*>(insts[2]);
    ASSERT_TRUE(use1 != nullptr && use2 != nullptr);

    auto* use1_rhs = dynamic_cast<Operation*>(use1->value());
    auto* use2_rhs = dynamic_cast<Operation*>(use2->value());
    ASSERT_TRUE(use1_rhs != nullptr && use2_rhs != nullptr);
    ASSERT_EQ(use1_rhs->type(), OperationType::mul);
    ASSERT_EQ(use2_rhs->type(), OperationType::mul);

    auto* use1_lhs = dynamic_cast<Variable*>(use1_rhs->operands()[0]);
    auto* use2_lhs = dynamic_cast<Variable*>(use2_rhs->operands()[0]);
    ASSERT_TRUE(use1_lhs != nullptr);
    ASSERT_TRUE(use2_lhs != nullptr);
    ASSERT_EQ(use1_lhs->name(), def_var->name());
    ASSERT_EQ(use2_lhs->name(), def_var->name());

    std::cout << "[+] test_common_subexpression_definition_generator_stage passed.\n";
}

void test_expression_simplification_collapse_constants() {
    DecompilerTask task(0x7300);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(70);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    auto* flag = task.arena().create<Variable>("flag", 1);

    // x = (2 + 3) * 4  -> 20
    auto* add = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{task.arena().create<Constant>(2, 4), task.arena().create<Constant>(3, 4)},
        4);
    auto* mul = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{add, task.arena().create<Constant>(4, 4)},
        4);
    auto* assign_x = task.arena().create<Assignment>(x, mul);

    // flag = (10 > 7) -> 1
    auto* cmp = task.arena().create<Condition>(
        OperationType::gt,
        task.arena().create<Constant>(10, 4),
        task.arena().create<Constant>(7, 4),
        1);
    auto* assign_flag = task.arena().create<Assignment>(flag, cmp);

    // if (5 <= 4) branch condition should fold into neq(0, 0)
    auto* br_cond = task.arena().create<Condition>(
        OperationType::le,
        task.arena().create<Constant>(5, 4),
        task.arena().create<Constant>(4, 4),
        1);
    auto* br = task.arena().create<Branch>(br_cond);

    bb->add_instruction(assign_x);
    bb->add_instruction(assign_flag);
    bb->add_instruction(br);

    ExpressionSimplificationStage stage;
    stage.execute(task);

    auto* x_val = dynamic_cast<Constant*>(assign_x->value());
    ASSERT_TRUE(x_val != nullptr);
    ASSERT_EQ(x_val->value(), 20);

    auto* flag_val = dynamic_cast<Constant*>(assign_flag->value());
    ASSERT_TRUE(flag_val != nullptr);
    ASSERT_EQ(flag_val->value(), 1);

    auto* folded_cond = br->condition();
    ASSERT_TRUE(folded_cond != nullptr);
    ASSERT_EQ(folded_cond->type(), OperationType::neq);
    auto* lhs = dynamic_cast<Constant*>(folded_cond->lhs());
    auto* rhs = dynamic_cast<Constant*>(folded_cond->rhs());
    ASSERT_TRUE(lhs != nullptr && rhs != nullptr);
    ASSERT_EQ(lhs->value(), 0);
    ASSERT_EQ(rhs->value(), 0);

    std::cout << "[+] test_expression_simplification_collapse_constants passed.\n";
}

void test_expression_simplification_trivial_arithmetic() {
    DecompilerTask task(0x7400);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(71);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    auto* y = task.arena().create<Variable>("y", 4);
    auto* z = task.arena().create<Variable>("z", 4);
    auto* q = task.arena().create<Variable>("q", 4);
    auto* r = task.arena().create<Variable>("r", 4);

    auto* add_zero = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{x, task.arena().create<Constant>(0, 4)},
        4);
    auto* mul_one = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{y, task.arena().create<Constant>(1, 4)},
        4);
    auto* mul_zero = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{z, task.arena().create<Constant>(0, 4)},
        4);
    auto* sub_zero = task.arena().create<Operation>(
        OperationType::sub,
        std::vector<Expression*>{q, task.arena().create<Constant>(0, 4)},
        4);
    auto* div_one = task.arena().create<Operation>(
        OperationType::div,
        std::vector<Expression*>{r, task.arena().create<Constant>(1, 4)},
        4);

    auto* out_a = task.arena().create<Variable>("out_a", 4);
    auto* out_b = task.arena().create<Variable>("out_b", 4);
    auto* out_c = task.arena().create<Variable>("out_c", 4);
    auto* out_d = task.arena().create<Variable>("out_d", 4);
    auto* out_e = task.arena().create<Variable>("out_e", 4);

    auto* a1 = task.arena().create<Assignment>(out_a, add_zero);
    auto* a2 = task.arena().create<Assignment>(out_b, mul_one);
    auto* a3 = task.arena().create<Assignment>(out_c, mul_zero);
    auto* a4 = task.arena().create<Assignment>(out_d, sub_zero);
    auto* a5 = task.arena().create<Assignment>(out_e, div_one);

    bb->add_instruction(a1);
    bb->add_instruction(a2);
    bb->add_instruction(a3);
    bb->add_instruction(a4);
    bb->add_instruction(a5);

    ExpressionSimplificationStage stage;
    stage.execute(task);

    ASSERT_TRUE(a1->value() == x);
    ASSERT_TRUE(a2->value() == y);

    auto* folded_zero = dynamic_cast<Constant*>(a3->value());
    ASSERT_TRUE(folded_zero != nullptr);
    ASSERT_EQ(folded_zero->value(), 0);

    ASSERT_TRUE(a4->value() == q);
    ASSERT_TRUE(a5->value() == r);

    std::cout << "[+] test_expression_simplification_trivial_arithmetic passed.\n";
}

void test_expression_simplification_trivial_bit_arithmetic() {
    DecompilerTask task(0x7500);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(72);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* a = task.arena().create<Variable>("a", 4);
    auto* b = task.arena().create<Variable>("b", 4);
    auto* c = task.arena().create<Variable>("c", 4);
    auto* d = task.arena().create<Variable>("d", 4);

    auto* and_zero = task.arena().create<Operation>(
        OperationType::bit_and,
        std::vector<Expression*>{a, task.arena().create<Constant>(0, 4)},
        4);
    auto* or_zero = task.arena().create<Operation>(
        OperationType::bit_or,
        std::vector<Expression*>{b, task.arena().create<Constant>(0, 4)},
        4);
    auto* xor_zero = task.arena().create<Operation>(
        OperationType::bit_xor,
        std::vector<Expression*>{task.arena().create<Constant>(0, 4), c},
        4);
    auto* and_all_ones = task.arena().create<Operation>(
        OperationType::bit_and,
        std::vector<Expression*>{task.arena().create<Constant>(0xffffffffULL, 4), d},
        4);

    auto* out_a = task.arena().create<Variable>("bit_out_a", 4);
    auto* out_b = task.arena().create<Variable>("bit_out_b", 4);
    auto* out_c = task.arena().create<Variable>("bit_out_c", 4);
    auto* out_d = task.arena().create<Variable>("bit_out_d", 4);

    auto* i1 = task.arena().create<Assignment>(out_a, and_zero);
    auto* i2 = task.arena().create<Assignment>(out_b, or_zero);
    auto* i3 = task.arena().create<Assignment>(out_c, xor_zero);
    auto* i4 = task.arena().create<Assignment>(out_d, and_all_ones);

    bb->add_instruction(i1);
    bb->add_instruction(i2);
    bb->add_instruction(i3);
    bb->add_instruction(i4);

    ExpressionSimplificationStage stage;
    stage.execute(task);

    auto* and_folded = dynamic_cast<Constant*>(i1->value());
    ASSERT_TRUE(and_folded != nullptr);
    ASSERT_EQ(and_folded->value(), 0);

    ASSERT_TRUE(i2->value() == b);
    ASSERT_TRUE(i3->value() == c);
    ASSERT_TRUE(i4->value() == d);

    std::cout << "[+] test_expression_simplification_trivial_bit_arithmetic passed.\n";
}

void test_expression_simplification_sub_to_add() {
    DecompilerTask task(0x7600);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(73);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    auto* out = task.arena().create<Variable>("out", 4);

    auto* neg_const = task.arena().create<Operation>(
        OperationType::negate,
        std::vector<Expression*>{task.arena().create<Constant>(7, 4)},
        4);
    auto* sub_expr = task.arena().create<Operation>(
        OperationType::sub,
        std::vector<Expression*>{x, neg_const},
        4);
    auto* assign = task.arena().create<Assignment>(out, sub_expr);
    bb->add_instruction(assign);

    ExpressionSimplificationStage stage;
    stage.execute(task);

    auto* simplified = dynamic_cast<Operation*>(assign->value());
    ASSERT_TRUE(simplified != nullptr);
    ASSERT_EQ(simplified->type(), OperationType::add);
    ASSERT_TRUE(simplified->operands()[0] == x);

    auto* rhs = dynamic_cast<Constant*>(simplified->operands()[1]);
    ASSERT_TRUE(rhs != nullptr);
    ASSERT_EQ(rhs->value(), 7);

    std::cout << "[+] test_expression_simplification_sub_to_add passed.\n";
}

void test_expression_simplification_collapse_nested_constants() {
    DecompilerTask task(0x7700);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(74);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    auto* y = task.arena().create<Variable>("y", 4);
    auto* out_add = task.arena().create<Variable>("out_add", 4);
    auto* out_mul = task.arena().create<Variable>("out_mul", 4);

    auto* inner_add = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{x, task.arena().create<Constant>(3, 4)},
        4);
    auto* nested_add = task.arena().create<Operation>(
        OperationType::add,
        std::vector<Expression*>{inner_add, task.arena().create<Constant>(5, 4)},
        4);

    auto* inner_mul = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{task.arena().create<Constant>(4, 4), y},
        4);
    auto* nested_mul = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{task.arena().create<Constant>(8, 4), inner_mul},
        4);

    auto* add_assign = task.arena().create<Assignment>(out_add, nested_add);
    auto* mul_assign = task.arena().create<Assignment>(out_mul, nested_mul);

    bb->add_instruction(add_assign);
    bb->add_instruction(mul_assign);

    ExpressionSimplificationStage stage;
    stage.execute(task);

    auto* add_value = dynamic_cast<Operation*>(add_assign->value());
    ASSERT_TRUE(add_value != nullptr);
    ASSERT_EQ(add_value->type(), OperationType::add);
    ASSERT_TRUE(add_value->operands()[0] == x);
    auto* add_const = dynamic_cast<Constant*>(add_value->operands()[1]);
    ASSERT_TRUE(add_const != nullptr);
    ASSERT_EQ(add_const->value(), 8);

    auto* mul_value = dynamic_cast<Operation*>(mul_assign->value());
    ASSERT_TRUE(mul_value != nullptr);
    ASSERT_EQ(mul_value->type(), OperationType::mul);
    ASSERT_TRUE(mul_value->operands()[0] == y);
    auto* mul_const = dynamic_cast<Constant*>(mul_value->operands()[1]);
    ASSERT_TRUE(mul_const != nullptr);
    ASSERT_EQ(mul_const->value(), 32);

    std::cout << "[+] test_expression_simplification_collapse_nested_constants passed.\n";
}

void test_dead_component_pruner_stage() {
    DecompilerTask task(0x7800);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(75);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* a = task.arena().create<Variable>("a", 4);
    auto* b = task.arena().create<Variable>("b", 4);
    auto* t1 = task.arena().create<Variable>("t1", 4);
    auto* t2 = task.arena().create<Variable>("t2", 4);
    auto* dead1 = task.arena().create<Variable>("dead1", 4);
    auto* dead2 = task.arena().create<Variable>("dead2", 4);
    auto* out = task.arena().create<Variable>("out", 4);

    auto* i1 = task.arena().create<Assignment>(
        t1,
        task.arena().create<Operation>(
            OperationType::add,
            std::vector<Expression*>{a, task.arena().create<Constant>(1, 4)},
            4));
    auto* i2 = task.arena().create<Assignment>(
        t2,
        task.arena().create<Operation>(
            OperationType::mul,
            std::vector<Expression*>{t1, task.arena().create<Constant>(2, 4)},
            4));

    auto* d1 = task.arena().create<Assignment>(
        dead1,
        task.arena().create<Operation>(
            OperationType::add,
            std::vector<Expression*>{b, task.arena().create<Constant>(3, 4)},
            4));
    auto* d2 = task.arena().create<Assignment>(
        dead2,
        task.arena().create<Operation>(
            OperationType::mul,
            std::vector<Expression*>{dead1, task.arena().create<Constant>(4, 4)},
            4));
    (void)d2;

    auto* i3 = task.arena().create<Assignment>(
        out,
        task.arena().create<Operation>(
            OperationType::add,
            std::vector<Expression*>{t2, task.arena().create<Constant>(5, 4)},
            4));
    auto* ret = task.arena().create<Return>(std::vector<Expression*>{out});

    bb->add_instruction(i1);
    bb->add_instruction(i2);
    bb->add_instruction(d1);
    bb->add_instruction(d2);
    bb->add_instruction(i3);
    bb->add_instruction(ret);

    DeadComponentPrunerStage stage;
    stage.execute(task);

    const auto& insts = bb->instructions();
    ASSERT_EQ(insts.size(), 4);
    ASSERT_TRUE(insts[0] == i1);
    ASSERT_TRUE(insts[1] == i2);
    ASSERT_TRUE(insts[2] == i3);
    ASSERT_TRUE(insts[3] == ret);

    std::cout << "[+] test_dead_component_pruner_stage passed.\n";
}

void test_edge_pruner_stage() {
    DecompilerTask task(0x7c00);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(79);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    auto* y = task.arena().create<Variable>("y", 4);
    auto* z = task.arena().create<Variable>("z", 4);
    auto* a = task.arena().create<Variable>("a", 4);
    auto* b = task.arena().create<Variable>("b", 4);

    auto* expr1 = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{
            task.arena().create<Operation>(
                OperationType::add,
                std::vector<Expression*>{x, y},
                4),
            z},
        4);
    auto* expr2 = task.arena().create<Operation>(
        OperationType::mul,
        std::vector<Expression*>{
            task.arena().create<Operation>(
                OperationType::add,
                std::vector<Expression*>{x, y},
                4),
            z},
        4);

    auto* i1 = task.arena().create<Assignment>(a, expr1);
    auto* i2 = task.arena().create<Assignment>(b, expr2);
    auto* ret = task.arena().create<Return>(std::vector<Expression*>{b});
    bb->add_instruction(i1);
    bb->add_instruction(i2);
    bb->add_instruction(ret);

    EdgePrunerStage stage;
    stage.execute(task);

    const auto& insts = bb->instructions();
    ASSERT_EQ(insts.size(), 4);

    auto* tmp_def = dynamic_cast<Assignment*>(insts[0]);
    ASSERT_TRUE(tmp_def != nullptr);
    auto* tmp_var = dynamic_cast<Variable*>(tmp_def->destination());
    ASSERT_TRUE(tmp_var != nullptr);
    ASSERT_TRUE(tmp_var->name().starts_with("edge_"));

    auto* a_assign = dynamic_cast<Assignment*>(insts[1]);
    auto* b_assign = dynamic_cast<Assignment*>(insts[2]);
    ASSERT_TRUE(a_assign != nullptr);
    ASSERT_TRUE(b_assign != nullptr);

    auto* a_value = dynamic_cast<Variable*>(a_assign->value());
    auto* b_value = dynamic_cast<Variable*>(b_assign->value());
    ASSERT_TRUE(a_value != nullptr);
    ASSERT_TRUE(b_value != nullptr);
    ASSERT_EQ(a_value->name(), tmp_var->name());
    ASSERT_EQ(b_value->name(), tmp_var->name());

    std::cout << "[+] test_edge_pruner_stage passed.\n";
}

void test_bitfield_comparison_unrolling_stage() {
    DecompilerTask task(0x7d00);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(81);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* amount = task.arena().create<Variable>("amount", 4);

    auto make_mask_test = [&](std::uint64_t mask) -> Expression* {
        auto* shift = task.arena().create<Operation>(
            OperationType::shl,
            std::vector<Expression*>{task.arena().create<Constant>(1, 4), amount->copy(task.arena())},
            4);
        return task.arena().create<Operation>(
            OperationType::bit_and,
            std::vector<Expression*>{shift, task.arena().create<Constant>(mask, 1)},
            1);
    };

    auto* neq_branch = task.arena().create<Branch>(
        task.arena().create<Condition>(
            OperationType::neq,
            make_mask_test(0x0A),
            task.arena().create<Constant>(0, 1),
            1));
    auto* eq_branch = task.arena().create<Branch>(
        task.arena().create<Condition>(
            OperationType::eq,
            make_mask_test(0x05),
            task.arena().create<Constant>(0, 1),
            1));

    bb->add_instruction(neq_branch);
    bb->add_instruction(eq_branch);

    BitFieldComparisonUnrollingStage stage;
    stage.execute(task);

    auto collect_eq_constants = [&](auto&& self, Expression* expr, std::vector<std::uint64_t>& out) -> bool {
        auto* stripped = expr;
        while (auto* cast_op = dynamic_cast<Operation*>(stripped)) {
            if (cast_op->type() != OperationType::cast || cast_op->operands().size() != 1) {
                break;
            }
            stripped = cast_op->operands()[0];
        }

        if (auto* cond = dynamic_cast<Condition*>(stripped)) {
            if (cond->type() != OperationType::eq) {
                return false;
            }
            auto* lhs_var = dynamic_cast<Variable*>(cond->lhs());
            auto* rhs_const = dynamic_cast<Constant*>(cond->rhs());
            if (!lhs_var || !rhs_const || lhs_var->name() != "amount") {
                return false;
            }
            out.push_back(rhs_const->value());
            return true;
        }

        auto* op = dynamic_cast<Operation*>(stripped);
        if (!op || op->type() != OperationType::logical_or || op->operands().size() != 2) {
            return false;
        }
        return self(self, op->operands()[0], out) && self(self, op->operands()[1], out);
    };

    auto verify_branch = [&](Branch* branch, OperationType expected_cmp, std::vector<std::uint64_t> expected_bits) {
        auto* cond = branch->condition();
        ASSERT_TRUE(cond != nullptr);
        ASSERT_EQ(cond->type(), expected_cmp);

        auto* zero = dynamic_cast<Constant*>(cond->rhs());
        ASSERT_TRUE(zero != nullptr);
        ASSERT_EQ(zero->value(), 0);

        std::vector<std::uint64_t> extracted;
        ASSERT_TRUE(collect_eq_constants(collect_eq_constants, cond->lhs(), extracted));
        std::sort(extracted.begin(), extracted.end());
        std::sort(expected_bits.begin(), expected_bits.end());
        ASSERT_EQ(extracted.size(), expected_bits.size());
        for (std::size_t i = 0; i < extracted.size(); ++i) {
            ASSERT_EQ(extracted[i], expected_bits[i]);
        }
    };

    verify_branch(neq_branch, OperationType::neq, {1, 3});
    verify_branch(eq_branch, OperationType::eq, {0, 2});

    std::cout << "[+] test_bitfield_comparison_unrolling_stage passed.\n";
}

void test_redundant_casts_elimination_stage() {
    DecompilerTask task(0x7900);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(76);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x = task.arena().create<Variable>("x", 4);
    x->set_ir_type(Integer::int32_t());
    auto* out1 = task.arena().create<Variable>("cast_out_1", 4);
    auto* out2 = task.arena().create<Variable>("cast_out_2", 1);

    auto* same_cast = task.arena().create<Operation>(
        OperationType::cast,
        std::vector<Expression*>{x},
        4);
    same_cast->set_ir_type(Integer::int32_t());

    auto* const_src = task.arena().create<Constant>(0x1234, 4);
    const_src->set_ir_type(Integer::uint32_t());
    auto* const_cast_expr = task.arena().create<Operation>(
        OperationType::cast,
        std::vector<Expression*>{const_src},
        1);
    const_cast_expr->set_ir_type(Integer::uint8_t());

    auto* a1 = task.arena().create<Assignment>(out1, same_cast);
    auto* a2 = task.arena().create<Assignment>(out2, const_cast_expr);
    bb->add_instruction(a1);
    bb->add_instruction(a2);

    RedundantCastsEliminationStage stage;
    stage.execute(task);

    ASSERT_TRUE(a1->value() == x);
    auto* folded = dynamic_cast<Constant*>(a2->value());
    ASSERT_TRUE(folded != nullptr);
    ASSERT_EQ(folded->value(), 0x34);
    ASSERT_EQ(folded->size_bytes, 1);

    std::cout << "[+] test_redundant_casts_elimination_stage passed.\n";
}

void test_coherence_stage() {
    DecompilerTask task(0x7a00);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(77);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto* x_typed = task.arena().create<Variable>("x", 4);
    x_typed->set_ir_type(Integer::int32_t());

    auto* x_mismatch = task.arena().create<Variable>("x", 4);
    x_mismatch->set_ir_type(Integer::uint32_t());

    auto* x_untyped = task.arena().create<Variable>("x", 4);

    auto* out1 = task.arena().create<Variable>("coh1", 4);
    auto* out2 = task.arena().create<Variable>("coh2", 4);
    auto* out3 = task.arena().create<Variable>("coh3", 4);

    bb->add_instruction(task.arena().create<Assignment>(out1, x_typed));
    bb->add_instruction(task.arena().create<Assignment>(
        out2,
        task.arena().create<Operation>(
            OperationType::add,
            std::vector<Expression*>{x_mismatch, task.arena().create<Constant>(1, 4)},
            4)));
    bb->add_instruction(task.arena().create<Assignment>(
        out3,
        task.arena().create<Operation>(
            OperationType::add,
            std::vector<Expression*>{x_untyped, task.arena().create<Constant>(2, 4)},
            4)));

    CoherenceStage stage;
    stage.execute(task);

    ASSERT_TRUE(x_mismatch->ir_type() != nullptr);
    ASSERT_TRUE(x_untyped->ir_type() != nullptr);
    ASSERT_TRUE(*(x_mismatch->ir_type()) == *(Integer::int32_t()));
    ASSERT_TRUE(*(x_untyped->ir_type()) == *(Integer::int32_t()));

    std::cout << "[+] test_coherence_stage passed.\n";
}

void test_type_propagation_stage() {
    DecompilerTask task(0x7e00);

    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb = task.arena().create<BasicBlock>(82);
    cfg->set_entry_block(bb);
    cfg->add_block(bb);
    task.set_cfg(std::move(cfg));

    auto ptr_widget_type = std::make_shared<const Pointer>(
        std::make_shared<const CustomType>("Widget", 64),
        64);

    auto* v0 = task.arena().create<Variable>("v0", 8);
    auto* v1 = task.arena().create<Variable>("v1", 8);
    auto* v2 = task.arena().create<Variable>("v2", 8);
    auto* v3 = task.arena().create<Variable>("v3", 8);
    auto* prim_src = task.arena().create<Variable>("prim_src", 4);
    auto* prim_dst = task.arena().create<Variable>("prim_dst", 4);

    v1->set_ir_type(ptr_widget_type);
    v2->set_ir_type(Integer::int32_t());
    prim_src->set_ir_type(Integer::int32_t());

    bb->add_instruction(task.arena().create<Assignment>(v0, v1));
    bb->add_instruction(task.arena().create<Assignment>(v2, v0));
    bb->add_instruction(task.arena().create<Assignment>(v3, v2));
    bb->add_instruction(task.arena().create<Assignment>(prim_dst, prim_src));

    TypePropagationStage stage;
    stage.execute(task);

    ASSERT_TRUE(v0->ir_type() != nullptr);
    ASSERT_TRUE(v1->ir_type() != nullptr);
    ASSERT_TRUE(v2->ir_type() != nullptr);
    ASSERT_TRUE(v3->ir_type() != nullptr);
    ASSERT_TRUE(*(v0->ir_type()) == *ptr_widget_type);
    ASSERT_TRUE(*(v1->ir_type()) == *ptr_widget_type);
    ASSERT_TRUE(*(v2->ir_type()) == *ptr_widget_type);
    ASSERT_TRUE(*(v3->ir_type()) == *ptr_widget_type);

    ASSERT_TRUE(prim_dst->ir_type() == nullptr);

    std::cout << "[+] test_type_propagation_stage passed.\n";
}

int main() {
    test_codegen_dump();
    test_codegen_switch_case();
    test_codegen_loop_variants();
    test_codegen_operator_precedence();
    test_codegen_compound_assignment_increment();
    test_codegen_constant_formatting();
    test_codegen_unknown_operation_placeholder();
    test_codegen_fallback_branch_marker();
    test_codegen_if_branch_swapping();
    test_array_access_detection_stage();
    test_global_variable_declarations();
    test_variable_name_generation_default();
    test_variable_name_generation_system_hungarian();
    test_variable_name_generation_skips_globals();
    test_loop_name_generator_for_counters();
    test_loop_name_generator_while_counters();
    test_instruction_length_handler();
    test_condition_handler();
    test_transition_cfg_switch_tags();
    test_cbr_complementary_conditions();
    test_cbr_cnf_subexpression_grouping();
    test_car_initial_switch_constructor();
    test_car_switch_extractor_and_missing_case_sequence();
    test_car_missing_case_finder_condition();
    test_guarded_do_while_rewrite();
    test_while_loop_replacer();
    test_sibling_reachability();
    test_case_dependency_graph_ordering();
    test_phi_dependency();
    std::cout << "Running Aletheia tests...\n";
    test_arena();
    test_pipeline_stage_tracking();
    test_dominators();
    test_ssa_phi_insertion();
    test_phi_lifting_edge_splitting();
    test_instruction_hierarchy();
    test_minimal_variable_renamer();
    test_conditional_variable_renamer();
    test_out_of_ssa_mode_config();
    test_compiler_idiom_handling_stage();
    test_register_pair_handling_stage();
    test_switch_variable_detection_stage();
    test_mem_phi_converter_stage();
    test_remove_go_prologue_stage();
    test_remove_stack_canary_stage();
    test_remove_noreturn_boilerplate_stage();
    test_insert_missing_definitions_stage();
    test_phi_function_fixer_stage();
    test_identity_elimination_stage();
    test_common_subexpression_existing_replacer_stage();
    test_common_subexpression_definition_generator_stage();
    test_edge_pruner_stage();
    test_bitfield_comparison_unrolling_stage();
    test_expression_simplification_collapse_constants();
    test_expression_simplification_trivial_arithmetic();
    test_expression_simplification_trivial_bit_arithmetic();
    test_expression_simplification_sub_to_add();
    test_expression_simplification_collapse_nested_constants();
    test_dead_component_pruner_stage();
    test_redundant_casts_elimination_stage();
    test_coherence_stage();
    test_type_propagation_stage();
    test_type_system();
    test_loop_structurer();
    test_range_simplifier();
    test_logic_operation_simplifier();
    test_logic_normal_form_conversion();
    test_world_map_condition_no_simplification();
    test_range_simplifier_legacy_dag_path();
    test_z3_converter_no_hard_false_fallback();
    std::cout << "All tests passed successfully.\n";
    return 0;
}
