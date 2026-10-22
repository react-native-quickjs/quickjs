# AOT corpus

Our own corpus of small deterministic programs for the AOT compiler, written for this
project (nothing is copied from another project's tests).

- `gen.py` writes the templated half (about 2,900 programs: an operation or a call
  form crossed with a type and a context). `calib.json` holds the loop counts that make
  each program take 60-250 ms in the interpreter.
- `hand/` holds about 100 hand-written programs of realistic code.
- `run.py` generates, calibrates and runs the corpus. `m1.sh corpus` (the generator
  harness) runs every program interpreted, with speculation on, with speculation off
  and with forced misses; an output that differs from the interpreter's is a failure.
  The report gives the geometric mean of compiled / interpreted time, the share of
  programs faster compiled, the families (the first two name parts) by geometric
  mean, the worst 50 programs, and compiled bytes per function.
- Every program prints one line ending in a checksum. The `flip_*` programs have a
  `.prof.js` companion whose profile sees only the early phase.
