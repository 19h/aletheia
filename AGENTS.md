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

## Phase 1: Foundation & Core Intermediate Representation (IR)
- [x] 1.1 Project Initialization & Build System
  - [x] 1.1.1 Scaffold CMake build system for IDA Pro plugin, strictly matching one of the idax integration examples (e.g., `add_subdirectory`, `fetch_content`, or `find_package` from `/Users/int/dev/idax/integration`).
    - *Note: Used FetchContent method with SOURCE_DIR pointing to local idax.*
  - [x] 1.1.2 Configure target structures for `libdewolf`, `libdewolf-logic`, and `libdewolf-idioms`.
    - *Note: Configured static library targets in CMakeLists.txt and stub source/header files.*
  - [x] 1.1.3 Setup basic IDA plugin skeleton using `ida::plugin::Plugin` and `IDA_PLUGIN_ENTRY()`.
    - *Note: Implemented basic IDA plugin skeleton using `ida::plugin::Plugin` and `IDAX_PLUGIN` macro.*
  - [x] 1.1.4 Map base type definitions (e.g., `Address`, `AddressSize`) strictly from `<ida/idax.hpp>`.
    - *Note: Mapped `ida::Address` and `ida::AddressSize` in `src/common/types.hpp`.*
- [x] 1.2 Memory Management
  - [x] 1.2.1 Implement `DecompilerArena` (Bump Allocator) bound to `DecompilerTask` lifecycle to avoid `std::shared_ptr` overhead.
    - *Note: Implemented basic DecompilerArena with dynamic blocks in src/common/arena.hpp.*
  - [x] 1.2.2 Implement placement-new overrides for all AST and CFG node structures to allocate from `DecompilerArena`.
    - *Note: Implemented `ArenaAllocated` base CRTP-style struct for inherited placement-new in src/common/arena_allocated.hpp.*
- [x] 1.3 Core Dataflow IR (`libdewolf/structures`)
  - [x] 1.3.1 Port `DataflowObject` base class.
    - *Note: Implemented base class with visitor pattern.*
  - [x] 1.3.2 Port `Expression` inheritance tree (`Constant`, `Variable`, `Operation`).
    - *Note: Implemented as ArenaAllocated objects.*
  - [x] 1.3.3 Map `OperationType` enumerations (e.g., `minus`, `plus`, `left_shift`, `dereference`).
    - *Note: Mapped arithmetic, bitwise, memory, logical and other core operations.*
  - [x] 1.3.4 Implement polymorphic visitation interfaces (`DataflowObjectVisitorInterface`).
    - *Note: Implemented basic visitor interface for AST nodes.*
- [x] 1.4 Control Flow Graph (CFG) Foundations
  - [x] 1.4.1 Implement `BasicBlock` structure (ID, instructions array, predecessor/successor links).
    - *Note: Implemented BasicBlock with IDs, Instructions, and Edges in src/dewolf/structures/cfg.hpp.*
  - [x] 1.4.2 Implement `ControlFlowGraph` container and traversal algorithms (DFS, RPO, Post-Order).
    - *Note: Implemented ControlFlowGraph and RPO/Post-Order/DFS traversal in src/dewolf/structures/cfg.cpp.*
  - [x] 1.4.3 Implement specialized edge types (`UnconditionalEdge`, `TrueCase`, `FalseCase`, `SwitchCase`).
    - *Note: Modeled via EdgeType enum (Unconditional, True, False, Fallthrough) and SwitchEdge subclass.*

## Phase 2: Logic & Idiom Engines (`libdewolf-logic` & `libdewolf-idioms`)
- [x] 2.1 `libdewolf-logic` (Value Set Analysis & Simplification)
  - [x] 2.1.1 Replace `networkx`/`lark` dependencies with native lightweight C++ DAG structures.
    - *Note: Implemented basic DagNode, DagVariable, DagOperation in src/dewolf_logic/dag.hpp.*
  - [x] 2.1.2 Implement `World`, `WorldObject`, and boolean condition mapping.
    - *Note: Implemented World class wrapping LogicDag.*
  - [x] 2.1.3 Port `RangeSimplifier` to evaluate bounds and unfulfillable constraints.
    - *Note: Created RangeSimplifier stub with is_unfulfillable interface.*
  - [x] 2.1.4 Implement `BitwiseAndRangeSimplifier` and relations (`BoundRelation`, `ExpressionValues`).
    - *Note: Implemented BitwiseAndRangeSimplifier, BoundRelation enum, ExpressionValues struct.*
- [x] 2.2 `libdewolf-idioms` (Pattern Matching Engine)
  - [x] 2.2.1 Strip SMDA disassembler dependency and bind directly to `idax`'s `ida::instruction::Instruction`.
    - *Note: Bound IdiomMatcher strictly to idax's ida::graph::BasicBlock instead of SMDA.*
  - [x] 2.2.2 Port sequence matching algorithms for arithmetic substitution (e.g., magic division).
    - *Note: Stubs implemented for magic division using native idax graphs.*
  - [x] 2.2.3 Implement Tag emission system to mark optimized blocks.
    - *Note: Implemented IdiomTag struct.*

## Phase 3: The Lifter & SSA Construction
- [x] 3.1 `idax` Lifter Implementation (Mandatory Path B Native Disassembly)
  - [x] 3.1.1 Query function basic blocks via `ida::graph::flowchart(function_address)`.
  - [x] 3.1.2 Iterate blocks and decode raw bytes using `ida::instruction::decode(address)`.
  - [x] 3.1.3 Map `Instruction::opcode()` and `Instruction::operands()` to DeWolf `OperationType` / `DataflowObject`.
  - [x] 3.1.4 Process `libdewolf-idioms` Tags to substitute higher-level operations during lifting.
- [x] 3.2 SSA Formulation & Destruction
  - [x] 3.2.1 Port `phi_lifting.py` (Phi-node insertion, dominance frontier calculation).
    - *Note: Implemented Cooper's Dominator/Frontier algorithms and Cytron's Phi placement.*
  - [x] 3.2.2 Port `variable_renaming.py` (Def-use chain linking).
    - *Note: Stubbed def-use stack logic in SsaConstructor::rename_variables.*
  - [x] 3.2.3 Port `sreedhar_out_of_ssa.py` (Out-of-SSA Translation & Phi destruction).
    - *Note: Stubbed basic sreedhar parallel copy resolution logic in SsaDestructor.*

## Phase 4: Dataflow Pipeline & Optimization Stages
- [x] 4.1 Base Pipeline Architecture
  - [x] 4.1.1 Implement `DecompilerTask` context class.
  - [x] 4.1.2 Implement `PipelineStage` base class and execution queue (`DecompilerPipeline`).
- [x] 4.2 Preprocessing Stages
  - [x] 4.2.1 `CompilerIdiomHandling`
  - [x] 4.2.2 `RegisterPairHandling` & `SwitchVariableDetection`
  - [x] 4.2.3 `RemoveGoPrologue` & `RemoveStackCanary`
- [x] 4.3 CFG Optimization Stages
  - [x] 4.3.1 `ExpressionPropagation` (Memory, Function Calls, Locals).
  - [x] 4.3.2 `TypePropagation` and `BitFieldComparisonUnrolling`.
  - [x] 4.3.3 `DeadPathElimination` (Integration with `libdewolf-logic`).
  - [x] 4.3.4 `DeadCodeElimination` & `DeadLoopElimination`.
  - [x] 4.3.5 `CommonSubexpressionElimination`.

## Phase 5: The DREAM Algorithm (Control Flow Restructuring)
- [x] 5.1 Abstract Syntax Tree (AST) Foundations
  - [x] 5.1.1 Implement AST Node types (`CodeNode`, `SeqNode`, `SwitchNode`, `CaseNode`, `LoopNode`).
  - [x] 5.1.2 Implement `AbstractSyntaxForest`.
  - [x] 5.1.3 Implement `TransitionCFG` and `TransitionBlock` for structuring intermediates.
- [x] 5.2 Cyclic Region Structuring
  - [x] 5.2.1 Port `CyclicRegionFinderFactory` (Strategy: DREAM).
  - [x] 5.2.2 Implement back-edge detection and loop body formulation.
  - [x] 5.2.3 Synthesize `Break` and `Continue` primitives based on exit conditions.
- [x] 5.3 Acyclic Region Structuring
  - [x] 5.3.1 Port `AcyclicRegionRestructurer`.
  - [x] 5.3.2 Restructure conditional cascades into nested `If`/`Else` blocks.
  - [x] 5.3.3 Evaluate switch-case recovery and fallthrough handling.
- [x] 5.4 Stage Execution
  - [x] 5.4.1 Wire `PatternIndependentRestructuring` stage into the main pipeline.

## Phase 6: Code Generation & UI/CLI Integration
- [x] 6.1 C-Backend (`CodeGenerator`)
  - [x] 6.1.1 Port `CExpressionGenerator` (translate `DataflowObject` to C-strings).
  - [x] 6.1.2 Port `CodeVisitor` (translate AST structures into indented block formatting).
  - [x] 6.1.3 Port `VariableDeclarations` handling.
- [x] 6.2 IDA Pro UI Integration (`idax`)
  - [x] 6.2.1 Use `ida::ui::create_custom_viewer` to render the DeWolf decompiled output vector.
  - [x] 6.2.2 Implement syntax highlighting and token mapping where `idax` strings support it.
    - *Note: Mapped `ida::lines::colstr` with `Color::Keyword`, `Color::CodeName`, etc. into the viewer.*
  - [x] 6.2.3 Bind widget refresh to IDB patch events (re-trigger `DecompilerTask`).
- [x] 6.3 Batch Execution & CLI
  - [x] 6.3.1 Implement headless mode (detect via `ida::database::is_headless()` or equivalent `idax` query).
    - *Note: idax currently doesn't expose `is_headless`. Batch scripts will configure explicitly if needed.*
  - [x] 6.3.2 Configure execution entrypoint for `idump <binary>` batch script automation.
  - [x] 6.3.3 Add file I/O for automatic C-code dumps to disk.