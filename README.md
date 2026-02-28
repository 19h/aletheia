<p align="center">
    <strong>aletheia</strong><br>
  <em>A decompiler based on DeWolf, rewritten for IDA Pro in C++23 using idax.</em>
</p>

<p align="center">
  <code>C++23</code> &middot; <code>idax-powered</code> &middot; <code>Zero Python</code> &middot; <code>Standalone Decompilation</code>
</p>

---

**Aletheia** is a ground-up, statically typed, high-performance port of the [DeWolf](https://github.com/fkie-cad/dewolf) research decompiler ecosystem, built entirely on the [`idax`](https://github.com/anomalyco/idax) C++23 SDK wrapper.

It operates **completely independently of the Hex-Rays decompiler**. It lifts native IDA disassembly directly into its own IR, structures it using the advanced DREAM control-flow restructuring algorithm, and emits highly readable C-like pseudocode.

```cpp
#include <aletheia/pipeline/pipeline.hpp>
#include <aletheia/lifter/lifter.hpp>
#include <aletheia/codegen/visitor.hpp>

// Create a decompilation task for a specific function address
aletheia::DecompilerTask task(function_address);

// Lift native IDA instructions directly into Aletheia's IR via idax
aletheia::Lifter lifter;
lifter.lift(task);

// Build and run the full DREAM restructuring pipeline
auto pipeline = aletheia::DecompilerPipeline::create_structured_pipeline();
pipeline.run(task);

if (!task.failed() && task.ast) {
    // Generate C-like pseudocode from the restructured AST
    aletheia::CodeVisitor visitor;
    task.ast->accept(visitor);
    
    for (const auto& line : visitor.get_lines()) {
        std::cout << line.text << "\n";
    }
}
```

---

## Why Aletheia exists

The original DeWolf ecosystem is a brilliant piece of decompilation research, implementing the **DREAM** and **DREAM++** algorithms (Yakdan et al.) to restructure complex and heavily optimized control flow without relying strictly on syntactic pattern matching.

However, its original Python implementation relies heavily on external tools (Binary Ninja for lifting, SMDA for disassembly, NetworkX for graphs, Python's GC). This makes it slow, memory-intensive, and difficult to deploy as a seamless interactive plugin.

Aletheia was built to fix this by bringing DeWolf's logic to native C++23:

1. **Native IDA Integration:** Bypasses Binary Ninja and Hex-Rays entirely. Lifts directly from IDA's native disassembly (`ida::instruction::decode`) via the `idax` wrapper.
2. **Performance:** Eliminates the Python interpreter overhead. Custom Arena and Bump allocators enable instantaneous O(1) AST/CFG node creation and teardown, avoiding garbage collection pauses.
3. **Robust IR:** Replaces fragile string-based types and ad-hoc graphs with a strict, statically typed Intermediate Representation hierarchy.
4. **Standalone:** Does not require a Hex-Rays license to decompile code.

---

## The Subsystems

The ecosystem is split into three thematic static libraries, named after Greek philosophical terms to reflect their role in revealing the true structure of the code:

| Library | Name Meaning | Role in Pipeline | Source Project |
|---------|-------------|------------------|----------------|
| **`aletheia`** | *Truth* (revealing what is hidden) | Core decompiler: IR, CFG, SSA, dataflow optimization, DREAM algorithm, C backend. | `dewolf` |
| **`logos`** | *Reason* / *Logic* | Constraint engine powered by Z3. Resolves dead paths and simplifies boolean ranges. | `dewolf-logic` |
| **`idiomata`**| *Idioms* / *Peculiarities* | Pattern matcher that detects compiler-optimized artifacts (e.g. magic division) and restores them. | `dewolf-idioms` |

---

## The Decompilation Pipeline

Aletheia processes functions through a strict, multi-stage pipeline, managed by `DecompilerTask`:

### 1. Lifting (Native Disassembly)
Uses `idax` to iterate `ida::graph::BasicBlock` and decode instructions. Maps x86-64 and ARM64 mnemonics directly into Aletheia's `DataflowObject` and `Instruction` IR. Discovers stack and frame variables heuristically.

### 2. Early Preprocessing
- **`CompilerIdiomHandling`**: Uses `idiomata` to identify magic-number divisions/modulos and replaces the obfuscated `imul`/`shr` sequences with clean `divide(x, y)` operations.
- **`RemoveGoPrologue` / `RemoveStackCanary`**: Prunes compiler boilerplate paths.

### 3. SSA Construction
Implements Cytron et al.'s algorithm for Static Single Assignment. Inserts φ-functions and computes dominance frontiers to version variables.

### 4. Dataflow Optimization
Over 15 distinct optimization passes run, including:
- **`ExpressionPropagation`**: Inter-block iterative fixed-point substitution of single-definition variables.
- **`DeadPathElimination`**: Uses `logos` (Z3) to test branch conditions. Prunes unfulfillable edges and unreachable blocks.
- **`CommonSubexpressionElimination`**: Extracts high-complexity repeated subexpressions into reusable temporaries.

### 5. DREAM Control-Flow Restructuring
Alternates between cyclic (loops) and acyclic (DAGs) structuring:
- **Loops**: Identifies back-edges, synthesizes `Break`/`Continue` nodes, and applies rules to recover `while`, `do-while`, and `for` loops.
- **Conditionals**: Employs Condition-Based Refinement (CBR) and Condition-Aware Refinement (CAR) to synthesize `if-else` chains and recover `switch-case` statements from nested equality checks.

### 6. Code Generation
`CodeVisitor` walks the final Abstract Syntax Tree (AST), tracking operator precedence to emit properly bracketed C code, collapsing compound assignments (`x += y`), and organizing local variable declarations.

---

## Architecture Highlights

**Strict `idax` Boundary**  
Direct inclusion of raw IDA SDK headers (`pro.h`, `ida.hpp`) is strictly forbidden. Aletheia relies 100% on the `idax` wrapper's explicit `Result<T>` and `Status` error-handling model.

**The Instruction Hierarchy**  
Instead of flat operation tags, every instruction is a distinct C++ type (`Assignment`, `Branch`, `Return`, `Phi`, `MemPhi`, etc.) inheriting from an abstract `Instruction` class. This enforces type-safe dispatch across all optimization passes.

**Arena Memory Management**  
Every IR node is allocated via `DecompilerArena`. Objects use the `ArenaAllocated<T>` CRTP base. Destructors are bypassed entirely; when a function completes, the arena's memory is bulk-freed instantly. Large strings (like generated variable names) are deduplicated via an interning pool to prevent heap leaks.

---

## Building and Usage

### Requirements
- CMake 3.27+
- C++23 Compiler (Clang 16+ / GCC 13+)
- IDA Pro 9.3 (idalib/idasdk)
- `idax` SDK (automatically fetched via CMake from GitHub)
- Microsoft Z3 (`brew install z3`)

### IDA Plugin

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build --target aletheia_ida
```
Drop `build/aletheia_ida.dylib` into your IDA `plugins/` directory. 
- Place the cursor inside a function and press **Ctrl-Shift-D**.
- The structured C code will appear in the **"Aletheia Decompiler"** custom viewer. 
- Byte patches dynamically trigger a re-decompilation.

### Headless Batch Decompiler (`idump`)

`idump` leverages `idalib` to run Aletheia headlessly across an entire binary without launching the IDA GUI. It attempts full DREAM structuring and falls back to a heavily annotated CFG snapshot if the output becomes degenerate.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build --target idump

# Decompile all functions to a C file
./build/idump firmware.bin -o decompiled.c
```

### Tests
```bash
# Run logic tests (no IDA required)
cmake --build build --target aletheia_tests
./build/aletheia_tests

# Run pattern-matching idiom tests
cmake --build build --target test_idiom_resolver
./build/test_idiom_resolver
```

---

## Configuration

Aletheia's pipeline is highly tunable via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `ALETHEIA_OUT_OF_SSA_MODE` | `lift_minimal` | Options: `simple`, `lift_minimal` (Lex-BFS coloring), `conditional`. |
| `ALETHEIA_VARIABLE_NAMING` | `default` | Uses `var_1` by default, or `system_hungarian` for typed prefixes (`iVar1`). |
| `ALETHEIA_INT_HEX_THRESHOLD` | `256` | Constants above this value are printed in hexadecimal. |
| `ALETHEIA_IF_BRANCH_PREFERENCE` | `none` | Options: `smallest` or `largest` to bias `if` true-branch output. |
| `ALETHEIA_IDUMP_ENABLE_STRUCTURING`| `1` | Set to `0` to force unstructured CFG fallback output in `idump`. |
| `ALETHEIA_IDUMP_FORCE_STRUCTURED_OUTPUT` | `0` | Prevent `idump` from falling back to unstructured CFG even if output is lossy. |

---

## Known Limitations

- **Structured Output Stability**: Complex irreducible control flow may occasionally cause the DREAM structuring phases (CBR/CAR) to fail. In `idump`, this gracefully falls back to a CFG snapshot.
- **Stack Variable Recovery**: Pointer-style temporaries (`*(var_0)`) still appear in some complex frame layouts before local variables are fully coalesced.
- **Floating-Point & SIMD**: While float operations are present in the IR, floating-point SSA tracking and SSE/AVX vector lifting are currently incomplete, emitting `__aletheia_unknown_op(...)` placeholders.

---

## License & Credits

- Derived from [DeWolf](https://github.com/fkie-cad/dewolf) (Fraunhofer FKIE).
- Built on [idax](https://github.com/anomalyco/idax) & [Z3](https://github.com/Z3Prover/z3).
