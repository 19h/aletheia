--- START OF FILE Paste March 02, 2026 - 3:56PM ---
# Aletheia C++ / IDA Port Specification & Agent Task Tracker
**CRITICAL DIRECTIVE**: All progress must be reflected in the nested to-dos immediately. Append brief progress notes beneath tasks.
---
## 🤖 AGENT ONBOARDING & EXECUTION PROTOCOL
**Role**: Autonomous AI software engineer porting the DeWolf/Aletheia Python ecosystem to a native C++23 IDA Pro plugin.
1. **Constraints**: Strictly use `idax` C++ wrapper (`/Users/int/dev/idax`). NO raw IDA SDK headers (`pro.h`, `ida.hpp`). Use C++23 stdlib and `Result<T>`/`Status` error handling.
2. **NO `dynamic_cast`**: Strictly forbidden (causes O(N) string comparisons on macOS). Use LLVM-style RTTI:
   - **Dataflow**: `NodeKind`, `isa<T>`, `cast<T>` (`dataflow.hpp`).
   - **AST**: `AstKind`, `ast_isa<T>`, `ast_cast<T>` (`ast.hpp`).
   - **DAG**: `DagKind`, `dag_isa<T>`, `dag_cast<T>` (`dag.hpp`).
   - **Type**: `TypeKind`, `type_isa<T>`, `type_cast<T>` (`types.hpp`).
   - **Edge**: `EdgeKind` (`edge->edge_kind() == EdgeKind::SwitchEdge`).
3. **Algorithmic Efficiency**: Always evaluate Big-O time/space complexity for every data structure, algorithm, and allocation. Be highly creative and intelligent: achieve maximum functionality with the absolute minimum overhead. Avoid redundant copies, prefer cache-friendly flat structures, and aggressively leverage the Arena allocator to eliminate allocation bottlenecks.
4. **Next Task**: Find the first unchecked task (`- [ ]`).
5. **Execute**: Be proactive. Document hard blockers and pivot if necessary.
6. **Update State**: Check off (`- [x]`) and append terse completion notes immediately.
---
## Project Context & Objectives
* **Source (DeWolf)**: Python-based research decompiler (DREAM/DREAM++ algorithms) translating assembly to C-like ASTs. Includes `dewolf` (Core/CFG/DREAM), `dewolf-logic` (VSA/math), `dewolf-idioms` (compiler artifacts).
* **Objective**: Native C++23 IDA plugin via `idax`. Drop Python, Binary Ninja, SMDA, NetworkX. Use custom Arena allocators, native DAGs, and support headless/GUI modes.
* **idax Integration**: Opaque wrapper. Path B (Native IDA Disassembly) is mandatory: iterate `BasicBlock`s, decode via `ida::instruction::decode()`, map to IR. Bypasses Hex-Rays.
---
## Skeleton Phase Tracker (COMPLETED)
<details>
<summary>Click to expand completed skeleton phases 1-6 (initial scaffolding)</summary>
- [x] **Phase 1**: Build system, Arena memory, Core IR (`DataflowObject`, `Operation`), CFG foundations.
- [x] **Phase 2**: `liblogos` (DAG, World stubs), `libidiomata` (IdiomMatcher stub).
- [x] **Phase 3**: Lifter (flowchart/decode), SSA (Cooper dominators, Cytron phi, basic out-of-SSA).
- [x] **Phase 4**: Pipeline architecture, Preprocessing stubs, CFG Optimization stubs.
- [x] **Phase 5**: AST Foundations, Cyclic/Acyclic structuring (DREAM stubs).
- [x] **Phase 6**: C-Backend, IDA UI (`create_custom_viewer`), Batch/CLI stubs.
</details>
---
## PRODUCTION GAP TRACKER: Reference-Faithful Implementation
*Current estimated implementation depth vs. Python reference: 100% (Parity Reached).*
### CRITICAL PRIORITY
- [x] **C.1 `Instruction` Type Hierarchy**: Replaced flat `Operation` enum-dispatch with polymorphic hierarchy to enable semantic dispatch.
  - *Implemented*: `Instruction` base, `Assignment`, `Branch`, `Return`, `Phi`, `Break`, `Continue`, `Comment`, `IndirectBranch`, `Relation`. Refactored pipeline/SSA/codegen to use visitors.
- [x] **C.2 `Type` System**: Replaced `size_bytes` with full hierarchy for C declarations, casts, and pointer analysis.
  - *Implemented*: `Type` base, `Unknown`, `Integer`, `Float`, `Pointer`, `Array`, `Custom` (void/bool), `Function`, `Complex` (Struct/Union/Enum). Added standard factories and `TypeParser` for IDA strings.
- [x] **C.3 DREAM Loop Structuring Rules**: Replaced generic `while(true)` with accurate loop types.
  - *Implemented*: `WhileLoopNode`, `DoWhileLoopNode`, `ForLoopNode`. Added `WhileLoopRule`, `DoWhileLoopRule`, `NestedDoWhileLoopRule`, `SequenceRule`, `ConditionToSequenceRule`, and `LoopStructurer` orchestrator.
- [x] **C.4 Break/Continue Synthesis**: Inserted synthetic nodes for exit/back edges to enable acyclic restructuring.
  - *Implemented*: Exit/back edge computation in `CyclicRegionFinder`, synthetic `Break`/`Continue` insertion, and DAG restructuring.
- [x] **C.5 Inter-Block Expression Propagation**: Upgraded from block-local to iterative fixed-point.
  - *Implemented*: Global `DefMap`/`UseMap`, RPO traversal, fixed-point loop (max 100 iters), 6 core rule checks, and redundant phi removal.
### HIGH PRIORITY
- [x] **H.1 `OperationType` Enum Expansion**: Added missing signed/unsigned/float/bitwise ops.
  - *Implemented*: Unsigned arithmetic/comparisons, floats, negate, cast, member_access, ternary, pointer, low, field, rotates, power. Updated `CExpressionGenerator`.
- [x] **H.2 `DataflowObjectVisitorInterface`**: Expanded from 3 to 18 methods.
  - *Implemented*: Full visitor coverage for all concrete types, `accept()` dispatch, removed enum switches in codegen.
- [x] **H.3 `copy()` & `substitute()`**: Enabled deep cloning and in-place AST replacement.
  - *Implemented*: Recursive `copy(DecompilerArena&)` and `substitute()` across all IR nodes.
- [x] **H.4 Lifter Mnemonic Coverage**: Expanded x86/ARM coverage.
  - *Implemented*: x86 (`imul`, `xor`, `or`, `and`, `not`, `neg`, `shl/r`, `rol/r`, `lea`, `movsx/zx`, `jcc`, `test`, `cmovcc`, `div`, `idiv`), ARM (`bl`, `adr`, `stp`, `madd`, `cset`, `tbz`, etc.).
- [x] **H.5 `RangeSimplifier` (VSA)**: Implemented real VSA for dead path elimination.
  - *Implemented*: `ExpressionValues` (must/forbidden/bounds), unfulfillability checks, 6-step simplification, `BoundRelation`, `Single/BitwiseAnd/BitwiseOrRangeSimplifier`.
- [x] **H.6 `DeadPathEliminationStage`**: Pruned unsatisfiable branches via Z3.
  - *Implemented*: Z3 satisfiability checks, unreachable block removal, phi origin fixes, and `DeadLoopEliminationStage`.
- [x] **H.7 `ExpressionPropagationMemory`**: Safely propagated aliased/memory variables.
  - *Implemented*: Path-based memory safety checks and DFS reachability.
- [x] **H.8 `ExpressionPropagationFunctionCall`**: Inlined single-use call results.
  - *Implemented*: Call-result substitution with memory safety checks.
- [x] **H.9 CFG Edge Classification**: DFS classification for DREAM loop detection.
  - *Implemented*: `classify_edges()` (tree, back, forward, cross, retreating), `back_edges()`, `retreating_edges()`.
- [x] **H.10 Abnormal Entry/Exit Restructuring**: Handled multi-entry/exit loops.
  - *Implemented*: `AbnormalEntryRestructurer` and `AbnormalExitRestructurer` via synthetic dispatch variables.
- [x] **H.11 `LogicCondition` Tags**: Attached symbolic conditions to edges.
  - *Implemented*: `TransitionEdge` with tags/properties, updated `TransitionCFG` and `ReachingConditions`.
- [x] **H.12 Function Signatures**: Emitted valid C headers.
  - *Implemented*: IDA API extraction for name/return/params, emitted signature in codegen.
- [x] **H.13 Local Declarations**: Emitted C variable declarations.
  - *Implemented*: AST variable walker, type grouping, and declaration emission.
- [x] **H.14 `PhiFunctionCleaner`**: Removed trivial `a = phi(b, b)` phis.
- [x] **H.15 `PhiDependencyResolver`**: Broke circular phi dependencies.
  - *Implemented*: Dependency graph, directed FVS, and copy variable insertion.
- [x] **H.16 Arena String Leak**: Fixed `std::string` leak in `Variable`.
  - *Implemented*: LIFO destructor registry in `DecompilerArena`.
- [x] **H.17 Idiom Engine**: Matched compiler optimizations (magic division, etc.).
  - *Implemented*: Operand anonymization, 32/64-bit magic tables, signed/unsigned div/mod, multiplication, bundled JSON/YAML patterns.
- [x] **H.18 Type Lifting**: Extracted IDA types.
  - *Implemented*: `ida::function` prototype queries, mapped to `Type` hierarchy, attached to variables.
- [x] **H.19 UI Integration**: Connected pipeline to IDA viewer.
  - *Implemented*: `DecompilerTask`, `Lifter`, `DecompilerPipeline`, `CodeVisitor`, `create_custom_viewer`.
### MEDIUM PRIORITY
- [x] **M.1 Out-of-SSA Strategies**: Added `MinimalVariableRenamer` (Lex-BFS) and `ConditionalVariableRenamer`.
- [x] **M.2 `IdentityElimination`**: Merged identity graphs and phi chains.
- [x] **M.3 Common Subexpression Elimination**: Added `ExistingSubexpressionReplacer` and `DefinitionGenerator`.
- [x] **M.4 `ExpressionSimplification`**: Added constant folding, trivial arithmetic/bitwise, SubToAdd, and nested constants.
- [x] **M.5 `DeadComponentPruner`**: Removed dead code via backward reachability from sinks.
- [x] **M.6 Switch/Case Codegen**: Emitted `switch`, `case`, `break`.
- [x] **M.7 Loop Codegen**: Emitted `while`, `do-while`, `for`.
- [x] **M.8 `ConditionHandler`**: Mapped Z3 symbols to IR conditions for switch-case metadata.
- [x] **M.9 CBR Enhancement**: Added complementary conditions and CNF subexpression matching.
- [x] **M.10 CAR Enhancement**: Added 4-stage switch recovery (nested, missing cases, redundant wrappers).
- [x] **M.11 `ReachabilityGraph`**: Enforced correct AST child ordering (Sibling/Case dependencies).
- [x] **M.12 `RedundantCastsElimination`**: Removed no-op casts and folded constant casts.
- [x] **M.13 Preprocessing Stages**: Implemented Idioms, RegisterPairs, SwitchVars, GoPrologue, and StackCanary removal.
- [x] **M.14 Precedence & Brackets**: Added `PRECEDENCE` table and auto-parentheses in codegen.
- [x] **M.15 Acyclic Region Finding**: Upgraded to minimal subset algorithm (Improved DREAM).
- [x] **M.16 Edge Splitting**: Split conditional edges for safe phi lifting.
- [x] **M.17 `Coherence` Stage**: Enforced uniform types across SSA versions.
- [x] **M.18 Logic Simplification**: Added constant folding, De Morgan's, and associative/commutative flattening to DAG.
### LOW PRIORITY
- [x] **L.1 Readability Refinement**: Added guarded do-while and while-to-for conversions.
- [x] **L.2 Variable Naming**: Added default (`var_N`) and System Hungarian naming.
- [x] **L.3 Loop Naming**: Renamed counters to `i/j/k` and `counter`.
- [x] **L.4 `InstructionLengthHandler`**: Split complex expressions into temporaries.
- [x] **L.5 Compound/Increment Syntax**: Emitted `+=`, `++`, `--`.
- [x] **L.6 Literals**: Formatted chars, strings, hex thresholds, and U/L suffixes.
- [x] **L.7 Array Access**: Detected `*(base + idx*sz)` and emitted `base[idx]`.
- [x] **L.8 `EdgePruner`**: Extracted multi-use expressions into temporaries.
- [x] **L.9 Globals**: Added `GlobalVariable` IR and `extern` declarations.
- [x] **L.10 Batch/CLI**: Implemented `idump` headless mode and file I/O.
- [x] **L.11 Noreturn Boilerplate**: Pruned dead code post-noreturn via post-dominators.
- [x] **L.12 Missing Definitions**: Inserted synthetic defs for aliased vars via dominator tree.
- [x] **L.13 `PhiFunctionFixer`**: Populated `origin_block` via predecessor dominator trees.
- [x] **L.14 BitField Unrolling**: Converted `(1<<x)&mask` to equality chains.
- [x] **L.15 `TypePropagation`**: Propagated dominant types across assignment graphs.
- [x] **L.16 CNF/DNF Conversion**: Added normal form conversion in logic engine.
- [x] **L.17 If-Else Swapping**: Added else-if chaining and size heuristics.
### POST-CHECKLIST PARITY AUDIT (2026-02-27)
- [x] **P.0 Parity Audit**: Completed against Python reference.
- [x] **P.1 Wire Optimization Stages**: Wired all missing stages into plugin/`idump` pipelines in reference order.
- [x] **P.2 Fix `idump` Headless Timeout**: Optimized idiom matcher hot-path and added fallback AST sequencing.
- [x] **P.3 Switch-Edge Semantics**: Propagated case-value sets from lifter to `TransitionCFG` edge tags.
- [x] **P.4 Explicit Branch Truth Mapping**: Inferred true/false edges from explicit targets, not successor indices.
- [x] **P.5 Harden `Z3Converter`**: Expanded op coverage and replaced hard-false fallbacks with symbolic placeholders.
- [x] **P.6 `MemPhi` Parity**: Added `MemPhi` IR and `MemPhiConverterStage` for aliased paths.
- [x] **P.7 Unknown-Op Placeholders**: Emitted explicit placeholders for unknown ops instead of dropping lines.
- [x] **P.8 Pipeline Tracking**: Added stage dependency validation and exception-safe tracking.
- [x] **P.9 `World::map_condition()`**: Implemented deep structural DAG mapping with memoization.
- [x] **P.10 `BitwiseAndRangeSimplifier`**: Implemented DAG conjunction-range simplification.
- [x] **P.11 Wire Legacy `RangeSimplifier`**: Connected legacy API to new DAG AND-range simplifier.
- [x] **P.12 Stabilize Structured Mode (`idump`)**: Fixed 6 crash/hang causes:
  - (1) Arena destructor leak (fixed via LIFO execution).
  - (2) Null deref in `negate_condition_expr()`.
  - (3) CBR node-dropping on empty branches.
  - (4) Cyclic force-collapse dropping nodes (fixed by stripping back-edges first).
  - (5) O(V^2*E) dominance (replaced with Cooper dominator tree).
  - (6) Null edge derefs in `classify_edges()`.
  - *Follow-ups*: Added COW expression propagation and iteration safeguards.
- [x] **P.13 Stack/Local/Parameter Recovery**: Mapped stack/frame references to stable locals.
  - *Implemented*: `VariableKind`, `FrameLayout`, `resolve_frame_variable()`, SP-delta integration, calling-convention parameter mapping, and `LocalDeclarationGenerator` filtering.
- [x] **P.14 Collapse Nested Sub Chains**: Folded `sub(sub(x, c1), c2)` into `sub(x, c1 + c2)`.
- [x] **P.15 Canonicalize Linear Add/Sub Chains**: Rewrote chained assignments into direct base-plus-constant form.
- [x] **P.16 SP-Aware Push/Pop Lowering**: Resolved implicit SP-relative slots to named stack variables instead of raw `*(rsp)` derefs.
- [x] **P.17 CodeVisitor CFG-Fallback Gotos**: Emitted explicit `if (...) goto` and fallthrough `goto` for flat CFG AST roots.
### DEEP PARITY AUDIT GAPS (Discovered Mar 02, 2026)
- [x] **A.1 AST Structuring Pre/Post-Processors**: Implement missing `AstProcessor` and `LoopProcessor` mutations.
  - *Implemented*: `extract_conditional_interruption`, `clean_node`, `combine_cascading_breaks`, and injected `preprocess/postprocess` into `LoopStructurer` and `AcyclicRegionRestructurer` to flatten degenerate `if(cond){break;}` structures.
- [x] **A.2 AST-Level Expression Simplification (`ExpressionSimplificationAst`)**: Run simplification on the final AST.
- [x] **A.3 `logos` Logic DAG Integration & Arithmetic Factorization**: 
  - *Implemented*: Explicitly bypassed `LogicDag` arithmetic scaffolding to save bloat, instead injecting a native C++ recursive arithmetic factorizer directly into `simplify_expression_tree` via structural `expr_fingerprint` matching to correctly fold `c1*X + c2*X -> (c1+c2)*X`.
- [x] **A.4 Deep Expression Simplification Rules**: Implement `TermOrder` and `CollapseAddNeg`.
  - *Implemented*: Injected `TermOrder` (sorting constants to RHS for commutative/relational ops) and `CollapseAddNeg` (`x + (-y) -> x - y`) directly into the `simplify_expression_tree` traversal for immediate O(1) deduplication wins.
- [x] **A.5 CAR: Intersecting Constants Switch Recovery**: Implement `MissingCaseFinderIntersectingConstants`.
  - *Implemented*: Logic added to extract missing cases optimized via bitwise/interval boundary overlaps (represented as logical OR eq chains in C++) and merge them back into the `SwitchNode`.
- [x] **A.6 Ancillary Guardrails**: `EmptyBasicBlockRemover` & AST Dataflow Validation.
  - *Implemented*: `EmptyBasicBlockRemoverStage` added immediately prior to restructuring to prune semantically empty basic blocks that otherwise break the acyclic restructuring reachability paths.
- [x] **A.7 Fix `AcyclicRegionRestructurer` AST Dropping**: Fix pointer invalidation and force collapse logic.
  - *Implemented*: Fixed `TransitionCFG::entry_` being set to `nullptr` during subset collapsing due to deletion order. Fixed the `else` branch (force collapse) to actually emit the collapsed block to `cfg` so `AcyclicRegionRestructurer` completes correctly and produces a valid AST tree instead of detached nodes.
- [x] **A.8 Fix `GraphSlice` Cycle Dropping**: Removed Kahn's topological sort from `GraphSlice` slice builder.
  - *Implemented*: Kahn's algorithm was silently dropping cyclic nodes from slices (as their in-degree never reached 0). Changed `GraphSlice` to just insert the DFS-discovered nodes directly into the slice CFG.
- [x] **A.9 Fix `CyclicRegionFinder` Abnormal Entry Missing Nodes**: Added missing `loop_region->add_block` calls.
  - *Implemented*: When restructuring abnormal loop entries, the synthetic `new_head` and dispatch `condition_nodes` were not being added to the internal `loop_region` CFG. This caused the subsequent `loop_cfg` extraction to fail mapping the head node, silently aborting cyclic structuring and leaving the loop body completely flat.
- [x] **A.10 Refine `generated_output_too_lossy` Fallback Heuristic**: Fixed overly aggressive fallback threshold.
  - *Implemented*: The heuristic (`emitted * 2 < lifted`) was triggering fallbacks on perfectly structured ASTs because optimization and constant folding often compresses instructions by >50%. Replaced with a more robust metric that counts unresolved `goto` and `/* branch if` tokens to accurately detect unstructured messes instead of just counting line reductions.

### DEEP AST STRUCTURING PARITY AUDIT (Discovered Mar 02, 2026)
- [x] **S.1 `extract_conditional_interruption` Block Flattening**: Fixed the C++ AST processor to correctly collapse the extracted branch when breaking conditional returns/breaks.
- [x] **S.2 Deep `combine_cascading_breaks` Evaluation**: Upgraded the cascading break combiner to evaluate reachability downwards using `does_end_with_break` instead of shallow `is_break_node`.
- [x] **S.3 Aggressive Break Lifting (`while_over_do_while`)**: Implemented aggressive forward-floating of break conditions in sequence nodes.
  - *Context*: Currently C++ only floats a break condition if it's the very last element. Python floats *any* break condition to the front of the block if sibling reachability permits, which is mandatory for transforming raw assembly back-edges into canonical `while(cond)` loops rather than `do { ... if (cond) break; } while(1)`.
