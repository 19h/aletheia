# P-Code Front-End Review (reviewer planning)

## Current understanding

- The task is to design and review a credible P-Code front-end for Aletheia without breaking the existing native lifter or downstream pipeline.
- Aletheia already has a strong reusable middle/back end: `DecompilerTask`, `ControlFlowGraph`, SSA, optimization, structuring, and codegen are in place.
- The current front-end contract is implicit rather than formal: the lifter must populate task signature metadata, emit a CFG with typed instructions/expressions, preserve original instruction addresses, and produce call/memory/control-flow shapes that downstream stages already understand.
- The available P-Code input is raw Sleigh per-instruction P-Code from `oneInstruction(...)`, not a ready-made high-level decompiler IR. That means ABI recovery, stack modeling, naming, and much control-flow ownership still remain Aletheia responsibilities.

## Decisions and rationale

1. Keep the existing pipeline and IR as the integration target; do not propose a downstream rewrite unless the front-end contract truly proves insufficient.
2. Treat P-Code support as a new front-end translation layer, not as a viewer feature and not as a replacement for IDA metadata.
3. Keep IDA responsible for function discovery, CFG block layout, names, prototypes, frame data, SP deltas, and database-backed symbol/string recovery.
4. Require the worker to separate raw P-Code collection from P-Code-to-Aletheia lowering so lowering can be unit-tested without depending on IDA/Sleigh runtime.
5. Call out native-path dependencies explicitly: several optimization stages still re-decode machine instructions through IDA, so a P-Code front-end must preserve original machine addresses and remain compatible with those assumptions.

## Open questions and blockers

- Whether Sleigh register varnodes can be mapped cleanly to canonical register names via accessible translator APIs, or whether a stable offset/size-based fallback naming layer is needed first.
- Whether current IR needs a minimal extension for common raw P-Code ops such as `PIECE` / non-zero-offset `SUBPIECE`, or whether an existing `cast`/`low` combination is sufficient for the initial implementation.
- How much of call-argument recovery can be reused from the native path versus needing a new P-Code-aware ABI helper, given that raw P-Code `CALL` does not carry explicit arguments.
- How to gate build integration with Sleigh so `make -j4` and `make test` stay green even if Sleigh targets are unavailable in another environment.

## Progress notes

- Reviewed `AGENTS.md` and existing shared notes.
- Inspected the native lifter, core IR, CFG, pipeline orchestration, CLI entry path, plugin entry path, SSA, codegen, invariants, and structuring expectations.
- Verified the concrete P-Code plugin example at `/Users/int/dev/idax/examples/plugin/idapcode_port_plugin.cpp` and the associated Sleigh build wiring in `/Users/int/dev/idax/examples/CMakeLists.txt`.
- Wrote detailed findings to:
  - `.ratio/research/pcode-frontend-architecture-review.md`
  - `.ratio/research/pcode-lowering-gap-analysis.md`
