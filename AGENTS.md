# AGENTS.md

## Engine changes

The engine (`engine/quickjs-ng`) is our fork, [react-native-quickjs/quickjs-ng](https://github.com/react-native-quickjs/quickjs-ng), branch `develop`. An engine change is a commit in the fork; see `engine/README.md`.

1. Commit in `engine/quickjs-ng` on `develop`, and push the fork.
2. Commit the new submodule pointer here, with anything that depends on it.
3. Push with `git push --recurse-submodules=on-demand`.

- `BC_VERSION` (`quickjs.c`) has one value per VM build (stack, register). Bump the value of the VM whose bytecode format changed, in a fork commit of its own.
- Upstream is merged into `develop`, never rebased.

## Commits

- One line: a conventional subject in the present tense, saying what the code does now. A short summary body only when the subject can't carry it.
- No AI attribution: no `Co-Authored-By`, no "Generated with".
- One mechanism per commit. Stage exact files, never a directory.

## Code

- Comments state what the code does and why. No process words (phase, step, batch, plan, ticket numbers, names).
- Semantics never change. A fast path runs only under its stated conditions; output equals the interpreter's. A difference is a bug, never a baseline.
- The profile is the contract: AOT fast bodies are built for the profiled behaviour only. A violation is caught cheaply at function entry or where a value enters, and falls back to the generic body or the interpreter.
- Keep reference counting and the interpreter fallback.
- A cached pointer to a GC object holds a reference, or is invalidated by an engine event before the object is freed.
- Debug-only checks (verifiers, stress modes) are compiled out of release builds.
- Every bug fix gets a minimal repro test, checked on both VMs and under AOT where it applies.

## Fast iteration loop

One try is change → build → measure → decide, in minutes.

- Generate first, compile later: when a change alters generated code, regenerate and compare before compiling.
- Compile only what changed (the unit object cache); no PGO while iterating.
- Measure only the rows the change targets.
- Verify with the affected rows (compile and run with correct output) and the change's own repro tests, stack and register VM, plain and AOT. Full suites run once, before the work is committed.

## Measuring

- Benchmarks, profiles and AOT run on qjsc bytecode; compile speed is measured separately (`tools/bytecode/compile-bench.sh`).
- Cycles decide: `perf stat -e cycles:u,instructions:u` on a fixed-work run. Instructions explain, never decide. Benchmark scores are not used.
- One run each for A and B, back to back; repeat only when the difference is under ~2%.
- A and B use the identical build recipe; only the change differs.
- One heavy job at a time: nothing else runs during a build or a timing run.
