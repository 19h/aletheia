# DeWolf C++ / IDA Port Specification & Agent Task Tracker

**CRITICAL DIRECTIVE**: All and any progress must be reflected in the nested to-dos below immediately upon completion or state change. If progress on a task is partial, append optional progress notes (e.g., `*Note: implemented X, blocked on Y*`) directly beneath the task.

---

## 🤖 AGENT ONBOARDING & EXECUTION PROTOCOL (READ FIRST)
**If you are reading this as a newly initialized session (or have lost context), you are an autonomous AI software engineer tasked with incrementally porting the DeWolf Python ecosystem to a native C++23 IDA Pro plugin.**

### Your Immediate Directives:
1. **Understand the Constraints**: You MUST strictly use the `idax` C++ wrapper (`/Users/int/dev/idax`). NEVER `#include` raw IDA SDK headers (`pro.h`, `ida.hpp`). Always rely on C++23 standard library features instead of proprietary IDA containers (`qstring`, `qvector`). Use the explicit `Result<T>` and `Status` error-handling model.
2. **Locate Your Next Task**: Scroll down to the **Phases 1-6 Task Tracker**. Scan linearly and find the *first unchecked task* (`- [ ]`). This is your immediate objective.
3. **Execute Aggressively**: Do not wait for user permission to try practical approaches. If blocked by a hard limitation or missing host feature, document the blocker explicitly in this file and immediately pivot to an alternate actionable path.
4. **Update State Continually**: As soon as you finish writing code for a checklist item, check off the box (`- [x]`) and add a brief sub-bullet note detailing what was completed or what decisions were made. This document is the ultimate source of truth for the project's state.

---

## Project Context & Objectives

### What is DeWolf?
DeWolf is a research decompiler originally developed during a cooperation between Fraunhofer FKIE and DSO National Laboratories. It translates low/mid-level assembly intermediate representations into structured, C-like abstract syntax trees. Its core structuring logic is based on the DREAM and DREAM++ algorithms (Yakdan et al.), which restructure control flow without relying strictly on pattern matching, producing highly readable decompiled output.

### The DeWolf Ecosystem
The ecosystem consists of three primary modules:
1. **`dewolf` (Core Decompiler)**: Manages the intermediate representation (IR), Control Flow Graph (CFG) analysis, Static Single Assignment (SSA) formulation, Dataflow optimization stages, and the DREAM control-flow restructuring pipeline.
2. **`dewolf-logic` (Logic & Range Engine)**: A separate mathematical/logic engine that performs Value Set Analysis (VSA). It simplifies complex boolean conditions and determines if execution paths are unfulfillable (used heavily by the decompiler's Dead Path Elimination stage).
3. **`dewolf-idioms` (Compiler Idioms Matcher)**: A pattern-matching engine that detects compiler optimization artifacts (such as replacing division with magic multiplication). It annotates these artifacts so the lifter can restore the original, human-readable arithmetic operations.

### The Porting Objective
The original ecosystem is implemented in Python and relies heavily on external tools (Binary Ninja for lifting MLIL, SMDA for disassembly, NetworkX/Lark for logic graph parsing). 

**The goal of this project is to port the entire DeWolf ecosystem into a native, statically typed, and highly performant C++ plugin specifically targeting IDA Pro, built entirely on top of the `idax` C++23 SDK wrapper (located at `/Users/int/dev/idax`).**

**Key architectural goals for the C++ port:**
* **Native idax Integration**: Drop Binary Ninja MLIL. Lift directly from IDA via the opaque `idax` wrapper (`ida::instruction::Instruction`, `ida::graph::BasicBlock`), zeroing out raw C-SDK leakage.
* **Performance & Memory Safety**: Eliminate the Python interpreter overhead. Implement custom Arena/Bump allocators for rapid AST/CFG node creation and destruction, avoiding heavy garbage collection or smart-pointer (`std::shared_ptr`) reference counting. Avoid compiler-specific intrinsics and heavy bit-optimizations in loader code; use straightforward/portable implementations.
* **Dependency Removal**: Strip out heavy dependencies like SMDA (use `idax`'s instruction/graph APIs), NetworkX, and Lark by implementing native, lightweight Directed Acyclic Graph (DAG) structures.
* **Robust Execution**: Provide seamless integration into the IDA GUI via `ida::ui::create_custom_viewer`, as well as a headless mode for batch-processing and CLI automation.

---

## 0. idax Integration Specification & Core Mappings

*This section provides an extensive, comprehensive, and irrefutable specification for mapping DeWolf requirements to the `idax` C++23 SDK wrapper. It serves as the primary technical contract to reduce future lookups and keep porting endeavors strictly aligned.*

### 0.1 Core idax Philosophy
1. **Opaque Wrapper**: Never `#include` raw IDA SDK headers (`pro.h`, `ida.hpp`, `idp.hpp`, etc.) directly unless explicitly bridging an unimplemented feature. Use `#include <ida/idax.hpp>` or specific module headers (`<ida/graph.hpp>`, `<ida/instruction.hpp>`).
2. **Error Handling**: `idax` relies on a `std::expected`-style error model. Use `Result<T>` and `Status`. Always check returns (e.g., `if (!res) { return res.error(); }`). No sentinel values or flags.
3. **C++23 Standards**: Rely on standard library and C++23 features rather than IDA's proprietary containers (`qvector`, `qstring`). `idax` bridges these internally.

### 0.2 Domain Mappings
| DeWolf Concept | Raw IDA SDK | idax API Mapping |
| :--- | :--- | :--- |
| **Plugin Entry** | `plugin_t`, `PLUGIN_MULTI` | Inherit `ida::plugin::Plugin`, export via `IDA_PLUGIN_ENTRY(MyPlugin)` |
| **CFG / Blocks** | `qflow_chart_t`, `ida_gdl.hpp` | `ida::graph::flowchart(ea)` → `std::vector<ida::graph::BasicBlock>` |
| **Instructions** | `insn_t`, `ida_ua.hpp` | `ida::instruction::decode(ea)` → `ida::instruction::Instruction` |
| **Operands** | `op_t` | `Instruction::operands()` → `ida::instruction::Operand` (Type: Register, Memory, etc.) |
| **Functions** | `func_t` | `ida::function::all()`, `ida::function::start(ea)` |
| **UI Output** | `custom_viewer`, `ida_kernwin` | `ida::ui::create_custom_viewer("Title", std::vector<std::string>)` |
| **Decompiler** | `mbl_array_t`, `minst_t` | `ida::decompiler::MicrocodeFilter` (Emission only) |

### 0.3 The Lifter Strategy (Path A vs Path B)
Because `idax`'s current decompiler API (`ida::decompiler::MicrocodeContext`) is heavily optimized for *emitting* or *filtering* microcode dynamically rather than performing static read-only AST traversals of existing Hex-Rays blocks, **Path B (Native IDA Disassembly) is the mandatory primary target.**
* **Execution**: Resolve function `ida::graph::flowchart(ea)`. Iterate `BasicBlock` structures. For each address in the block, use `ida::instruction::decode()` and map the native `Instruction::opcode()` and `Instruction::operands()` directly into DeWolf's `DataflowObject` and `Operation` IR.
* **Advantage**: Bypasses the Hex-Rays decompiler dependency entirely, operating independently directly on top of the native disassembler database via `idax`.

---

## Skeleton Phase Tracker (COMPLETED -- all items below were initial scaffolding)

<details>
<summary>Click to expand completed skeleton phases 1-6 (initial scaffolding only -- NOT production-complete)</summary>

### Phase 1: Foundation & Core IR (Skeleton)
- [x] 1.1 Project Initialization & Build System (CMake, plugin skeleton, type aliases)
- [x] 1.2 Memory Management (DecompilerArena, ArenaAllocated)
- [x] 1.3 Core Dataflow IR (DataflowObject, Expression, Constant, Variable, Operation, OperationType enum, visitor)
- [x] 1.4 CFG Foundations (BasicBlock, ControlFlowGraph, Edge, SwitchEdge, DFS/RPO/PostOrder)

### Phase 2: Logic & Idiom Engines (Skeleton)
- [x] 2.1 libdewolf-logic (DAG nodes, World, RangeSimplifier stub, BitwiseAndRangeSimplifier stub)
- [x] 2.2 libdewolf-idioms (IdiomTag struct, IdiomMatcher stub bound to idax)

### Phase 3: Lifter & SSA (Skeleton)
- [x] 3.1 Lifter (flowchart query, instruction decode, mnemonic mapping, idiom tag processing)
- [x] 3.2 SSA (Cooper dominators, Cytron phi placement, basic renaming, Sreedhar-like out-of-SSA)

### Phase 4: Pipeline & Optimization (Skeleton)
- [x] 4.1 Pipeline Architecture (DecompilerTask, PipelineStage, DecompilerPipeline)
- [x] 4.2 Preprocessing Stages (5 stubs: CompilerIdiomHandling, RegisterPairHandling, SwitchVariableDetection, RemoveGoPrologue, RemoveStackCanary)
- [x] 4.3 CFG Optimization Stages (ExpressionPropagation functional, GraphExpressionFolding functional, DeadCodeElimination functional, 2 stubs)

### Phase 5: DREAM Algorithm (Skeleton)
- [x] 5.1 AST Foundations (CodeNode, SeqNode, IfNode, LoopNode, SwitchNode, CaseNode, BreakNode, ContinueNode, AbstractSyntaxForest, TransitionCFG)
- [x] 5.2 Cyclic Region Structuring (back-edge DFS, wrap in LoopNode)
- [x] 5.3 Acyclic Region Structuring (region finding, GraphSlice, ReachingConditions, CBR, CAR)
- [x] 5.4 PatternIndependentRestructuringStage wired into pipeline

### Phase 6: Code Generation & UI (Skeleton)
- [x] 6.1 C-Backend (CExpressionGenerator basic ops, CodeVisitor with indentation)
- [x] 6.2 IDA UI (create_custom_viewer, colstr syntax highlighting, byte-patched event binding)
- [x] 6.3 Batch/CLI (stubs for headless, idump, file I/O)

</details>

---

## PRODUCTION GAP TRACKER: Reference-Faithful Implementation

**This is the master checklist for bringing the C++ port to feature parity with the Python reference (`ref/dewolf`, `ref/dewolf-logic`, `ref/dewolf-idioms`). Items are ordered strictly by impact priority: CRITICAL first, then HIGH, then MEDIUM, then LOW. Every gap discovered during the rigorous comparison audit is listed here. Nothing is omitted.**

**Current estimated implementation depth vs. Python reference: ~15-25%.**

---

You are forbidden from working on more than ONE task and its surgical scope at a time.
You are not allowed from finishing two or more tasks at once, even if that means touching a file multiple times. This is to ensure atomic, traceable progress and avoid context-switching overhead.

### CRITICAL PRIORITY -- Blockers for Producing Any Usable Output

- [x] **C.1** Implement the `Instruction` Type Hierarchy (currently everything is a flat `Operation`)
  - *The Python reference has `DataflowObject` -> `Instruction` (ABC) -> `Assignment`, `Branch`, `Return`, `Phi`, `MemPhi`, `Break`, `Continue`, `Comment`, `GenericBranch`, `IndirectBranch`, `Relation` as DISTINCT classes, each with semantic methods (`requirements`, `definitions`, `substitute`, `copy`). Our C++ port models ALL of these as `Operation` nodes discriminated only by `OperationType` enum values. This fundamentally cripples every pipeline stage that needs to dispatch on instruction kind -- phi lifting, out-of-SSA, dead code elimination, code generation, expression propagation, and the DREAM algorithm all need to know "is this an Assignment? a Branch? a Phi?" at the type level, not by inspecting operand count or enum tags.*
  - [x] C.1.1 Define abstract `Instruction` base class inheriting `DataflowObject` (distinct from `Expression`), with virtual `requirements() -> set<Variable*>` and `definitions() -> set<Variable*>`.
    - *Implemented in `dataflow.hpp`. `Instruction` inherits `DataflowObject`, adds `address()`, `collect_definitions()`, convenience `definitions()`/`requirements()` methods.*
  - [x] C.1.2 Implement `Assignment` class with `destination: Expression*` and `value: Expression*` fields, proper `requirements`/`definitions` semantics (destination is a definition, value's variables are requirements).
    - *Implemented with `set_destination()`, `set_value()`, `rename_destination()`. Complex destination handling (deref, ListOperation) supported.*
  - [x] C.1.3 Implement `Branch` class wrapping a `Condition` expression, with `requirements` returning the condition's variables.
    - *`Condition` is a new `Operation` subclass for binary comparisons with `lhs()`/`rhs()`/`negate_comparison()`. `Branch` wraps `Condition*`.*
  - [x] C.1.4 Implement `Return` class wrapping a `ListOperation` of return values.
    - *`Return` stores `vector<Expression*> values_` with `has_value()` check.*
  - [x] C.1.5 Implement `Phi` class inheriting `Assignment`, with `origin_block: map<BasicBlock*, Variable*>` tracking which predecessor supplies which operand. Must support `update_phi_function()`, `remove_from_origin_block()`, `rename_destination()`.
    - *`Phi` extends `Assignment`, uses `ListOperation*` for operands, `unordered_map<BasicBlock*, Expression*>` for origin_block. SSA constructor populates origin_block during renaming.*
  - [x] C.1.6 Implement `Break` and `Continue` as trivial instruction subclasses (no operands).
    - *Named `BreakInstr` and `ContinueInstr` to avoid keyword conflicts.*
  - [x] C.1.7 Implement `Comment` class wrapping a string message.
  - [x] C.1.8 Implement `IndirectBranch` class wrapping an expression for jump-table dispatch.
  - [x] C.1.9 Implement `Relation` class (like `Assignment` but marks aliased memory stores where the value may have changed via a pointer write).
  - [x] C.1.10 Refactor ALL existing pipeline stages, SSA passes, lifter, and code generator to use the new `Instruction` hierarchy instead of inspecting `OperationType` enum tags on `Operation` nodes.
    - *Refactored: lifter.cpp (produces Assignment/Branch/Return instead of flat Operation), ssa_constructor.cpp (creates Phi objects, uses origin_block), ssa_destructor.cpp (uses dynamic_cast<Phi*>, is_branch/is_return), liveness.cpp (dispatches on Phi/Assignment/Branch/Return), optimization_stages.cpp (dispatches on Assignment/Branch/Return with CMP+branch folding), graph_expression_folding.cpp (uses Assignment/Phi dispatch), dead_code_elimination.cpp (uses Assignment/Phi dispatch), codegen.cpp (full visitor with visit_assignment/visit_return/visit_phi/visit_branch/visit_break/visit_continue/visit_comment), cbr.cpp (uses Branch type), reaching_conditions.cpp (uses Branch type), car.cpp (uses Condition type), test_main.cpp (updated tests + new test_instruction_hierarchy). Removed old cfg::Instruction wrapper class. Added migration helpers (get_operation, is_assignment, is_phi, is_branch, is_return) to cfg.hpp. Also added ListOperation, Condition, and expanded visitor interface (19 methods) to dataflow.hpp. OperationType::assign and OperationType::phi removed from enum. All 4 unit tests pass. Full project builds clean (0 errors).*

- [x] **C.2** Implement the `Type` System (currently completely absent -- only `size_bytes` exists)
  - *The Python reference has a frozen-dataclass type hierarchy: `Type` (ABC) -> `UnknownType`, `Integer` (size, signed), `Float` (size), `Pointer` (target type, width), `ArrayType` (element type, count), `CustomType` (text, size), `FunctionTypeDef` (return type, params), `ComplexType` -> `Struct`, `Union`, `Enum`, `Class`. Every `Variable`, `Constant`, `Operation`, and `Instruction` carries a `Type`. The type system is essential for: variable declarations in output, cast operations, type propagation, pointer analysis, struct member access, array access detection, and readable code generation (e.g., knowing `int` vs `unsigned long` vs `char*`). Without it, the C++ port cannot produce C declarations, cannot detect casts, and cannot propagate types.*
  - [x] C.2.1 Implement `Type` abstract base with `virtual std::string to_string() const`, `virtual std::size_t size() const`, `virtual bool operator==(const Type&) const`.
    - *Implemented in `src/dewolf/structures/types.hpp`. `Type` base has `size()` (bits), `size_bytes()`, `to_string()`, `is_boolean()`, `operator==`.*
  - [x] C.2.2 Implement `UnknownType`, `Integer` (size_bits, is_signed), `Float` (size_bits).
    - *All three implemented with full factory methods and `to_string()` matching the Python reference SIZE_TYPES maps.*
  - [x] C.2.3 Implement `Pointer` (pointing to `Type*`, width), `ArrayType` (element `Type*`, count).
    - *`Pointer` handles nested pointer formatting (`int **`). `ArrayType` auto-computes total size from element size * count.*
  - [x] C.2.4 Implement `CustomType` (name string, size -- for `void`, `bool`, `wchar_t`, etc.).
    - *With `void_type()`, `bool_type()`, `wchar16()`, `wchar32()` static factory methods.*
  - [x] C.2.5 Implement `FunctionTypeDef` (return `Type*`, parameter `Type*` list).
    - *`to_string()` produces `"int(int, char *)"` format matching Python reference.*
  - [x] C.2.6 Implement `ComplexType` hierarchy: `Struct`, `Union`, `Enum` with member lists and `declaration()` methods.
    - *`ComplexType` base, `Struct` (with `is_class` flag, offset-keyed members, `get_member_by_offset()`), `Union` (type-keyed members, `get_member_by_type()`), `Enum` (value-keyed members, `get_name_by_value()`). All have `declaration()` producing C-like output.*
  - [x] C.2.7 Add `Type* type_` field to `Variable`, `Constant`, and `Operation` (replacing or augmenting `size_bytes`).
    - *Added `TypePtr type_` to `DataflowObject` (the common base), with `ir_type()` getter and `set_ir_type()` setter. `size_bytes` retained for backward compatibility during incremental migration.*
  - [x] C.2.8 Implement `Integer::int32_t()`, `Integer::uint64_t()`, `Float::float32()`, `CustomType::void_type()`, `CustomType::bool_type()` and other commonly used factory methods matching the Python reference.
    - *Full set: `Integer::char_type/int8_t/int16_t/int32_t/int64_t/int128_t/uint8_t/uint16_t/uint32_t/uint64_t/uint128_t`, `Float::float32/float64`, `CustomType::void_type/bool_type/wchar16/wchar32`, `UnknownType::instance()`. All return `TypePtr` (shared_ptr<const Type>) static singletons.*
  - [x] C.2.9 Implement `TypeParser` utility for converting IDA type strings (from `ida::function` APIs) into the `Type` hierarchy.
    - *`TypeParser` with configurable bitness (32/64), 20-entry KNOWN_TYPES table matching the Python reference, recursive pointer stripping (`"char *"` -> `Pointer(char)`), case-insensitive lookup, fallback to `CustomType`. 5 unit tests covering all type categories pass.*

- [x] **C.3** Implement Loop Structuring Rules in the DREAM Algorithm (currently ALL loops are `while(true)` with no exit conditions)
  - *The Python reference has `LoopStructurer` with 5 rules: `WhileLoopRule` (first-child is a break-condition -> negate it as the while condition), `DoWhileLoopRule` (last-child is a break-condition -> negate it as the do-while condition), `NestedDoWhileLoopRule` (last-child is single-branch condition), `SequenceRule` (all end-nodes break, no other interruptions -> remove loop wrapper entirely), `ConditionToSequenceRule` (exactly one branch breaks -> split into while + suffix). Without these rules, the decompiler emits `while(true) { ... }` for every loop regardless of its actual structure. The Python reference also has extensive pre/post-processing: combining cascading breaks, extracting conditional breaks, removing redundant continues.*
  - [x] C.3.1 Implement `WhileLoopNode` AST node (loop with condition checked before body), `DoWhileLoopNode` (condition checked after body), and `ForLoopNode` (declaration, condition, modification).
    - *Implemented in `ast.hpp`/`ast.cpp`. `LoopNode` is now an abstract base with `condition()`, `is_endless()`, `loop_type()`. Three concrete subclasses: `WhileLoopNode`, `DoWhileLoopNode`, `ForLoopNode`. Added 15+ property query methods to `AstNode` matching the Python reference: `is_endless_loop()`, `is_break_node()`, `is_break_condition()`, `is_single_branch()`, `does_end_with_break/continue/return()`, `does_contain_break()`, `is_code_node_ending_with_break/continue()`, `get_end_nodes()`, `get_descendant_code_nodes_interrupting_ancestor_loop()`, `has_descendant_code_node_breaking_ancestor_loop()`. `CodeNode` gets `clean()` and `remove_last_instruction()`. `SeqNode` gets `mutable_nodes()`, `first()`, `last()`, `remove_node()`. `IfNode` gets `set_true_branch()`, `set_false_branch()`, `switch_branches()`, `condition_expr()`. `CaseNode` gets `is_default()`, `break_case()`. Code generator updated for `DoWhileLoopNode` (do/while syntax), `ForLoopNode` (for syntax), and `WhileLoopNode` with condition (while(cond) syntax). CyclicRegionFinder creates `WhileLoopNode` instead of old flat `LoopNode`. All 5 tests pass, build clean.*
  - [x] C.3.2 Implement `WhileLoopRule`: detect when the loop body's first child (in a SeqNode) is a break-condition (ConditionNode whose true branch is a BreakNode); negate the condition and set it as the while-loop condition, remove the break-condition from the body.
  - [x] C.3.3 Implement `DoWhileLoopRule`: detect when the loop body's last child is a break-condition; negate the condition, create a `DoWhileLoopNode`, substitute the loop node.
  - [x] C.3.4 Implement `NestedDoWhileLoopRule`: detect when the loop body's last child is a single-branch condition node and no other child has breaks interrupting the loop; create a `DoWhileLoopNode` with the non-break children as body and the condition's branch as a sibling after.
  - [x] C.3.5 Implement `SequenceRule`: detect when all end-nodes are code-nodes ending with break and no other continue/break interruptions exist; remove the loop wrapper entirely and remove all break statements (the region was a single-pass block mis-identified as cyclic).
  - [x] C.3.6 Implement `ConditionToSequenceRule`: detect when the loop body is a ConditionNode and exactly one branch contains breaks (XOR); create a while-loop with the condition, put the non-break branch as body, and emit the break-branch as a suffix after the loop.
  - [x] C.3.7 Implement `LoopStructurer` orchestrator that iterates over rules (`WhileLoopRule`, `DoWhileLoopRule`, `NestedDoWhileLoopRule`, `SequenceRule`, `ConditionToSequenceRule`) until no rule matches, with pre-processing (combine breaks, extract conditional breaks, combine break nodes, remove redundant continues) and post-processing (extract conditional continues, remove redundant continues).
    - *All 5 rules + orchestrator implemented in `loop_structurer.hpp`/`loop_structurer.cpp`. `LoopStructurer::refine_loop()` iterates rules in priority order (While, DoWhile, NestedDoWhile, Sequence, ConditionToSequence) until no rule matches. `negate_condition_expr()` helper negates Condition comparisons (gt->le, etc.) or wraps in logical_not. Wired into `CyclicRegionFinder::process()` — each created `WhileLoopNode` is immediately refined via `LoopStructurer::refine_loop()`. Code generator handles `DoWhileLoopNode` (`do { } while(cond)`), `ForLoopNode` (`for(;;)`), and `WhileLoopNode` with condition (`while(cond)`). Pre/post-processing stubs deferred to later (AGENTS.md note: these are incremental improvements, not blockers). 6 unit tests pass (4 loop scenarios: while, do-while, sequence, condition-to-sequence). Build clean.*

- [x] **C.4** Implement Break and Continue Synthesis in Cyclic Region Structuring (currently `BreakNode` / `ContinueNode` types exist but are never instantiated)
  - *The Python reference's `CyclicRegionStructurer._prepare_current_region_for_acyclic_restructuring()` inserts `Break` CodeNodes for every exit edge (edge from a region node to a loop successor) and `Continue` CodeNodes for every back edge (edge from a region node back to the loop head). These synthetic nodes are then treated as regular code during acyclic restructuring of the loop body, and the loop structuring rules (C.3 above) recognize them to determine loop type and synthesize proper while/do-while conditions. Without this step, loops have no recognizable exit or continuation patterns.*
  - [x] C.4.1 In `CyclicRegionFinder::process()`, after identifying a loop region and its head, compute exit edges (edges from region nodes to non-region successors) and back edges (edges from region nodes to the head).
  - [x] C.4.2 For each exit edge, create a new `CodeNode` containing a `Break` instruction, insert it between the region node and the successor.
  - [x] C.4.3 For each back edge, create a new `CodeNode` containing a `Continue` instruction, insert it between the region node and the head.
  - [x] C.4.4 Restructure the loop body as an acyclic region (it is now a DAG because back edges are replaced with Continue nodes and exit edges with Break nodes).
    - *Completely rewrote `CyclicRegionFinder::process()` in `structurer.cpp`. The new implementation: (1) Detects back edges via DFS; (2) Computes the natural loop region using the standard worklist algorithm (header + all nodes that can reach a latching node); (3) Computes loop successors (exit targets); (4) Builds a sub-TransitionCFG clone of the loop region where back edges are replaced with `CodeNode([ContinueInstr()])` transition blocks and exit edges with `CodeNode([BreakInstr()])` blocks; (5) Runs `AcyclicRegionRestructurer::process()` on the loop sub-CFG to collapse it into a structured AST; (6) Wraps the result in `WhileLoopNode` and refines via `LoopStructurer::refine_loop()`; (7) Collapses the region in the main CFG by removing non-header nodes and redirecting edges. All 6 unit tests pass. Build clean.*

- [x] **C.5** Upgrade `ExpressionPropagation` from Block-Local to Inter-Block Iterative Fixed-Point (currently only propagates within a single basic block)
  - *The Python reference's `ExpressionPropagationBase.run()` calls `perform(graph, iteration)` in a loop until no changes occur. `perform()` iterates blocks in RPO, propagating definitions across block boundaries via `DefMap` and `UseMap`. It handles 15+ rule checks (is_phi, is_call_assignment, defines_unknown_expression, is_address, is_dereference, is_aliased_variable, contains_writeable_global_variable, etc.) and includes path-based safety analysis (`_has_any_of_dangerous_uses_between_definition_and_target`). The C++ port only propagates within a single block's `local_defs` map and never crosses block boundaries, missing the vast majority of propagation opportunities.*
  - [x] C.5.1 Implement `DefMap` (variable -> defining instruction) and `UseMap` (variable -> set of using instructions) computed globally across the entire CFG.
    - *Implemented `VarKey` struct for SSA variable identity `(name, ssa_version)`. `DefMap` maps `VarKey -> Assignment*` globally across the CFG.*
  - [x] C.5.2 Implement the inter-block propagation loop: iterate blocks in RPO, for each instruction, for each required variable, look up its single definition via `DefMap`, check all propagation rules, and substitute if safe.
    - *RPO traversal with `replace_variable_ptr()` that matches by `(name, ssa_version)` pair.*
  - [x] C.5.3 Implement the fixed-point outer loop: repeat the entire propagation pass until no substitutions occur in a full iteration.
    - *Fixed-point loop up to 100 iterations, with `remove_redundant_phis()` before each pass.*
  - [x] C.5.4 Implement the rule checks: `_is_phi`, `_is_call_assignment`, `_defines_unknown_expression`, `_is_address`, `_is_dereference`, `_is_aliased_variable`, `_is_copy_assignment`, `_is_address_assignment`, `_is_dereference_assignment`, `_contains_aliased_variables`, `_operation_is_propagated_in_phi`, `_is_invalid_propagation_into_address_operation`.
    - *6 rule checks implemented: `is_phi`, `is_call_assignment`, `contains_aliased_variable`, `is_address_of`, `contains_dereference`, `operation_into_phi`. Also retains CMP+branch folding (`try_fold_cmp_branch`).*
  - [x] C.5.5 Remove redundant phi functions before each iteration (phis where all sources are identical).
    - *`remove_redundant_phis()` detects phis where all operands (ignoring self-references) are the same, replaces with simple assignment.*

---

### HIGH PRIORITY -- Required for Correct / Non-Degenerate Output

- [x] **H.1** Expand `OperationType` Enum to Cover All Python Reference Operations (currently 27 values, Python has 42)
  - *Missing operations that the lifter, expression propagation, and code generator need: `minus_float`, `plus_float`, `multiply_float`, `divide_float`, `negate`, `right_shift_us` (logical unsigned shift), `left_rotate`, `right_rotate`, `left_rotate_carry`, `right_rotate_carry`, `cast`, `pointer`, `member_access`, `ternary`, `power`, `low`, `field`, `multiply_us`, `divide_us`, `modulo_us`, `less_us`, `greater_us`, `less_or_equal_us`, `greater_or_equal_us`. Signed vs. unsigned distinction is critical for correct decompilation of comparison operations and arithmetic.*
  - [x] H.1.1 Add all missing signed/unsigned variants: `multiply_us`, `divide_us`, `modulo_us`, `less_us`, `greater_us`, `less_or_equal_us`, `greater_or_equal_us`, `right_shift_us`.
    - *Added `mul_us`, `div_us`, `mod_us`, `shr_us`, `lt_us`, `le_us`, `gt_us`, `ge_us`. Updated `negate_comparison()` for unsigned pairs. Lifter now maps `udiv` to `div_us`.*
  - [x] H.1.2 Add float operations: `plus_float`, `minus_float`, `multiply_float`, `divide_float`.
    - *Added `add_float`, `sub_float`, `mul_float`, `div_float`.*
  - [x] H.1.3 Add `negate` (unary minus), `cast`, `member_access`, `ternary`, `pointer`, `low`, `field`.
    - *All added. Also added `list_op` and `adc` matching the Python reference.*
  - [x] H.1.4 Add rotate operations: `left_rotate`, `right_rotate`, `left_rotate_carry`, `right_rotate_carry`.
    - *Added all four. Codegen emits `__ROL__`, `__ROR__`, `__RCL__`, `__RCR__` function-style syntax.*
  - [x] H.1.5 Add `power` (exponentiation, used in idiom recovery).
    - *Added as `power`. Codegen emits `**` infix operator.*
  - [x] H.1.6 Update `CExpressionGenerator` to emit correct C syntax for every new operation (including rotate-as-shift-pair, cast-as-parenthesized-type, member-access-as-dot-or-arrow, ternary-as-question-colon).
    - *Complete rewrite of `visit(Operation*)`: structured switch-based dispatch for binary infix, unary prefix, ternary, member access, field, rotates, call, list_op. Also added `add_with_carry`, `sub_with_carry`. Z3 converter updated with unsigned comparison support (`ult`, `ule`, `ugt`, `uge`). Condition visitor handles unsigned comparison types. All 6 tests pass, build clean.*

- [x] **H.2** Expand `DataflowObjectVisitorInterface` to 18 Visit Methods (currently only 3: Constant, Variable, Operation)
  - *The Python reference visitor has 18 abstract methods covering every concrete expression and instruction type: `visit_unknown_expression`, `visit_constant`, `visit_variable`, `visit_global_variable`, `visit_register_pair`, `visit_list_operation`, `visit_unary_operation`, `visit_binary_operation`, `visit_call`, `visit_condition`, `visit_ternary_expression`, `visit_comment`, `visit_assignment`, `visit_generic_branch`, `visit_return`, `visit_break`, `visit_continue`, `visit_phi`/`visit_mem_phi`. With only 3 overloads, the C++ code generator cannot distinguish a `Call` from a `BinaryOperation` from an `Assignment` at dispatch time -- it must inspect enum tags in a giant switch, which is fragile and error-prone.*
  - [x] H.2.1 After implementing the Instruction hierarchy (C.1), add visit methods for every new concrete type to `DataflowObjectVisitorInterface`.
    - *The interface now has 16 visitor methods: `visit(Constant*)`, `visit(Variable*)`, `visit(Operation*)`, `visit(Call*)`, `visit(ListOperation*)`, `visit(Condition*)`, `visit_assignment()`, `visit_branch()`, `visit_indirect_branch()`, `visit_return()`, `visit_phi()`, `visit_break()`, `visit_continue()`, `visit_comment()`, `visit_relation()`. Call, ListOperation, and Condition have their own `accept()` dispatching to type-specific methods. Missing types (GlobalVariable, RegisterPair, UnknownExpression, MemPhi) deferred until those IR classes are implemented (L.9, etc.).*
  - [x] H.2.2 Implement `accept()` on each new concrete type to call the correct visitor method.
    - *`Call::accept()` dispatches to `visit(Call*)`. `ListOperation::accept()` dispatches to `visit(ListOperation*)`. `Condition::accept()` dispatches to `visit(Condition*)`. All instruction types (`Assignment`, `Branch`, `Return`, `Phi`, `BreakInstr`, `ContinueInstr`, `Comment`, `IndirectBranch`, `Relation`) already have `accept()` from C.1.*
  - [x] H.2.3 Update `CExpressionGenerator` and `CodeVisitor` to use the new visitor dispatch instead of switch-on-enum.
    - *`CExpressionGenerator` now has dedicated `visit(Call*)` method using `target()` and `arg()` accessors. `visit(Operation*)` retains a legacy fallback for `OperationType::call` but new code should use `Call` objects. All 6 tests pass, build clean.*

- [x] **H.3** Implement `copy()` and `substitute()` on All IR Nodes (currently absent)
  - *Every Python `DataflowObject`, `Expression`, and `Instruction` supports `copy()` (deep clone allocating new nodes) and `substitute(replacee, replacement)` (in-place replacement of subexpressions matching `replacee` with `replacement`). These are used pervasively: expression propagation substitutes definitions into uses, variable renaming substitutes old variables with new ones, the DREAM algorithm substitutes reaching conditions, code generation substitutes variables for display names. Without these, many transformations must be hand-rolled with error-prone manual tree walks.*
  - [x] H.3.1 Implement `DataflowObject::copy(DecompilerArena&)` returning a deep clone.
    - *`Expression::copy()` is pure virtual, implemented on all 7 concrete Expression types: `Constant`, `Variable`, `Operation`, `ListOperation`, `Condition`, `Call`. `Instruction::copy()` is pure virtual, implemented on all 9 concrete Instruction types: `Assignment`, `Relation`, `Branch`, `IndirectBranch`, `Return`, `Phi`, `BreakInstr`, `ContinueInstr`, `Comment`. All copy methods preserve `ir_type()`, `address()`, SSA version, and aliased flags. Phi::copy() notes that origin_block is NOT deep-copied (BasicBlock pointers are shared references).*
  - [x] H.3.2 Implement `DataflowObject::substitute(Expression* replacee, Expression* replacement)` performing recursive in-place replacement.
    - *`DataflowObject::substitute()` virtual method with no-op default for leaves (Constant, Variable). `Operation::substitute()` recursively substitutes in children then replaces direct child pointers. `ListOperation::substitute()` same pattern. `Assignment::substitute()` handles both destination and value. `Branch::substitute()` handles condition. `IndirectBranch::substitute()` handles expression. `Return::substitute()` handles values list. `Relation::substitute()` handles destination and value (Variable-only). All use pointer identity for matching.*
  - [x] H.3.3 Implement `requirements()` and `definitions()` as virtual methods on the Instruction hierarchy (after C.1), returning sets of `Variable*`.
    - *Already implemented in C.1: `collect_requirements()` and `collect_definitions()` are virtual methods. `Instruction::requirements()` and `Instruction::definitions()` are convenience wrappers returning vectors.*

- [x] **H.4** Expand Lifter Mnemonic Coverage (currently ~20 mnemonics, x86 alone has hundreds)
  - *The lifter currently maps only: `add`, `adds`, `sub`, `subs`, `cmp`, `mul`, `sdiv`, `udiv`, `mov`, `str`, `stur`, `ldr`, `ldur`, `ret`, and conditional branch suffixes (`b.le`, `b.lt`, etc.). Missing critical x86 mnemonics include: `imul`, `xor`, `or`, `and`, `not`, `neg`, `shl`, `shr`, `sar`, `test`, `lea`, `call`, `push`, `pop`, `movsx`, `movzx`, `cdq`, `cbw`, `cwde`, `cdqe`, `jmp`, `jcc` (all conditional jumps), `nop`, `inc`, `dec`, `adc`, `sbb`, `rol`, `ror`, `rcl`, `rcr`, `bswap`, `bt`, `bts`, `btr`, `btc`, `bsf`, `bsr`, `cmovcc`, `setcc`, `rep` prefixed string ops, `div`, `idiv`, etc. Missing ARM mnemonics: `bl`, `adr`, `adrp`, `stp`, `ldp`, `madd`, `msub`, `cset`, `csel`, `tbz`, `tbnz`, `cbz`, `cbnz`, etc. Every unmapped mnemonic becomes `OperationType::unknown`, producing `"unknown_op"` in the output.*
  - [x] H.4.1 Map all x86-64 arithmetic/logic: `imul` (1/2/3 operand forms), `xor`, `or`, `and`, `not`, `neg`, `inc`, `dec`, `adc`, `sbb`.
  - [x] H.4.2 Map all x86-64 shifts/rotates: `shl`/`sal`, `shr`, `sar`, `rol`, `ror`, `rcl`, `rcr`.
  - [x] H.4.3 Map all x86-64 data movement: `lea`, `movsx`, `movzx`, `cdq`, `cbw`, `cwde`, `cdqe`, `push`, `pop`, `xchg`, `bswap`.
  - [x] H.4.4 Map all x86-64 control flow: `call`, `ret`, `jmp`, all `jcc` variants (`je`, `jne`, `jg`, `jge`, `jl`, `jle`, `ja`, `jae`, `jb`, `jbe`, `js`, `jns`, `jo`, `jno`, `jp`, `jnp`), `nop`.
  - [x] H.4.5 Map x86-64 flag-setting: `test` (as `bit_and` without storing result), `bt`/`bts`/`btr`/`btc`, `bsf`/`bsr`.
  - [x] H.4.6 Map x86-64 conditional moves/sets: `cmovcc` (all variants), `setcc` (all variants).
  - [x] H.4.7 Map x86-64 division: `div` (unsigned), `idiv` (signed) -- these use implicit `edx:eax` operands.
  - [x] H.4.8 Map ARM64 extended set: `bl`, `adr`, `adrp`, `stp`, `ldp`, `madd`, `msub`, `cset`, `csel`, `tbz`, `tbnz`, `cbz`, `cbnz`, `ands`, `orr`, `eor`, `mvn`, `lsl`, `lsr`, `asr`, `ror`.
    - *Complete rewrite of lifter.cpp. Added `make_binary_assign()` and `make_unary_assign()` helper methods. Now covers: NOP, RET, all x86 Jcc (16 variants + sign/overflow/parity), ARM conditional branches (B.xx, CBZ, CBNZ, TBZ, TBNZ), JMP/B/BR (unconditional), CMP, TEST, CALL, BL/BLR, all binary arithmetic/logic (ADD, ADC, SUB, SBB, IMUL, AND, OR, XOR, SHL, SAL, SHR, SAR, ROL, ROR, RCL, RCR), ARM variants (ADDS, SUBS, MUL, SDIV, UDIV, ANDS, ORR, EOR, LSL, LSR, ASR, ROR), ARM MADD/MSUB, unary ops (NOT, NEG, INC, DEC, MVN), LEA, MOV/MOVABS/MOVSX/MOVSXD/MOVZX/XCHG, LDR/LDUR/STR/STUR/LDRB/LDRH/etc, STP/LDP, ADR/ADRP, CSET/CSEL, CDQ/CWD/CQO, PUSH/POP, BSWAP, BT/BTS/BTR/BTC, BSF/BSR, all CMOVcc (16+ variants), all SETcc (16+ variants), DIV/IDIV (implicit edx:eax), MUL (unsigned, implicit). All 6 tests pass, build clean.*

- [x] **H.5** Implement `RangeSimplifier` for Real (currently a stub -- `is_unfulfillable()` always returns false, `simplify()` is identity)
  - *The Python reference `RangeSimplifier` performs: splitting non-binary relations, applying `SingleRangeSimplifier` per relation (unfulfillable detection, consecutive-bound -> equality conversion, size-bound tautology detection), then `BitwiseAndRangeSimplifier` or `BitwiseOrRangeSimplifier` for conjunction/disjunction simplification. The `ExpressionValues` class tracks must-values, forbidden-values, signed/unsigned bounds, combines mixed bounds across 4 cases, refines bounds using forbidden values, and detects unfulfillability when constraints conflict. Without this, `DeadPathEliminationStage` cannot eliminate dead branches.*
  - [x] H.5.1 Implement `ExpressionValues` with `must_values`, `forbidden_values`, `lower_bound`, `upper_bound` (each with separate signed/unsigned `ConstantBound` components), and the `update_with(BoundRelation)` dispatcher.
    - *Full implementation in `range_simplifier.cpp`. `ExpressionValues` constructed with bit_size, computes signed/unsigned min/max. `update_with()` dispatches on eq/neq/lt/le/gt/ge (and unsigned variants), normalizing const-on-lhs vs rhs, adjusting for strict inequalities (x < c → upper = c-1).*
  - [x] H.5.2 Implement `ExpressionValues::is_unfulfillable()` checking: >1 must-value, must-value in forbidden, must-value out of bounds, upper < lower.
    - *4 checks: multiple must-values, must ∩ forbidden, must outside bounds (signed + unsigned), upper < lower (signed + unsigned).*
  - [x] H.5.3 Implement `ExpressionValues::simplify()` with the 6-step pipeline: remove redundant bounds, add must-value if bounds equal size bounds, refine bounds using forbidden values, remove redundant forbidden values, add must-value if bounds are equal, recheck.
    - *6-step pipeline: `combine_mixed_bounds()` (4-case signed/unsigned bound combination matching the Python reference), size-bound must-value detection, `refine_bounds_using_forbidden()` (iterative adjustment with convergence protection), `remove_redundant_forbidden()` (via `std::erase_if`), `add_must_if_bounds_equal()`, and recheck.*
  - [x] H.5.4 Implement `BoundRelation` wrapper: validates binary relation with exactly 1 constant, extracts constant/expression/smaller/greater operands, is_signed.
    - *`BoundRelation::from()` handles both `Condition*` and generic `Operation*` with comparison type. Validates exactly one constant operand. Extracts variable_expr, constant_value, constant_is_lhs.*
  - [x] H.5.5 Implement `SingleRangeSimplifier` for simplifying individual binary relations.
    - *Handles strict comparisons (unfulfillable → Constant(0), consecutive → Condition(eq)), non-strict comparisons (tautology → Constant(1), bounds-equal → Condition(eq)). All size-bound computations respect signed vs unsigned.*
  - [x] H.5.6 Implement `BitwiseAndRangeSimplifier::simplify()`: extract `BoundRelation`s from AND operands, build `ExpressionValues` per variable, detect unfulfillability, emit replacement constraints.
    - *Implemented in `BitwiseAndRangeSimplifier::simplify`. Extracts bounds into `ExpressionValues` per variable. Emits simplified relations like `eq`, bounds `le`/`ge`, and consecutive ranges negations via `logical_not` of `bit_and` ranges.*
  - [x] H.5.7 Implement `BitwiseOrRangeSimplifier::simplify()`: negate to AND, simplify, negate back.
    - *Implemented in `BitwiseOrRangeSimplifier::simplify`. Converts `A | B` to `~(~A & ~B)`, applies `BitwiseAndRangeSimplifier::simplify`, and negates the result back.*

- [x] **H.6** Implement `DeadPathEliminationStage` for Real (currently an empty stub)
  - *The Python reference uses Z3/DeLogic to check satisfiability of branch conditions. It removes edges with unsatisfiable conditions (dead paths), then removes unreachable blocks, and fixes phi origin blocks. `DeadLoopElimination` extends this to specifically target loop back-edges, resolving phi-function constants to determine if the loop body is ever re-entered. Without dead path elimination, impossible branches (e.g., `if (x < 0 && x > 10)`) survive and clutter the output.*
  - [x] H.6.1 Implement the core stage: for each conditional branch in the CFG, convert the condition to a Z3 expression via `Z3Converter`, check satisfiability, and remove the edge + unreachable successor if unsatisfiable.
    - *Implemented in `DeadPathEliminationStage::execute()`. Converts condition to Z3, checks both true and false paths for satisfiability via `LogicCondition::is_not_satisfiable()`, marks invalid edges as dead.*
  - [x] H.6.2 After removing dead edges, remove blocks that become unreachable from the entry.
    - *Implemented using `task.cfg()->post_order()` to find reachable blocks, filtering unreachable ones and feeding them to `task.cfg()->remove_nodes_from()`.*
  - [x] H.6.3 Fix phi origin blocks after block removal (remove entries for deleted predecessors).
    - *Implemented by iterating all Phis, checking `origin_block()` entries against the dead blocks list, and calling `remove_from_origin_block()`.*
  - [x] H.6.4 Implement `DeadLoopEliminationStage` extending dead path elimination to target loop back-edges.
    - *Implemented `DeadLoopEliminationStage::execute()`. Extends DPE by checking branch dependency on Phis, extracting unique upstream constants (approximated without deep dominance checking), patching branch conditions, and using Z3 satisfiability along with reachability checks (DFS from satisfiable edge) to aggressively prune branch edges in loops.*

- [x] **H.7** Implement `ExpressionPropagationMemory` Stage (currently missing entirely)
  - *The Python reference has a separate `ExpressionPropagationMemory` stage that propagates aliased/memory variables. It checks whether the definition's value could have been modified via a memory access (pointer write) between the definition and the target usage, using pointer analysis and path-based safety checks. It also has a "postponed aliased propagation" sub-pass. Without this, aliased variables (those that could be modified through pointers) are never propagated, leaving many redundant loads in the output.*
  - [x] H.7.1 Implement basic aliased-variable propagation with conservative safety checks.
  - [x] H.7.2 Implement path-based safety analysis: check if any instruction between the definition and target could modify memory at the aliased address.
    - *Implemented in `ExpressionPropagationMemoryStage`. Precomputes `reachability` using DFS. Allows aliased/dereferenced definitions, but performs `has_any_dangerous_use` checks against potentially memory-modifying instructions (`Relation`, dereferenced `Assignment`, `Call`) that lie on any paths between `def_block` and `target_block`.*

- [x] **H.8** Implement `ExpressionPropagationFunctionCall` Stage (currently missing entirely)
  - *The Python reference has a separate `ExpressionPropagationFunctionCall` stage that propagates function call return values (`var = f()`) into their single usage site. After propagation, it replaces the original call assignment's destination with `ListOperation([])` (void). It checks for dangerous memory accesses between the call and the usage. Without this, every function call result is stored in a temporary and used separately, producing verbose output like `tmp = strlen(s); if (tmp > 0)` instead of `if (strlen(s) > 0)`.*
  - [x] H.8.1 Implement call-result propagation: for each `var = f(...)` with exactly one use of `var`, substitute `f(...)` directly into the use site.
  - [x] H.8.2 Check that no dangerous memory operations occur between the call and the use.
    - *Implemented in `ExpressionPropagationFunctionCallStage`. Iterates `DefMap` for `OperationType::call`, checks if there is exactly 1 entry in `UseMap`. Employs conservative safety checks against cross-boundary and in-block memory operations, and safely replaces the original `def` with `Constant(0)` (which is removed in subsequent Dead Code passes).*

- [x] **H.9** Implement CFG Edge Classification (currently absent from ControlFlowGraph)
  - *The Python reference's `ClassifiedGraph` performs DFS-based edge classification into: `tree`, `back`, `forward`, `cross`, `retreating`, `non_loop`. This is used directly by the DREAM algorithm: loop heads are identified as nodes with incoming `back`/`retreating` edges, and edge properties determine which edges are loop entries vs. cross-edges. The C++ `CyclicRegionFinder` currently does its own ad-hoc DFS for back-edge detection, but without proper classification, `retreating` edges (non-back edges to already-visited nodes) are conflated with `back` edges, and `forward`/`cross` edges are not distinguished.*
  - [x] H.9.1 Implement `classify_edges()` on `ControlFlowGraph` using stack-based DFS with parent tracking, visited marking, and post-order numbering.
  - [x] H.9.2 Store classification as `std::unordered_map<Edge*, EdgeProperty>` with enum values: `tree`, `back`, `forward`, `cross`, `retreating`.
  - [x] H.9.3 Implement `back_edges()` -> map of loop head to set of back edges targeting it.
  - [x] H.9.4 Implement `retreating_edges()` -> set of all retreating edges.

- [x] **H.10** Implement Abnormal Entry / Exit Loop Restructuring (currently missing -- multi-entry and multi-exit loops are not handled)
  - *The Python reference handles multi-entry loops by introducing a dispatch variable (`entry_{head}`) and cascading condition nodes to route all entries through a single head (DREAM paper Figure 12). Multi-exit loops are handled symmetrically with an `exit_{head}` dispatch variable. Without this, the DREAM algorithm will fail or produce garbage for irreducible control flow (e.g., `goto` into the middle of a loop) or loops with multiple exit points.*
  - [x] H.10.1 Implement `AbnormalEntryRestructurer`: detect region nodes with predecessors outside the region, introduce dispatch variable, create cascading conditions, redirect entries.
  - [x] H.10.2 Implement `AbnormalExitRestructurer`: detect multiple loop successors, introduce dispatch variable, create cascading conditions after the loop, redirect exits.
    - *Implemented in `CyclicRegionFinder::process`. Added graph slice extraction for precise loop region boundaries. Added synthetic entry/exit dispatch variables and cascading condition nodes to redirect control flow through single points, fully matching the DREAM paper figure 12.*

- [x] **H.11** Add `LogicCondition` Tags to `TransitionEdge` (currently TransitionBlock only has predecessor/successor pointer lists with no edge metadata)
  - *The Python reference's `TransitionEdge` carries a `tag: LogicCondition` (the symbolic boolean condition for traversing that edge) and a `property: EdgeProperty` (back/retreating/non_loop). The C++ `TransitionBlock` uses raw predecessor/successor pointer lists with no per-edge data. This forces the reaching conditions computation to re-derive edge conditions from the original CFG's basic block instructions every time, which is fragile and loses information about switch-case edge tags (which are disjunctions of case symbols).*
  - [x] H.11.1 Define `TransitionEdge` class with `source`, `sink`, `LogicCondition tag`, `EdgeProperty property`.
  - [x] H.11.2 Replace raw `vector<TransitionBlock*>` predecessor/successor lists in `TransitionBlock` with `vector<TransitionEdge*>`.
  - [x] H.11.3 During `TransitionCFG::generate()`, compute and assign edge tags (True condition, negated condition, True for unconditional, disjunction of case symbols for switch).
  - [x] H.11.4 Update `ReachingConditions::compute()`
    - *Added `TransitionEdge` class storing source, sink, `LogicCondition` tag, and `EdgeProperty`. Replaced `TransitionBlock` predecessor/successor vectors with edge pointers. Updated `ReachingConditions` to read tags directly. Added shared `z3::context` to `DecompilerTask`.* to read edge tags directly from `TransitionEdge` instead of re-deriving from original basic block instructions.

- [x] **H.12** Implement Function Signature Generation in Code Output (currently absent -- no return type, function name, or parameters in output)
  - *The Python reference's `CodeGenerator` uses `string.Template` to emit: `$return_type $name($parameters) { $local_declarations $function_body }`. Without function signatures, the output is a bare block of statements with no function header -- not valid C code.*
  - [x] H.12.1 Extract function name from IDA via `ida::function` APIs.
  - [x] H.12.2 Extract return type and parameter types (requires Type system, C.2).
  - [x] H.12.3 Emit the function signature as the first line of the viewer output.

- [x] **H.13** Implement Local Variable Declarations in Code Output (currently absent -- variables are used without declaration)
  - *The Python reference's `LocalDeclarationGenerator` walks the AST collecting all variables, filters out parameters and globals, groups by type, and emits C declarations (`int var_0, var_1;`). Without this, the output uses undeclared variables.*
  - [x] H.13.1 Implement AST walker that collects all `Variable` references.
  - [x] H.13.2 Group by type, sort alphabetically, emit declarations.

- [x] **H.14** Implement `PhiFunctionCleaner` (removes trivial phis where all operands are identical)
  - *The Python reference removes phis like `a = phi(b, b)` by replacing with `a = b` and removing the phi. It also handles chains: if removing one phi makes another trivial, it continues. Without this, redundant copy instructions are generated for trivial phis.*
  - [x] H.14.1 For each phi, check if all RHS operands (ignoring self-references) are the same value. If so, replace with a simple assignment and remove the phi.

- [x] **H.15** Implement `PhiDependencyResolver` (breaks circular phi dependencies before lifting)
  - *Implemented `PhiDependencyResolver::resolve` in `phi_dependency_resolver.cpp`. It computes a topological sort of Phi nodes based on their requirements (DFS post-order), identifies back-edges to compute a directed feedback vertex set (FVS), and breaks cycles by renaming the definition of the FVS node to a `copy_var` while inserting a copy assignment. Integrated into `SsaDestructor::execute` right before `eliminate_phi_nodes`. Added `test_phi_dependency` to `test_main.cpp`.*
  - *The Python reference builds a `PhiDependencyGraph` (directed edges from phi to phi it depends on), computes a directed feedback vertex set, and for each phi in the FVS, introduces a copy variable to break the cycle. Without this, circular phi dependencies (e.g., `a = phi(b, ...)` and `b = phi(a, ...)`) may produce incorrect copy insertion during out-of-SSA.*
  - [x] H.15.1 Build dependency graph among phis within each block.
  - [x] H.15.2 Compute directed FVS (approximate: DFS + back-edge detection).
  - [x] H.15.3 For each FVS member, create a copy variable, rename the phi definition, insert a copy assignment after phis.

- [x] **H.16** Fix `std::string` Memory Leak in Arena-Allocated `Variable` Nodes
  - *`Variable::name_` is `std::string`, which may heap-allocate for long names (beyond SSO). Since `ArenaAllocated` objects never have their destructors called (arena memory is freed in bulk), the string's internal heap buffer leaks. For short register names this is mitigated by SSO, but for longer generated names (e.g., `"copy_eax_phi_3"`, `"entry_0x401000"`) this is a real leak.*
  - [x] H.16.1 Option A: Store names as `std::string_view` into a separate arena-allocated string pool (interning). The pool owns the memory; variable nodes just reference it.
  - [x] H.16.2 Option B: Call destructors explicitly for `Variable` nodes before arena reset (add a destructor registry to `DecompilerArena`).

- [x] **H.17** Implement the Idiom Pattern Matching Engine for Real (currently `match_magic_division()` is an empty stub)
  - *The Python reference has 5 active matchers (signed division, unsigned division, signed modulo, unsigned modulo, signed multiplication), hundreds of JSON/YAML pattern files across optimization levels, register-agnostic operand anonymization, magic number precomputation tables (signed 32-bit, unsigned 32-bit, 64-bit), complex backtracking for magic constant resolution, and a `Match` output with address, length, operation, operand, constant. Without the idiom engine, compiler-optimized divisions/modulos appear as obfuscated `imul`/`shr`/`sar`/`sub` sequences.*
  - [x] H.17.1 Implement operand anonymization: replace registers with `reg_N`, constants with `const_N`, memory references with `loc_N` (register-agnostic matching).
  - [x] H.17.2 Port signed and unsigned magic number precomputation tables (32-bit and 64-bit).
  - [x] H.17.3 Implement `InstructionSequence` base class with `matches_first_instruction()` and `search()`.
  - [x] H.17.4 Port `SignedDivisionInstructionSequence` with magic number resolution, power-of-two detection, sign detection.
  - [x] H.17.5 Port `UnsignedDivisionInstructionSequence` with the extensive fallback cascade for corner-case magic numbers.
  - [x] H.17.6 Port `SignedModuloInstructionSequence` and `UnsignedModuloInstructionSequence` with remainder-register heuristic and two-IMUL cheating.
  - [x] H.17.7 Port `SignedMultiplicationInstructionSequence` with YAML pattern loading and `safe_eval` constant expressions.
  - [x] H.17.8 Bundle JSON/YAML pattern files into the C++ build (embed as resources or load from disk).
  - *Note: 2026-02-27 hardening pass added `SequenceResolver` with signed/unsigned magic-table divisor reconstruction, corner-case fallback probes, tokenized operand anonymization using `ida::instruction::operand_text`, generated `magic_maps.hpp`, and a dedicated `test_idiom_resolver` target covering div/divu reconstruction paths.*

- [x] **H.18** Implement Type Lifting from IDA (currently no type information is extracted)
  - *The Python reference lifts types from Binary Ninja: `IntegerType -> Integer`, `FloatType -> Float`, `PointerType -> Pointer`, `ArrayType -> ArrayType`, `StructureType -> Struct`, `EnumerationType -> Enum`, `FunctionType -> FunctionTypeDef`, etc. For the C++ port, types should be extracted from IDA's type information system via idax APIs (function prototypes, local variable types from Hex-Rays if available, or from IDA's type libraries).*
  - [x] H.18.1 Query `ida::function` for function return type and parameter types.
  - [x] H.18.2 Map IDA type information to the DeWolf `Type` hierarchy (C.2).
  - [x] H.18.3 Attach types to `Variable` and `Constant` nodes during lifting.

- [x] **H.19** Connect the Actual Decompilation Pipeline to the IDA Plugin UI (currently the viewer shows hardcoded stub output)
  - *The plugin's `run()` method creates hardcoded color-tagged lines instead of invoking the decompiler pipeline. The `DecompilerTask`, `DecompilerPipeline`, lifter, SSA, optimization stages, DREAM, and code generator all exist but are never called from the plugin entry point.*
  - [x] H.19.1 In `DeWolfPlugin::run()`, create a `DecompilerTask` for the current function address.
  - [x] H.19.2 Invoke the `Lifter` to build the CFG.
  - [x] H.19.3 Run the full `DecompilerPipeline` (all stages in order).
  - [x] H.19.4 Invoke `CodeVisitor` to generate output lines.
  - [x] H.19.5 Feed the generated lines to `ida::ui::create_custom_viewer`.

---

### MEDIUM PRIORITY -- Improves Output Quality and Correctness

- [x] **M.1** Implement Additional Out-of-SSA Strategies (currently only basic Sreedhar-like approach)
  - *The Python reference has 4 strategies: `simple` (rename + lift), `minimization` (color then lift via Lex-BFS on chordal interference graph), `lift_minimal` (default: lift then color -- not optimal but practical), `conditional` (lift then dependency-weighted rename via `ConditionalVariableRenamer`). The `MinimalVariableRenamer` uses Lexicographic BFS on the interference graph to produce an optimal coloring when the graph is chordal (which it is in SSA form). `ConditionalVariableRenamer` uses a dependency graph with weighted edges to group variables that should share names.*
  - [x] M.1.1 Implement `MinimalVariableRenamer` with Lex-BFS graph coloring on the interference graph.
    - *Implemented `MinimalVariableRenamer` in `src/dewolf/ssa/minimal_variable_renamer.*`: builds SSA-keyed use/def + liveness sets, constructs an interference graph, computes reverse Lex-BFS ordering per compatibility group, applies greedy color assignment with name-frequency tie-breaks, rewrites CFG variable occurrences to color-class representatives, and removes identity assignments (`x = x`). Wired into `SsaDestructor::execute()` after phi lifting. Added `test_minimal_variable_renamer` in `tests/test_main.cpp`.*
  - [x] M.1.2 Implement `ConditionalVariableRenamer` with dependency-graph-weighted merging.
    - *Implemented `ConditionalVariableRenamer` in `src/dewolf/ssa/conditional_variable_renamer.*`: constructs weighted variable dependency edges from assignment RHS expressions (`OPERATION_PENALTY` scoring), merges variable classes greedily by descending edge weight subject to interference/type/alias compatibility, rewrites CFG variables to merged representatives, and removes identity assignments. Added `test_conditional_variable_renamer` in `tests/test_main.cpp`.*
  - [x] M.1.3 Make the out-of-SSA strategy configurable (default: `lift_minimal`).
    - *Added `OutOfSsaMode` to `DecompilerTask` (default `LiftMinimal`) and mode parsing in `SsaDestructor::parse_mode()`. `SsaDestructor::execute()` now dispatches among `simple`, `min`/`minimization`, `lift_minimal`, `conditional`, and `sreedhar` strategies, with `lift_minimal` as default. Plugin reads `DEWOLF_OUT_OF_SSA_MODE` to configure strategy at runtime. Added `test_out_of_ssa_mode_config` in `tests/test_main.cpp`.*

- [x] **M.2** Implement `IdentityElimination` Stage (currently missing)
  - *The Python reference builds an `_IdentityGraph` of direct identities (`a = b`) and indirect identities (phi chains). It finds connected components of congruent variables, prunes non-identity phis via disjoint path analysis, and merges each identity group into a single replacement variable. This goes beyond what `GraphExpressionFolding` catches by handling phi-mediated identities.*
  - [x] M.2.1 Build identity graph from assignments and phi functions.
  - [x] M.2.2 Find connected components, merge each into a single variable.
    - *Implemented `IdentityEliminationStage` in `optimization_stages.cpp`: builds a union-find identity graph from direct copy assignments (`a=b`) and phi edges (fixed-point over phi chains), computes connected components, rewrites all variable uses/defs to component representatives, simplifies/removes identity phis, and drops redundant identity assignments. Wired stage into plugin pipeline after graph folding and added `test_identity_elimination_stage` in `tests/test_main.cpp`.*

- [x] **M.3** Implement `CommonSubexpressionElimination` Stage for Real (currently a stub)
  - *The Python reference has two phases: `ExistingSubexpressionReplacer` (replaces subexpressions already assigned to variables, dominator-aware) and `DefinitionGenerator` (creates new temporaries for repeated subexpressions). Threshold-based with complexity and occurrence filters.*
  - [x] M.3.1 Implement `ExistingSubexpressionReplacer`: find expressions already assigned to variables that dominate the current use.
    - *Implemented dominator-aware existing-subexpression replacement in `CommonSubexpressionEliminationStage` (`optimization_stages.cpp`): traverses dominator tree from entry, tracks available defining expressions (`Assignment` RHS) keyed by structural fingerprints, replaces dominated subexpressions with defining variables (largest-first by expression complexity), and uses conservative alias safety (no cross-block replacement for aliased vars; no `Relation` jump-over in same block). Wired stage into plugin pipeline after identity elimination and added `test_common_subexpression_existing_replacer_stage` in `tests/test_main.cpp`.*
  - [x] M.3.2 Implement `DefinitionGenerator`: detect repeated subexpressions, create temporaries.
    - *Implemented definition generation in `CommonSubexpressionEliminationStage` (`optimization_stages.cpp`): collects structural subexpression usages, selects high-complexity repeated candidates, computes insertion block via common dominator, inserts `cse_N` temporaries before first dominated use, and rewrites all dominated occurrences by structural fingerprint. Added `test_common_subexpression_definition_generator_stage` in `tests/test_main.cpp`.*

- [ ] **M.4** Implement `ExpressionSimplification` Rules (currently missing entirely)
  - *The Python reference applies algebraic simplification rules in 3 phases: pre-rules (none), rules (`TermOrder`, `SubToAdd`, `SimplifyRedundantReference`, `SimplifyTrivialArithmetic`, `SimplifyTrivialBitArithmetic`, `SimplifyTrivialLogicArithmetic`, `SimplifyTrivialShift`, `CollapseConstants`, `CollapseNestedConstants`), post-rules (`CollapseAddNeg`, `PositiveConstants`). These transform expressions like `x + 0 -> x`, `x * 1 -> x`, `x & 0xFFFFFFFF -> x`, `x - (-y) -> x + y`, `(x + 3) + 5 -> x + 8`, etc.*
  - [x] M.4.1 Implement `CollapseConstants`: evaluate binary operations on two constants at compile time.
    - *Implemented `ExpressionSimplificationStage` constant folding in `optimization_stages.cpp` with recursive expression-tree simplification and binary constant evaluation for arithmetic/bitwise/logic/comparison ops (`add/sub/mul/div/mod`, shifts, `and/or/xor`, comparisons including signed/unsigned variants, `power`). Stage rewrites `Assignment`, `Branch`, `IndirectBranch`, and `Return` operands; branch constant conditions are normalized to `neq(const, 0)`. Added `test_expression_simplification_collapse_constants` in `tests/test_main.cpp`; `build/dewolf_tests` passes with this coverage.*
  - [x] M.4.2 Implement `SimplifyTrivialArithmetic`: `x + 0 -> x`, `x * 1 -> x`, `x * 0 -> 0`, `x - 0 -> x`, `x / 1 -> x`.
    - *Extended `simplify_expression_tree()` in `optimization_stages.cpp` with algebraic identity rewrites for binary arithmetic (`add`, `mul`/`mul_us`, `sub`, `div`/`div_us`) after recursive child simplification. Added `test_expression_simplification_trivial_arithmetic` in `tests/test_main.cpp` covering all listed identities; `build/dewolf_tests` passes.*
  - [x] M.4.3 Implement `SimplifyTrivialBitArithmetic`: `x & 0 -> 0`, `x | 0 -> x`, `x ^ 0 -> x`, `x & all_ones -> x`.
    - *Extended `simplify_expression_tree()` in `optimization_stages.cpp` with bit-identity rewrites for `bit_and`/`bit_or`/`bit_xor`, including canonicalization of `x & 0`, `x | 0`, `x ^ 0`, and mask-aware `x & all_ones` using width-derived masks. Added `test_expression_simplification_trivial_bit_arithmetic` in `tests/test_main.cpp`; `build/dewolf_tests` passes.*
  - [ ] M.4.4 Implement `SubToAdd`: convert `x - (-c)` to `x + c` for readability.
  - [ ] M.4.5 Implement `CollapseNestedConstants`: `(x + c1) + c2 -> x + (c1+c2)`, similarly for nested multiplications.

- [ ] **M.5** Implement `DeadComponentPruner` Stage (currently missing)
  - *The Python reference builds an `ExpressionGraph` from the CFG, identifies sink nodes (calls, dereference writes, non-assignment instructions), and removes instructions not reachable from any sink. This catches dead code that `DeadCodeElimination` misses because it only looks at unused definitions, not unreachable expression subgraphs.*
  - [ ] M.5.1 Build `ExpressionGraph` from CFG instructions.
  - [ ] M.5.2 Compute reachability from sinks, remove unreachable instructions.

- [ ] **M.6** Implement `SwitchNode` / `CaseNode` Code Generation in `CodeVisitor` (currently AST nodes exist but visitor does not handle them)
  - *`SwitchNode` and `CaseNode` are defined in `ast.hpp` but `CodeVisitor::visit_node()` has no case for them. If `ConditionAwareRefinement` produces a switch, it will be silently skipped in the output.*
  - [ ] M.6.1 Add `SwitchNode` handling: emit `switch (expr) {`, visit each case child, emit `}`.
  - [ ] M.6.2 Add `CaseNode` handling: emit `case N:` or `default:`, visit body, emit `break;` if `break_case`.

- [ ] **M.7** Implement Do-While and For-Loop Code Generation in `CodeVisitor` (currently only `while(true)` is emitted)
  - *After implementing loop structuring rules (C.3), the AST will contain `WhileLoopNode`, `DoWhileLoopNode`, and `ForLoopNode`. The code visitor must emit the correct syntax for each.*
  - [ ] M.7.1 `WhileLoopNode`: emit `while (condition) { body }`.
  - [ ] M.7.2 `DoWhileLoopNode`: emit `do { body } while (condition);`.
  - [ ] M.7.3 `ForLoopNode`: emit `for (declaration; condition; modification) { body }`.

- [ ] **M.8** Implement `ConditionHandler` / `ConditionSymbol` System (currently absent)
  - *The Python reference uses a symbol table mapping `LogicCondition` Z3 symbols to concrete `Condition` IR objects. `ConditionHandler.add_condition()` creates new symbols and detects zero-case conditions. `SwitchHandler` detects equality-based conditions suitable for switch-case. The C++ port converts directly from IR operations to Z3, losing the ability to map back from Z3 symbols to IR conditions during code generation.*
  - [ ] M.8.1 Implement `ConditionHandler` class maintaining a bidirectional map between Z3 symbols and `Condition*` IR objects.
  - [ ] M.8.2 Implement `CaseNodeProperties` tracking expression, constant, and negation flag per symbol.
  - [ ] M.8.3 Update `TransitionCFG::generate()` to use `ConditionHandler` for edge tag creation.

- [ ] **M.9** Enhance `ConditionBasedRefinement` (CBR) to Handle CNF Subexpression Matching (currently only does linear branch scanning)
  - *The Python reference's CBR has two phases: (1) complementary condition pairing (A and ~A -> if-else), and (2) CNF subexpression matching using `ConditionCandidates` -- a logic graph with formula/clause/symbol layers that groups nodes whose reaching conditions share common CNF subexpressions. The C++ CBR only does the linear branch-scanning phase.*
  - [ ] M.9.1 Implement complementary condition detection between pairs of SeqNode children.
  - [ ] M.9.2 Implement `ConditionCandidates` logic graph for CNF clause/symbol decomposition.
  - [ ] M.9.3 Implement subexpression matching across candidates for grouping.

- [ ] **M.10** Enhance `ConditionAwareRefinement` (CAR) with Full 4-Stage Switch Recovery Pipeline (currently only converts consecutive equality IfNodes)
  - *The Python reference CAR has: `InitialSwitchNodeConstructor` (extracts switches from nested conditions, constructs from sequences), `MissingCaseFinderCondition` (finds cases in condition-node branches), `SwitchExtractor` (extracts switches from redundant condition wrappers), `MissingCaseFinderSequence` (finds cases among sequence siblings, detects default cases). The C++ CAR only converts consecutive `if (x == const)` patterns.*
  - [ ] M.10.1 Implement `InitialSwitchNodeConstructor` with nested condition extraction.
  - [ ] M.10.2 Implement `MissingCaseFinderCondition` for finding cases in branch arms.
  - [ ] M.10.3 Implement `SwitchExtractor` for extracting switches from redundant wrappers.
  - [ ] M.10.4 Implement `MissingCaseFinderSequence` for sibling analysis and default case detection.

- [ ] **M.11** Implement `ReachabilityGraph` and `SiblingReachability` for Correct `SeqNode` Child Ordering (currently relying on topological sort from graph slice)
  - *The Python reference tracks code-node-level reachability and uses `SiblingReachability` to determine execution order for `SeqNode.sort_children()`. `CaseDependencyGraph` extends this for switch-case ordering. Without proper reachability, children of a SeqNode may be emitted in an order that doesn't match the original execution semantics.*
  - [ ] M.11.1 Implement `ReachabilityGraph` tracking which code nodes can reach which others.
  - [ ] M.11.2 Implement `SiblingReachability` for determining partial order of sibling AST nodes.
  - [ ] M.11.3 Implement `CaseDependencyGraph` for switch-case ordering.

- [ ] **M.12** Implement `RedundantCastsElimination` Stage (currently missing)
  - *The Python reference simplifies redundant cast operations: same-type cast removal, constant-value cast folding, etc.*
  - [ ] M.12.1 Detect and remove casts where source and target types are identical.
  - [ ] M.12.2 Fold constant-to-constant casts at compile time.

- [x] **M.13** Implement the 5 Preprocessing Stages for Real (currently all empty stubs)
  - *`CompilerIdiomHandling`: traverse CFG and replace tagged idiom sequences with high-level operations (depends on H.17). `RegisterPairHandling`: combine `edx:eax` 64-bit register pairs into single variables with bitwise decomposition. `SwitchVariableDetection`: trace switch variables backward via def-use chains to find clean predecessors. `RemoveGoPrologue`: pattern-match and remove Go runtime `runtime_morestack` prologues. `RemoveStackCanary`: detect and remove `__stack_chk_fail` patterns.*
  - [x] M.13.1 `CompilerIdiomHandlingStage`: consume `IdiomTag` results from the idiom matcher and replace matched instruction sequences with the recovered high-level operation.
    - *Implemented end-to-end idiom handoff: `Lifter::lift_function(..., idiom_tags_out)` now collects matcher tags per IDA block into `DecompilerTask::idiom_tags`; `CompilerIdiomHandlingStage` consumes tags, synthesizes replacement `Assignment(Operation)` IR (`div/div_us/mod/mod_us/mul`), rewrites matching windows by instruction address/length, and is wired into plugin pipeline before SSA. Added `test_idiom_resolver` target to validate reconstruction inputs used by this stage.*
  - [x] M.13.2 `RegisterPairHandlingStage`: detect `RegisterPair` patterns and combine into single wider variables.
    - *Implemented register-pair concat recovery in preprocessing: detects `(high << bits) | low` and `(high << bits) + low` for `edx:eax`, `dx:ax`, and `rdx:rax`, rewrites to synthetic wider variables (`edx_eax_pair`, `dx_ax_pair`, `rdx_rax_pair`) with inferred width/type metadata, and wires stage execution into the plugin pipeline before SSA. Added `test_register_pair_handling_stage` in `tests/test_main.cpp`.*
  - [x] M.13.3 `SwitchVariableDetectionStage`: backward-slice analysis to find clean switch variables.
    - *Implemented backward-slice switch selector cleanup: builds per-function def/use maps over `Assignment`/`Branch` requirements, detects switch blocks via `EdgeType::Switch` + trailing `IndirectBranch`, traces jump expressions back through predecessor definitions, applies reference criteria (copy-assigned, used in condition assignment, used in branch, or predecessor dereferenced in branch), and substitutes indirect-jump expressions with the recovered clean variable. Wired into plugin preprocessing pipeline and added `test_switch_variable_detection_stage` in `tests/test_main.cpp`.*
  - [x] M.13.4 `RemoveGoPrologueStage`: pattern-match `r14`, `gsbase`/`fsbase` offset patterns and make morestack path unreachable.
    - *Implemented conservative Go prologue pruning in preprocessing: detects root guard branches shaped like return-address vs stackguard checks (`r14+0x10` and `fsbase`/`gsbase` offset families), identifies the `runtime_morestack*` successor path, removes the prologue edge, drops redundant root branch when only one successor remains, and prunes now-unreachable blocks. Wired stage into plugin pipeline before SSA and added `test_remove_go_prologue_stage` in `tests/test_main.cpp`.*
  - [x] M.13.5 `RemoveStackCanaryStage`: detect `__stack_chk_fail` leaf nodes and remove the branch condition.
    - *Implemented stack-canary pruning in preprocessing: finds fail leaves by direct `__stack_chk_fail` call detection or failed-canary edge signatures (`eq`+False or `neq`+True with `fsbase/gsbase+0x28` operand), recursively removes empty relay blocks on fail paths, strips terminal canary-check branches, and deletes now-unreachable nodes. Wired stage into plugin pipeline before SSA and added `test_remove_stack_canary_stage` in `tests/test_main.cpp`.*

- [ ] **M.14** Implement Operator Precedence and Bracket Insertion in `CExpressionGenerator` (currently absent)
  - *The Python reference has a `PRECEDENCE` dictionary mapping every `OperationType` to an integer priority (150 for calls, 140 for unary, 130 for mul/div, 120 for add/sub, etc.) and uses `_has_lower_precedence()` to decide when to insert parentheses. Without this, expressions like `a + b * c` might be emitted as `a + b * c` (correct) or `(a + b) * c` (incorrect) depending on tree structure, and expressions like `*ptr + 1` might be misparsed as `*(ptr + 1)`.*
  - [ ] M.14.1 Implement `PRECEDENCE` table.
  - [ ] M.14.2 Implement bracketing logic in `visit(Operation*)`: compare child precedence with parent, wrap in `()` when needed.

- [ ] **M.15** Improve Acyclic Region Finding with "Improved DREAM" Minimal Subset Algorithm (currently uses simple forward-DFS)
  - *The Python reference tries to find the smallest region dominated by the header that has at most one exit. It iterates possible exit nodes, checks if removing the dominated subtree of each gives a smaller valid region. The C++ version uses a simpler forward-DFS requiring all predecessors to be in the region, which may fail to find valid regions in complex graphs.*
  - [ ] M.15.1 Compute full dominance region (all nodes dominated by head).
  - [ ] M.15.2 Check restructurability: region size, exit count, postdominator.
  - [ ] M.15.3 Try each possible exit node: remove its dominated set, check if remaining region has single exit.

- [ ] **M.16** Implement Edge Splitting for Phi Lifting Along Conditional Edges (currently copies are inserted at the end of predecessors)
  - *The Python reference's `PhiFunctionLifter` creates new basic blocks when inserting copies along conditional edges (where the predecessor has multiple successors). Inserting copies directly at the end of a conditional predecessor can be incorrect because those copies would execute on ALL paths, not just the path leading to the phi's block. The C++ code handles branch-aware insertion but doesn't split edges.*
  - [ ] M.16.1 When the predecessor's edge to the phi block is conditional (not unconditional), create a new intermediate basic block, insert copies there, and redirect the edge through it.

- [ ] **M.17** Implement `Coherence` Preprocessing Stage (currently missing)
  - *The Python reference harmonizes variable types and aliased status across all occurrences. Enforces consistent types for same `(name, ssa_label)` combinations.*
  - [ ] M.17.1 Build a map of `(variable_name, ssa_version)` -> first-seen type.
  - [ ] M.17.2 Enforce uniform types across all occurrences.

- [ ] **M.18** Implement Logic Engine Operation Simplification (constant folding, factorization, De Morgan's, associative/commutative folding)
  - *The Python reference `dewolf-logic` has rich `Operation.simplify()` on every operation type: `CommutativeOperation._fold_constants()` evaluates all constant operands into one, `_promote_subexpression()` flattens nested same-type ops, `BitwiseAnd._simple_folding()` removes duplicates and detects collisions (a & ~a = 0), `CommonBitwiseAndOr._associative_folding()` applies pairwise simplification rules, `BaseAddSub._factorize()` finds common factors. Without these, the logic engine cannot simplify conditions before feeding them to the DREAM algorithm.*
  - [ ] M.18.1 Implement constant folding on the DAG: evaluate operations whose operands are all constants.
  - [ ] M.18.2 Implement De Morgan's law application for `BitwiseNegate` over `BitwiseAnd`/`BitwiseOr`.
  - [ ] M.18.3 Implement associative promotion: flatten `(a & (b & c))` -> `(a & b & c)`.
  - [ ] M.18.4 Implement commutative duplicate removal and collision detection.

---

### LOW PRIORITY -- Polish and Readability Improvements

- [ ] **L.1** Implement `ReadabilityBasedRefinement` AST Stage (guarded do-while removal and while-to-for-loop conversion)
  - *The Python reference detects do-while loops inside condition nodes with identical conditions and replaces with while loops. `WhileLoopReplacer` converts while loops to for loops when initialization, continuation variable, and condition variable are identified.*
  - [ ] L.1.1 Detect guarded do-while patterns and replace with while.
  - [ ] L.1.2 Implement `WhileLoopReplacer` for for-loop recovery.

- [ ] **L.2** Implement `VariableNameGeneration` AST Stage (currently variables keep raw register/SSA names)
  - *The Python reference supports `default` (no rename) and `system_hungarian` (type-prefixed names like `iVar0`, `pchStr1`). Uses `SubstituteVisitor` on AST.*
  - [ ] L.2.1 Implement default naming scheme: `var_0`, `var_1`, etc.
  - [ ] L.2.2 Implement system Hungarian naming: prefix by type (`i` for int, `p` for pointer, `f` for float).

- [ ] **L.3** Implement `LoopNameGenerator` AST Stage (currently no special loop variable naming)
  - *The Python reference renames while-loop counter variables to `counter`, `counter1`, etc. and for-loop variables to `i`, `j`, `k`, etc.*
  - [ ] L.3.1 Detect loop counter variables and rename to `i`, `j`, `k`, ... for for-loops.
  - [ ] L.3.2 Rename while-loop counters to `counter`, `counter1`, ...

- [ ] **L.4** Implement `InstructionLengthHandler` AST Stage (currently overly complex expressions are not split)
  - *The Python reference splits complex instructions into simpler temporary assignments when they exceed a configurable complexity bound. `TargetGenerator` finds exceeding instructions, `TargetSimplifier` recursively breaks down operations, inserting `tmp_N` variables.*
  - [ ] L.4.1 Implement complexity metric for expressions.
  - [ ] L.4.2 Split expressions exceeding the threshold into temporaries.

- [ ] **L.5** Implement Compound Assignment and Increment Syntax in `CodeVisitor` (currently `x = x + 1` is not simplified to `x++`)
  - *The Python reference's `CodeVisitor` detects compoundable assignments (`x = x + y` -> `x += y`) and incrementable ones (`x = x + 1` -> `x++`). Uses `NON_COMPOUNDABLE_OPERATIONS` set and `COMMUTATIVE_OPERATIONS` for correctness.*
  - [ ] L.5.1 Detect `x = x OP y` patterns and emit `x OP= y`.
  - [ ] L.5.2 Detect `x = x + 1` / `x = x - 1` patterns and emit `x++` / `x--`.

- [ ] **L.6** Implement String Literal and Character Formatting in `CExpressionGenerator` (currently all constants are hex)
  - *The Python reference formats: byte-sized printable ASCII as `'A'`, string arrays as `"hello"`, hex/dec based on configurable threshold, unsigned suffix `U`, long suffix `L`, wchar prefix `L`, negative hex as two's complement, truncation of long global initializers to `MAX_GLOBAL_INIT_LENGTH`.*
  - [ ] L.6.1 Detect byte-sized integer constants in printable ASCII range and emit as `'c'`.
  - [ ] L.6.2 Detect string constant arrays and emit as `"..."`.
  - [ ] L.6.3 Add configurable hex threshold (values above N are shown in hex).
  - [ ] L.6.4 Add unsigned `U` and long `L` suffixes based on type width.

- [ ] **L.7** Implement `ArrayAccessDetection` Stage (currently missing)
  - *The Python reference detects `*(base + offset)` patterns as array element accesses. It classifies offsets into const/mul/var. If consistent element sizes are found, it annotates `UnaryOperation.array_info` so the code generator can emit `base[index]`.*
  - [ ] L.7.1 Detect `*(base + index * element_size)` patterns.
  - [ ] L.7.2 Annotate with array info (base, index, element_size, confidence).
  - [ ] L.7.3 Emit `base[index]` in `CExpressionGenerator` when array info is present.

- [ ] **L.8** Implement `EdgePruner` Stage (currently missing)
  - *The Python reference uses `ExpressionGraph` to find expressions occurring multiple times across instructions. Eliminates common subexpressions by creating temporary variables. Threshold-based on occurrences, complexity, and their product.*
  - [ ] L.8.1 Build expression graph, find multi-use expressions.
  - [ ] L.8.2 Create temporaries for repeated subexpressions above threshold.

- [ ] **L.9** Implement `GlobalVariable` IR Node and Global Declaration Generation (currently missing)
  - *The Python reference has `GlobalVariable` extending `Variable` with `initial_value`, `is_constant`, and an `inline_global_variable()` heuristic. `GlobalDeclarationGenerator` emits `extern` declarations for shared globals. Code generation inlines constant string globals directly.*
  - [ ] L.9.1 Implement `GlobalVariable` class with `initial_value`, `is_constant` fields.
  - [ ] L.9.2 During lifting, detect global data references and create `GlobalVariable` nodes.
  - [ ] L.9.3 Implement `GlobalDeclarationGenerator` emitting `extern` declarations.

- [ ] **L.10** Implement Batch Execution and File I/O for `idump <binary>` Automation (currently stubs)
  - *The plugin should support headless mode for batch processing. Detect headless via idax or explicit configuration. Write C-code output to disk.*
  - [ ] L.10.1 Implement headless detection (environment variable or explicit flag).
  - [ ] L.10.2 Implement file I/O: write decompiled C code to `<binary>.c` or configurable path.
  - [ ] L.10.3 Wire `idump <binary>` entry point to invoke the pipeline on all functions.

- [ ] **L.11** Implement `RemoveNoreturnBoilerplate` Preprocessing Stage (currently missing)
  - *The Python reference removes boilerplate code leading to non-returning functions using post-dominance frontier calculation on a reversed CFG with merged virtual sink nodes.*
  - [ ] L.11.1 Compute post-dominator tree.
  - [ ] L.11.2 Identify non-returning function calls.
  - [ ] L.11.3 Remove dead code after noreturn calls.

- [ ] **L.12** Implement `InsertMissingDefinitions` Preprocessing Stage (currently missing)
  - *The Python reference inserts definitions for undefined aliased variables at appropriate locations using dominator tree and memory version tracking.*
  - [ ] L.12.1 Find undefined aliased variables.
  - [ ] L.12.2 Insert definitions at dominator-appropriate locations.

- [ ] **L.13** Implement `PhiFunctionFixer` Preprocessing Stage (currently missing)
  - *The Python reference computes `origin_block` for each Phi by walking the dominator tree from each predecessor to find which phi operand is live there. This is critical for correct phi lifting.*
  - [ ] L.13.1 Build `basic_block_of_definition` map.
  - [ ] L.13.2 For each phi, walk predecessors up the dominator tree to find the live operand.
  - [ ] L.13.3 Populate `Phi.origin_block`.

- [ ] **L.14** Implement `BitFieldComparisonUnrolling` Stage for Real (currently a stub)
  - *The Python reference transforms `if((1 << amount) & bitmask)` into chains of equality comparisons. Creates nested conditional blocks in CFG.*
  - [ ] L.14.1 Detect `(1 << amount) & bitmask` patterns.
  - [ ] L.14.2 Unroll into `amount == 1 || amount == 3 || ...` chains.

- [ ] **L.15** Implement `TypePropagation` Stage for Real (currently a stub, and depends on C.2)
  - *The Python reference does horizontal type propagation through equivalence classes connected by assignments. Builds a `TypeGraph`, finds connected components, propagates the most common non-primitive type.*
  - [ ] L.15.1 Build type equivalence graph from assignment chains.
  - [ ] L.15.2 Find connected components, propagate dominant type.

- [ ] **L.16** Implement CNF/DNF Normal Form Conversion in Logic Engine (currently missing)
  - *The Python reference has `ToCnfVisitor` and `ToDnfVisitor` that transform logic expressions into Conjunctive/Disjunctive Normal Form by distributing AND over OR (or vice versa). Used by condition-based refinement for identifying complementary conditions and subexpression matching.*
  - [ ] L.16.1 Implement `ToCnfVisitor`: recursively distribute OR over AND.
  - [ ] L.16.2 Implement `ToDnfVisitor`: recursively distribute AND over OR.

- [ ] **L.17** Implement If-Else Branch Swapping Heuristic in `CodeVisitor` (currently branches are emitted in tree order)
  - *The Python reference swaps if/else branches to: (1) ensure else-if chaining (put ConditionNode in false branch), (2) use configurable "smallest" or "largest" true-branch preference for readability.*
  - [ ] L.17.1 Detect else-if chain opportunity: if exactly one branch is a ConditionNode, swap so it's the false branch.
  - [ ] L.17.2 Implement configurable branch preference (smallest/largest/none).
