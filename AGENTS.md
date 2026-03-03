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

## OUTPUT QUALITY IMPROVEMENT AUDIT (March 03, 2026)

### Baseline Defect Analysis

Six critical defects identified in the decompiled C output, each traced to a
specific pipeline stage and source location:

| # | Defect | Pipeline Stage | Files | Impact |
|---|--------|---------------|-------|--------|
| 1 | Call targets as numbers/`tmp_N()` | Lifter | `lifter.cpp:1789-1830` | Highest |
| 2 | Self-XOR/OR not simplified | Optimization | `optimization_stages.cpp:2815-2818` | High |
| 3 | `(cast)` without type info | Lifter | `lifter.cpp:2211-2294` | High |
| 4 | `bb_0:` flat CFG fallback | Codegen | `codegen.cpp:610-624` | Medium |
| 5 | ARM 8-arg injection | Lifter | `lifter.cpp:1816-1823` | Medium |
| 6 | `tmp_N` everywhere | Variable Naming | `variable_name_generation.cpp:235-267` | Medium |

### Implementation Decisions

1. **Call resolution uses `ida::name::get(addr)`** -- covers both function and
   non-function symbols. Confirmed available via `idax`. Uses `GlobalVariable` to
   prevent variable renaming from overwriting the resolved name. A custom
   `GlobalVariableCollector` prevents call-target GlobalVariables from generating
   spurious `extern` declarations.
2. **Self-XOR uses fingerprint comparison** -- same pattern as `bit_and` at line 2807.
   Also added `x | x -> x` and `x | 0xFFFF -> 0xFFFF` rules.
3. **Cast types use `Integer(bits, signed)`** -- `movsx`->signed, `movzx`->unsigned.
   Applied to `cbw`/`cwde`/`cdqe` and all `movsx`/`movzx`/`sxtb`/`uxth` variants.
4. **Single-block CodeNode** -- suppress `bb_0:` label when root is single CodeNode
   with no successors. Blocks with branches still enter fallback mode for gotos.
5. **ARM args** -- query callee prototype via `ida::type::retrieve()`, fall back
   to 0 args (not 8). Only inject AAPCS registers up to actual param count.
6. **Register name coverage** -- added `r8d`/`r9d`/`r8w`/`r9w`/`r8b`/`r9b`, `sil`/`dil`,
   `r10d`-`r15d` etc. to `is_register_like_name()`. Added 32-bit x86 register
   aliases (`edi`, `esi`, `edx`, `ecx`) to `infer_parameter_index_from_register_name()`.
7. **Call target protection** -- `rename_expression()` skips Call target renaming,
   only renames Call arguments.

### Progress Notes
- [x] Build baseline verified (release, all tests pass)
- [x] All 6 defects traced with exact line numbers
- [x] Fix 1: Call target resolution -- `ida::name::get()` + `GlobalVariable` + rename protection
- [x] Fix 2: Self-XOR/OR simplification -- fingerprint-based `x^x->0`, `x|x->x`, `x|0xFF->0xFF`
- [x] Fix 3: Cast type info -- `set_ir_type(Integer(bits, signed))` on all cast ops
- [x] Fix 4: bb_0 suppression -- no labels for single-block straight-line functions
- [x] Fix 5: ARM arg count -- prototype-based arg injection, 0 fallback
- [x] Fix 6: Variable naming -- extended register coverage, parameter inference
- [x] Fix 7: Conservative fallback rename -- removed env var gate, always rename
- [x] Fix 8: IDA address-prefix stripping -- `__0000000000000748grub_errno__` -> `grub_errno`
- [x] Fix 9: Self-assignment elimination -- post-rename + post-simplification + SSA identity fix
- [x] Fix 10: Call target rename scoping -- only GlobalVariable targets skip rename
- [x] Fix 11: ARM param register bound -- x0-x7 only, x8+ treated as temporaries
- [x] All unit tests pass (3/3)
- [x] `idump testbin` produces 10/10 functions
- [x] `idump tests/targets/test_binary` produces 6/6 functions, no bb_0 on simple functions
- [x] `idump test_main` produces 14/14 functions, ARM args dramatically reduced

### Verified Improvements (Before -> After)

| Issue | Before | After |
|-------|--------|-------|
| Self-XOR | `r8d ^= r8d` | `r8d = 0x0` |
| Cast types | `(cast)*(tmp_3)` | `(unsigned int)*(tmp_3)` |
| bb_0 labels | `bb_0:` on simple functions | No label on single-block functions |
| ARM args | `func(x0,x1,x2,x3,x4,x5,x6,x7)` | `func()` or `func(tmp_0)` |
| Self-OR | `x \| x` in expressions | Simplified to `x` |
| Register names | `eax, ecx, edx, esi, r8d, rax` | `tmp_N, arg_N` |
| Extern mangling | `__0000000000000748grub_errno__` | `grub_errno` |
| Self-assignments | `tmp_4 = tmp_4` | Eliminated |
| Call targets | `rax()` (raw register) | `tmp_3()` (renamed temporary) |
| ARM x16 | `arg_16` (wrong param) | `tmp_0` (temporary) |

## CYCLE 2: OUTPUT QUALITY IMPROVEMENT (March 04, 2026)

### Issues Addressed

| # | Issue | Root Cause | Fix |
|---|-------|-----------|-----|
| 1 | `tmp_4 = tmp_4` self-assignments in `iterate_device` | `remove_self_assignments()` only iterated CFG blocks, not AST-wrapped blocks | Added `remove_self_assignments_ast()` that traverses AST tree to find CodeNode blocks |
| 2 | Call targets unresolved (`tmp_N()`) for PLT calls | Only resolved `Constant` operands; PLT relocations use different addressing | Added `ida::xref::code_refs_from(addr)` fallback to resolve call targets via cross-references |
| 3 | `arg_0` undeclared in `grub_cmd_do_search` | `param_display_names_` only keyed by 64-bit register names; 32-bit aliases not mapped | Added `x86_64_sub_register_aliases()` helper; registered edi/esi/edx/ecx/r8d-r9d alongside rdi/rsi/rdx/rcx/r8/r9 |
| 4 | `__0000000000000748grub_errno__` not stripped | `sanitize_identifier()` didn't call `strip_ida_address_prefix()` | Integrated `strip_ida_address_prefix()` into `sanitize_identifier()` so ALL names are stripped |
| 5 | `arg_N` not resolved to prototype names (cmd, argc, args) | `param_display_names_` keyed by register names; after rename to `arg_N`, lookup failed | Added `arg_N` → best display name mapping using parameter index; prefer longest (most specific) name per index |

### Implementation Details

1. **Self-assignment elimination (AST-level)**:
   - Added `VariableNameGeneration::remove_self_assignments_ast(AbstractSyntaxForest*)` in `variable_name_generation.cpp`.
   - Traverses entire AST tree recursively to find `CodeNode`s and removes self-assignments from their blocks.
   - Called after `remove_self_assignments(cfg)` in both `idump.cpp` and `plugin.cpp` to catch assignments in AST-wrapped blocks that may differ from the flat CFG block list.

2. **x86-64 sub-register aliases**:
   - Added `x86_64_sub_register_aliases()` function in `lifter.cpp` that maps 64-bit register names to their 32/16/8-bit variants (rdi→edi/di/dil, r8→r8d/r8w/r8b, etc.).
   - Used in both prototype-based and fallback parameter mapping loops in `populate_task_signature()`.
   - Ensures `param_display_names_` has entries for ALL register widths, not just 64-bit.

3. **arg_N → prototype parameter name mapping**:
   - In codegen.cpp and idump.cpp, when building `param_display_names_`, now also adds `arg_0` → best display name for index 0, `arg_1` → best for index 1, etc.
   - Uses "longest name wins" heuristic to prefer IDA regvar names (`cmd`) over default names (`a1`).

4. **IDA address-prefix stripping**:
   - Moved `strip_ida_address_prefix()` call into `sanitize_identifier()` itself.
   - Now ALL identifier names go through the stripping, regardless of code path.
   - The `global_name_from_operand()` double-call is idempotent.

5. **xref-based call target resolution**:
   - Added `ida::xref::code_refs_from(addr)` fallback in both x86 and ARM call handlers.
   - Checks for `CallNear`/`CallFar` reference types.
   - Resolves the `.to` address to a symbol name via `ida::name::get()` or `ida::function::name_at()`.

### Progress Notes
- [x] Fix 1: Self-assignment elimination -- AST-level removal via `remove_self_assignments_ast()`
- [x] Fix 2: Call target xref fallback -- `ida::xref::code_refs_from()` for PLT/GOT resolution
- [x] Fix 3: 32-bit register aliases -- `x86_64_sub_register_aliases()` + parameter mapping
- [x] Fix 4: Address prefix in sanitize_identifier -- integrated into `sanitize_identifier()`
- [x] Fix 5: arg_N display mapping -- best-per-index lookup in `param_display_names_`
- [x] All unit tests pass (3/3)
- [x] `idump testbin` produces 10/10 functions

### Verified Cycle 2 Improvements (Before -> After)

| Issue | Before (Cycle 1) | After (Cycle 2) |
|-------|-------------------|------------------|
| Self-assignments | `tmp_4 = tmp_4; tmp_5 = tmp_5;` | Eliminated |
| Extern mangling | `__0000000000000748grub_errno__` | `grub_errno` |
| Register vars | `eax, ecx, edx, esi, r8d, rax` | `tmp_N, arg_N, argc, args, cmd` |
| Param names | `arg_0 = *(tmp_2)` (undeclared) | `cmd = *(tmp_2)` (prototype name) |
| Parameter refs | `esi, rdi, edx` → `arg_N` | `esi` → `argc`, `rdi` → `cmd`, `edx` → `args` |
