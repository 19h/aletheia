#include <iostream>
#include <cstdlib>
#include <cctype>
#include <string>
#include <sstream>
#include <unordered_set>
#include <vector>
#include "../src/common/arena.hpp"
#include "../src/aletheia/structures/cfg.hpp"
#include "../src/aletheia/structures/dataflow.hpp"
#include "../src/aletheia/structuring/ast.hpp"
#include "../src/aletheia/pipeline/pipeline.hpp"

#include "../src/aletheia/debug/ir_serializer.hpp"
#include "../src/aletheia/debug/expr_tree.hpp"
#include "../src/aletheia/debug/stage_metrics.hpp"
#include "../src/aletheia/debug/ir_diff.hpp"
#include "../src/aletheia/debug/ir_invariants.hpp"
#include "../src/aletheia/debug/variable_provenance.hpp"
#include "../src/aletheia/debug/ir_query.hpp"
#include "../src/aletheia/debug/debug_observer.hpp"

#define ASSERT_TRUE(cond) if (!(cond)) { \
    std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    std::exit(1); \
}
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_CONTAINS(str, substr) ASSERT_TRUE((str).find(substr) != std::string::npos)

using namespace aletheia;
using namespace aletheia::debug;

// ============================================================================
// Phase 2: IR Serializer Tests
// ============================================================================

void test_ir_serializer_constant() {
    DecompilerArena arena;
    auto* c = arena.create<Constant>(0x42, 4);
    auto s = ir_to_string(static_cast<const Expression*>(c));
    ASSERT_CONTAINS(s, "Constant(0x42 [i32])");
    std::cout << "[+] test_ir_serializer_constant passed.\n";
}

void test_ir_serializer_variable() {
    DecompilerArena arena;
    auto* v = arena.create<Variable>("rdi", 8);
    v->set_ssa_version(3);
    auto s = ir_to_string(static_cast<const Expression*>(v));
    ASSERT_CONTAINS(s, "var:rdi_3 [i64]");
    std::cout << "[+] test_ir_serializer_variable passed.\n";
}

void test_ir_serializer_global_variable() {
    DecompilerArena arena;
    auto* gv = arena.create<GlobalVariable>("_printf", 8);
    auto s = ir_to_string(static_cast<const Expression*>(gv));
    ASSERT_CONTAINS(s, "GlobalVar:_printf [i64]");
    std::cout << "[+] test_ir_serializer_global_variable passed.\n";
}

void test_ir_serializer_operation_all_types() {
    DecompilerArena arena;

    // Test a sampling of operation types
    OperationType types[] = {
        OperationType::add, OperationType::sub, OperationType::mul,
        OperationType::deref, OperationType::ternary, OperationType::shl,
        OperationType::bit_and, OperationType::negate, OperationType::cast,
        OperationType::eq, OperationType::unknown
    };

    for (auto t : types) {
        auto* a = arena.create<Variable>("x", 4);
        auto* b = arena.create<Constant>(1, 4);
        SmallVector<Expression*, 4> ops{a, b};
        auto* op = arena.create<Operation>(t, std::move(ops), 4);
        auto s = ir_to_string(static_cast<const Expression*>(op));
        ASSERT_CONTAINS(s, operation_type_name(t));
    }

    // All 53 operation types have names
    for (int i = 0; i <= static_cast<int>(OperationType::unknown); ++i) {
        auto name = operation_type_name(static_cast<OperationType>(i));
        ASSERT_TRUE(name != nullptr);
        ASSERT_TRUE(std::string(name).length() > 0);
    }

    std::cout << "[+] test_ir_serializer_operation_all_types passed.\n";
}

void test_ir_serializer_call() {
    DecompilerArena arena;
    auto* target = arena.create<GlobalVariable>("_printf", 8);
    std::vector<Expression*> args{arena.create<Variable>("x0", 8)};
    auto* call = arena.create<Call>(target, std::move(args), 8);
    auto s = ir_to_string(static_cast<const Expression*>(call));
    ASSERT_CONTAINS(s, "Call(");
    ASSERT_CONTAINS(s, "GlobalVar:_printf");
    ASSERT_CONTAINS(s, "var:x0_0");
    std::cout << "[+] test_ir_serializer_call passed.\n";
}

void test_ir_serializer_condition() {
    DecompilerArena arena;
    auto* lhs = arena.create<Variable>("eax", 4);
    auto* rhs = arena.create<Constant>(0, 4);
    auto* cond = arena.create<Condition>(OperationType::eq, lhs, rhs);
    auto s = ir_to_string(static_cast<const Expression*>(cond));
    ASSERT_CONTAINS(s, "Condition(eq,");
    ASSERT_CONTAINS(s, "var:eax_0");
    ASSERT_CONTAINS(s, "Constant(0x0");
    std::cout << "[+] test_ir_serializer_condition passed.\n";
}

void test_ir_serializer_list_operation() {
    DecompilerArena arena;
    auto* a = arena.create<Variable>("x", 4);
    auto* b = arena.create<Variable>("y", 4);
    SmallVector<Expression*, 4> ops{a, b};
    auto* lo = arena.create<ListOperation>(std::move(ops));
    auto s = ir_to_string(static_cast<const Expression*>(lo));
    ASSERT_CONTAINS(s, "List(");
    ASSERT_CONTAINS(s, "var:x_0");
    ASSERT_CONTAINS(s, "var:y_0");
    std::cout << "[+] test_ir_serializer_list_operation passed.\n";
}

void test_ir_serializer_assignment() {
    DecompilerArena arena;
    auto* lhs = arena.create<Variable>("eax", 4);
    auto* rhs_a = arena.create<Variable>("ebx", 4);
    auto* rhs_b = arena.create<Constant>(0x42, 4);
    SmallVector<Expression*, 4> ops{rhs_a, rhs_b};
    auto* add_op = arena.create<Operation>(OperationType::add, std::move(ops), 4);
    auto* assign = arena.create<Assignment>(lhs, add_op);
    assign->set_address(0x1000);
    auto s = ir_to_string(static_cast<const Instruction*>(assign));
    ASSERT_CONTAINS(s, "Assignment(");
    ASSERT_CONTAINS(s, "[0x1000]");
    ASSERT_CONTAINS(s, "Operation(add,");
    std::cout << "[+] test_ir_serializer_assignment passed.\n";
}

void test_ir_serializer_phi() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1);
    auto* bb2 = arena.create<BasicBlock>(2);
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->add_block(bb2);
    cfg->set_entry_block(bb0);

    auto* dest = arena.create<Variable>("x", 4);
    dest->set_ssa_version(3);
    auto* src1 = arena.create<Variable>("x", 4);
    src1->set_ssa_version(1);
    auto* src2 = arena.create<Variable>("x", 4);
    src2->set_ssa_version(2);
    SmallVector<Expression*, 4> phi_args{src1, src2};
    auto* list = arena.create<ListOperation>(std::move(phi_args));
    auto* phi = arena.create<Phi>(dest, list);
    phi->update_phi_function({{bb0, src1}, {bb1, src2}});

    auto s = ir_to_string(static_cast<const Instruction*>(phi));
    ASSERT_CONTAINS(s, "Phi(");
    ASSERT_CONTAINS(s, "var:x_3");
    ASSERT_CONTAINS(s, "bb_0:");
    ASSERT_CONTAINS(s, "bb_1:");
    std::cout << "[+] test_ir_serializer_phi passed.\n";
}

void test_ir_serializer_phi_no_origin_map() {
    DecompilerArena arena;
    auto* dest = arena.create<Variable>("x", 4);
    dest->set_ssa_version(3);
    auto* src1 = arena.create<Variable>("x", 4);
    src1->set_ssa_version(1);
    auto* src2 = arena.create<Variable>("x", 4);
    src2->set_ssa_version(2);
    SmallVector<Expression*, 4> phi_args{src1, src2};
    auto* list = arena.create<ListOperation>(std::move(phi_args));
    auto* phi = arena.create<Phi>(dest, list);
    // Note: NOT calling update_phi_function

    auto s = ir_to_string(static_cast<const Instruction*>(phi));
    ASSERT_CONTAINS(s, "Phi(");
    ASSERT_CONTAINS(s, "<<NO ORIGIN MAP>>");
    std::cout << "[+] test_ir_serializer_phi_no_origin_map passed.\n";
}

void test_ir_serializer_branch() {
    DecompilerArena arena;
    auto* lhs = arena.create<Variable>("eax", 4);
    auto* rhs = arena.create<Constant>(0, 4);
    auto* cond = arena.create<Condition>(OperationType::eq, lhs, rhs);
    auto* branch = arena.create<Branch>(cond);
    branch->set_address(0x2000);
    auto s = ir_to_string(static_cast<const Instruction*>(branch));
    ASSERT_CONTAINS(s, "Branch(Condition(eq,");
    ASSERT_CONTAINS(s, "[0x2000]");
    // Must NOT contain target= or fallthrough= (Branch doesn't store targets)
    ASSERT_TRUE(s.find("target=") == std::string::npos);
    std::cout << "[+] test_ir_serializer_branch passed.\n";
}

void test_ir_serializer_return() {
    DecompilerArena arena;
    // Return with value
    auto* val = arena.create<Variable>("x0", 8);
    SmallVector<Expression*, 2> vals{val};
    auto* ret = arena.create<Return>(std::move(vals));
    auto s = ir_to_string(static_cast<const Instruction*>(ret));
    ASSERT_CONTAINS(s, "Return(var:x0_0");

    // Void return
    auto* void_ret = arena.create<Return>();
    auto s2 = ir_to_string(static_cast<const Instruction*>(void_ret));
    ASSERT_CONTAINS(s2, "Return(void)");
    std::cout << "[+] test_ir_serializer_return passed.\n";
}

void test_ir_serializer_break_continue() {
    DecompilerArena arena;
    auto* brk = arena.create<BreakInstr>();
    auto* cont = arena.create<ContinueInstr>();
    ASSERT_EQ(ir_to_string(static_cast<const Instruction*>(brk)), "Break");
    ASSERT_EQ(ir_to_string(static_cast<const Instruction*>(cont)), "Continue");
    std::cout << "[+] test_ir_serializer_break_continue passed.\n";
}

void test_ir_serializer_comment() {
    DecompilerArena arena;
    auto* c = arena.create<Comment>("test message");
    auto s = ir_to_string(static_cast<const Instruction*>(c));
    ASSERT_CONTAINS(s, "Comment(\"test message\")");
    std::cout << "[+] test_ir_serializer_comment passed.\n";
}

void test_ir_serializer_relation() {
    DecompilerArena arena;
    auto* dst = arena.create<Variable>("mem", 8);
    dst->set_ssa_version(3);
    auto* val = arena.create<Variable>("mem", 8);
    val->set_ssa_version(2);
    auto* rel = arena.create<Relation>(dst, val);
    auto s = ir_to_string(static_cast<const Instruction*>(rel));
    ASSERT_CONTAINS(s, "Relation(");
    ASSERT_CONTAINS(s, " -> ");
    ASSERT_CONTAINS(s, "var:mem_3");
    ASSERT_CONTAINS(s, "var:mem_2");
    std::cout << "[+] test_ir_serializer_relation passed.\n";
}

void test_ir_serializer_null_handling() {
    ASSERT_EQ(ir_to_string(static_cast<const Expression*>(nullptr)), "<null>");
    ASSERT_EQ(ir_to_string(static_cast<const Instruction*>(nullptr)), "<null>");
    std::cout << "[+] test_ir_serializer_null_handling passed.\n";
}

void test_cfg_to_string() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1);
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->set_entry_block(bb0);
    auto* edge = arena.create<Edge>(bb0, bb1, EdgeType::Unconditional);
    bb0->add_successor(edge);
    bb1->add_predecessor(edge);

    auto* assign = arena.create<Assignment>(
        arena.create<Variable>("eax", 4),
        arena.create<Constant>(1, 4));
    bb0->add_instruction(assign);

    auto s = cfg_to_string(cfg.get());
    ASSERT_CONTAINS(s, "bb_0 [entry]");
    ASSERT_CONTAINS(s, "bb_1(Unconditional)");
    ASSERT_CONTAINS(s, "Assignment(");
    std::cout << "[+] test_cfg_to_string passed.\n";
}

void test_ast_to_string() {
    DecompilerArena arena;
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* assign = arena.create<Assignment>(
        arena.create<Variable>("x", 4),
        arena.create<Constant>(1, 4));
    bb0->add_instruction(assign);

    auto* code = arena.create<CodeNode>(bb0);
    auto* seq = arena.create<SeqNode>();
    seq->add_node(code);

    auto ast = std::make_unique<AbstractSyntaxForest>();
    ast->set_root(seq);

    auto s = ast_to_string(ast.get());
    ASSERT_CONTAINS(s, "SeqNode:");
    ASSERT_CONTAINS(s, "CodeNode(bb_0):");
    ASSERT_CONTAINS(s, "Assignment(");
    std::cout << "[+] test_ast_to_string passed.\n";
}

void test_variable_info() {
    DecompilerArena arena;
    auto* v = arena.create<Variable>("rdi", 8);
    v->set_ssa_version(0);
    v->set_kind(VariableKind::Parameter);
    v->set_parameter_index(0);
    auto s = variable_info(v);
    ASSERT_CONTAINS(s, "name=\"rdi\"");
    ASSERT_CONTAINS(s, "kind=Parameter");
    ASSERT_CONTAINS(s, "param_idx=0");
    ASSERT_CONTAINS(s, "size=64b");

    auto* gv = arena.create<GlobalVariable>("_printf", 8);
    auto sg = variable_info(gv);
    ASSERT_CONTAINS(sg, "GlobalVariable(");
    ASSERT_CONTAINS(sg, "is_constant=false");
    std::cout << "[+] test_variable_info passed.\n";
}

// ============================================================================
// Phase 3: Expression Tree Tests
// ============================================================================

void test_expression_tree_simple() {
    DecompilerArena arena;
    auto* c = arena.create<Constant>(42, 4);
    auto s = expr_tree(c);
    ASSERT_CONTAINS(s, "Constant(0x2a [i32])");
    std::cout << "[+] test_expression_tree_simple passed.\n";
}

void test_expression_tree_nested() {
    DecompilerArena arena;
    auto* a = arena.create<Variable>("x", 4);
    auto* b = arena.create<Constant>(1, 4);
    SmallVector<Expression*, 4> ops{a, b};
    auto* op = arena.create<Operation>(OperationType::add, std::move(ops), 4);
    auto s = expr_tree(op);
    ASSERT_CONTAINS(s, "Operation(add):");
    ASSERT_CONTAINS(s, "var:x_0");
    ASSERT_CONTAINS(s, "Constant(0x1");
    std::cout << "[+] test_expression_tree_nested passed.\n";
}

void test_self_reference_detection() {
    DecompilerArena arena;
    auto* dst = arena.create<Variable>("ecx", 4);
    dst->set_ssa_version(1);
    auto* true_val = arena.create<Variable>("eax", 4);
    true_val->set_ssa_version(1);
    auto* false_val = arena.create<Variable>("ecx", 4);
    false_val->set_ssa_version(1); // Same name+SSA as dst!

    auto* cond_var = arena.create<Constant>(1, 1);
    SmallVector<Expression*, 4> ops{cond_var, true_val, false_val};
    auto* ternary = arena.create<Operation>(OperationType::ternary, std::move(ops), 4);

    ASSERT_TRUE(has_self_reference(ternary, dst));

    // Also check tree annotation
    auto s = expr_tree_with_dest(ternary, dst);
    ASSERT_CONTAINS(s, "SELF-REFERENCE");
    std::cout << "[+] test_self_reference_detection passed.\n";
}

void test_self_reference_negative() {
    DecompilerArena arena;
    auto* dst = arena.create<Variable>("ecx", 4);
    dst->set_ssa_version(1);
    auto* true_val = arena.create<Variable>("eax", 4);
    auto* false_val = arena.create<Variable>("ebx", 4); // Different name

    auto* cond_var = arena.create<Constant>(1, 1);
    SmallVector<Expression*, 4> ops{cond_var, true_val, false_val};
    auto* ternary = arena.create<Operation>(OperationType::ternary, std::move(ops), 4);

    ASSERT_TRUE(!has_self_reference(ternary, dst));
    std::cout << "[+] test_self_reference_negative passed.\n";
}

void test_expression_depth() {
    DecompilerArena arena;
    auto* c = arena.create<Constant>(1, 4);
    ASSERT_EQ(expression_depth(c), 1);

    auto* v = arena.create<Variable>("x", 4);
    SmallVector<Expression*, 4> ops{v, c};
    auto* op = arena.create<Operation>(OperationType::add, std::move(ops), 4);
    ASSERT_EQ(expression_depth(op), 2);

    ASSERT_EQ(expression_depth(nullptr), 0);
    std::cout << "[+] test_expression_depth passed.\n";
}

void test_expression_weight() {
    DecompilerArena arena;
    auto* c = arena.create<Constant>(1, 4);
    ASSERT_EQ(expression_weight(c), 1);

    auto* v = arena.create<Variable>("x", 4);
    SmallVector<Expression*, 4> ops{v, c};
    auto* op = arena.create<Operation>(OperationType::add, std::move(ops), 4);
    ASSERT_EQ(expression_weight(op), 3); // op + v + c

    ASSERT_EQ(expression_weight(nullptr), 0);
    std::cout << "[+] test_expression_weight passed.\n";
}

// ============================================================================
// Phase 4: Stage Metrics Tests
// ============================================================================

void test_stage_metrics_collector() {
    DecompilerTask task(0x10000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    auto* assign = task.arena().create<Assignment>(
        task.arena().create<Variable>("x", 4),
        task.arena().create<Constant>(1, 4));
    bb0->add_instruction(assign);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    StageMetricsCollector collector;
    collector.before_stage("TestStage", task);
    collector.after_stage("TestStage", task);

    ASSERT_EQ(collector.metrics().size(), 1);
    ASSERT_EQ(collector.metrics()[0].stage_name, "TestStage");
    ASSERT_EQ(collector.metrics()[0].stage_ordinal, 1);
    ASSERT_EQ(collector.metrics()[0].failed, false);
    std::cout << "[+] test_stage_metrics_collector passed.\n";
}

void test_stage_metrics_format_table() {
    DecompilerTask task(0x10000);
    StageMetricsCollector collector;
    collector.before_stage("StageA", task);
    collector.after_stage("StageA", task);
    collector.before_stage("StageB", task);
    collector.after_stage("StageB", task);

    auto table = collector.format_table();
    ASSERT_CONTAINS(table, "StageA");
    ASSERT_CONTAINS(table, "StageB");
    ASSERT_CONTAINS(table, "TOTAL");
    std::cout << "[+] test_stage_metrics_format_table passed.\n";
}

void test_stage_metrics_failed_stage() {
    DecompilerTask task(0x10000);
    StageMetricsCollector collector;
    collector.before_stage("FailStage", task);
    // Simulate: stage throws, no after_stage call
    collector.before_stage("NextStage", task); // This triggers orphan handling
    collector.after_stage("NextStage", task);

    ASSERT_EQ(collector.metrics().size(), 2);
    ASSERT_EQ(collector.metrics()[0].stage_name, "FailStage");
    ASSERT_TRUE(collector.metrics()[0].failed);
    ASSERT_EQ(collector.metrics()[1].stage_name, "NextStage");
    ASSERT_TRUE(!collector.metrics()[1].failed);
    std::cout << "[+] test_stage_metrics_failed_stage passed.\n";
}

void test_stage_metrics_duplicate_names() {
    DecompilerTask task(0x10000);
    StageMetricsCollector collector;
    collector.before_stage("DeadComponentPruner", task);
    collector.after_stage("DeadComponentPruner", task);
    collector.before_stage("SomeOther", task);
    collector.after_stage("SomeOther", task);
    collector.before_stage("DeadComponentPruner", task);
    collector.after_stage("DeadComponentPruner", task);

    ASSERT_EQ(collector.metrics().size(), 3);
    ASSERT_EQ(collector.metrics()[0].stage_ordinal, 1);
    ASSERT_EQ(collector.metrics()[2].stage_ordinal, 3);
    // Both are "DeadComponentPruner" but different ordinals
    ASSERT_EQ(collector.metrics()[0].stage_name, "DeadComponentPruner");
    ASSERT_EQ(collector.metrics()[2].stage_name, "DeadComponentPruner");
    ASSERT_NE(collector.metrics()[0].stage_ordinal, collector.metrics()[2].stage_ordinal);
    std::cout << "[+] test_stage_metrics_duplicate_names passed.\n";
}

// ============================================================================
// Phase 5: IR Diff Tests
// ============================================================================

void test_ir_snapshot_capture() {
    DecompilerTask task(0x10000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    auto* assign = task.arena().create<Assignment>(
        task.arena().create<Variable>("x", 4),
        task.arena().create<Constant>(1, 4));
    bb0->add_instruction(assign);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    auto snap = capture_snapshot(task);
    ASSERT_EQ(snap.blocks.size(), 1);
    ASSERT_EQ(snap.blocks[0].block_id, 0);
    ASSERT_EQ(snap.blocks[0].instructions.size(), 1);
    std::cout << "[+] test_ir_snapshot_capture passed.\n";
}

void test_ir_diff_no_changes() {
    DecompilerTask task(0x10000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    bb0->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("x", 4),
        task.arena().create<Constant>(1, 4)));
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    auto snap1 = capture_snapshot(task);
    auto snap2 = capture_snapshot(task);
    auto diff = diff_snapshots(snap1, snap2);
    ASSERT_TRUE(!diff.has_changes());
    std::cout << "[+] test_ir_diff_no_changes passed.\n";
}

void test_ir_diff_added_instruction() {
    DecompilerTask task(0x10000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    bb0->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("x", 4),
        task.arena().create<Constant>(1, 4)));
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    auto snap_before = capture_snapshot(task);

    // Add an instruction
    bb0->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("y", 4),
        task.arena().create<Constant>(2, 4)));

    auto snap_after = capture_snapshot(task);
    auto diff = diff_snapshots(snap_before, snap_after);
    ASSERT_TRUE(diff.has_changes());

    bool found_added = false;
    for (const auto& change : diff.instruction_changes) {
        if (change.kind == IrDiff::InstructionChange::Added) {
            found_added = true;
        }
    }
    ASSERT_TRUE(found_added);
    std::cout << "[+] test_ir_diff_added_instruction passed.\n";
}

void test_ir_diff_removed_instruction() {
    DecompilerTask task(0x10000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    bb0->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("x", 4),
        task.arena().create<Constant>(1, 4)));
    bb0->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("y", 4),
        task.arena().create<Constant>(2, 4)));
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    auto snap_before = capture_snapshot(task);

    // Remove second instruction
    auto& insts = bb0->mutable_instructions();
    insts.pop_back();

    auto snap_after = capture_snapshot(task);
    auto diff = diff_snapshots(snap_before, snap_after);
    ASSERT_TRUE(diff.has_changes());

    bool found_removed = false;
    for (const auto& change : diff.instruction_changes) {
        if (change.kind == IrDiff::InstructionChange::Removed) {
            found_removed = true;
        }
    }
    ASSERT_TRUE(found_removed);
    std::cout << "[+] test_ir_diff_removed_instruction passed.\n";
}

void test_ir_diff_block_added() {
    DecompilerTask task(0x10000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    auto snap_before = capture_snapshot(task);

    task.cfg()->add_block(task.arena().create<BasicBlock>(1));

    auto snap_after = capture_snapshot(task);
    auto diff = diff_snapshots(snap_before, snap_after);
    ASSERT_TRUE(diff.has_changes());
    ASSERT_EQ(diff.blocks_added.size(), 1);
    ASSERT_EQ(diff.blocks_added[0], 1);
    std::cout << "[+] test_ir_diff_block_added passed.\n";
}

void test_ir_diff_block_removed() {
    // Create a snapshot with 2 blocks, then one with 1 block
    IrSnapshot before;
    before.blocks.push_back({0, {"inst1"}, 111});
    before.blocks.push_back({1, {"inst2"}, 222});

    IrSnapshot after;
    after.blocks.push_back({0, {"inst1"}, 111});

    auto diff = diff_snapshots(before, after);
    ASSERT_TRUE(diff.has_changes());
    ASSERT_EQ(diff.blocks_removed.size(), 1);
    ASSERT_EQ(diff.blocks_removed[0], 1);
    std::cout << "[+] test_ir_diff_block_removed passed.\n";
}

// ============================================================================
// Phase 6: Invariant Checker Tests
// ============================================================================

void test_invariant_checker_cfg_consistency() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1);
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->set_entry_block(bb0);

    // Create broken edge: successor not in target's predecessors
    auto* edge = arena.create<Edge>(bb0, bb1, EdgeType::Unconditional);
    bb0->add_successor(edge);
    // NOT adding to bb1->predecessors -- this is the violation

    IrInvariantChecker checker;
    auto v = checker.check_cfg_consistency(cfg.get());
    bool found_edge_sym = false;
    for (auto& viol : v) {
        if (viol.invariant_name == "edge_symmetry") found_edge_sym = true;
    }
    ASSERT_TRUE(found_edge_sym);
    std::cout << "[+] test_invariant_checker_cfg_consistency passed.\n";
}

void test_invariant_checker_phi_arg_mismatch() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1);
    auto* bb2 = arena.create<BasicBlock>(2);
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->add_block(bb2);
    cfg->set_entry_block(bb0);

    // bb2 has 1 predecessor but phi has 2 args
    auto* edge = arena.create<Edge>(bb0, bb2, EdgeType::Unconditional);
    bb0->add_successor(edge);
    bb2->add_predecessor(edge);

    auto* dest = arena.create<Variable>("x", 4);
    auto* src1 = arena.create<Variable>("x", 4);
    auto* src2 = arena.create<Variable>("x", 4);
    SmallVector<Expression*, 4> phi_args{src1, src2};
    auto* list = arena.create<ListOperation>(std::move(phi_args));
    auto* phi = arena.create<Phi>(dest, list);
    bb2->add_instruction(phi);

    IrInvariantChecker checker;
    auto v = checker.check_ssa_consistency(cfg.get());
    bool found = false;
    for (auto& viol : v) {
        if (viol.invariant_name == "phi_arg_count") found = true;
    }
    ASSERT_TRUE(found);
    std::cout << "[+] test_invariant_checker_phi_arg_mismatch passed.\n";
}

void test_invariant_checker_undefined_variable() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    // Use variable "x_5" (SSA version > 0) without defining it.
    // Version-0 Register/Parameter and GlobalVariable are excluded as implicit,
    // but a non-zero SSA version with no definition is a real violation.
    auto* x = arena.create<Variable>("x", 4);
    x->set_ssa_version(5);
    auto* ret = arena.create<Return>(SmallVector<Expression*, 2>{x});
    bb0->add_instruction(ret);

    IrInvariantChecker checker;
    auto v = checker.check_variable_liveness(cfg.get());
    bool found = false;
    for (auto& viol : v) {
        if (viol.invariant_name == "undefined_variable") found = true;
    }
    ASSERT_TRUE(found);
    std::cout << "[+] test_invariant_checker_undefined_variable passed.\n";
}

void test_invariant_checker_entry_block_preds() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1);
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->set_entry_block(bb0);

    // Add a predecessor to entry block (violation)
    auto* edge = arena.create<Edge>(bb1, bb0, EdgeType::Unconditional);
    bb1->add_successor(edge);
    bb0->add_predecessor(edge);

    IrInvariantChecker checker;
    auto v = checker.check_cfg_consistency(cfg.get());
    bool found = false;
    for (auto& viol : v) {
        if (viol.invariant_name == "entry_block_no_preds") found = true;
    }
    ASSERT_TRUE(found);
    std::cout << "[+] test_invariant_checker_entry_block_preds passed.\n";
}

void test_invariant_checker_unreachable_block() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1); // No edges from bb0
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->set_entry_block(bb0);

    IrInvariantChecker checker;
    auto v = checker.check_cfg_consistency(cfg.get());
    bool found = false;
    for (auto& viol : v) {
        if (viol.invariant_name == "unreachable_block") found = true;
    }
    ASSERT_TRUE(found);
    std::cout << "[+] test_invariant_checker_unreachable_block passed.\n";
}

void test_invariant_checker_phase_gating() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    // Add a phi with mismatched args (but check in PreSSA should NOT trigger SSA checks)
    auto* dest = arena.create<Variable>("x", 4);
    auto* src1 = arena.create<Variable>("x", 4);
    SmallVector<Expression*, 4> phi_args{src1};
    auto* list = arena.create<ListOperation>(std::move(phi_args));
    auto* phi = arena.create<Phi>(dest, list);
    bb0->add_instruction(phi);

    IrInvariantChecker checker;
    // PreSSA: SSA checks should be skipped
    auto v_pre = checker.check_all(cfg.get(), PipelinePhase::PreSSA);
    bool found_phi = false;
    for (auto& viol : v_pre) {
        if (viol.invariant_name == "phi_arg_count") found_phi = true;
    }
    ASSERT_TRUE(!found_phi);

    // SSA: SSA checks should fire
    auto v_ssa = checker.check_all(cfg.get(), PipelinePhase::SSA);
    for (auto& viol : v_ssa) {
        if (viol.invariant_name == "phi_arg_count") found_phi = true;
    }
    ASSERT_TRUE(found_phi);
    std::cout << "[+] test_invariant_checker_phase_gating passed.\n";
}

void set_cfg_with_single_return_use(DecompilerTask& task, const std::string& name,
                                    std::size_t ssa_version, bool with_definition) {
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    if (with_definition) {
        auto* def = task.arena().create<Variable>(name, 8);
        def->set_ssa_version(ssa_version);
        bb0->add_instruction(task.arena().create<Assignment>(def, task.arena().create<Constant>(1, 8)));
    }

    auto* use = task.arena().create<Variable>(name, 8);
    use->set_ssa_version(ssa_version);
    bb0->add_instruction(task.arena().create<Return>(SmallVector<Expression*, 2>{use}));
    task.set_cfg(std::move(cfg));
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (true) {
        pos = text.find(needle, pos);
        if (pos == std::string::npos) {
            break;
        }
        ++count;
        pos += needle.size();
    }
    return count;
}

struct JsonCursor {
    const std::string& text;
    std::size_t pos = 0;
};

void json_skip_ws(JsonCursor& c) {
    while (c.pos < c.text.size() && std::isspace(static_cast<unsigned char>(c.text[c.pos])) != 0) {
        ++c.pos;
    }
}

bool json_parse_value(JsonCursor& c);

bool json_parse_string(JsonCursor& c) {
    if (!(c.pos < c.text.size() && c.text[c.pos] == '"')) {
        return false;
    }
    ++c.pos;
    while (c.pos < c.text.size()) {
        const char ch = c.text[c.pos++];
        if (ch == '\\') {
            if (c.pos >= c.text.size()) {
                return false;
            }
            ++c.pos;
            continue;
        }
        if (ch == '"') {
            return true;
        }
    }
    return false;
}

bool json_parse_number(JsonCursor& c) {
    const std::size_t begin = c.pos;
    if (c.pos < c.text.size() && c.text[c.pos] == '-') {
        ++c.pos;
    }
    if (!(c.pos < c.text.size() && std::isdigit(static_cast<unsigned char>(c.text[c.pos])) != 0)) {
        return false;
    }
    while (c.pos < c.text.size() && std::isdigit(static_cast<unsigned char>(c.text[c.pos])) != 0) {
        ++c.pos;
    }
    return c.pos > begin;
}

bool json_parse_array(JsonCursor& c) {
    if (!(c.pos < c.text.size() && c.text[c.pos] == '[')) {
        return false;
    }
    ++c.pos;
    json_skip_ws(c);
    if (c.pos < c.text.size() && c.text[c.pos] == ']') {
        ++c.pos;
        return true;
    }
    while (true) {
        if (!json_parse_value(c)) {
            return false;
        }
        json_skip_ws(c);
        if (c.pos >= c.text.size()) {
            return false;
        }
        if (c.text[c.pos] == ']') {
            ++c.pos;
            return true;
        }
        if (c.text[c.pos] != ',') {
            return false;
        }
        ++c.pos;
        json_skip_ws(c);
    }
}

bool json_parse_object(JsonCursor& c) {
    if (!(c.pos < c.text.size() && c.text[c.pos] == '{')) {
        return false;
    }
    ++c.pos;
    json_skip_ws(c);
    if (c.pos < c.text.size() && c.text[c.pos] == '}') {
        ++c.pos;
        return true;
    }
    while (true) {
        if (!json_parse_string(c)) {
            return false;
        }
        json_skip_ws(c);
        if (!(c.pos < c.text.size() && c.text[c.pos] == ':')) {
            return false;
        }
        ++c.pos;
        json_skip_ws(c);
        if (!json_parse_value(c)) {
            return false;
        }
        json_skip_ws(c);
        if (c.pos >= c.text.size()) {
            return false;
        }
        if (c.text[c.pos] == '}') {
            ++c.pos;
            return true;
        }
        if (c.text[c.pos] != ',') {
            return false;
        }
        ++c.pos;
        json_skip_ws(c);
    }
}

bool json_parse_literal(JsonCursor& c, const char* lit) {
    const std::size_t start = c.pos;
    while (*lit != '\0') {
        if (!(c.pos < c.text.size() && c.text[c.pos] == *lit)) {
            return false;
        }
        ++c.pos;
        ++lit;
    }
    return c.pos > start;
}

bool json_parse_value(JsonCursor& c) {
    json_skip_ws(c);
    if (c.pos >= c.text.size()) {
        return false;
    }
    const char ch = c.text[c.pos];
    if (ch == '{') {
        return json_parse_object(c);
    }
    if (ch == '[') {
        return json_parse_array(c);
    }
    if (ch == '"') {
        return json_parse_string(c);
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
        return json_parse_number(c);
    }
    if (ch == 't') {
        return json_parse_literal(c, "true");
    }
    if (ch == 'f') {
        return json_parse_literal(c, "false");
    }
    if (ch == 'n') {
        return json_parse_literal(c, "null");
    }
    return false;
}

bool parse_json_document_fully(const std::string& text) {
    JsonCursor c{text, 0};
    if (!json_parse_value(c)) {
        return false;
    }
    json_skip_ws(c);
    return c.pos == text.size();
}

void test_invariant_checker_arm64_alias_positive() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    auto* def_x0 = arena.create<Variable>("x0", 8);
    def_x0->set_ssa_version(1);
    auto* use_w0 = arena.create<Variable>("w0", 4);
    use_w0->set_ssa_version(1);
    bb0->add_instruction(arena.create<Assignment>(def_x0, arena.create<Constant>(1, 8)));
    bb0->add_instruction(arena.create<Return>(SmallVector<Expression*, 2>{use_w0}));

    IrInvariantChecker checker;
    auto v = checker.check_variable_liveness(cfg.get());
    bool found_undefined = false;
    for (auto& viol : v) {
        if (viol.invariant_name == "undefined_variable") {
            found_undefined = true;
        }
    }
    ASSERT_TRUE(!found_undefined);
    std::cout << "[+] test_invariant_checker_arm64_alias_positive passed.\n";
}

void test_invariant_checker_arm64_alias_negative_cases() {
    IrInvariantChecker checker;

    // Different SSA version must not alias.
    {
        DecompilerArena arena;
        auto cfg = std::make_unique<ControlFlowGraph>();
        auto* bb0 = arena.create<BasicBlock>(0);
        cfg->add_block(bb0);
        cfg->set_entry_block(bb0);

        auto* def_x0 = arena.create<Variable>("x0", 8);
        def_x0->set_ssa_version(1);
        auto* use_w0 = arena.create<Variable>("w0", 4);
        use_w0->set_ssa_version(2);
        bb0->add_instruction(arena.create<Assignment>(def_x0, arena.create<Constant>(1, 8)));
        bb0->add_instruction(arena.create<Return>(SmallVector<Expression*, 2>{use_w0}));

        auto v = checker.check_variable_liveness(cfg.get());
        bool found = false;
        for (const auto& viol : v) {
            if (viol.description.find("w0_2") != std::string::npos) {
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }

    // Non-register names must not alias.
    {
        DecompilerArena arena;
        auto cfg = std::make_unique<ControlFlowGraph>();
        auto* bb0 = arena.create<BasicBlock>(0);
        cfg->add_block(bb0);
        cfg->set_entry_block(bb0);

        auto* def = arena.create<Variable>("x0_tmp", 8);
        def->set_ssa_version(1);
        auto* use = arena.create<Variable>("w0_tmp", 4);
        use->set_ssa_version(1);
        bb0->add_instruction(arena.create<Assignment>(def, arena.create<Constant>(1, 8)));
        bb0->add_instruction(arena.create<Return>(SmallVector<Expression*, 2>{use}));

        auto v = checker.check_variable_liveness(cfg.get());
        bool found = false;
        for (const auto& viol : v) {
            if (viol.description.find("w0_tmp_1") != std::string::npos) {
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }

    // Unrelated register numbers must not alias.
    {
        DecompilerArena arena;
        auto cfg = std::make_unique<ControlFlowGraph>();
        auto* bb0 = arena.create<BasicBlock>(0);
        cfg->add_block(bb0);
        cfg->set_entry_block(bb0);

        auto* def = arena.create<Variable>("x1", 8);
        def->set_ssa_version(1);
        auto* use = arena.create<Variable>("w0", 4);
        use->set_ssa_version(1);
        bb0->add_instruction(arena.create<Assignment>(def, arena.create<Constant>(1, 8)));
        bb0->add_instruction(arena.create<Return>(SmallVector<Expression*, 2>{use}));

        auto v = checker.check_variable_liveness(cfg.get());
        bool found = false;
        for (const auto& viol : v) {
            if (viol.description.find("w0_1") != std::string::npos) {
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }

    std::cout << "[+] test_invariant_checker_arm64_alias_negative_cases passed.\n";
}

// ============================================================================
// Phase 8: IR Query Tests
// ============================================================================

void test_ir_query_find_variables() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    auto* x = arena.create<Variable>("x", 4);
    auto* y = arena.create<Variable>("y", 4);
    bb0->add_instruction(arena.create<Assignment>(x, arena.create<Constant>(1, 4)));
    bb0->add_instruction(arena.create<Assignment>(y, arena.create<Constant>(2, 4)));

    auto vars = IrQuery::find_variables(cfg.get(), [](const Variable* v) {
        return v->name() == "x";
    });
    ASSERT_TRUE(!vars.empty());
    ASSERT_EQ(vars[0]->name(), "x");
    std::cout << "[+] test_ir_query_find_variables passed.\n";
}

void test_ir_query_find_definitions() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    bb0->add_instruction(arena.create<Assignment>(
        arena.create<Variable>("x", 4),
        arena.create<Constant>(1, 4)));

    auto defs = IrQuery::find_definitions(cfg.get(), "x");
    ASSERT_EQ(defs.size(), 1);
    std::cout << "[+] test_ir_query_find_definitions passed.\n";
}

void test_ir_query_find_uses() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    auto* x = arena.create<Variable>("x", 4);
    bb0->add_instruction(arena.create<Return>(SmallVector<Expression*, 2>{x}));

    auto uses = IrQuery::find_uses(cfg.get(), "x");
    ASSERT_EQ(uses.size(), 1);
    std::cout << "[+] test_ir_query_find_uses passed.\n";
}

// ============================================================================
// Fix 4: DebugObserver unit tests
// ============================================================================

void test_debug_observer_debug_all() {
    DebugOptions opts;
    opts.debug_all = true;
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    // After construction with debug_all, format_summary should work
    // (stage_metrics was enabled by debug_all)
    auto summary = observer.format_summary();
    // Empty stages but header still present in metrics table
    ASSERT_TRUE(!observer.metrics().format_table().empty());
    std::cout << "[+] test_debug_observer_debug_all passed.\n";
}

void test_debug_observer_diff_stage_filter() {
    DebugOptions opts;
    opts.diff_stages = true;
    opts.diff_stage_name = "OnlyThisStage";
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    // Create a task with a simple CFG
    DecompilerTask task(0x20000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    bb0->add_instruction(task.arena().create<Assignment>(
        task.arena().create<Variable>("x", 4),
        task.arena().create<Constant>(1, 4)));
    task.set_cfg(std::move(cfg));

    // Call with a stage name that doesn't match the filter — should produce no output
    observer("WrongStage", true, task);
    observer("WrongStage", false, task);
    ASSERT_TRUE(oss.str().empty());

    // Call with the matching stage — should capture (even if no changes, no output for unchanged)
    observer("OnlyThisStage", true, task);
    observer("OnlyThisStage", false, task);
    // No changes so diff should still be empty, but the code path was exercised
    std::cout << "[+] test_debug_observer_diff_stage_filter passed.\n";
}

void test_debug_observer_format_summary_with_metrics() {
    DebugOptions opts;
    opts.stage_metrics = true;
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    // Create a task with a CFG
    DecompilerTask task(0x20001);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    // Run a stage
    observer("TestStage", true, task);
    observer("TestStage", false, task);

    auto summary = observer.format_summary();
    ASSERT_TRUE(!summary.empty());
    ASSERT_CONTAINS(summary, "TestStage");
    std::cout << "[+] test_debug_observer_format_summary_with_metrics passed.\n";
}

void test_check_call_integrity_null_target() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = arena.create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);

    // Create a Call with null target
    std::vector<Expression*> args;
    auto* call = arena.create<Call>(nullptr, args, 4);
    auto* assign = arena.create<Assignment>(
        arena.create<Variable>("result", 4), call);
    bb0->add_instruction(assign);

    IrInvariantChecker checker;
    auto violations = checker.check_call_integrity(cfg.get());
    // Should find at least one call_null_target violation
    bool found = false;
    for (const auto& v : violations) {
        if (v.invariant_name == "call_null_target") {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    std::cout << "[+] test_check_call_integrity_null_target passed.\n";
}

void test_debug_observer_invariant_selector_exact_name_all_invocations() {
    DebugOptions opts;
    opts.check_invariants_after = "GraphExpressionFolding";
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    DecompilerTask task(0x21000);
    set_cfg_with_single_return_use(task, "x", 5, false);
    observer("GraphExpressionFolding", true, task);
    observer("GraphExpressionFolding", false, task);
    observer("OtherStage", true, task);
    observer("OtherStage", false, task);
    observer("GraphExpressionFolding", true, task);
    observer("GraphExpressionFolding", false, task);

    const std::string out = oss.str();
    ASSERT_EQ(count_occurrences(out, "INVARIANTS ==="), 2);
    std::cout << "[+] test_debug_observer_invariant_selector_exact_name_all_invocations passed.\n";
}

void test_debug_observer_invariant_selector_stage_ordinal() {
    DebugOptions opts;
    opts.check_invariants_after = "GraphExpressionFolding#2";
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    DecompilerTask task(0x21001);
    set_cfg_with_single_return_use(task, "x", 5, false);
    observer("GraphExpressionFolding", true, task);
    observer("GraphExpressionFolding", false, task);
    observer("GraphExpressionFolding", true, task);
    observer("GraphExpressionFolding", false, task);

    const std::string out = oss.str();
    ASSERT_EQ(count_occurrences(out, "INVARIANTS ==="), 1);
    ASSERT_CONTAINS(out, "#2");
    std::cout << "[+] test_debug_observer_invariant_selector_stage_ordinal passed.\n";
}

void test_debug_observer_invariant_selector_unknown_diagnostic() {
    DebugOptions opts;
    opts.check_invariants_after = "NoSuchStage";
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    DecompilerTask task(0x21002);
    set_cfg_with_single_return_use(task, "x", 5, false);
    observer("Alpha", true, task);
    observer("Alpha", false, task);
    observer("Beta", true, task);
    observer("Beta", false, task);
    observer("Alpha", true, task);
    observer("Alpha", false, task);

    auto summary = observer.format_summary();
    ASSERT_CONTAINS(summary, "NoSuchStage");
    ASSERT_CONTAINS(summary, "observed stages");
    ASSERT_CONTAINS(summary, "Alpha (x2)");
    ASSERT_CONTAINS(summary, "Beta");
    std::cout << "[+] test_debug_observer_invariant_selector_unknown_diagnostic passed.\n";
}

void test_debug_observer_invariant_dedup_persistence() {
    DebugOptions opts;
    opts.check_invariants = true;
    std::ostringstream oss;
    DebugObserver observer(opts, oss);

    DecompilerTask task(0x21003);

    // First appearance: new + detailed.
    set_cfg_with_single_return_use(task, "x", 5, false);
    observer("Stage1", true, task);
    observer("Stage1", false, task);

    // Repeat next stage: suppressed.
    set_cfg_with_single_return_use(task, "x", 5, false);
    observer("Stage2", true, task);
    observer("Stage2", false, task);

    // Disappear.
    set_cfg_with_single_return_use(task, "x", 5, true);
    observer("Stage3", true, task);
    observer("Stage3", false, task);

    // Reappear must be treated as new.
    set_cfg_with_single_return_use(task, "x", 5, false);
    observer("Stage4", true, task);
    observer("Stage4", false, task);

    const std::string out = oss.str();
    ASSERT_CONTAINS(out, "summary: new=1 repeated_suppressed=0 total_active=1");
    ASSERT_CONTAINS(out, "summary: new=0 repeated_suppressed=1 total_active=1");
    ASSERT_EQ(count_occurrences(out, "[undefined_variable]"), 2);
    std::cout << "[+] test_debug_observer_invariant_dedup_persistence passed.\n";
}

void test_stage_metrics_json_schema_and_ordering() {
    DecompilerTask task(0x22000);
    auto cfg = std::make_unique<ControlFlowGraph>();
    auto* bb0 = task.arena().create<BasicBlock>(0);
    cfg->add_block(bb0);
    cfg->set_entry_block(bb0);
    task.set_cfg(std::move(cfg));

    StageMetricsCollector collector;
    collector.before_stage("StageA", task);
    collector.after_stage("StageA", task);
    collector.before_stage("StageB", task);
    collector.after_stage("StageB", task);

    const std::string json = collector.format_json();
    ASSERT_TRUE(!json.empty());
    ASSERT_TRUE(json.front() == '[');
    ASSERT_CONTAINS(json, "\"ordinal\"");
    ASSERT_CONTAINS(json, "\"name\"");
    ASSERT_CONTAINS(json, "\"duration_us\"");
    ASSERT_CONTAINS(json, "\"instructions_before\"");
    ASSERT_CONTAINS(json, "\"instructions_after\"");
    ASSERT_CONTAINS(json, "\"variables_before\"");
    ASSERT_CONTAINS(json, "\"variables_after\"");
    ASSERT_CONTAINS(json, "\"blocks_before\"");
    ASSERT_CONTAINS(json, "\"blocks_after\"");
    ASSERT_CONTAINS(json, "\"failed\"");

    const std::size_t ord1 = json.find("\"ordinal\": 1");
    const std::size_t ord2 = json.find("\"ordinal\": 2");
    ASSERT_TRUE(ord1 != std::string::npos);
    ASSERT_TRUE(ord2 != std::string::npos);
    ASSERT_TRUE(ord1 < ord2);
    ASSERT_TRUE(count_occurrences(json, "{") == 2);
    std::cout << "[+] test_stage_metrics_json_schema_and_ordering passed.\n";
}

void test_stage_metrics_full_stream_json_report_validity_and_ordering() {
    std::vector<FunctionStageMetrics> functions;

    FunctionStageMetrics fn_a;
    fn_a.function_name = "alpha";
    fn_a.function_address = 0x1000;
    fn_a.stages.push_back(StageMetrics{
        .stage_name = "Lifter",
        .stage_ordinal = 1,
        .duration_us = 10,
        .instructions_before = 0,
        .instructions_after = 3,
        .variables_before = 0,
        .variables_after = 2,
        .blocks_before = 0,
        .blocks_after = 1,
        .failed = false,
    });
    functions.push_back(fn_a);

    FunctionStageMetrics fn_b;
    fn_b.function_name = "beta";
    fn_b.function_address = 0x2000;
    fn_b.stages.push_back(StageMetrics{
        .stage_name = "GraphExpressionFolding",
        .stage_ordinal = 1,
        .duration_us = 12,
        .instructions_before = 3,
        .instructions_after = 2,
        .variables_before = 2,
        .variables_after = 2,
        .blocks_before = 1,
        .blocks_after = 1,
        .failed = false,
    });
    functions.push_back(fn_b);

    const std::string report = format_stage_metrics_report_json(
        "meta/fibonacci",
        2,
        2,
        functions);

    ASSERT_TRUE(parse_json_document_fully(report));
    ASSERT_CONTAINS(report, "\"input_binary\": \"meta/fibonacci\"");
    ASSERT_CONTAINS(report, "\"total_functions\": 2");
    ASSERT_CONTAINS(report, "\"decompiled_functions\": 2");
    ASSERT_CONTAINS(report, "\"functions\": [");

    const std::size_t alpha_pos = report.find("\"name\": \"alpha\"");
    const std::size_t beta_pos = report.find("\"name\": \"beta\"");
    ASSERT_TRUE(alpha_pos != std::string::npos);
    ASSERT_TRUE(beta_pos != std::string::npos);
    ASSERT_TRUE(alpha_pos < beta_pos);

    const std::string contaminated = "noise\n" + report;
    ASSERT_TRUE(!parse_json_document_fully(contaminated));
    const std::string fragmented = report + "[]\n";
    ASSERT_TRUE(!parse_json_document_fully(fragmented));
    std::cout << "[+] test_stage_metrics_full_stream_json_report_validity_and_ordering passed.\n";
}

void test_debug_observer_selector_diagnostic_dedup_scoped_per_observer_run() {
    DebugOptions opts;
    opts.check_invariants_after = "NoSuchStage";

    {
        std::ostringstream oss;
        DebugObserver observer(opts, oss);
        DecompilerTask task(0x33000);
        set_cfg_with_single_return_use(task, "x", 5, true);
        observer("Alpha", true, task);
        observer("Alpha", false, task);

        const std::string first = observer.format_summary();
        ASSERT_CONTAINS(first, "NoSuchStage");
        const std::string second = observer.format_summary();
        ASSERT_TRUE(second.empty());
    }

    {
        std::ostringstream oss;
        DebugObserver observer(opts, oss);
        DecompilerTask task(0x33001);
        set_cfg_with_single_return_use(task, "x", 5, true);
        observer("Alpha", true, task);
        observer("Alpha", false, task);

        const std::string third = observer.format_summary();
        ASSERT_CONTAINS(third, "NoSuchStage");
    }

    std::cout << "[+] test_debug_observer_selector_diagnostic_dedup_scoped_per_observer_run passed.\n";
}

// ============================================================================
// Fix 5: Known-good invariant test
// ============================================================================

void test_invariant_checker_well_formed_cfg() {
    DecompilerArena arena;
    auto cfg = std::make_unique<ControlFlowGraph>();

    // Create a well-formed CFG:
    //   bb0 (entry) -> bb1 -> bb2 (exit)
    //                     \-> bb2
    auto* bb0 = arena.create<BasicBlock>(0);
    auto* bb1 = arena.create<BasicBlock>(1);
    auto* bb2 = arena.create<BasicBlock>(2);
    cfg->add_block(bb0);
    cfg->add_block(bb1);
    cfg->add_block(bb2);
    cfg->set_entry_block(bb0);

    // Add proper bidirectional edges
    auto* e01 = arena.create<Edge>(bb0, bb1, EdgeType::Unconditional);
    bb0->add_successor(e01);
    bb1->add_predecessor(e01);

    auto* e12 = arena.create<Edge>(bb1, bb2, EdgeType::Unconditional);
    bb1->add_successor(e12);
    bb2->add_predecessor(e12);

    // All variables are properly defined before use
    auto* x = arena.create<Variable>("x", 4);
    x->set_ssa_version(1);
    auto* y = arena.create<Variable>("y", 4);
    y->set_ssa_version(1);
    auto* x_use = arena.create<Variable>("x", 4);
    x_use->set_ssa_version(1);
    auto* y_use = arena.create<Variable>("y", 4);
    y_use->set_ssa_version(1);

    // bb0: x_1 = 10
    bb0->add_instruction(arena.create<Assignment>(x, arena.create<Constant>(10, 4)));
    // bb1: y_1 = x_1
    bb1->add_instruction(arena.create<Assignment>(y, x_use));
    // bb2: return y_1
    bb2->add_instruction(arena.create<Return>(SmallVector<Expression*, 2>{y_use}));

    IrInvariantChecker checker;
    auto violations = checker.check_all(cfg.get(), PipelinePhase::PreSSA);
    
    // A well-formed CFG should have zero violations
    if (!violations.empty()) {
        std::cerr << "Unexpected violations in well-formed CFG:\n";
        std::cerr << IrInvariantChecker::format_violations(violations);
    }
    ASSERT_EQ(violations.size(), 0);
    std::cout << "[+] test_invariant_checker_well_formed_cfg passed.\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== Debug Infrastructure Unit Tests ===\n\n";

    // Phase 2: IR Serializer
    test_ir_serializer_constant();
    test_ir_serializer_variable();
    test_ir_serializer_global_variable();
    test_ir_serializer_operation_all_types();
    test_ir_serializer_call();
    test_ir_serializer_condition();
    test_ir_serializer_list_operation();
    test_ir_serializer_assignment();
    test_ir_serializer_phi();
    test_ir_serializer_phi_no_origin_map();
    test_ir_serializer_branch();
    test_ir_serializer_return();
    test_ir_serializer_break_continue();
    test_ir_serializer_comment();
    test_ir_serializer_relation();
    test_ir_serializer_null_handling();
    test_cfg_to_string();
    test_ast_to_string();
    test_variable_info();

    // Phase 3: Expression Tree
    test_expression_tree_simple();
    test_expression_tree_nested();
    test_self_reference_detection();
    test_self_reference_negative();
    test_expression_depth();
    test_expression_weight();

    // Phase 4: Stage Metrics
    test_stage_metrics_collector();
    test_stage_metrics_format_table();
    test_stage_metrics_failed_stage();
    test_stage_metrics_duplicate_names();

    // Phase 5: IR Diff
    test_ir_snapshot_capture();
    test_ir_diff_no_changes();
    test_ir_diff_added_instruction();
    test_ir_diff_removed_instruction();
    test_ir_diff_block_added();
    test_ir_diff_block_removed();

    // Phase 6: Invariant Checker
    test_invariant_checker_cfg_consistency();
    test_invariant_checker_phi_arg_mismatch();
    test_invariant_checker_undefined_variable();
    test_invariant_checker_entry_block_preds();
    test_invariant_checker_unreachable_block();
    test_invariant_checker_phase_gating();
    test_invariant_checker_arm64_alias_positive();
    test_invariant_checker_arm64_alias_negative_cases();

    // Phase 8: IR Query
    test_ir_query_find_variables();
    test_ir_query_find_definitions();
    test_ir_query_find_uses();

    // Fix 4: DebugObserver tests
    test_debug_observer_debug_all();
    test_debug_observer_diff_stage_filter();
    test_debug_observer_format_summary_with_metrics();
    test_check_call_integrity_null_target();
    test_debug_observer_invariant_selector_exact_name_all_invocations();
    test_debug_observer_invariant_selector_stage_ordinal();
    test_debug_observer_invariant_selector_unknown_diagnostic();
    test_debug_observer_invariant_dedup_persistence();

    // Stage metrics JSON
    test_stage_metrics_json_schema_and_ordering();
    test_stage_metrics_full_stream_json_report_validity_and_ordering();
    test_debug_observer_selector_diagnostic_dedup_scoped_per_observer_run();

    // Fix 5: Known-good invariant test
    test_invariant_checker_well_formed_cfg();

    std::cout << "\n=== All debug infrastructure tests passed. ===\n";
    return 0;
}
